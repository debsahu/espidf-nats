#ifndef ESPIDF_NATS_CLIENT_H
#define ESPIDF_NATS_CLIENT_H

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <limits.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <esp_log.h>
#include <esp_tls.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config.h"
#include "util.h"
#include "types.h"
#include "subscription.h"

typedef void (*event_cb)();
typedef void (*connect_cb)(bool success);

class NATS {

    public:
        typedef nats_msg_t msg;

    private:
        // Use types from types.h and subscription.h instead of duplicating
        enum ConnectState {
            DISCONNECTED,
            CONNECTING,
            CONNECTED
        };

        int sockfd;
        esp_tls_t* tls;
        nats_tls_config_t tls_config;
        NATSUtil::Array<nats_server_t> servers;
        size_t current_server_idx;
        const char* user;
        const char* pass;

        NATSUtil::Array<Sub*> subs;
        NATSUtil::Queue<size_t> free_sids;
        NATSUtil::Queue<pending_msg_t> pending_messages;

        NATSUtil::MillisTimer ping_timer;
        NATSUtil::MillisTimer reconnect_timer;

        int outstanding_pings;
        int reconnect_attempts;
        unsigned long last_reconnect_attempt;

        ConnectState connect_state;
        connect_cb async_connect_cb;

        nats_connection_metrics_t metrics;
        nats_error_code_t last_error_code;
        bool draining;
        SemaphoreHandle_t mutex;

    public:
        bool connected;
        int max_outstanding_pings;
        int max_reconnect_attempts;
        bool exponential_backoff_enabled;
        bool message_buffering_enabled;
        size_t max_pending_messages;

        event_cb on_connect;
        event_cb on_disconnect;
        event_cb on_error;

    public:
        // Single server constructor
        NATS(const char* hostname,
                int port = NATS_DEFAULT_PORT,
                const char* user = NULL,
                const char* pass = NULL,
                const nats_tls_config_t* tls_cfg = NULL) :
            sockfd(-1),
            tls(NULL),
            current_server_idx(0),
            user(user),
            pass(pass),
            ping_timer(NATS_PING_INTERVAL),
            reconnect_timer(NATS_RECONNECT_INTERVAL),
            outstanding_pings(0),
            reconnect_attempts(0),
            last_reconnect_attempt(0),
            connect_state(DISCONNECTED),
            async_connect_cb(NULL),
            connected(false),
            max_outstanding_pings(3),
            max_reconnect_attempts(-1),
            exponential_backoff_enabled(true),
            message_buffering_enabled(true),
            max_pending_messages(NATS_MAX_PENDING_MESSAGES),
            on_connect(NULL),
            on_disconnect(NULL),
            on_error(NULL) {
                nats_server_t server = {hostname, port};
                servers.push_back(server);
                if (tls_cfg != NULL) {
                    tls_config = *tls_cfg;
                } else {
                    memset(&tls_config, 0, sizeof(nats_tls_config_t));
                }
                memset(&metrics, 0, sizeof(nats_connection_metrics_t));
                last_error_code = NATS_ERR_NONE;
                draining = false;
                mutex = xSemaphoreCreateMutex();
            }

        // Multiple servers constructor with failover
        NATS(const nats_server_t* server_list,
                size_t server_count,
                const char* user = NULL,
                const char* pass = NULL,
                const nats_tls_config_t* tls_cfg = NULL) :
            sockfd(-1),
            tls(NULL),
            current_server_idx(0),
            user(user),
            pass(pass),
            ping_timer(NATS_PING_INTERVAL),
            reconnect_timer(NATS_RECONNECT_INTERVAL),
            outstanding_pings(0),
            reconnect_attempts(0),
            last_reconnect_attempt(0),
            connect_state(DISCONNECTED),
            async_connect_cb(NULL),
            connected(false),
            max_outstanding_pings(3),
            max_reconnect_attempts(-1),
            exponential_backoff_enabled(true),
            message_buffering_enabled(true),
            max_pending_messages(NATS_MAX_PENDING_MESSAGES),
            on_connect(NULL),
            on_disconnect(NULL),
            on_error(NULL) {
                for (size_t i = 0; i < server_count; i++) {
                    servers.push_back(server_list[i]);
                }
                // Randomize initial server selection for load distribution
                if (server_count > 1) {
                    current_server_idx = NATSUtil::random(server_count);
                }
                if (tls_cfg != NULL) {
                    tls_config = *tls_cfg;
                } else {
                    memset(&tls_config, 0, sizeof(nats_tls_config_t));
                }
                memset(&metrics, 0, sizeof(nats_connection_metrics_t));
                last_error_code = NATS_ERR_NONE;
                draining = false;
                mutex = xSemaphoreCreateMutex();
            }

