/**
 * @file tcp_transport.h
 * @brief TCP/TLS transport implementation for NATS client
 *
 * This file implements the NatsTransport interface using BSD sockets
 * and ESP-IDF's esp-tls/mbedtls for TLS support.
 */

#ifndef ESPIDF_NATS_TCP_TRANSPORT_H
#define ESPIDF_NATS_TCP_TRANSPORT_H

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <esp_log.h>
#include <esp_tls.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "transport.h"
#include "config.h"

/**
 * @brief TCP/TLS transport implementation
 *
 * Implements NatsTransport using BSD sockets with optional TLS via esp-tls
 * or mbedtls (for STARTTLS support).
 */
class TcpTransport : public NatsTransport {
private:
    static constexpr const char* TAG = "tcp_transport";

    int sockfd;
    esp_tls_t* tls;

    // mbedtls contexts for STARTTLS upgrade
    mbedtls_ssl_context* mbedtls_ssl;
    mbedtls_ssl_config* mbedtls_conf;
    mbedtls_x509_crt* mbedtls_cacert;
    mbedtls_entropy_context* mbedtls_entropy;
    mbedtls_ctr_drbg_context* mbedtls_ctr_drbg;
    bool using_mbedtls_directly;

    nats_tls_config_t tls_config;
    nats_transport_state_t state;
    nats_error_code_t last_error;

    SemaphoreHandle_t io_mutex;

    // Current connection hostname (for SNI)
    char* current_hostname;

public:
    TcpTransport() :
        sockfd(-1),
        tls(NULL),
        mbedtls_ssl(NULL),
        mbedtls_conf(NULL),
        mbedtls_cacert(NULL),
        mbedtls_entropy(NULL),
        mbedtls_ctr_drbg(NULL),
        using_mbedtls_directly(false),
        state(TRANSPORT_DISCONNECTED),
        last_error(NATS_ERR_NONE),
        current_hostname(NULL)
    {
        memset(&tls_config, 0, sizeof(nats_tls_config_t));
        io_mutex = xSemaphoreCreateRecursiveMutex();
        if (io_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create io_mutex");
            last_error = NATS_ERR_OUT_OF_MEMORY;
        }
    }

    virtual ~TcpTransport() {
        disconnect();
        if (io_mutex != NULL) {
            vSemaphoreDelete(io_mutex);
            io_mutex = NULL;
        }
        if (current_hostname != NULL) {
            free(current_hostname);
            current_hostname = NULL;
        }
    }

    // ========================================================================
    // Connection Lifecycle
    // ========================================================================

    bool connect(const nats_transport_config_t* config) override {
        if (config == NULL || config->hostname == NULL) {
            last_error = NATS_ERR_INVALID_ARG;
            return false;
        }

        // Store TLS config if provided
        if (config->tls_config != NULL) {
            tls_config = *config->tls_config;
        } else {
            memset(&tls_config, 0, sizeof(nats_tls_config_t));
        }

        // Store hostname for SNI
        if (current_hostname != NULL) {
            free(current_hostname);
        }
        current_hostname = strdup(config->hostname);

        state = TRANSPORT_CONNECTING;

        // Perform TCP connection
        if (!try_connect_to_host(config->hostname, config->port)) {
            state = TRANSPORT_ERROR;
            return false;
        }

        state = TRANSPORT_CONNECTED;
        last_error = NATS_ERR_NONE;
        return true;
    }

    void disconnect() override {
        if (io_mutex != NULL) xSemaphoreTakeRecursive(io_mutex, portMAX_DELAY);

        // Clean up mbedtls resources (STARTTLS mode)
        cleanup_mbedtls();

        // Clean up esp-tls resources
        if (tls != NULL) {
            esp_tls_conn_destroy(tls);
            tls = NULL;
        }

        // Close raw socket
        if (sockfd >= 0) {
            close(sockfd);
            sockfd = -1;
        }

        if (io_mutex != NULL) xSemaphoreGiveRecursive(io_mutex);

        state = TRANSPORT_DISCONNECTED;
    }

    nats_transport_state_t get_state() const override {
        return state;
    }

    bool is_connected() const override {
        return state == TRANSPORT_CONNECTED && sockfd >= 0;
    }

    // ========================================================================
    // Data Transmission
    // ========================================================================