        // Destructor to cleanup resources
        ~NATS() {
            // Disconnect if still connected
            if (connected) {
                disconnect();
            }

            // Clean up all subscriptions
            if (mutex != NULL) {
                xSemaphoreTake(mutex, portMAX_DELAY);
                for (size_t i = 0; i < subs.size(); i++) {
                    if (subs[i] != NULL) {
                        delete subs[i];
                        subs[i] = NULL;
                    }
                }

                // Clean up pending messages
                while (!pending_messages.empty()) {
                    pending_msg_t pmsg = pending_messages.pop();
                    if (pmsg.subject != NULL) free(pmsg.subject);
                    if (pmsg.message != NULL) free(pmsg.message);
                    if (pmsg.reply_to != NULL) free(pmsg.reply_to);
                    if (pmsg.headers != NULL) free(pmsg.headers);
                }
                xSemaphoreGive(mutex);

                // Delete the mutex
                vSemaphoreDelete(mutex);
                mutex = NULL;
            }
        }

    private:
        void send(const char* msg) {
            if (msg == NULL) return;
            size_t len = strlen(msg);
            ssize_t ret;
            if (tls_config.enabled && tls != NULL) {
                ret = esp_tls_conn_write(tls, msg, len);
                if (ret < 0 || (size_t)ret != len) {
                    ESP_LOGE(tag, "TLS write failed: %d", (int)ret);
                    last_error_code = NATS_ERR_SOCKET_FAILED;
                    disconnect();
                    return;
                }
                ret = esp_tls_conn_write(tls, NATS_CR_LF, strlen(NATS_CR_LF));
                if (ret < 0) {
                    ESP_LOGE(tag, "TLS write CRLF failed: %d", (int)ret);
                    last_error_code = NATS_ERR_SOCKET_FAILED;
                    disconnect();
                    return;
                }
            } else {
                if (sockfd < 0) return;
                ret = ::send(sockfd, msg, len, 0);
                if (ret < 0) {
                    ESP_LOGE(tag, "Socket send failed: %d", errno);
                    last_error_code = NATS_ERR_SOCKET_FAILED;
                    disconnect();
                    return;
                }
                ret = ::send(sockfd, NATS_CR_LF, strlen(NATS_CR_LF), 0);
                if (ret < 0) {
                    ESP_LOGE(tag, "Socket send CRLF failed: %d", errno);
                    last_error_code = NATS_ERR_SOCKET_FAILED;
                    disconnect();
                    return;
                }
            }
            metrics.bytes_sent += len + strlen(NATS_CR_LF);
        }

        int vasprintf(char** strp, const char* fmt, va_list ap) {
            va_list ap2;
            va_copy(ap2, ap);
            char tmp[1];
            int size = vsnprintf(tmp, 1, fmt, ap2);
            if (size <= 0) return size;
            va_end(ap2);
            size += 1;
            *strp = (char*)malloc(size * sizeof(char));
            return vsnprintf(*strp, size, fmt, ap);
        }

        void send_fmt(const char* fmt, ...) {
            va_list args;
            va_start(args, fmt);
            char* buf;
            vasprintf(&buf, fmt, args);
            va_end(args);
            send(buf);
            free(buf);
        }

        void send_connect() {
            send_fmt(
                    "CONNECT {"
                        "\"verbose\": %s,"
                        "\"pedantic\": %s,"
                        "\"lang\": \"%s\","
                        "\"version\": \"%s\","
                        "\"user\":\"%s\","
                        "\"pass\":\"%s\""
                    "}",
                    NATS_CONF_VERBOSE? "true" : "false",
                    NATS_CONF_PEDANTIC? "true" : "false",
                    NATS_CLIENT_LANG,
                    NATS_CLIENT_VERSION,
                    (user == NULL)? "null" : user,
                    (pass == NULL)? "null" : pass);
        }

        char* client_readline(size_t cap = 128) {
            char* buf = (char*)malloc(cap * sizeof(char));
            if (buf == NULL) {
                ESP_LOGE(tag, "Failed to allocate readline buffer");
                disconnect();
                return (char*)calloc(1, sizeof(char)); // Return empty string
            }
            int i = 0;
            char c;
            int ret;
            while (true) {
                if (tls_config.enabled && tls != NULL) {
                    ret = esp_tls_conn_read(tls, &c, 1);
                } else {
                    ret = ::recv(sockfd, &c, 1, 0);
                }
                if (ret <= 0) {
                    // Connection closed (0) or error (-1)
                    if (ret < 0) {
                        ESP_LOGE(tag, "Read error: %d", ret);
                        last_error_code = NATS_ERR_SOCKET_FAILED;
                    }
                    free(buf);
                    disconnect();
                    return (char*)calloc(1, sizeof(char)); // Return empty string
                }
                if (c == '\r') continue;
                if (c == '\n') break;
                if (i >= cap) {
                    char* newbuf = (char*)realloc(buf, (cap *= 2) * sizeof(char) + 1);
                    if (newbuf == NULL) {
                        ESP_LOGE(tag, "Failed to realloc readline buffer");
                        free(buf);
                        disconnect();
                        return (char*)calloc(1, sizeof(char));
                    }
                    buf = newbuf;
                }
                buf[i++] = c;
            }
            buf[i] = '\0';
            return buf;
        }

        void recv() {
            char* buf = client_readline();
            size_t argc = 0;
            const char* argv[NATS_MAX_ARGV] = {};
            for (int i = 0; i < NATS_MAX_ARGV; i++) {
                argv[i] = strtok((i == 0) ? buf : NULL, " ");
                if (argv[i] == NULL) break;
                argc++;
            }
            if (argc == 0) {}
            if (strcmp(argv[0], NATS_CTRL_MSG) == 0) {
                if (argc != 4 && argc != 5) { free(buf); return; }
                char* endptr;
                long sid_long = strtol(argv[2], &endptr, 10);
                if (*endptr != '\0' || sid_long < 0 || sid_long > INT_MAX) { free(buf); return; }
                int sid = (int)sid_long;
                // Validate SID bounds (check with mutex)
                xSemaphoreTake(mutex, portMAX_DELAY);
                if (sid < 0 || sid >= subs.size() || subs[sid] == NULL) {
                    xSemaphoreGive(mutex);
                    free(buf);
                    return;
                };
                xSemaphoreGive(mutex);

                long payload_size_long = strtol((argc == 5)? argv[4] : argv[3], &endptr, 10);
                if (*endptr != '\0' || payload_size_long < 0 || payload_size_long > INT_MAX) { free(buf); return; }
                int payload_size = (int)payload_size_long;
                // Check for integer overflow
                if (payload_size < 0 || payload_size > INT_MAX - 1) { free(buf); return; }
                payload_size += 1;
                char* payload_buf = client_readline(payload_size);
                msg e = {
                    argv[1],
                    sid,
                    (argc == 5)? argv[3] : "",
                    NULL,
                    0,
                    payload_buf,
                    payload_size
                };
                metrics.msgs_received++;
                metrics.bytes_received += payload_size;

                // Get callback without holding mutex for long
                xSemaphoreTake(mutex, portMAX_DELAY);
                Sub* sub = subs[sid];
                xSemaphoreGive(mutex);

                if (sub != NULL) {
                    // Call user callback WITHOUT holding mutex to prevent deadlock
                    sub->call(e);

                    // Check if we should unsubscribe
                    xSemaphoreTake(mutex, portMAX_DELAY);
                    bool should_unsub = (subs[sid] != NULL && subs[sid]->maxed());
                    xSemaphoreGive(mutex);

                    if (should_unsub) unsubscribe(sid);
                }
                free(payload_buf);
            }
            else if (strcmp(argv[0], NATS_CTRL_HMSG) == 0) {
                // HMSG <subject> <sid> [reply-to] <header bytes> <total bytes>
                if (argc != 5 && argc != 6) { free(buf); return; }
                char* endptr;
                long sid_long = strtol(argv[2], &endptr, 10);
                if (*endptr != '\0' || sid_long < 0 || sid_long > INT_MAX) { free(buf); return; }
                int sid = (int)sid_long;
                // Validate SID bounds (check with mutex)
                xSemaphoreTake(mutex, portMAX_DELAY);
                if (sid < 0 || sid >= subs.size() || subs[sid] == NULL) {
                    xSemaphoreGive(mutex);
                    free(buf);
                    return;
                };
                xSemaphoreGive(mutex);

                long header_size_long = strtol((argc == 6)? argv[4] : argv[3], &endptr, 10);
                if (*endptr != '\0' || header_size_long < 0 || header_size_long > INT_MAX) { free(buf); return; }
                int header_size = (int)header_size_long;
                long total_size_long = strtol((argc == 6)? argv[5] : argv[4], &endptr, 10);
                if (*endptr != '\0' || total_size_long < 0 || total_size_long > INT_MAX) { free(buf); return; }
                int total_size = (int)total_size_long;
                // Check for integer overflow
                if (total_size < 0 || total_size > INT_MAX - 1) { free(buf); return; }
                total_size += 1;
                int data_size = total_size - header_size;

                char* full_buf = client_readline(total_size);
                char* header_buf = full_buf;
                char* data_buf = full_buf + header_size;

                msg e = {
                    argv[1],
                    sid,
                    (argc == 6)? argv[3] : "",
                    header_buf,
                    header_size,
                    data_buf,
                    data_size
                };
                metrics.msgs_received++;
                metrics.bytes_received += total_size;

                // Get callback without holding mutex for long
                xSemaphoreTake(mutex, portMAX_DELAY);
                Sub* sub = subs[sid];
                xSemaphoreGive(mutex);

                if (sub != NULL) {
                    // Call user callback WITHOUT holding mutex to prevent deadlock
                    sub->call(e);

                    // Check if we should unsubscribe
                    xSemaphoreTake(mutex, portMAX_DELAY);
                    bool should_unsub = (subs[sid] != NULL && subs[sid]->maxed());
                    xSemaphoreGive(mutex);

                    if (should_unsub) unsubscribe(sid);
                }
                free(full_buf);
            }
            else if (strcmp(argv[0], NATS_CTRL_OK) == 0) {
            }
            else if (strcmp(argv[0], NATS_CTRL_ERR) == 0) {
                last_error_code = NATS_ERR_PROTOCOL_ERROR;
                if (on_error != NULL) on_error();
                disconnect();
            }
            else if (strcmp(argv[0], NATS_CTRL_PING) == 0) {
                send(NATS_CTRL_PONG);
            }
            else if (strcmp(argv[0], NATS_CTRL_PONG) == 0) {
                outstanding_pings--;
            }
            else if (strcmp(argv[0], NATS_CTRL_INFO) == 0) {
                send_connect();
                connected = true;
                if (on_connect != NULL) on_connect();
                // Send any pending messages that were buffered while offline
                send_pending_messages();
            }
            free(buf);
        }