    int send_data(const char* data, size_t len) override {
        if (data == NULL || len == 0) return 0;
        if (!is_connected()) {
            last_error = NATS_ERR_NOT_CONNECTED;
            return -1;
        }

        ssize_t ret;
        bool success = true;

        if (io_mutex != NULL) xSemaphoreTakeRecursive(io_mutex, portMAX_DELAY);

        if (using_mbedtls_directly && mbedtls_ssl != NULL) {
            // STARTTLS mode
            size_t total_sent = 0;
            while (total_sent < len) {
                ret = mbedtls_ssl_write(mbedtls_ssl, (const unsigned char*)(data + total_sent), len - total_sent);
                if (ret <= 0) {
                    ESP_LOGE(TAG, "mbedtls write failed: %d", (int)ret);
                    last_error = NATS_ERR_SOCKET_FAILED;
                    success = false;
                    break;
                }
                total_sent += ret;
            }
            if (success) ret = total_sent;
        } else if (tls_config.enabled && tls != NULL) {
            // esp-tls mode
            size_t total_sent = 0;
            while (total_sent < len) {
                ret = esp_tls_conn_write(tls, data + total_sent, len - total_sent);
                if (ret <= 0) {
                    ESP_LOGE(TAG, "TLS write failed: %d", (int)ret);
                    last_error = NATS_ERR_SOCKET_FAILED;
                    success = false;
                    break;
                }
                total_sent += ret;
            }
            if (success) ret = total_sent;
        } else {
            // Raw TCP
            if (sockfd < 0) {
                if (io_mutex != NULL) xSemaphoreGiveRecursive(io_mutex);
                last_error = NATS_ERR_NOT_CONNECTED;
                return -1;
            }
            size_t total_sent = 0;
            while (total_sent < len) {
                ret = ::send(sockfd, data + total_sent, len - total_sent, 0);
                if (ret <= 0) {
                    ESP_LOGE(TAG, "Socket send failed: %d", errno);
                    last_error = NATS_ERR_SOCKET_FAILED;
                    success = false;
                    break;
                }
                total_sent += ret;
            }
            if (success) ret = total_sent;
        }

        if (io_mutex != NULL) xSemaphoreGiveRecursive(io_mutex);

        if (!success) {
            disconnect();
            return -1;
        }

        return (int)ret;
    }

    int send_line(const char* line) override {
        if (line == NULL) return 0;

        size_t len = strlen(line);
        int bytes_sent = 0;

        // Send the line
        int ret = send_data(line, len);
        if (ret < 0) return ret;
        bytes_sent += ret;

        // Send CRLF
        ret = send_data(NATS_CR_LF, 2);
        if (ret < 0) return ret;
        bytes_sent += ret;

        return bytes_sent;
    }

    // ========================================================================
    // Data Reception
    // ========================================================================

    char* read_line(size_t initial_cap) override {
        if (!is_connected()) {
            last_error = NATS_ERR_NOT_CONNECTED;
            return (char*)calloc(1, sizeof(char));
        }

        size_t cap = initial_cap;
        char* buf = (char*)malloc(cap * sizeof(char));
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to allocate readline buffer");
            last_error = NATS_ERR_OUT_OF_MEMORY;
            return (char*)calloc(1, sizeof(char));
        }

        int i = 0;
        char c;
        int ret;

        if (io_mutex != NULL) xSemaphoreTakeRecursive(io_mutex, portMAX_DELAY);