        void ping() {
            if (outstanding_pings > max_outstanding_pings) {
                last_error_code = NATS_ERR_MAX_PINGS_EXCEEDED;
                disconnect();
                return;
            }
            outstanding_pings++;
            send(NATS_CTRL_PING);
        }

        char* generate_inbox_subject() {
            size_t size = strlen(NATS_INBOX_PREFIX) + NATS_INBOX_ID_LENGTH + 1;
            char* buf = (char*)malloc(size);
            if (buf == NULL) {
                ESP_LOGE(tag, "Failed to allocate memory for inbox subject");
                return NULL;
            }
            strcpy(buf, NATS_INBOX_PREFIX);
            int i;
            size_t alphanum_len = sizeof(NATSUtil::alphanums) - 1;
            for (i = strlen(NATS_INBOX_PREFIX); i < size - 1; i++) {
                // Avoid modulo bias by using rejection sampling
                uint32_t random_val;
                uint32_t max_valid = (UINT32_MAX / alphanum_len) * alphanum_len;
                do {
                    random_val = esp_random();
                } while (random_val >= max_valid);
                int random_idx = random_val % alphanum_len;
                buf[i] = NATSUtil::alphanums[random_idx];
            }
            buf[i] = '\0';
            return buf;
        }

        unsigned long get_reconnect_delay() {
            if (!exponential_backoff_enabled) {
                return NATS_RECONNECT_INTERVAL;
            }

            // Exponential backoff: 1s, 2s, 4s, 8s, 16s, 30s (max)
            unsigned long delay = NATS_RECONNECT_INTERVAL * (1 << reconnect_attempts);
            if (delay > NATS_MAX_RECONNECT_DELAY) {
                delay = NATS_MAX_RECONNECT_DELAY;
            }
            return delay;
        }

        void buffer_message(const char* subject, const char* msg, const char* replyto, const char* headers) {
            if (!message_buffering_enabled) return;
            if (pending_messages.size() >= max_pending_messages) return;

            pending_msg_t pmsg;
            pmsg.subject = (subject != NULL) ? strdup(subject) : NULL;
            pmsg.message = (msg != NULL) ? strdup(msg) : NULL;
            pmsg.reply_to = (replyto != NULL) ? strdup(replyto) : NULL;
            pmsg.headers = (headers != NULL) ? strdup(headers) : NULL;

            // Check for allocation failures
            if (subject != NULL && pmsg.subject == NULL) return;
            if (msg != NULL && pmsg.message == NULL) {
                if (pmsg.subject != NULL) free(pmsg.subject);
                return;
            }
            if (replyto != NULL && pmsg.reply_to == NULL) {
                if (pmsg.subject != NULL) free(pmsg.subject);
                if (pmsg.message != NULL) free(pmsg.message);
                return;
            }
            if (headers != NULL && pmsg.headers == NULL) {
                if (pmsg.subject != NULL) free(pmsg.subject);
                if (pmsg.message != NULL) free(pmsg.message);
                if (pmsg.reply_to != NULL) free(pmsg.reply_to);
                return;
            }

            pending_messages.push(pmsg);
        }

        void send_pending_messages() {
            while (!pending_messages.empty()) {
                pending_msg_t pmsg = pending_messages.pop();

                if (pmsg.headers != NULL) {
                    publish_with_headers(pmsg.subject, pmsg.headers, pmsg.message, pmsg.reply_to);
                } else {
                    publish(pmsg.subject, pmsg.message, pmsg.reply_to);
                }

                // Free allocated memory
                if (pmsg.subject != NULL) free(pmsg.subject);
                if (pmsg.message != NULL) free(pmsg.message);
                if (pmsg.reply_to != NULL) free(pmsg.reply_to);
                if (pmsg.headers != NULL) free(pmsg.headers);
            }
        }

    private:
        bool try_connect_to_server(const nats_server_t& server) {
            if (tls_config.enabled) {
                // TLS connection
                esp_tls_cfg_t cfg = {};
                cfg.cacert_buf = (const unsigned char*)tls_config.ca_cert;
                cfg.cacert_bytes = tls_config.ca_cert_len;
                cfg.timeout_ms = 30000;  // 30 second timeout
                cfg.non_block = false;    // Use blocking mode for simplicity

                if (tls_config.client_cert != NULL && tls_config.client_key != NULL) {
                    cfg.clientcert_buf = (const unsigned char*)tls_config.client_cert;
                    cfg.clientcert_bytes = tls_config.client_cert_len;
                    cfg.clientkey_buf = (const unsigned char*)tls_config.client_key;
                    cfg.clientkey_bytes = tls_config.client_key_len;
                }

                if (tls_config.skip_cert_verification) {
                    // Skip both CN check and server certificate verification
                    cfg.skip_common_name = true;
                    // Note: esp_tls doesn't have skip_server_cert_verify in older versions
                    // This only skips CN verification, not the full cert chain
                    ESP_LOGW(tag, "Certificate CN verification disabled (chain still validated)");
                }

                if (tls_config.server_name != NULL) {
                    cfg.common_name = tls_config.server_name;
                }

                tls = esp_tls_init();
                if (tls == NULL) {
                    ESP_LOGE(tag, "Failed to initialize TLS");
                    last_error_code = NATS_ERR_TLS_INIT_FAILED;
                    return false;
                }

                int ret = esp_tls_conn_new_sync(server.hostname, strlen(server.hostname), server.port, &cfg, tls);
                if (ret != 1) {
                    ESP_LOGE(tag, "TLS connection failed to %s:%d: %d", server.hostname, server.port, ret);
                    esp_tls_conn_destroy(tls);
                    tls = NULL;
                    last_error_code = NATS_ERR_TLS_CONNECTION_FAILED;
                    return false;
                }

                sockfd = 1; // Set to valid for compatibility with process()
                return true;
            } else {
                // Non-TLS connection with DNS resolution
                struct addrinfo hints = {};
                struct addrinfo *result, *rp;
                char port_str[6];
                snprintf(port_str, sizeof(port_str), "%d", server.port);

                hints.ai_family = AF_UNSPEC;    // Allow IPv4 or IPv6
                hints.ai_socktype = SOCK_STREAM;
                hints.ai_protocol = IPPROTO_TCP;

                int ret = getaddrinfo(server.hostname, port_str, &hints, &result);
                if (ret != 0) {
                    ESP_LOGE(tag, "DNS resolution failed for %s: %d", server.hostname, ret);
                    last_error_code = NATS_ERR_DNS_RESOLUTION_FAILED;
                    return false;
                }

                // Try each address until we successfully connect
                for (rp = result; rp != NULL; rp = rp->ai_next) {
                    sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
                    if (sockfd < 0) continue;

                    if (::connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0) {
                        // Connection successful
                        freeaddrinfo(result);
                        last_error_code = NATS_ERR_NONE;
                        return true;
                    }

                    close(sockfd);
                    sockfd = -1;
                }

                // All addresses failed
                freeaddrinfo(result);
                last_error_code = NATS_ERR_CONNECTION_FAILED;
                return false;
            }
        }