        while (true) {
            if (using_mbedtls_directly && mbedtls_ssl != NULL) {
                ret = mbedtls_ssl_read(mbedtls_ssl, (unsigned char*)&c, 1);
            } else if (tls_config.enabled && tls != NULL) {
                ret = esp_tls_conn_read(tls, &c, 1);
            } else {
                ret = ::recv(sockfd, &c, 1, 0);
            }

            if (ret <= 0) {
                if (ret < 0) {
                    ESP_LOGE(TAG, "Read error: %d", ret);
                    last_error = NATS_ERR_SOCKET_FAILED;
                }
                if (io_mutex != NULL) xSemaphoreGiveRecursive(io_mutex);
                free(buf);
                disconnect();
                return (char*)calloc(1, sizeof(char));
            }

            if (c == '\r') continue;
            if (c == '\n') break;

            if ((size_t)i >= cap - 1) {
                if (cap >= NATS_MAX_LINE_SIZE) {
                    ESP_LOGE(TAG, "Line too long (exceeds %d bytes)", NATS_MAX_LINE_SIZE);
                    if (io_mutex != NULL) xSemaphoreGiveRecursive(io_mutex);
                    free(buf);
                    disconnect();
                    return (char*)calloc(1, sizeof(char));
                }
                if (cap > SIZE_MAX / 2) {
                    ESP_LOGE(TAG, "Readline buffer too large");
                    if (io_mutex != NULL) xSemaphoreGiveRecursive(io_mutex);
                    free(buf);
                    disconnect();
                    return (char*)calloc(1, sizeof(char));
                }
                cap *= 2;
                char* newbuf = (char*)realloc(buf, cap + 1);
                if (newbuf == NULL) {
                    ESP_LOGE(TAG, "Failed to realloc readline buffer");
                    if (io_mutex != NULL) xSemaphoreGiveRecursive(io_mutex);
                    free(buf);
                    disconnect();
                    return (char*)calloc(1, sizeof(char));
                }
                buf = newbuf;
            }
            buf[i++] = c;
        }