    public:
        bool connect() {
            // Try connecting to all servers in the list
            size_t start_idx = current_server_idx;
            do {
                ESP_LOGI(tag, "Attempting to connect to %s:%d",
                    servers[current_server_idx].hostname,
                    servers[current_server_idx].port);

                if (try_connect_to_server(servers[current_server_idx])) {
                    outstanding_pings = 0;
                    reconnect_attempts = 0;
                    connect_state = CONNECTING;
                    metrics.last_connect_time = NATSUtil::millis();
                    if (metrics.reconnections > 0 || connected) {
                        metrics.reconnections++;
                    }
                    ESP_LOGI(tag, "Connected to %s:%d",
                        servers[current_server_idx].hostname,
                        servers[current_server_idx].port);
                    return true;
                }

                // Move to next server
                current_server_idx = (current_server_idx + 1) % servers.size();
            } while (current_server_idx != start_idx);

            // All servers failed
            reconnect_attempts++;
            connect_state = DISCONNECTED;
            return false;
        }

        void connect_async(connect_cb callback) {
            async_connect_cb = callback;
            connect_state = CONNECTING;
            // Connection will be attempted in process()
        }

        void disconnect() {
            if (!connected) return;
            connected = false;
            connect_state = DISCONNECTED;
            async_connect_cb = NULL;

            if (last_error_code == NATS_ERR_NONE) {
                last_error_code = NATS_ERR_DISCONNECTED;
            }

            if (tls_config.enabled && tls != NULL) {
                esp_tls_conn_destroy(tls);
                tls = NULL;
            }

            if (sockfd >= 0) {
                if (!tls_config.enabled) {
                    close(sockfd);
                }
                sockfd = -1;
            }

            // Clean up subscriptions with mutex protection
            // Check if mutex is held by current task to avoid recursive lock
            if (mutex != NULL) {
                if (xSemaphoreTake(mutex, 0) == pdTRUE) {
                    // We got the mutex, so we weren't holding it
                    subs.empty();
                    xSemaphoreGive(mutex);
                } else {
                    // Mutex might be held by us already (recursive call from send() etc.)
                    // Just clear without mutex in this case
                    subs.empty();
                }
            } else {
                subs.empty();
            }

            if (on_disconnect != NULL) on_disconnect();
        }

        void publish(const char* subject, const char* msg = NULL, const char* replyto = NULL) {
            if (subject == NULL || subject[0] == 0) return;
            if (!connected) {
                buffer_message(subject, msg, replyto, NULL);
                return;
            }
            send_fmt("PUB %s %s %lu",
                    subject,
                    (replyto == NULL)? "" : replyto,
                    (unsigned long)strlen(msg));
            send((msg == NULL)? "" : msg);
            metrics.msgs_sent++;
        }
        void publish(const char* subject, const bool msg) {
            publish(subject, (msg)? "true" : "false");
        }
        void publish_with_headers(const char* subject, const char* headers, const char* msg = NULL, const char* replyto = NULL) {
            if (subject == NULL || subject[0] == 0) return;
            if (!connected) {
                buffer_message(subject, msg, replyto, headers);
                return;
            }
            size_t header_len = (headers == NULL) ? 0 : strlen(headers);
            size_t msg_len = (msg == NULL) ? 0 : strlen(msg);
            size_t total_len = header_len + msg_len;

            send_fmt("HPUB %s %s %lu %lu",
                    subject,
                    (replyto == NULL)? "" : replyto,
                    (unsigned long)header_len,
                    (unsigned long)total_len);
            if (headers != NULL) send(headers);
            if (msg != NULL) send(msg);
            metrics.msgs_sent++;
        }
        void publish_fmt(const char* subject, const char* fmt, ...) {
            va_list args;
            va_start(args, fmt);
            char* buf;
            vasprintf(&buf, fmt, args);
            va_end(args);
            publish(subject, buf);
            free(buf);
        }
        void publishf(const char* subject, const char* fmt, ...) {
            va_list args;
            va_start(args, fmt);
            char* buf;
            vasprintf(&buf, fmt, args);
            va_end(args);
            publish(subject, buf);
            free(buf);
        }

        int subscribe(const char* subject, sub_cb cb, const char* queue = NULL, const int max_wanted = 0) {
            if (!connected) return -1;

            xSemaphoreTake(mutex, portMAX_DELAY);
            Sub* sub = new Sub(cb, max_wanted);
            int sid;
            if (free_sids.empty()) {
                sid = subs.push_back(sub);
            } else {
                sid = free_sids.pop();
                subs[sid] = sub;
            }
            send_fmt("SUB %s %s %d",
                    subject,
                    (queue == NULL)? "" : queue,
                    sid);
            xSemaphoreGive(mutex);
            return sid;
        }

        void unsubscribe(const int sid) {
            if (!connected) return;
            xSemaphoreTake(mutex, portMAX_DELAY);
            send_fmt("UNSUB %d", sid);
            delete subs[sid];  // Fix: Use delete for objects created with new
            subs[sid] = NULL;
            free_sids.push(sid);
            xSemaphoreGive(mutex);
        }

        int request(const char* subject, const char* msg, sub_cb cb, const int max_wanted = 1) {
            if (subject == NULL || subject[0] == 0) return -1;
            if (!connected) return -1;
            char* inbox = generate_inbox_subject();
            if (inbox == NULL) {
                ESP_LOGE(tag, "Failed to generate inbox subject");
                return -1;
            }
            int sid = subscribe(inbox, cb, NULL, max_wanted);
            publish(subject, msg, inbox);
            free(inbox);
            return sid;
        }

        int request_with_timeout(const char* subject, const char* msg, sub_cb cb, unsigned long timeout_ms, timeout_cb on_timeout = NULL, const int max_wanted = 1) {
            if (subject == NULL || subject[0] == 0) return -1;
            if (!connected) return -1;
            char* inbox = generate_inbox_subject();
            if (inbox == NULL) {
                ESP_LOGE(tag, "Failed to generate inbox subject");
                return -1;
            }

            xSemaphoreTake(mutex, portMAX_DELAY);
            Sub* sub = new Sub(cb, max_wanted, timeout_ms, on_timeout);
            int sid;
            if (free_sids.empty()) {
                sid = subs.push_back(sub);
            } else {
                sid = free_sids.pop();
                subs[sid] = sub;
            }
            send_fmt("SUB %s %d", inbox, sid);
            xSemaphoreGive(mutex);

            publish(subject, msg, inbox);
            free(inbox);
            return sid;
        }

        // Basic JetStream publish with ACK
        int jetstream_publish(const char* subject, const char* msg, sub_cb ack_cb, timeout_cb on_timeout = NULL, unsigned long timeout_ms = 5000) {
            if (subject == NULL || subject[0] == 0) return -1;
            if (!connected) return -1;

            // Build JetStream API subject: $JS.API.PUBLISH.<subject>
            size_t api_subject_len = strlen("$JS.API.PUBLISH.") + strlen(subject) + 1;
            char* api_subject = (char*)malloc(api_subject_len);
            snprintf(api_subject, api_subject_len, "$JS.API.PUBLISH.%s", subject);

            int sid = request_with_timeout(api_subject, msg, ack_cb, timeout_ms, on_timeout, 1);

            free(api_subject);
            return sid;
        }

        nats_connection_metrics_t get_metrics() {
            nats_connection_metrics_t current_metrics = metrics;
            if (connected && metrics.last_connect_time > 0) {
                current_metrics.uptime = NATSUtil::millis() - metrics.last_connect_time;
            }
            return current_metrics;
        }

        nats_error_code_t last_error() const {
            return last_error_code;
        }

        const char* error_string(nats_error_code_t err) const {
            switch (err) {
                case NATS_ERR_NONE: return "No error";
                case NATS_ERR_CONNECTION_FAILED: return "Connection failed";
                case NATS_ERR_DNS_RESOLUTION_FAILED: return "DNS resolution failed";
                case NATS_ERR_TLS_INIT_FAILED: return "TLS initialization failed";
                case NATS_ERR_TLS_CONNECTION_FAILED: return "TLS connection failed";
                case NATS_ERR_SOCKET_FAILED: return "Socket operation failed";
                case NATS_ERR_PROTOCOL_ERROR: return "Protocol error";
                case NATS_ERR_MAX_PINGS_EXCEEDED: return "Maximum outstanding pings exceeded";
                case NATS_ERR_DISCONNECTED: return "Disconnected";
                case NATS_ERR_INVALID_SUBJECT: return "Invalid subject";
                case NATS_ERR_NOT_CONNECTED: return "Not connected";
                default: return "Unknown error";
            }
        }

        const char* last_error_string() const {
            return error_string(last_error_code);
        }