        if (io_mutex != NULL) xSemaphoreGiveRecursive(io_mutex);
        buf[i] = '\0';
        return buf;
    }

    char* read_bytes(size_t n) override {
        if (n == 0) {
            return (char*)calloc(1, sizeof(char));
        }

        if (!is_connected()) {
            last_error = NATS_ERR_NOT_CONNECTED;
            return NULL;
        }

        char* buf = (char*)malloc(n + 1);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to allocate read buffer for %zu bytes", n);
            last_error = NATS_ERR_OUT_OF_MEMORY;
            return NULL;
        }

        size_t total_read = 0;

        if (io_mutex != NULL) xSemaphoreTakeRecursive(io_mutex, portMAX_DELAY);

        while (total_read < n) {
            int ret;
            size_t to_read = n - total_read;

            if (using_mbedtls_directly && mbedtls_ssl != NULL) {
                ret = mbedtls_ssl_read(mbedtls_ssl, (unsigned char*)(buf + total_read), to_read);
            } else if (tls_config.enabled && tls != NULL) {
                ret = esp_tls_conn_read(tls, buf + total_read, to_read);
            } else {
                ret = ::recv(sockfd, buf + total_read, to_read, 0);
            }

            if (ret <= 0) {
                if (ret < 0) {
                    ESP_LOGE(TAG, "Read error: %d", ret);
                    last_error = NATS_ERR_SOCKET_FAILED;
                }
                if (io_mutex != NULL) xSemaphoreGiveRecursive(io_mutex);
                free(buf);
                disconnect();
                return NULL;
            }
            total_read += ret;
        }

        if (io_mutex != NULL) xSemaphoreGiveRecursive(io_mutex);
        buf[n] = '\0';
        return buf;
    }

    void skip_bytes(size_t n) override {
        if (n == 0 || !is_connected()) return;

        char discard[16];
        size_t remaining = n;

        if (io_mutex != NULL) xSemaphoreTakeRecursive(io_mutex, portMAX_DELAY);

        while (remaining > 0) {
            size_t to_read = (remaining < sizeof(discard)) ? remaining : sizeof(discard);
            int ret;

            if (using_mbedtls_directly && mbedtls_ssl != NULL) {
                ret = mbedtls_ssl_read(mbedtls_ssl, (unsigned char*)discard, to_read);
            } else if (tls_config.enabled && tls != NULL) {
                ret = esp_tls_conn_read(tls, discard, to_read);
            } else {
                ret = ::recv(sockfd, discard, to_read, 0);
            }

            if (ret <= 0) {
                if (ret < 0) {
                    ESP_LOGE(TAG, "Skip bytes error: %d", ret);
                }
                if (io_mutex != NULL) xSemaphoreGiveRecursive(io_mutex);
                disconnect();
                return;
            }
            remaining -= ret;
        }

        if (io_mutex != NULL) xSemaphoreGiveRecursive(io_mutex);
    }

    // ========================================================================
    // Polling
    // ========================================================================

    bool has_data_available(int timeout_ms) override {
        if (!is_connected()) return false;

        int fd = get_fd();
        if (fd < 0) return false;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int ret = select(fd + 1, &rfds, NULL, NULL, &tv);
        return (ret > 0 && FD_ISSET(fd, &rfds));
    }

    int get_fd() const override {
        if (tls_config.enabled && tls != NULL) {
            int fd = -1;
            if (esp_tls_get_conn_sockfd(tls, &fd) == ESP_OK) {
                return fd;
            }
            return -1;
        }
        return sockfd;
    }

    // ========================================================================
    // Error Handling
    // ========================================================================

    nats_error_code_t get_last_error() const override {
        return last_error;
    }

    const char* get_error_string() const override {
        switch (last_error) {
            case NATS_ERR_NONE: return "No error";
            case NATS_ERR_CONNECTION_FAILED: return "Connection failed";
            case NATS_ERR_DNS_RESOLUTION_FAILED: return "DNS resolution failed";
            case NATS_ERR_TLS_INIT_FAILED: return "TLS initialization failed";
            case NATS_ERR_TLS_CONNECTION_FAILED: return "TLS connection failed";
            case NATS_ERR_SOCKET_FAILED: return "Socket operation failed";
            case NATS_ERR_NOT_CONNECTED: return "Not connected";
            case NATS_ERR_OUT_OF_MEMORY: return "Out of memory";
            case NATS_ERR_INVALID_ARG: return "Invalid argument";
            default: return "Unknown error";
        }
    }

    // ========================================================================
    // TCP-Specific: STARTTLS Support
    // ========================================================================

    bool upgrade_to_tls(const nats_tls_config_t* tls_cfg) override {
        if (tls_cfg == NULL || sockfd < 0) {
            last_error = NATS_ERR_INVALID_ARG;
            return false;
        }

        // Store TLS config
        tls_config = *tls_cfg;

        if (!tls_config.enabled) {
            return true;  // TLS not enabled, nothing to do
        }

        ESP_LOGI(TAG, "Upgrading connection to TLS on socket %d (using mbedtls)...", sockfd);

        int ret;
        char err_buf[100];

        // Allocate mbedtls contexts
        mbedtls_ssl = (mbedtls_ssl_context*)malloc(sizeof(mbedtls_ssl_context));
        mbedtls_conf = (mbedtls_ssl_config*)malloc(sizeof(mbedtls_ssl_config));
        mbedtls_cacert = (mbedtls_x509_crt*)malloc(sizeof(mbedtls_x509_crt));
        mbedtls_entropy = (mbedtls_entropy_context*)malloc(sizeof(mbedtls_entropy_context));
        mbedtls_ctr_drbg = (mbedtls_ctr_drbg_context*)malloc(sizeof(mbedtls_ctr_drbg_context));

        if (!mbedtls_ssl || !mbedtls_conf || !mbedtls_cacert || !mbedtls_entropy || !mbedtls_ctr_drbg) {
            ESP_LOGE(TAG, "Failed to allocate mbedtls contexts");
            cleanup_mbedtls();
            last_error = NATS_ERR_OUT_OF_MEMORY;
            return false;
        }

        // Initialize contexts
        mbedtls_ssl_init(mbedtls_ssl);
        mbedtls_ssl_config_init(mbedtls_conf);
        mbedtls_x509_crt_init(mbedtls_cacert);
        mbedtls_entropy_init(mbedtls_entropy);
        mbedtls_ctr_drbg_init(mbedtls_ctr_drbg);

        // Seed the random number generator
        ret = mbedtls_ctr_drbg_seed(mbedtls_ctr_drbg, mbedtls_entropy_func, mbedtls_entropy,
                                    (const unsigned char*)"nats_tls", 8);
        if (ret != 0) {
            mbedtls_strerror(ret, err_buf, sizeof(err_buf));
            ESP_LOGE(TAG, "mbedtls_ctr_drbg_seed failed: %s (0x%x)", err_buf, -ret);
            cleanup_mbedtls();
            last_error = NATS_ERR_TLS_INIT_FAILED;
            return false;
        }

        // Parse CA certificate
        if (tls_config.ca_cert != NULL && tls_config.ca_cert_len > 0) {
            ret = mbedtls_x509_crt_parse(mbedtls_cacert,
                                         (const unsigned char*)tls_config.ca_cert,
                                         tls_config.ca_cert_len);
            if (ret != 0) {
                mbedtls_strerror(ret, err_buf, sizeof(err_buf));
                ESP_LOGE(TAG, "mbedtls_x509_crt_parse failed: %s (0x%x)", err_buf, -ret);
                cleanup_mbedtls();
                last_error = NATS_ERR_TLS_INIT_FAILED;
                return false;
            }
        }

        // Set up SSL config
        ret = mbedtls_ssl_config_defaults(mbedtls_conf,
                                          MBEDTLS_SSL_IS_CLIENT,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT);
        if (ret != 0) {
            mbedtls_strerror(ret, err_buf, sizeof(err_buf));
            ESP_LOGE(TAG, "mbedtls_ssl_config_defaults failed: %s (0x%x)", err_buf, -ret);
            cleanup_mbedtls();
            last_error = NATS_ERR_TLS_INIT_FAILED;
            return false;
        }

        // Configure certificate verification
        if (tls_config.ca_cert != NULL) {
            mbedtls_ssl_conf_ca_chain(mbedtls_conf, mbedtls_cacert, NULL);
            if (tls_config.skip_cert_verification) {
                mbedtls_ssl_conf_authmode(mbedtls_conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
            } else {
                mbedtls_ssl_conf_authmode(mbedtls_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
            }
        } else {
            mbedtls_ssl_conf_authmode(mbedtls_conf, MBEDTLS_SSL_VERIFY_NONE);
        }

        mbedtls_ssl_conf_rng(mbedtls_conf, mbedtls_ctr_drbg_random, mbedtls_ctr_drbg);

        // Set up SSL context
        ret = mbedtls_ssl_setup(mbedtls_ssl, mbedtls_conf);
        if (ret != 0) {
            mbedtls_strerror(ret, err_buf, sizeof(err_buf));
            ESP_LOGE(TAG, "mbedtls_ssl_setup failed: %s (0x%x)", err_buf, -ret);
            cleanup_mbedtls();
            last_error = NATS_ERR_TLS_INIT_FAILED;
            return false;
        }

        // Set hostname for SNI and certificate verification
        const char* hostname = tls_config.server_name;
        if (hostname == NULL) {
            hostname = current_hostname;
        }
        if (hostname != NULL) {
            ret = mbedtls_ssl_set_hostname(mbedtls_ssl, hostname);
            if (ret != 0) {
                mbedtls_strerror(ret, err_buf, sizeof(err_buf));
                ESP_LOGE(TAG, "mbedtls_ssl_set_hostname failed: %s (0x%x)", err_buf, -ret);
                cleanup_mbedtls();
                last_error = NATS_ERR_TLS_INIT_FAILED;
                return false;
            }
        }

        // Set up bio callbacks to use our existing socket
        mbedtls_ssl_set_bio(mbedtls_ssl, &sockfd, mbedtls_net_send_cb, mbedtls_net_recv_cb, NULL);

        // Perform TLS handshake
        ESP_LOGI(TAG, "Starting TLS handshake...");
        while ((ret = mbedtls_ssl_handshake(mbedtls_ssl)) != 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                mbedtls_strerror(ret, err_buf, sizeof(err_buf));
                ESP_LOGE(TAG, "mbedtls_ssl_handshake failed: %s (0x%x)", err_buf, -ret);
                cleanup_mbedtls();
                last_error = NATS_ERR_TLS_CONNECTION_FAILED;
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        // Verify certificate (optional logging)
        uint32_t flags = mbedtls_ssl_get_verify_result(mbedtls_ssl);
        if (flags != 0) {
            char vrfy_buf[512];
            mbedtls_x509_crt_verify_info(vrfy_buf, sizeof(vrfy_buf), "  ! ", flags);
            ESP_LOGW(TAG, "Certificate verification warning: %s", vrfy_buf);
        }

        using_mbedtls_directly = true;
        ESP_LOGI(TAG, "TLS upgrade successful (cipher: %s)", mbedtls_ssl_get_ciphersuite(mbedtls_ssl));
        return true;
    }

private:
    // Static callback functions for mbedtls bio
    static int mbedtls_net_send_cb(void* ctx, const unsigned char* buf, size_t len) {
        int fd = *((int*)ctx);
        int ret = ::send(fd, buf, len, 0);
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return MBEDTLS_ERR_SSL_WANT_WRITE;
            }
            return MBEDTLS_ERR_NET_SEND_FAILED;
        }
        return ret;
    }

    static int mbedtls_net_recv_cb(void* ctx, unsigned char* buf, size_t len) {
        int fd = *((int*)ctx);
        int ret = ::recv(fd, buf, len, 0);
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return MBEDTLS_ERR_SSL_WANT_READ;
            }
            return MBEDTLS_ERR_NET_RECV_FAILED;
        }
        return ret;
    }

    void cleanup_mbedtls() {
        if (mbedtls_ssl) {
            mbedtls_ssl_free(mbedtls_ssl);
            free(mbedtls_ssl);
            mbedtls_ssl = NULL;
        }
        if (mbedtls_conf) {
            mbedtls_ssl_config_free(mbedtls_conf);
            free(mbedtls_conf);
            mbedtls_conf = NULL;
        }
        if (mbedtls_cacert) {
            mbedtls_x509_crt_free(mbedtls_cacert);
            free(mbedtls_cacert);
            mbedtls_cacert = NULL;
        }
        if (mbedtls_ctr_drbg) {
            mbedtls_ctr_drbg_free(mbedtls_ctr_drbg);
            free(mbedtls_ctr_drbg);
            mbedtls_ctr_drbg = NULL;
        }
        if (mbedtls_entropy) {
            mbedtls_entropy_free(mbedtls_entropy);
            free(mbedtls_entropy);
            mbedtls_entropy = NULL;
        }
        using_mbedtls_directly = false;
    }

    bool try_connect_to_host(const char* hostname, int port) {
        struct addrinfo hints = {};
        struct addrinfo *result, *rp;
        char port_str[6];
        snprintf(port_str, sizeof(port_str), "%d", port);

        hints.ai_family = AF_UNSPEC;     // Allow IPv4 or IPv6
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        int ret = getaddrinfo(hostname, port_str, &hints, &result);
        if (ret != 0) {
            ESP_LOGE(TAG, "DNS resolution failed for %s: %d", hostname, ret);
            last_error = NATS_ERR_DNS_RESOLUTION_FAILED;
            return false;
        }

        // Try each address until we successfully connect
        for (rp = result; rp != NULL; rp = rp->ai_next) {
            sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (sockfd < 0) continue;

            // Set socket to non-blocking mode for timeout support
            int flags = fcntl(sockfd, F_GETFL, 0);
            if (flags < 0 || fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
                ESP_LOGW(TAG, "Failed to set non-blocking mode, using blocking connect");
                if (::connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0) {
                    freeaddrinfo(result);
                    last_error = NATS_ERR_NONE;
                    return true;
                }
                close(sockfd);
                sockfd = -1;
                continue;
            }

            // Initiate non-blocking connect
            int conn_ret = ::connect(sockfd, rp->ai_addr, rp->ai_addrlen);
            if (conn_ret == 0) {
                fcntl(sockfd, F_SETFL, flags);
                freeaddrinfo(result);
                last_error = NATS_ERR_NONE;
                return true;
            }

            if (errno != EINPROGRESS) {
                close(sockfd);
                sockfd = -1;
                continue;
            }

            // Wait for connection with timeout (30 seconds)
            fd_set write_fds;
            FD_ZERO(&write_fds);
            FD_SET(sockfd, &write_fds);
            struct timeval timeout;
            timeout.tv_sec = 30;
            timeout.tv_usec = 0;

            int select_ret = select(sockfd + 1, NULL, &write_fds, NULL, &timeout);
            if (select_ret <= 0) {
                ESP_LOGE(TAG, "Connection timeout or select error for %s:%d", hostname, port);
                close(sockfd);
                sockfd = -1;
                continue;
            }

            // Check if connection succeeded
            int sock_err = 0;
            socklen_t len = sizeof(sock_err);
            if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &sock_err, &len) < 0 || sock_err != 0) {
                ESP_LOGE(TAG, "Connection failed for %s:%d: %d", hostname, port, sock_err);
                close(sockfd);
                sockfd = -1;
                continue;
            }

            // Connection successful - restore blocking mode
            fcntl(sockfd, F_SETFL, flags);
            freeaddrinfo(result);
            last_error = NATS_ERR_NONE;
            return true;
        }

        // All addresses failed
        freeaddrinfo(result);
        last_error = NATS_ERR_CONNECTION_FAILED;
        return false;
    }
};

#endif // ESPIDF_NATS_TCP_TRANSPORT_H