        bool flush(unsigned long timeout_ms = 5000) {
            if (!connected) {
                last_error_code = NATS_ERR_NOT_CONNECTED;
                return false;
            }

            // Send PING and wait for PONG
            int initial_pongs = outstanding_pings;
            send(NATS_CTRL_PING);
            outstanding_pings++;

            unsigned long start_time = NATSUtil::millis();
            while ((NATSUtil::millis() - start_time) < timeout_ms) {
                // Process incoming messages
                if (sockfd >= 0) {
                    int fd_to_check = sockfd;
                    if (tls_config.enabled && tls != NULL) {
                        if (esp_tls_get_conn_sockfd(tls, &fd_to_check) != ESP_OK) {
                            return false;
                        }
                    }

                    fd_set rfds;
                    struct timeval tv = {0, 10000}; // 10ms timeout
                    FD_ZERO(&rfds);
                    FD_SET(fd_to_check, &rfds);
                    int ret = select(fd_to_check + 1, &rfds, NULL, NULL, &tv);
                    if (ret > 0 && FD_ISSET(fd_to_check, &rfds)) {
                        recv();
                        // Check if we got the PONG
                        if (outstanding_pings <= initial_pongs) {
                            return true;
                        }
                    }
                }

                if (!connected) {
                    return false;
                }
            }

            // Timeout
            return false;
        }

        void drain(unsigned long timeout_ms = 5000) {
            if (!connected) return;

            draining = true;

            // Unsubscribe from all subscriptions
            xSemaphoreTake(mutex, portMAX_DELAY);
            for (size_t i = 0; i < subs.size(); i++) {
                if (subs[i] != NULL) {
                    send_fmt("UNSUB %d", i);
                    delete subs[i];  // Fix: Use delete for objects created with new
                    subs[i] = NULL;
                    free_sids.push(i);
                }
            }
            xSemaphoreGive(mutex);

            // Flush to ensure all unsubscribes are sent
            flush(timeout_ms);

            // Disconnect
            disconnect();
            draining = false;
        }

        int log_tick_count = 0;
        void process() {
            if (log_tick_count++ % 1000 == 0) {
                //ESP_LOGI(tag, "(tick %d) Outstanding pings: %d, Reconnect attempts: %d", log_tick_count, outstanding_pings, reconnect_attempts);
            }

            // Handle async connect
            if (connect_state == CONNECTING && async_connect_cb != NULL && sockfd < 0) {
                bool success = connect();
                if (success) {
                    connect_state = CONNECTED;
                } else {
                    connect_state = DISCONNECTED;
                }
                async_connect_cb(success);
                async_connect_cb = NULL;
                return;
            }

            if (sockfd >= 0) {
                int fd_to_check = sockfd;

                // For TLS connections, get the underlying socket descriptor
                if (tls_config.enabled && tls != NULL) {
                    if (esp_tls_get_conn_sockfd(tls, &fd_to_check) != ESP_OK) {
                        disconnect();
                        return;
                    }
                }

                fd_set rfds;
                struct timeval tv = {0, 0};
                FD_ZERO(&rfds);
                FD_SET(fd_to_check, &rfds);
                int ret = select(fd_to_check + 1, &rfds, NULL, NULL, &tv);
                if (ret > 0 && FD_ISSET(fd_to_check, &rfds)) {
                    recv();
                }

                // Check for request timeouts
                // Collect timed out subscriptions first to avoid race conditions
                NATSUtil::Array<int> timed_out_sids;
                NATSUtil::Array<timeout_cb> timeout_callbacks;

                xSemaphoreTake(mutex, portMAX_DELAY);
                for (size_t i = 0; i < subs.size(); i++) {
                    if (subs[i] != NULL && subs[i]->timed_out()) {
                        timed_out_sids.push_back(i);
                        timeout_callbacks.push_back(subs[i]->on_timeout);
                    }
                }
                xSemaphoreGive(mutex);

                // Now process timeouts without holding the mutex
                for (size_t i = 0; i < timed_out_sids.size(); i++) {
                    if (timeout_callbacks[i] != NULL) {
                        timeout_callbacks[i]();
                    }
                    unsubscribe(timed_out_sids[i]);
                }

                if (ping_timer.process())
                    ping();
            } else {
                disconnect();
                if (max_reconnect_attempts == -1 || reconnect_attempts < max_reconnect_attempts) {
                    unsigned long current_time = NATSUtil::millis();
                    unsigned long reconnect_delay = get_reconnect_delay();

                    // Check if enough time has passed for the next reconnect attempt
                    if (current_time - last_reconnect_attempt >= reconnect_delay) {
                        last_reconnect_attempt = current_time;
                        connect();
                    }
                }
            }
        }

};

#endif // ESPIDF_NATS_CLIENT_H
