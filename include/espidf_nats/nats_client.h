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
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config.h"
#include "util.h"
#include "types.h"
#include "subscription.h"
#include "transport.h"
#include "tcp_transport.h"

// Auto-detect esp_websocket_client availability using __has_include (C++17)
// This is more reliable than CMake-defined macros for managed components
#if defined(__has_include)
    #if __has_include(<esp_websocket_client.h>)
        #ifndef CONFIG_ESP_WEBSOCKET_CLIENT_ENABLE
            #define CONFIG_ESP_WEBSOCKET_CLIENT_ENABLE 1
        #endif
    #endif
#endif

#ifdef CONFIG_ESP_WEBSOCKET_CLIENT_ENABLE
#include "ws_transport.h"
#endif

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

        // Transport abstraction layer
        NatsTransport* transport;
        bool owns_transport;  // true if NATS owns the transport (delete in destructor)

        // Legacy members kept for backward compatibility (used when transport is NULL)
        int sockfd;
        esp_tls_t* tls;
        // mbedtls contexts for STARTTLS upgrade
        mbedtls_ssl_context* mbedtls_ssl;
        mbedtls_ssl_config* mbedtls_conf;
        mbedtls_x509_crt* mbedtls_cacert;
        mbedtls_entropy_context* mbedtls_entropy;
        mbedtls_ctr_drbg_context* mbedtls_ctr_drbg;
        bool using_mbedtls_directly;  // true when using mbedtls directly (STARTTLS)
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
        size_t server_max_payload;        // Max payload size from server INFO (Issue #58, #77)
        SemaphoreHandle_t mutex;          // Protects data structures (subs, pending_messages)
        SemaphoreHandle_t io_mutex;       // Protects TLS/socket I/O operations
        SemaphoreHandle_t state_mutex;    // Protects outstanding_pings and reconnect_attempts

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
        /**
         * Single server constructor
         *
         * @param hostname Server hostname or IP address
         * @param port Server port (default: 4222)
         * @param user Optional username for authentication
         * @param pass Optional password for authentication
         * @param tls_cfg Optional TLS configuration
         *
         * @note IMPORTANT - TLS Pointer Lifetime:
         *       The tls_cfg struct is copied, but the pointers within it (ca_cert,
         *       client_cert, client_key) are NOT deep-copied. The caller MUST ensure
         *       that the data pointed to by these fields remains valid for the entire
         *       lifetime of the NATS client object. Typically this means using static
         *       const char[] arrays or embedding certificates in flash memory.
         */
        NATS(const char* hostname,
                int port = NATS_DEFAULT_PORT,
                const char* user = NULL,
                const char* pass = NULL,
                const nats_tls_config_t* tls_cfg = NULL) :
            transport(NULL),
            owns_transport(false),
            sockfd(-1),
            tls(NULL),
            mbedtls_ssl(NULL),
            mbedtls_conf(NULL),
            mbedtls_cacert(NULL),
            mbedtls_entropy(NULL),
            mbedtls_ctr_drbg(NULL),
            using_mbedtls_directly(false),
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
                // Validate port range (Issue #18)
                if (port < 1 || port > 65535) {
                    ESP_LOGE(tag, "Invalid port number: %d (must be 1-65535)", port);
                    last_error_code = NATS_ERR_INVALID_ARG;
                    port = NATS_DEFAULT_PORT;  // Use default as fallback
                }
                // Validate credential lengths (Issue #31)
                if (user != NULL && strlen(user) > NATS_MAX_CREDENTIAL_LEN) {
                    ESP_LOGE(tag, "Username too long: %zu bytes (max %d)", strlen(user), NATS_MAX_CREDENTIAL_LEN);
                    last_error_code = NATS_ERR_INVALID_ARG;
                    this->user = NULL;  // Reject invalid username
                }
                if (pass != NULL && strlen(pass) > NATS_MAX_CREDENTIAL_LEN) {
                    ESP_LOGE(tag, "Password too long: %zu bytes (max %d)", strlen(pass), NATS_MAX_CREDENTIAL_LEN);
                    last_error_code = NATS_ERR_INVALID_ARG;
                    this->pass = NULL;  // Reject invalid password
                }
                nats_server_t server = {hostname, port, NATS_TRANSPORT_TCP, NULL};
                servers.push_back(server);
                if (tls_cfg != NULL) {
                    tls_config = *tls_cfg;
                } else {
                    memset(&tls_config, 0, sizeof(nats_tls_config_t));
                }
                memset(&metrics, 0, sizeof(nats_connection_metrics_t));
                last_error_code = (last_error_code == NATS_ERR_INVALID_ARG) ? NATS_ERR_INVALID_ARG : NATS_ERR_NONE;
                draining = false;
                server_max_payload = NATS_MAX_PAYLOAD_SIZE;  // Default, updated by INFO
                mutex = xSemaphoreCreateRecursiveMutex();
                io_mutex = xSemaphoreCreateRecursiveMutex();
                state_mutex = xSemaphoreCreateRecursiveMutex();
                if (mutex == NULL || io_mutex == NULL || state_mutex == NULL) {
                    ESP_LOGE(tag, "Failed to create mutex - out of memory");
                    last_error_code = NATS_ERR_OUT_OF_MEMORY;
                    if (mutex != NULL) vSemaphoreDelete(mutex);
                    if (io_mutex != NULL) vSemaphoreDelete(io_mutex);
                    if (state_mutex != NULL) vSemaphoreDelete(state_mutex);
                    mutex = io_mutex = state_mutex = NULL;
                } else {
                    // Validate TLS configuration if provided
                    if (!validate_tls_config()) {
                        ESP_LOGE(tag, "Invalid TLS configuration in constructor");
                    }
                }
            }

        /**
         * Multiple servers constructor with failover support
         *
         * @param server_list Array of server definitions (hostname + port)
         * @param server_count Number of servers in the array
         * @param user Optional username for authentication
         * @param pass Optional password for authentication
         * @param tls_cfg Optional TLS configuration
         *
         * @note IMPORTANT - TLS Pointer Lifetime:
         *       The tls_cfg struct is copied, but the pointers within it (ca_cert,
         *       client_cert, client_key) are NOT deep-copied. The caller MUST ensure
         *       that the data pointed to by these fields remains valid for the entire
         *       lifetime of the NATS client object. Typically this means using static
         *       const char[] arrays or embedding certificates in flash memory.
         */
        NATS(const nats_server_t* server_list,
                size_t server_count,
                const char* user = NULL,
                const char* pass = NULL,
                const nats_tls_config_t* tls_cfg = NULL) :
            transport(NULL),
            owns_transport(false),
            sockfd(-1),
            tls(NULL),
            mbedtls_ssl(NULL),
            mbedtls_conf(NULL),
            mbedtls_cacert(NULL),
            mbedtls_entropy(NULL),
            mbedtls_ctr_drbg(NULL),
            using_mbedtls_directly(false),
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
                // Validate server configuration
                if (server_list != NULL && server_count > 0) {
                    for (size_t i = 0; i < server_count; i++) {
                        // Validate port range for each server
                        if (server_list[i].port < 1 || server_list[i].port > 65535) {
                            ESP_LOGE(tag, "Invalid port number for server %zu: %d (must be 1-65535)",
                                     i, server_list[i].port);
                            last_error_code = NATS_ERR_INVALID_ARG;
                            // Skip this server
                            continue;
                        }
                        servers.push_back(server_list[i]);
                    }
                    // Check if we added any valid servers
                    if (servers.size() == 0) {
                        ESP_LOGE(tag, "No valid servers in server list");
                        last_error_code = NATS_ERR_INVALID_ARG;
                    }
                } else {
                    ESP_LOGE(tag, "Invalid server configuration: server_list=%p, count=%zu",
                             server_list, server_count);
                    last_error_code = NATS_ERR_INVALID_ARG;
                }
                // Validate credential lengths (Issue #31)
                if (user != NULL && strlen(user) > NATS_MAX_CREDENTIAL_LEN) {
                    ESP_LOGE(tag, "Username too long: %zu bytes (max %d)", strlen(user), NATS_MAX_CREDENTIAL_LEN);
                    last_error_code = NATS_ERR_INVALID_ARG;
                    this->user = NULL;  // Reject invalid username
                }
                if (pass != NULL && strlen(pass) > NATS_MAX_CREDENTIAL_LEN) {
                    ESP_LOGE(tag, "Password too long: %zu bytes (max %d)", strlen(pass), NATS_MAX_CREDENTIAL_LEN);
                    last_error_code = NATS_ERR_INVALID_ARG;
                    this->pass = NULL;  // Reject invalid password
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
                server_max_payload = NATS_MAX_PAYLOAD_SIZE;  // Default, updated by INFO
                mutex = xSemaphoreCreateRecursiveMutex();
                io_mutex = xSemaphoreCreateRecursiveMutex();
                state_mutex = xSemaphoreCreateRecursiveMutex();
                if (mutex == NULL || io_mutex == NULL || state_mutex == NULL) {
                    ESP_LOGE(tag, "Failed to create mutex - out of memory");
                    last_error_code = NATS_ERR_OUT_OF_MEMORY;
                    if (mutex != NULL) vSemaphoreDelete(mutex);
                    if (io_mutex != NULL) vSemaphoreDelete(io_mutex);
                    if (state_mutex != NULL) vSemaphoreDelete(state_mutex);
                    mutex = io_mutex = state_mutex = NULL;
                } else {
                    // Validate TLS configuration if provided
                    if (!validate_tls_config()) {
                        ESP_LOGE(tag, "Invalid TLS configuration in constructor");
                    }
                }
            }

        // Destructor to cleanup resources
        ~NATS() {
            // Disconnect if still connected
            if (connected) {
                disconnect();
            }

            // Clean up transport if owned
            if (transport != NULL && owns_transport) {
                delete transport;
                transport = NULL;
            }

            // Clean up mbedtls resources if still allocated
            cleanup_mbedtls();

            // Clean up all subscriptions
            if (mutex != NULL) {
                xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
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
                xSemaphoreGiveRecursive(mutex);

                // Delete the mutexes
                vSemaphoreDelete(mutex);
                mutex = NULL;
                if (io_mutex != NULL) {
                    vSemaphoreDelete(io_mutex);
                    io_mutex = NULL;
                }
                if (state_mutex != NULL) {
                    vSemaphoreDelete(state_mutex);
                    state_mutex = NULL;
                }
            }
        }

        // ====================================================================
        // Factory Methods for Creating NATS with Different Transports
        // ====================================================================

        /**
         * Create a NATS client with TCP transport
         *
         * @param hostname Server hostname
         * @param port Server port (default: 4222)
         * @param user Optional username
         * @param pass Optional password
         * @param tls_cfg Optional TLS configuration
         * @return Pointer to new NATS instance (caller owns, delete when done)
         */
        static NATS* create_tcp(const char* hostname,
                                int port = NATS_DEFAULT_PORT,
                                const char* user = NULL,
                                const char* pass = NULL,
                                const nats_tls_config_t* tls_cfg = NULL) {
            // Use existing constructor which creates TcpTransport internally
            return new NATS(hostname, port, user, pass, tls_cfg);
        }

        /**
         * Create a NATS client with TCP transport and multiple servers
         *
         * @param server_list Array of server definitions
         * @param server_count Number of servers
         * @param user Optional username
         * @param pass Optional password
         * @param tls_cfg Optional TLS configuration
         * @return Pointer to new NATS instance (caller owns, delete when done)
         */
        static NATS* create_tcp(const nats_server_t* server_list,
                                size_t server_count,
                                const char* user = NULL,
                                const char* pass = NULL,
                                const nats_tls_config_t* tls_cfg = NULL) {
            return new NATS(server_list, server_count, user, pass, tls_cfg);
        }

#ifdef CONFIG_ESP_WEBSOCKET_CLIENT_ENABLE
        /**
         * Create a NATS client with WebSocket transport
         *
         * @param hostname Server hostname
         * @param port Server port (default: 9222 for ws, 443 for wss)
         * @param user Optional username
         * @param pass Optional password
         * @param tls_cfg Optional TLS configuration (set enabled=true for wss://)
         * @param ws_path WebSocket path (default: "/nats")
         * @return Pointer to new NATS instance, or NULL on failure (caller owns)
         */
        static NATS* create_websocket(const char* hostname,
                                       int port,
                                       const char* user = NULL,
                                       const char* pass = NULL,
                                       const nats_tls_config_t* tls_cfg = NULL,
                                       const char* ws_path = NATS_WEBSOCKET_PATH) {
            // Create WebSocket transport
            WebSocketTransport* ws_transport = new WebSocketTransport();
            if (ws_transport == NULL) {
                ESP_LOGE(tag, "Failed to allocate WebSocket transport");
                return NULL;
            }

            // Create NATS client with single server
            nats_server_t server = {hostname, port, NATS_TRANSPORT_WEBSOCKET, ws_path};
            NATS* nats = new NATS(&server, 1, user, pass, tls_cfg);
            if (nats == NULL) {
                delete ws_transport;
                return NULL;
            }

            // Assign transport
            nats->transport = ws_transport;
            nats->owns_transport = true;

            return nats;
        }

        /**
         * Create a NATS client with WebSocket transport from URI
         *
         * @param uri WebSocket URI (e.g., "ws://host:port/path" or "wss://host:port/path")
         * @param user Optional username
         * @param pass Optional password
         * @param tls_cfg Optional TLS configuration (auto-enabled for wss://)
         * @return Pointer to new NATS instance, or NULL on failure
         */
        static NATS* create_websocket_uri(const char* uri,
                                           const char* user = NULL,
                                           const char* pass = NULL,
                                           const nats_tls_config_t* tls_cfg = NULL) {
            // Parse URI to extract hostname, port, path
            // Format: ws[s]://hostname[:port][/path]
            if (uri == NULL) return NULL;

            bool is_tls = (strncmp(uri, "wss://", 6) == 0);
            const char* host_start = uri + (is_tls ? 6 : 5);  // Skip ws:// or wss://

            // Find port and path
            char hostname[256] = {0};
            int port = is_tls ? 443 : 9222;  // Default ports
            char path[256] = "/nats";

            const char* colon = strchr(host_start, ':');
            const char* slash = strchr(host_start, '/');

            if (colon != NULL && (slash == NULL || colon < slash)) {
                // Have port
                size_t host_len = colon - host_start;
                if (host_len >= sizeof(hostname)) host_len = sizeof(hostname) - 1;
                strncpy(hostname, host_start, host_len);
                hostname[host_len] = '\0';
                port = atoi(colon + 1);
            } else if (slash != NULL) {
                // No port, have path
                size_t host_len = slash - host_start;
                if (host_len >= sizeof(hostname)) host_len = sizeof(hostname) - 1;
                strncpy(hostname, host_start, host_len);
                hostname[host_len] = '\0';
            } else {
                // Just hostname
                strncpy(hostname, host_start, sizeof(hostname) - 1);
            }

            if (slash != NULL) {
                strncpy(path, slash, sizeof(path) - 1);
            }

            // Create TLS config if needed
            nats_tls_config_t tls_config_local;
            memset(&tls_config_local, 0, sizeof(nats_tls_config_t));
            const nats_tls_config_t* tls_to_use = tls_cfg;
            if (is_tls && tls_cfg == NULL) {
                tls_config_local.enabled = true;
                tls_config_local.skip_cert_verification = true;  // Default for easy setup
                tls_to_use = &tls_config_local;
            } else if (is_tls && tls_cfg != NULL) {
                tls_config_local = *tls_cfg;
                tls_config_local.enabled = true;
                tls_to_use = &tls_config_local;
            }

            return create_websocket(hostname, port, user, pass, tls_to_use, path);
        }

        /**
         * Create a NATS client with WebSocket transport and multiple servers
         *
         * @param server_list Array of server definitions (with transport_type=NATS_TRANSPORT_WEBSOCKET)
         * @param server_count Number of servers in the array
         * @param user Optional username
         * @param pass Optional password
         * @param tls_cfg Optional TLS configuration (set enabled=true for wss://)
         * @return Pointer to new NATS instance, or NULL on failure
         */
        static NATS* create_websocket(const nats_server_t* server_list,
                                       size_t server_count,
                                       const char* user = NULL,
                                       const char* pass = NULL,
                                       const nats_tls_config_t* tls_cfg = NULL) {
            if (server_list == NULL || server_count == 0) {
                ESP_LOGE(tag, "Invalid server list for WebSocket transport");
                return NULL;
            }

            // Create WebSocket transport
            WebSocketTransport* ws_transport = new WebSocketTransport();
            if (ws_transport == NULL) {
                ESP_LOGE(tag, "Failed to allocate WebSocket transport");
                return NULL;
            }

            // Create server list with WebSocket transport type
            nats_server_t* ws_servers = new nats_server_t[server_count];
            if (ws_servers == NULL) {
                delete ws_transport;
                ESP_LOGE(tag, "Failed to allocate server list");
                return NULL;
            }

            for (size_t i = 0; i < server_count; i++) {
                ws_servers[i].hostname = server_list[i].hostname;
                ws_servers[i].port = server_list[i].port;
                ws_servers[i].transport_type = NATS_TRANSPORT_WEBSOCKET;
                ws_servers[i].ws_path = server_list[i].ws_path ? server_list[i].ws_path : NATS_WEBSOCKET_PATH;
            }

            // Create NATS client
            NATS* nats = new NATS(ws_servers, server_count, user, pass, tls_cfg);
            delete[] ws_servers;  // NATS constructor copies the server list

            if (nats == NULL) {
                delete ws_transport;
                return NULL;
            }

            // Assign transport
            nats->transport = ws_transport;
            nats->owns_transport = true;

            return nats;
        }
#endif // CONFIG_ESP_WEBSOCKET_CLIENT_ENABLE

        /**
         * Set a custom transport (advanced usage)
         *
         * @param t Transport instance
         * @param take_ownership If true, NATS will delete transport in destructor
         */
        void set_transport(NatsTransport* t, bool take_ownership = false) {
            if (transport != NULL && owns_transport) {
                delete transport;
            }
            transport = t;
            owns_transport = take_ownership;
        }

        /**
         * Get current transport (for advanced usage)
         * @return Current transport, or NULL if using legacy socket mode
         */
        NatsTransport* get_transport() const {
            return transport;
        }

    private:
        bool validate_tls_config() {
            if (!tls_config.enabled) {
                return true;  // No validation needed if TLS is disabled
            }

            // If TLS is enabled and certificate verification is not skipped, CA cert must be provided
            if (!tls_config.skip_cert_verification) {
                if (tls_config.ca_cert == NULL || tls_config.ca_cert_len == 0) {
                    ESP_LOGE(tag, "TLS enabled with cert verification but no CA certificate provided");
                    last_error_code = NATS_ERR_INVALID_CONFIG;
                    return false;
                }
            }

            // If client cert is provided, client key must also be provided (and vice versa)
            if ((tls_config.client_cert != NULL) != (tls_config.client_key != NULL)) {
                ESP_LOGE(tag, "TLS client cert and key must both be provided or both be NULL");
                last_error_code = NATS_ERR_INVALID_CONFIG;
                return false;
            }

            // If client cert is provided, validate lengths
            if (tls_config.client_cert != NULL) {
                if (tls_config.client_cert_len == 0 || tls_config.client_key_len == 0) {
                    ESP_LOGE(tag, "TLS client cert/key provided but length is zero");
                    last_error_code = NATS_ERR_INVALID_CONFIG;
                    return false;
                }
            }

            // Warn if server_name is not set (SNI won't work properly)
            if (tls_config.server_name == NULL && !tls_config.skip_cert_verification) {
                ESP_LOGW(tag, "TLS enabled without server_name - SNI will use hostname from connection");
            }

            return true;
        }

        // Send raw data without appending CRLF (for binary payloads)
        bool send_raw(const char* data, size_t len) {
            if (data == NULL || len == 0) return true;

            // Use transport if available
            if (transport != NULL && transport->is_connected()) {
                int ret = transport->send_data(data, len);
                if (ret < 0 || (size_t)ret != len) {
                    ESP_LOGE(tag, "Transport send_data failed");
                    last_error_code = transport->get_last_error();
                    disconnect();
                    return false;
                }
                return true;
            }

            // Legacy socket mode
            ssize_t ret;
            bool success = true;

            // Protect TLS/socket I/O operations with io_mutex
            if (io_mutex != NULL) xSemaphoreTakeRecursive(io_mutex, portMAX_DELAY);

            if (using_mbedtls_directly && mbedtls_ssl != NULL) {
                // Use mbedtls directly (STARTTLS mode)
                ret = mbedtls_ssl_write(mbedtls_ssl, (const unsigned char*)data, len);
                if (ret < 0 || (size_t)ret != len) {
                    ESP_LOGE(tag, "mbedtls write failed: %d", (int)ret);
                    last_error_code = NATS_ERR_SOCKET_FAILED;
                    success = false;
                }
            } else if (tls_config.enabled && tls != NULL) {
                ret = esp_tls_conn_write(tls, data, len);
                if (ret < 0 || (size_t)ret != len) {
                    ESP_LOGE(tag, "TLS write failed: %d", (int)ret);
                    last_error_code = NATS_ERR_SOCKET_FAILED;
                    success = false;
                }
            } else {
                if (sockfd < 0) {
                    if (io_mutex != NULL) xSemaphoreGiveRecursive(io_mutex);
                    return false;
                }
                ret = ::send(sockfd, data, len, 0);
                if (ret < 0 || (size_t)ret != len) {
                    ESP_LOGE(tag, "Socket send failed: %d", errno);
                    last_error_code = NATS_ERR_SOCKET_FAILED;
                    success = false;
                }
            }

            if (io_mutex != NULL) xSemaphoreGiveRecursive(io_mutex);

            if (!success) {
                disconnect();
            }
            return success;
        }

        void send(const char* msg) {
            if (msg == NULL) return;
            size_t len = strlen(msg);

            // Use transport if available
            if (transport != NULL && transport->is_connected()) {
                int ret = transport->send_line(msg);
                if (ret < 0) {
                    ESP_LOGE(tag, "Transport send_line failed");
                    last_error_code = transport->get_last_error();
                    disconnect();
                    return;
                }
                metrics.bytes_sent += len + strlen(NATS_CR_LF);
                return;
            }

            // Legacy socket mode
            ssize_t ret;
            bool need_disconnect = false;  // Flag to track if we need to disconnect

            // Protect TLS/socket I/O operations with io_mutex
            if (io_mutex != NULL) xSemaphoreTakeRecursive(io_mutex, portMAX_DELAY);

            if (using_mbedtls_directly && mbedtls_ssl != NULL) {
                // Use mbedtls directly (STARTTLS mode)
                ret = mbedtls_ssl_write(mbedtls_ssl, (const unsigned char*)msg, len);
                if (ret < 0 || (size_t)ret != len) {
                    ESP_LOGE(tag, "mbedtls write failed: %d", (int)ret);
                    last_error_code = NATS_ERR_SOCKET_FAILED;
                    need_disconnect = true;
                } else {
                    ret = mbedtls_ssl_write(mbedtls_ssl, (const unsigned char*)NATS_CR_LF, strlen(NATS_CR_LF));
                    if (ret < 0) {
                        ESP_LOGE(tag, "mbedtls write CRLF failed: %d", (int)ret);
                        last_error_code = NATS_ERR_SOCKET_FAILED;
                        need_disconnect = true;
                    }
                }
            } else if (tls_config.enabled && tls != NULL) {
                ret = esp_tls_conn_write(tls, msg, len);
                if (ret < 0 || (size_t)ret != len) {
                    ESP_LOGE(tag, "TLS write failed: %d", (int)ret);
                    last_error_code = NATS_ERR_SOCKET_FAILED;
                    need_disconnect = true;  // Set flag instead of calling disconnect()
                } else {
                    ret = esp_tls_conn_write(tls, NATS_CR_LF, strlen(NATS_CR_LF));
                    if (ret < 0) {
                        ESP_LOGE(tag, "TLS write CRLF failed: %d", (int)ret);
                        last_error_code = NATS_ERR_SOCKET_FAILED;
                        need_disconnect = true;  // Set flag instead of calling disconnect()
                    }
                }
            } else {
                if (sockfd < 0) {
                    if (io_mutex != NULL) xSemaphoreGiveRecursive(io_mutex);
                    return;
                }
                ret = ::send(sockfd, msg, len, 0);
                if (ret < 0) {
                    ESP_LOGE(tag, "Socket send failed: %d", errno);
                    last_error_code = NATS_ERR_SOCKET_FAILED;
                    need_disconnect = true;  // Set flag instead of calling disconnect()
                } else {
                    ret = ::send(sockfd, NATS_CR_LF, strlen(NATS_CR_LF), 0);
                    if (ret < 0) {
                        ESP_LOGE(tag, "Socket send CRLF failed: %d", errno);
                        last_error_code = NATS_ERR_SOCKET_FAILED;
                        need_disconnect = true;  // Set flag instead of calling disconnect()
                    }
                }
            }

            // Release mutex BEFORE calling disconnect() to avoid holding mutex across blocking operations
            if (io_mutex != NULL) xSemaphoreGiveRecursive(io_mutex);

            // Check flag AFTER releasing mutex (fixes TOCTOU race condition)
            if (need_disconnect) {
                disconnect();
                return;
            }

            metrics.bytes_sent += len + strlen(NATS_CR_LF);
        }

        // Binary-safe send - sends data with explicit length (no strlen)
        void send_binary(const uint8_t* data, size_t len) {
            if (data == NULL || len == 0) return;
            ssize_t ret;
            bool need_disconnect = false;

            if (io_mutex != NULL) xSemaphoreTakeRecursive(io_mutex, portMAX_DELAY);

            if (using_mbedtls_directly && mbedtls_ssl != NULL) {
                // Use mbedtls directly (STARTTLS mode)
                size_t total_sent = 0;
                while (total_sent < len) {
                    ret = mbedtls_ssl_write(mbedtls_ssl, data + total_sent, len - total_sent);
                    if (ret <= 0) {
                        ESP_LOGE(tag, "mbedtls write failed: %d", (int)ret);
                        last_error_code = NATS_ERR_SOCKET_FAILED;
                        need_disconnect = true;
                        break;
                    }
                    total_sent += ret;
                }
                if (!need_disconnect) {
                    ret = mbedtls_ssl_write(mbedtls_ssl, (const unsigned char*)NATS_CR_LF, 2);
                    if (ret < 0) {
                        ESP_LOGE(tag, "mbedtls write CRLF failed: %d", (int)ret);
                        last_error_code = NATS_ERR_SOCKET_FAILED;
                        need_disconnect = true;
                    }
                }
            } else if (tls_config.enabled && tls != NULL) {
                size_t total_sent = 0;
                while (total_sent < len) {
                    ret = esp_tls_conn_write(tls, data + total_sent, len - total_sent);
                    if (ret <= 0) {
                        ESP_LOGE(tag, "TLS write failed: %d", (int)ret);
                        last_error_code = NATS_ERR_SOCKET_FAILED;
                        need_disconnect = true;
                        break;
                    }
                    total_sent += ret;
                }
                if (!need_disconnect) {
                    ret = esp_tls_conn_write(tls, NATS_CR_LF, 2);
                    if (ret < 0) {
                        ESP_LOGE(tag, "TLS write CRLF failed: %d", (int)ret);
                        last_error_code = NATS_ERR_SOCKET_FAILED;
                        need_disconnect = true;
                    }
                }
            } else {
                if (sockfd < 0) {
                    if (io_mutex != NULL) xSemaphoreGiveRecursive(io_mutex);
                    return;
                }
                size_t total_sent = 0;
                while (total_sent < len) {
                    ret = ::send(sockfd, data + total_sent, len - total_sent, 0);
                    if (ret <= 0) {
                        ESP_LOGE(tag, "Socket send failed: %d", errno);
                        last_error_code = NATS_ERR_SOCKET_FAILED;
                        need_disconnect = true;
                        break;
                    }
                    total_sent += ret;
                }
                if (!need_disconnect) {
                    ret = ::send(sockfd, NATS_CR_LF, 2, 0);
                    if (ret < 0) {
                        ESP_LOGE(tag, "Socket send CRLF failed: %d", errno);
                        last_error_code = NATS_ERR_SOCKET_FAILED;
                        need_disconnect = true;
                    }
                }
            }

            if (io_mutex != NULL) xSemaphoreGiveRecursive(io_mutex);

            if (need_disconnect) {
                disconnect();
                return;
            }

            metrics.bytes_sent += len + 2;
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
            if (*strp == NULL) {
                ESP_LOGE(tag, "Failed to allocate memory in vasprintf");
                last_error_code = NATS_ERR_OUT_OF_MEMORY;
                return -1;
            }
            return vsnprintf(*strp, size, fmt, ap);
        }

        void send_fmt(const char* fmt, ...) {
            va_list args;
            va_start(args, fmt);
            char* buf;
            int ret = vasprintf(&buf, fmt, args);
            va_end(args);
            if (ret >= 0) {
                send(buf);
                // Securely zero buffer before freeing (may contain credentials)
                NATSUtil::secure_free(buf);
            }
        }

        void send_connect() {
            // Build CONNECT JSON - only include user/pass if credentials are provided
            // Include headers:true to enable HPUB/HMSG support
            // Include no_responders:true to get fast failure on request/reply with no subscribers
            if (user != NULL && pass != NULL) {
                send_fmt(
                        "CONNECT {"
                            "\"verbose\":%s,"
                            "\"pedantic\":%s,"
                            "\"lang\":\"%s\","
                            "\"version\":\"%s\","
                            "\"protocol\":1,"
                            "\"headers\":true,"
                            "\"no_responders\":true,"
                            "\"user\":\"%s\","
                            "\"pass\":\"%s\""
                        "}",
                        NATS_CONF_VERBOSE? "true" : "false",
                        NATS_CONF_PEDANTIC? "true" : "false",
                        NATS_CLIENT_LANG,
                        NATS_CLIENT_VERSION,
                        user,
                        pass);
            } else {
                send_fmt(
                        "CONNECT {"
                            "\"verbose\":%s,"
                            "\"pedantic\":%s,"
                            "\"lang\":\"%s\","
                            "\"version\":\"%s\","
                            "\"protocol\":1,"
                            "\"headers\":true,"
                            "\"no_responders\":true"
                        "}",
                        NATS_CONF_VERBOSE? "true" : "false",
                        NATS_CONF_PEDANTIC? "true" : "false",
                        NATS_CLIENT_LANG,
                        NATS_CLIENT_VERSION);
            }
        }

        /**
         * Read exactly n bytes from socket (for binary payloads like MSG/HMSG)
         * Does NOT interpret \r or \n specially - reads raw bytes
         * Returns malloc'd buffer of size n+1 (null-terminated), or NULL on error
         */
        char* client_read_bytes(size_t n) {
            if (n == 0) {
                char* buf = (char*)calloc(1, sizeof(char));
                return buf;
            }

            // Use transport if available
            if (transport != NULL && transport->is_connected()) {
                char* buf = transport->read_bytes(n);
                if (buf == NULL) {
                    ESP_LOGE(tag, "Transport read_bytes failed");
                    last_error_code = transport->get_last_error();
                    disconnect();
                }
                return buf;
            }

            // Legacy socket mode - Allocate n+1 for null terminator
            char* buf = (char*)malloc(n + 1);
            if (buf == NULL) {
                ESP_LOGE(tag, "Failed to allocate read buffer for %zu bytes", n);
                disconnect();
                return NULL;
            }

            size_t total_read = 0;

            // Protect TLS/socket I/O operations with io_mutex
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
                        ESP_LOGE(tag, "Read error: %d", ret);
                        last_error_code = NATS_ERR_SOCKET_FAILED;
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

        /**
         * Skip exactly n bytes from socket (consume trailing \r\n after payloads)
         */
        void client_skip_bytes(size_t n) {
            if (n == 0) return;

            // Use transport if available
            if (transport != NULL && transport->is_connected()) {
                transport->skip_bytes(n);
                return;
            }

            // Legacy socket mode
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
                        ESP_LOGE(tag, "Skip bytes error: %d", ret);
                    }
                    if (io_mutex != NULL) xSemaphoreGiveRecursive(io_mutex);
                    disconnect();
                    return;
                }
                remaining -= ret;
            }

            if (io_mutex != NULL) xSemaphoreGiveRecursive(io_mutex);
        }

        char* client_readline(size_t cap = 128) {
            // Use transport if available
            if (transport != NULL && transport->is_connected()) {
                char* line = transport->read_line(cap);
                if (line == NULL || (line[0] == '\0' && !transport->is_connected())) {
                    // Empty string with disconnected transport means error
                    if (line != NULL && line[0] == '\0' && !transport->is_connected()) {
                        ESP_LOGE(tag, "Transport read_line failed");
                        last_error_code = transport->get_last_error();
                        free(line);
                        disconnect();
                        return (char*)calloc(1, sizeof(char));
                    }
                }
                return line;
            }

            // Legacy socket mode
            char* buf = (char*)malloc(cap * sizeof(char));
            if (buf == NULL) {
                ESP_LOGE(tag, "Failed to allocate readline buffer");
                disconnect();
                return (char*)calloc(1, sizeof(char)); // Return empty string
            }
            int i = 0;
            char c;
            int ret;

            // Protect TLS/socket I/O operations with io_mutex
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
                    // Connection closed (0) or error (-1)
                    if (ret < 0) {
                        ESP_LOGE(tag, "Read error: %d", ret);
                        last_error_code = NATS_ERR_SOCKET_FAILED;
                    }
                    if (io_mutex != NULL) xSemaphoreGiveRecursive(io_mutex);
                    free(buf);
                    disconnect();
                    return (char*)calloc(1, sizeof(char)); // Return empty string
                }
                if (c == '\r') continue;
                if (c == '\n') break;
                if (i >= cap - 1) {  // Leave room for null terminator
                    // Enforce maximum line size limit
                    if (cap >= NATS_MAX_LINE_SIZE) {
                        ESP_LOGE(tag, "Line too long (exceeds %d bytes)", NATS_MAX_LINE_SIZE);
                        if (io_mutex != NULL) xSemaphoreGiveRecursive(io_mutex);
                        free(buf);
                        disconnect();
                        return (char*)calloc(1, sizeof(char));
                    }
                    // Check for overflow before doubling capacity
                    if (cap > SIZE_MAX / 2) {
                        ESP_LOGE(tag, "Readline buffer too large");
                        if (io_mutex != NULL) xSemaphoreGiveRecursive(io_mutex);
                        free(buf);
                        disconnect();
                        return (char*)calloc(1, sizeof(char));
                    }
                    cap *= 2;
                    char* newbuf = (char*)realloc(buf, cap + 1);  // +1 for null terminator
                    if (newbuf == NULL) {
                        ESP_LOGE(tag, "Failed to realloc readline buffer");
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

        void recv() {
            char* buf = client_readline();
            size_t argc = 0;
            const char* argv[NATS_MAX_ARGV] = {};
            for (int i = 0; i < NATS_MAX_ARGV; i++) {
                argv[i] = strtok((i == 0) ? buf : NULL, " ");
                if (argv[i] == NULL) break;
                argc++;
                if (argc >= NATS_MAX_ARGV) break;  // Safety: prevent overflow
            }
            if (argc == 0) { free(buf); return; }
            if (strcmp(argv[0], NATS_CTRL_MSG) == 0) {
                if (argc != 4 && argc != 5) { free(buf); return; }
                char* endptr;
                long sid_long = strtol(argv[2], &endptr, 10);
                if (*endptr != '\0' || sid_long < 0 || sid_long > INT_MAX) { free(buf); return; }
                int sid = (int)sid_long;
                // Validate SID bounds (check with mutex)
                xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
                if (sid < 0 || sid >= subs.size() || subs[sid] == NULL) {
                    xSemaphoreGiveRecursive(mutex);
                    free(buf);
                    return;
                };
                xSemaphoreGiveRecursive(mutex);

                long payload_size_long = strtol((argc == 5)? argv[4] : argv[3], &endptr, 10);
                if (*endptr != '\0' || payload_size_long < 0 || payload_size_long > INT_MAX) { free(buf); return; }
                int payload_size = (int)payload_size_long;
                // Check for integer overflow and enforce max payload size
                if (payload_size < 0 || payload_size > NATS_MAX_PAYLOAD_SIZE) { free(buf); return; }

                // Read exactly payload_size bytes (binary safe)
                char* payload_buf = client_read_bytes(payload_size);
                if (payload_buf == NULL) { free(buf); return; }
                // Skip trailing \r\n after payload
                client_skip_bytes(2);

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

                // Get callback and cache maxed check using reference counting (Issues #1, #7)
                xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
                Sub* sub = subs[sid];
                bool will_max_out = false;
                if (sub != NULL && !sub->is_marked_for_deletion()) {
                    // Add reference to prevent deletion during callback
                    sub->add_ref();
                    // Cache whether this will max out BEFORE calling callback
                    will_max_out = (sub->max_wanted > 0 && sub->received + 1 >= sub->max_wanted);
                } else {
                    sub = NULL;  // Don't call callback if marked for deletion
                }
                xSemaphoreGiveRecursive(mutex);

                if (sub != NULL) {
                    // Clear timeout to prevent stale timeout callbacks
                    sub->clear_timeout();
                    // Call user callback WITHOUT holding mutex to prevent deadlock
                    sub->call(e);

                    // Release reference and cleanup if needed
                    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
                    bool should_delete = sub->release();
                    if (should_delete || sub->can_delete()) {
                        subs[sid] = NULL;
                        free_sids.push(sid);
                        delete sub;
                    } else if (will_max_out) {
                        // Mark for deletion - will be deleted when ref_count reaches 0
                        sub->mark_for_deletion();
                    }
                    xSemaphoreGiveRecursive(mutex);
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
                xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
                if (sid < 0 || sid >= subs.size() || subs[sid] == NULL) {
                    xSemaphoreGiveRecursive(mutex);
                    free(buf);
                    return;
                };
                xSemaphoreGiveRecursive(mutex);

                long header_size_long = strtol((argc == 6)? argv[4] : argv[3], &endptr, 10);
                if (*endptr != '\0' || header_size_long < 0 || header_size_long > INT_MAX) { free(buf); return; }
                int header_size = (int)header_size_long;
                // Enforce max header size
                if (header_size > NATS_MAX_HEADER_LEN) { free(buf); return; }

                long total_size_long = strtol((argc == 6)? argv[5] : argv[4], &endptr, 10);
                if (*endptr != '\0' || total_size_long < 0 || total_size_long > INT_MAX) { free(buf); return; }
                int total_size = (int)total_size_long;
                // Check for integer overflow and enforce max total size
                if (total_size < 0 || total_size > NATS_MAX_PAYLOAD_SIZE + NATS_MAX_HEADER_LEN) { free(buf); return; }
                // Validate that header_size doesn't exceed total_size (malicious server protection)
                if (header_size < 0 || header_size > total_size) {
                    ESP_LOGE(tag, "Invalid message sizes: header=%d, total=%d", header_size, total_size);
                    free(buf);
                    return;
                }
                // Calculate data size from header and total sizes
                int data_size = total_size - header_size;

                // Read exactly total_size bytes (binary safe - headers + payload)
                char* full_buf = client_read_bytes(total_size);
                if (full_buf == NULL) { free(buf); return; }
                // Skip trailing \r\n after payload
                client_skip_bytes(2);

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

                // Get callback and cache maxed check using reference counting (Issues #1, #7)
                xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
                Sub* sub = subs[sid];
                bool will_max_out = false;
                if (sub != NULL && !sub->is_marked_for_deletion()) {
                    // Add reference to prevent deletion during callback
                    sub->add_ref();
                    // Cache whether this will max out BEFORE calling callback
                    will_max_out = (sub->max_wanted > 0 && sub->received + 1 >= sub->max_wanted);
                } else {
                    sub = NULL;  // Don't call callback if marked for deletion
                }
                xSemaphoreGiveRecursive(mutex);

                if (sub != NULL) {
                    // Clear timeout to prevent stale timeout callbacks
                    sub->clear_timeout();
                    // Call user callback WITHOUT holding mutex to prevent deadlock
                    sub->call(e);

                    // Release reference and cleanup if needed
                    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
                    bool should_delete = sub->release();
                    if (should_delete || sub->can_delete()) {
                        subs[sid] = NULL;
                        free_sids.push(sid);
                        delete sub;
                    } else if (will_max_out) {
                        // Mark for deletion - will be deleted when ref_count reaches 0
                        sub->mark_for_deletion();
                    }
                    xSemaphoreGiveRecursive(mutex);
                }
                free(full_buf);
            }
            else if (strcmp(argv[0], NATS_CTRL_OK) == 0) {
            }
            else if (strcmp(argv[0], NATS_CTRL_ERR) == 0) {
                // Log the server's error message for debugging
                if (argc > 1 && argv[1] != NULL) {
                    ESP_LOGE(tag, "Server error: %s", argv[1]);
                } else {
                    ESP_LOGE(tag, "Server error (no details)");
                }
                last_error_code = NATS_ERR_PROTOCOL_ERROR;
                if (on_error != NULL) on_error();
                disconnect();
            }
            else if (strcmp(argv[0], NATS_CTRL_PING) == 0) {
                send(NATS_CTRL_PONG);
            }
            else if (strcmp(argv[0], NATS_CTRL_PONG) == 0) {
                if (state_mutex != NULL) xSemaphoreTakeRecursive(state_mutex, portMAX_DELAY);
                outstanding_pings--;
                if (state_mutex != NULL) xSemaphoreGiveRecursive(state_mutex);
            }
            else if (strcmp(argv[0], NATS_CTRL_INFO) == 0) {
                // Parse INFO JSON for server capabilities (Issue #58)
                bool server_tls_required = false;
                if (argc > 1 && argv[1] != NULL) {
                    // Extract max_payload from INFO JSON
                    const char* max_payload_key = "\"max_payload\":";
                    const char* pos = strstr(argv[1], max_payload_key);
                    if (pos != NULL) {
                        pos += strlen(max_payload_key);
                        size_t parsed_max = 0;
                        while (*pos >= '0' && *pos <= '9') {
                            parsed_max = parsed_max * 10 + (*pos - '0');
                            pos++;
                        }
                        if (parsed_max > 0) {
                            server_max_payload = parsed_max;
                            ESP_LOGI(tag, "Server max_payload: %zu bytes", server_max_payload);
                        }
                    }
                    // Check if server requires TLS (STARTTLS style)
                    if (strstr(argv[1], "\"tls_required\":true") != NULL) {
                        server_tls_required = true;
                        ESP_LOGI(tag, "Server requires TLS");
                    }
                }

                // NATS STARTTLS: Upgrade to TLS if server requires it and TLS is configured
                // Note: Skip TLS upgrade for transport connections (WebSocket) since:
                // - wss:// already provides TLS at the transport layer
                // - ws:// cannot be upgraded to TLS mid-connection
                if (server_tls_required && transport == NULL) {
                    if (!tls_config.enabled) {
                        ESP_LOGE(tag, "Server requires TLS but TLS not configured");
                        last_error_code = NATS_ERR_TLS_CONNECTION_FAILED;
                        disconnect();
                        free(buf);
                        return;
                    }
                    if (!upgrade_to_tls()) {
                        ESP_LOGE(tag, "TLS upgrade failed");
                        disconnect();
                        free(buf);
                        return;
                    }
                } else if (server_tls_required && transport != NULL) {
                    // For WebSocket transport, TLS is handled at transport level (wss://)
                    ESP_LOGI(tag, "Server requires TLS - already secured via transport (wss://)");
                }

                send_connect();
                connected = true;
                // Restore existing subscriptions on reconnect (Issues #39, #55)
                restore_subscriptions();
                if (on_connect != NULL) on_connect();
                // Send any pending messages that were buffered while offline
                send_pending_messages();
            }
            free(buf);
        }

        void ping() {
            if (state_mutex != NULL) xSemaphoreTakeRecursive(state_mutex, portMAX_DELAY);
            int pings = outstanding_pings;
            if (state_mutex != NULL) xSemaphoreGiveRecursive(state_mutex);

            if (pings > max_outstanding_pings) {
                last_error_code = NATS_ERR_MAX_PINGS_EXCEEDED;
                disconnect();
                return;
            }

            if (state_mutex != NULL) xSemaphoreTakeRecursive(state_mutex, portMAX_DELAY);
            outstanding_pings++;
            if (state_mutex != NULL) xSemaphoreGiveRecursive(state_mutex);

            send(NATS_CTRL_PING);
        }

        // Validate subject/reply-to for CR/LF injection attacks
        bool contains_crlf(const char* str) {
            if (str == NULL) return false;
            for (const char* p = str; *p != '\0'; p++) {
                if (*p == '\r' || *p == '\n') return true;
            }
            return false;
        }

        /**
         * Escape JSON special characters (Issue #48)
         *
         * @param src Source string to escape
         * @param dest Destination buffer
         * @param dest_size Size of destination buffer
         * @return Number of characters written (excluding null terminator), or -1 if truncated
         */
        int json_escape(const char* src, char* dest, size_t dest_size) {
            if (src == NULL || dest == NULL || dest_size == 0) return -1;

            size_t j = 0;
            for (size_t i = 0; src[i] != '\0' && j < dest_size - 1; i++) {
                char c = src[i];

                // Check if we need to escape this character
                if (c == '"' || c == '\\' || c == '\b' || c == '\f' || c == '\n' || c == '\r' || c == '\t') {
                    if (j >= dest_size - 2) {  // Need room for backslash + char + null
                        dest[j] = '\0';
                        return -1;  // Truncated
                    }
                    dest[j++] = '\\';

                    // Add the escaped character
                    switch (c) {
                        case '"':  dest[j++] = '"'; break;
                        case '\\': dest[j++] = '\\'; break;
                        case '\b': dest[j++] = 'b'; break;
                        case '\f': dest[j++] = 'f'; break;
                        case '\n': dest[j++] = 'n'; break;
                        case '\r': dest[j++] = 'r'; break;
                        case '\t': dest[j++] = 't'; break;
                    }
                } else if ((unsigned char)c < 32) {
                    // Control characters: use \uXXXX notation
                    if (j >= dest_size - 6) {  // Need room for \uXXXX + null
                        dest[j] = '\0';
                        return -1;  // Truncated
                    }
                    j += snprintf(dest + j, dest_size - j, "\\u%04x", (unsigned char)c);
                } else {
                    dest[j++] = c;
                }
            }

            dest[j] = '\0';
            return (src[j] == '\0') ? (int)j : -1;  // Return -1 if truncated
        }

        /**
         * Validate NATS header format (Issue #47)
         *
         * @param headers The headers string to validate
         * @return true if valid, false otherwise
         */
        bool validate_headers_format(const char* headers) {
            if (headers == NULL) return true;  // NULL headers are allowed

            size_t len = strlen(headers);

            // Check header length
            if (len > NATS_MAX_HEADER_LEN) {
                ESP_LOGE(tag, "Headers too long: %zu bytes (max %d)", len, NATS_MAX_HEADER_LEN);
                return false;
            }

            // NATS headers should start with "NATS/1.0\r\n" or at minimum have proper line endings
            // We'll do a basic check: headers should end with \r\n and not contain embedded nulls
            if (len < 2 || headers[len-2] != '\r' || headers[len-1] != '\n') {
                ESP_LOGW(tag, "Headers should end with \\r\\n");
                // This is a warning, not an error - allow it but log
            }

            // Check for embedded null characters (protocol corruption)
            for (size_t i = 0; i < len - 1; i++) {
                if (headers[i] == '\0') {
                    ESP_LOGE(tag, "Headers contain embedded null character at position %zu", i);
                    return false;
                }
            }

            return true;
        }

        /**
         * Validate subject format according to NATS protocol rules
         *
         * @param subject The subject string to validate
         * @param allow_wildcards Whether to allow wildcards (* and >)
         * @return true if valid, false otherwise
         */
        bool validate_subject_format(const char* subject, bool allow_wildcards = true) {
            // Check for NULL or empty subject (Issue #41)
            if (subject == NULL || subject[0] == '\0') {
                ESP_LOGE(tag, "Subject is NULL or empty");
                return false;
            }

            // Check for whitespace-only subject (Issue #41)
            bool has_non_whitespace = false;
            for (const char* p = subject; *p != '\0'; p++) {
                if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
                    has_non_whitespace = true;
                    break;
                }
            }
            if (!has_non_whitespace) {
                ESP_LOGE(tag, "Subject contains only whitespace");
                return false;
            }

            // Check subject length (Issue #26)
            size_t len = strlen(subject);
            if (len > NATS_MAX_SUBJECT_LEN) {
                ESP_LOGE(tag, "Subject too long: %zu bytes (max %d)", len, NATS_MAX_SUBJECT_LEN);
                return false;
            }

            // Check for invalid characters and empty tokens (Issue #26)
            bool prev_was_dot = true;  // Start as true to catch leading dots
            for (const char* p = subject; *p != '\0'; p++) {
                char c = *p;

                // Check for wildcards (Issue #40)
                if ((c == '*' || c == '>') && !allow_wildcards) {
                    ESP_LOGE(tag, "Wildcards not allowed in publish subject: '%c'", c);
                    return false;
                }

                // Valid characters: alphanumeric, '.', '-', '_', '*', '>', '$'
                // Note: '$' is required for JetStream system subjects like $JS.API.*
                if (!(isalnum(c) || c == '.' || c == '-' || c == '_' || c == '*' || c == '>' || c == '$')) {
                    ESP_LOGE(tag, "Invalid character in subject: '%c' (0x%02x)", c, (unsigned char)c);
                    return false;
                }

                // Check for empty tokens (consecutive dots like "..")
                if (c == '.') {
                    if (prev_was_dot) {
                        ESP_LOGE(tag, "Subject has empty token (consecutive dots)");
                        return false;
                    }
                    prev_was_dot = true;
                } else {
                    prev_was_dot = false;
                }
            }

            // Check for trailing dot (empty token at end)
            if (subject[len - 1] == '.') {
                ESP_LOGE(tag, "Subject ends with dot (empty trailing token)");
                return false;
            }

            return true;
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

        void generate_nuid(char* buf, size_t size) {
            if (buf == NULL || size == 0) return;
            size_t alphanum_len = sizeof(NATSUtil::alphanums) - 1;
            for (size_t i = 0; i < size - 1; i++) {
                // Avoid modulo bias by using rejection sampling
                uint32_t random_val;
                uint32_t max_valid = (UINT32_MAX / alphanum_len) * alphanum_len;
                do {
                    random_val = esp_random();
                } while (random_val >= max_valid);
                buf[i] = NATSUtil::alphanums[random_val % alphanum_len];
            }
            buf[size - 1] = '\0';
        }

        unsigned long get_reconnect_delay() {
            if (!exponential_backoff_enabled) {
                return NATS_RECONNECT_INTERVAL;
            }

            // Exponential backoff: 1s, 2s, 4s, 8s, 16s, 30s (max)
            // Prevent overflow by capping reconnect_attempts
            if (reconnect_attempts >= 30) {
                return NATS_MAX_RECONNECT_DELAY;
            }
            unsigned long delay = NATS_RECONNECT_INTERVAL * (1UL << reconnect_attempts);
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

        /**
         * Restore all active subscriptions on reconnect (Issues #39, #55)
         * Re-sends SUB commands to server for each non-deleted subscription
         */
        void restore_subscriptions() {
            xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
            size_t restored = 0;
            for (size_t i = 0; i < subs.size(); i++) {
                Sub* sub = subs[i];
                if (sub != NULL && !sub->is_marked_for_deletion() && sub->subject != NULL) {
                    // Re-send SUB command with original SID
                    // SUB protocol: SUB <subject> [queue group] <sid>
                    if (sub->queue != NULL && sub->queue[0] != '\0') {
                        send_fmt("SUB %s %s %zu", sub->subject, sub->queue, i);
                    } else {
                        send_fmt("SUB %s %zu", sub->subject, i);
                    }
                    restored++;
                }
            }
            xSemaphoreGiveRecursive(mutex);
            if (restored > 0) {
                ESP_LOGI(tag, "Restored %zu subscriptions after reconnect", restored);
            }
        }

        void send_pending_messages() {
            // Process messages one at a time to avoid holding mutex during publish
            while (true) {
                pending_msg_t pmsg = {};
                bool has_message = false;

                // Pop message with mutex protection
                xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
                if (!pending_messages.empty()) {
                    pmsg = pending_messages.pop();
                    has_message = true;
                }
                xSemaphoreGiveRecursive(mutex);

                if (!has_message) break;

                // Publish without holding mutex (to avoid deadlock if publish fails and calls disconnect)
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

                // Check if still connected after each publish
                if (!connected) break;
            }
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

        // Clean up mbedtls resources
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

        // Upgrade existing plain TCP socket to TLS (STARTTLS style)
        // Uses mbedtls directly since esp_tls doesn't support STARTTLS
        bool upgrade_to_tls() {
            if (!tls_config.enabled || sockfd < 0) {
                return false;
            }

            ESP_LOGI(tag, "Upgrading connection to TLS on socket %d (using mbedtls)...", sockfd);

            int ret;
            char err_buf[100];

            // Allocate mbedtls contexts
            mbedtls_ssl = (mbedtls_ssl_context*)malloc(sizeof(mbedtls_ssl_context));
            mbedtls_conf = (mbedtls_ssl_config*)malloc(sizeof(mbedtls_ssl_config));
            mbedtls_cacert = (mbedtls_x509_crt*)malloc(sizeof(mbedtls_x509_crt));
            mbedtls_entropy = (mbedtls_entropy_context*)malloc(sizeof(mbedtls_entropy_context));
            mbedtls_ctr_drbg = (mbedtls_ctr_drbg_context*)malloc(sizeof(mbedtls_ctr_drbg_context));

            if (!mbedtls_ssl || !mbedtls_conf || !mbedtls_cacert || !mbedtls_entropy || !mbedtls_ctr_drbg) {
                ESP_LOGE(tag, "Failed to allocate mbedtls contexts");
                cleanup_mbedtls();
                last_error_code = NATS_ERR_OUT_OF_MEMORY;
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
                ESP_LOGE(tag, "mbedtls_ctr_drbg_seed failed: %s (0x%x)", err_buf, -ret);
                cleanup_mbedtls();
                last_error_code = NATS_ERR_TLS_INIT_FAILED;
                return false;
            }

            // Parse CA certificate
            if (tls_config.ca_cert != NULL && tls_config.ca_cert_len > 0) {
                ret = mbedtls_x509_crt_parse(mbedtls_cacert,
                                             (const unsigned char*)tls_config.ca_cert,
                                             tls_config.ca_cert_len);
                if (ret != 0) {
                    mbedtls_strerror(ret, err_buf, sizeof(err_buf));
                    ESP_LOGE(tag, "mbedtls_x509_crt_parse failed: %s (0x%x)", err_buf, -ret);
                    cleanup_mbedtls();
                    last_error_code = NATS_ERR_TLS_INIT_FAILED;
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
                ESP_LOGE(tag, "mbedtls_ssl_config_defaults failed: %s (0x%x)", err_buf, -ret);
                cleanup_mbedtls();
                last_error_code = NATS_ERR_TLS_INIT_FAILED;
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
                ESP_LOGE(tag, "mbedtls_ssl_setup failed: %s (0x%x)", err_buf, -ret);
                cleanup_mbedtls();
                last_error_code = NATS_ERR_TLS_INIT_FAILED;
                return false;
            }

            // Set hostname for SNI and certificate verification
            const char* hostname = tls_config.server_name;
            if (hostname == NULL && current_server_idx < servers.size()) {
                hostname = servers[current_server_idx].hostname;
            }
            if (hostname != NULL) {
                ret = mbedtls_ssl_set_hostname(mbedtls_ssl, hostname);
                if (ret != 0) {
                    mbedtls_strerror(ret, err_buf, sizeof(err_buf));
                    ESP_LOGE(tag, "mbedtls_ssl_set_hostname failed: %s (0x%x)", err_buf, -ret);
                    cleanup_mbedtls();
                    last_error_code = NATS_ERR_TLS_INIT_FAILED;
                    return false;
                }
            }

            // Set up bio callbacks to use our existing socket
            mbedtls_ssl_set_bio(mbedtls_ssl, &sockfd, mbedtls_net_send_cb, mbedtls_net_recv_cb, NULL);

            // Perform TLS handshake
            ESP_LOGI(tag, "Starting TLS handshake...");
            while ((ret = mbedtls_ssl_handshake(mbedtls_ssl)) != 0) {
                if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                    mbedtls_strerror(ret, err_buf, sizeof(err_buf));
                    ESP_LOGE(tag, "mbedtls_ssl_handshake failed: %s (0x%x)", err_buf, -ret);
                    cleanup_mbedtls();
                    last_error_code = NATS_ERR_TLS_CONNECTION_FAILED;
                    return false;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            // Verify certificate (optional logging)
            uint32_t flags = mbedtls_ssl_get_verify_result(mbedtls_ssl);
            if (flags != 0) {
                char vrfy_buf[512];
                mbedtls_x509_crt_verify_info(vrfy_buf, sizeof(vrfy_buf), "  ! ", flags);
                ESP_LOGW(tag, "Certificate verification warning: %s", vrfy_buf);
            }

            using_mbedtls_directly = true;
            ESP_LOGI(tag, "TLS upgrade successful (cipher: %s)", mbedtls_ssl_get_ciphersuite(mbedtls_ssl));
            return true;
        }

        /**
         * Connect using the transport abstraction layer
         * Used when a transport is set via factory methods or set_transport()
         */
        bool connect_with_transport(unsigned long timeout_ms) {
            if (transport == NULL) {
                ESP_LOGE(tag, "No transport configured");
                last_error_code = NATS_ERR_NOT_CONNECTED;
                return false;
            }

            // Try connecting to all servers in the list
            size_t start_idx = current_server_idx;
            do {
                const nats_server_t& server = servers[current_server_idx];
                ESP_LOGI(tag, "Attempting transport connect to %s:%d",
                    server.hostname, server.port);

                // Build transport configuration
                nats_transport_config_t config = {NULL, 0, NULL, NULL, NULL, NULL, NULL};
                config.hostname = server.hostname;
                config.port = server.port;
                config.user = user;
                config.pass = pass;
                config.tls_config = tls_config.enabled ? &tls_config : NULL;
                config.ws_path = server.ws_path ? server.ws_path : NATS_WEBSOCKET_PATH;
                config.ws_subprotocol = NATS_WEBSOCKET_SUBPROTOCOL;

                // Attempt connection via transport
                if (transport->connect(&config)) {
                    if (state_mutex != NULL) xSemaphoreTakeRecursive(state_mutex, portMAX_DELAY);
                    outstanding_pings = 0;
                    reconnect_attempts = 0;
                    if (state_mutex != NULL) xSemaphoreGiveRecursive(state_mutex);
                    connect_state = CONNECTING;
                    metrics.last_connect_time = NATSUtil::millis();
                    ESP_LOGI(tag, "Transport connected to %s:%d",
                        server.hostname, server.port);

                    // Wait for NATS protocol handshake
                    unsigned long start_time = NATSUtil::millis();
                    while (!connected && (NATSUtil::millis() - start_time) < timeout_ms) {
                        // Check for incoming data
                        if (transport->has_data_available(100)) {  // 100ms timeout
                            recv();  // Process INFO message, send CONNECT
                        }
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }

                    if (connected) {
                        if (metrics.reconnections > 0) {
                            metrics.reconnections++;
                        }
                        return true;
                    } else {
                        ESP_LOGE(tag, "NATS handshake timeout via transport");
                        transport->disconnect();
                    }
                } else {
                    ESP_LOGE(tag, "Transport connection failed: %s", transport->get_error_string());
                    last_error_code = transport->get_last_error();
                }

                // Move to next server
                current_server_idx = (current_server_idx + 1) % servers.size();
            } while (current_server_idx != start_idx);

            // All servers failed
            if (state_mutex != NULL) xSemaphoreTakeRecursive(state_mutex, portMAX_DELAY);
            reconnect_attempts++;
            if (state_mutex != NULL) xSemaphoreGiveRecursive(state_mutex);
            connect_state = DISCONNECTED;
            return false;
        }

        bool try_connect_to_server(const nats_server_t& server) {
            // NATS uses STARTTLS style: always connect plain TCP first,
            // then upgrade to TLS after receiving INFO with tls_required
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

                // Set socket to non-blocking mode for timeout support
                int flags = fcntl(sockfd, F_GETFL, 0);
                if (flags < 0 || fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
                    ESP_LOGW(tag, "Failed to set non-blocking mode, using blocking connect");
                    // Fall back to blocking connect
                    if (::connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0) {
                        freeaddrinfo(result);
                        last_error_code = NATS_ERR_NONE;
                        return true;
                    }
                    close(sockfd);
                    sockfd = -1;
                    continue;
                }

                // Initiate non-blocking connect
                int conn_ret = ::connect(sockfd, rp->ai_addr, rp->ai_addrlen);
                if (conn_ret == 0) {
                    // Connected immediately (unlikely but possible)
                    fcntl(sockfd, F_SETFL, flags);  // Restore original flags
                    freeaddrinfo(result);
                    last_error_code = NATS_ERR_NONE;
                    return true;
                }

                // Check if connection is in progress
                if (errno != EINPROGRESS) {
                    close(sockfd);
                    sockfd = -1;
                    continue;
                }

                // Wait for connection with timeout (30 seconds, matching TLS timeout)
                fd_set write_fds;
                FD_ZERO(&write_fds);
                FD_SET(sockfd, &write_fds);
                struct timeval timeout;
                timeout.tv_sec = 30;
                timeout.tv_usec = 0;

                int select_ret = select(sockfd + 1, NULL, &write_fds, NULL, &timeout);
                if (select_ret <= 0) {
                    // Timeout or error
                    ESP_LOGE(tag, "Connection timeout or select error for %s:%d", server.hostname, server.port);
                    close(sockfd);
                    sockfd = -1;
                    continue;
                }

                // Check if connection succeeded or failed
                int sock_err = 0;
                socklen_t len = sizeof(sock_err);
                if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &sock_err, &len) < 0 || sock_err != 0) {
                    // Connection failed
                    ESP_LOGE(tag, "Connection failed for %s:%d: %d", server.hostname, server.port, sock_err);
                    close(sockfd);
                    sockfd = -1;
                    continue;
                }

                // Connection successful - restore blocking mode
                fcntl(sockfd, F_SETFL, flags);
                freeaddrinfo(result);
                last_error_code = NATS_ERR_NONE;
                return true;
            }

            // All addresses failed
            freeaddrinfo(result);
            last_error_code = NATS_ERR_CONNECTION_FAILED;
            return false;
        }

    public:
        bool connect(unsigned long timeout_ms = 5000) {
            // Check if already connected
            if (connected) {
                ESP_LOGW(tag, "Already connected to NATS server");
                return true;
            }

            // Use transport if available
            if (transport != NULL) {
                return connect_with_transport(timeout_ms);
            }

            // Try connecting to all servers in the list (legacy socket mode)
            size_t start_idx = current_server_idx;
            do {
                ESP_LOGI(tag, "Attempting to connect to %s:%d",
                    servers[current_server_idx].hostname,
                    servers[current_server_idx].port);

                if (try_connect_to_server(servers[current_server_idx])) {
                    if (state_mutex != NULL) xSemaphoreTakeRecursive(state_mutex, portMAX_DELAY);
                    outstanding_pings = 0;
                    reconnect_attempts = 0;
                    if (state_mutex != NULL) xSemaphoreGiveRecursive(state_mutex);
                    connect_state = CONNECTING;
                    metrics.last_connect_time = NATSUtil::millis();
                    ESP_LOGI(tag, "Connected to %s:%d",
                        servers[current_server_idx].hostname,
                        servers[current_server_idx].port);

                    // Wait for NATS protocol handshake (INFO/CONNECT) to complete
                    unsigned long start_time = NATSUtil::millis();
                    while (!connected && (NATSUtil::millis() - start_time) < timeout_ms) {
                        // Check for incoming data (INFO message from server)
                        if (sockfd >= 0) {
                            int fd_to_check = sockfd;
                            if (tls_config.enabled && tls != NULL) {
                                if (esp_tls_get_conn_sockfd(tls, &fd_to_check) != ESP_OK) {
                                    break;
                                }
                            }
                            fd_set rfds;
                            struct timeval tv = {0, 100000};  // 100ms timeout
                            FD_ZERO(&rfds);
                            FD_SET(fd_to_check, &rfds);
                            int ret = select(fd_to_check + 1, &rfds, NULL, NULL, &tv);
                            if (ret > 0 && FD_ISSET(fd_to_check, &rfds)) {
                                recv();  // Process INFO message, send CONNECT
                            }
                        }
                        vTaskDelay(pdMS_TO_TICKS(10));  // Small delay to prevent busy loop
                    }

                    if (connected) {
                        if (metrics.reconnections > 0) {
                            metrics.reconnections++;
                        }
                        return true;
                    } else {
                        ESP_LOGE(tag, "NATS handshake timeout");
                        disconnect();
                        // Fall through to try next server
                    }
                }

                // Move to next server
                current_server_idx = (current_server_idx + 1) % servers.size();
            } while (current_server_idx != start_idx);

            // All servers failed
            if (state_mutex != NULL) xSemaphoreTakeRecursive(state_mutex, portMAX_DELAY);
            reconnect_attempts++;
            if (state_mutex != NULL) xSemaphoreGiveRecursive(state_mutex);
            connect_state = DISCONNECTED;
            return false;
        }

        void connect_async(connect_cb callback) {
            async_connect_cb = callback;
            connect_state = CONNECTING;
            // Connection will be attempted in process()
        }

        void disconnect() {
            if (!connected && (transport == NULL || !transport->is_connected())) return;
            connected = false;
            connect_state = DISCONNECTED;
            async_connect_cb = NULL;

            if (last_error_code == NATS_ERR_NONE) {
                last_error_code = NATS_ERR_DISCONNECTED;
            }

            // Disconnect transport if using transport abstraction
            if (transport != NULL) {
                transport->disconnect();
                // Don't clean up legacy resources when using transport
                if (on_disconnect != NULL) on_disconnect();
                return;
            }

            // Legacy cleanup (when not using transport)
            // Clean up mbedtls resources (STARTTLS mode)
            if (using_mbedtls_directly) {
                cleanup_mbedtls();
            }

            if (tls_config.enabled && tls != NULL) {
                esp_tls_conn_destroy(tls);
                tls = NULL;
            }

            if (sockfd >= 0) {
                ::close(sockfd);  // Always close socket in STARTTLS mode
                sockfd = -1;
            }

            // NOTE: We intentionally do NOT delete subscriptions here!
            // Subscriptions are preserved so they can be restored on reconnect.
            // Use shutdown() or the destructor to fully clean up subscriptions.

            if (on_disconnect != NULL) on_disconnect();
        }

        // Fully close the connection and clean up all subscriptions
        // Use this when you want to completely shut down the client
        void shutdown() {
            disconnect();

            // Clean up subscriptions with mutex protection
            // Note: We set elements to NULL but don't resize the array
            // since NATSUtil::Array doesn't have a clear() method
            if (mutex != NULL) {
                xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
                for (size_t i = 0; i < subs.size(); i++) {
                    if (subs[i] != NULL) {
                        delete subs[i];
                        subs[i] = NULL;
                    }
                }
                // Reset free_sids stack
                while (!free_sids.empty()) {
                    free_sids.pop();
                }
                xSemaphoreGiveRecursive(mutex);
            } else {
                for (size_t i = 0; i < subs.size(); i++) {
                    if (subs[i] != NULL) {
                        delete subs[i];
                        subs[i] = NULL;
                    }
                }
            }
        }

        void publish(const char* subject, const char* msg = NULL, const char* replyto = NULL) {
            // Validate subject format (Issues #26, #40, #41)
            if (!validate_subject_format(subject, false)) {  // false = no wildcards in publish
                last_error_code = NATS_ERR_INVALID_SUBJECT;
                return;
            }
            // Prevent protocol injection via CR/LF in subject or reply-to
            if (contains_crlf(subject) || contains_crlf(replyto)) {
                ESP_LOGE(tag, "Subject or reply-to contains CR/LF (protocol injection attempt)");
                last_error_code = NATS_ERR_INVALID_ARG;
                return;
            }
            // Enforce server max_payload limit (Issue #77)
            size_t msg_len = (msg == NULL) ? 0 : strlen(msg);
            if (msg_len > server_max_payload) {
                ESP_LOGE(tag, "Message too large: %zu bytes (server max: %zu)", msg_len, server_max_payload);
                last_error_code = NATS_ERR_INVALID_ARG;
                return;
            }
            if (!connected) {
                buffer_message(subject, msg, replyto, NULL);
                return;
            }
            // PUB protocol: PUB <subject> [reply-to] <#bytes>\r\n[payload]\r\n
            // Note: reply-to is optional - don't include extra space if not present
            if (replyto != NULL && replyto[0] != '\0') {
                send_fmt("PUB %s %s %lu",
                        subject,
                        replyto,
                        (unsigned long)msg_len);
            } else {
                send_fmt("PUB %s %lu",
                        subject,
                        (unsigned long)msg_len);
            }
            send((msg == NULL)? "" : msg);
            metrics.msgs_sent++;
        }
        void publish(const char* subject, const bool msg) {
            publish(subject, (msg)? "true" : "false");
        }

        // Binary-safe publish - sends binary data with explicit length (no strlen)
        void publish_binary(const char* subject, const uint8_t* data, size_t data_len, const char* replyto = NULL) {
            // Validate subject format
            if (!validate_subject_format(subject, false)) {
                last_error_code = NATS_ERR_INVALID_SUBJECT;
                return;
            }
            if (contains_crlf(subject) || contains_crlf(replyto)) {
                ESP_LOGE(tag, "Subject or reply-to contains CR/LF (protocol injection attempt)");
                last_error_code = NATS_ERR_INVALID_ARG;
                return;
            }
            // Enforce server max_payload limit
            if (data_len > server_max_payload) {
                ESP_LOGE(tag, "Binary message too large: %zu bytes (server max: %zu)", data_len, server_max_payload);
                last_error_code = NATS_ERR_INVALID_ARG;
                return;
            }
            if (!connected) {
                // Can't buffer binary data easily, just skip
                return;
            }
            // PUB protocol: PUB <subject> [reply-to] <#bytes>\r\n[payload]\r\n
            if (replyto != NULL && replyto[0] != '\0') {
                send_fmt("PUB %s %s %zu", subject, replyto, data_len);
            } else {
                send_fmt("PUB %s %zu", subject, data_len);
            }
            send_binary(data, data_len);
            metrics.msgs_sent++;
        }

        void publish_with_headers(const char* subject, const char* headers, const char* msg = NULL, const char* replyto = NULL) {
            // Validate subject format (Issues #26, #40, #41)
            if (!validate_subject_format(subject, false)) {  // false = no wildcards in publish
                last_error_code = NATS_ERR_INVALID_SUBJECT;
                return;
            }
            // Validate header format (Issue #47)
            if (!validate_headers_format(headers)) {
                last_error_code = NATS_ERR_INVALID_ARG;
                return;
            }
            // Prevent protocol injection via CR/LF in subject or reply-to
            if (contains_crlf(subject) || contains_crlf(replyto)) {
                ESP_LOGE(tag, "Subject or reply-to contains CR/LF (protocol injection attempt)");
                last_error_code = NATS_ERR_INVALID_ARG;
                return;
            }
            // Enforce server max_payload limit (Issue #77) - for HPUB, check total size
            size_t header_len = (headers == NULL) ? 0 : strlen(headers);
            size_t msg_len = (msg == NULL) ? 0 : strlen(msg);
            size_t total_len = header_len + msg_len;
            if (total_len > server_max_payload) {
                ESP_LOGE(tag, "Message+headers too large: %zu bytes (server max: %zu)", total_len, server_max_payload);
                last_error_code = NATS_ERR_INVALID_ARG;
                return;
            }
            if (!connected) {
                buffer_message(subject, msg, replyto, headers);
                return;
            }

            // HPUB protocol: HPUB <subject> [reply-to] <header_size> <total_size>\r\n<headers><payload>\r\n
            // Build complete message in a single buffer for atomic send (like nats.c does)
            // Calculate buffer size needed
            size_t proto_len = 64 + strlen(subject) + (replyto ? strlen(replyto) : 0);  // Protocol line
            size_t buf_size = proto_len + header_len + msg_len + 4;  // +4 for CRLFs

            char* buf = (char*)malloc(buf_size);
            if (buf == NULL) {
                ESP_LOGE(tag, "Failed to allocate HPUB buffer");
                last_error_code = NATS_ERR_OUT_OF_MEMORY;
                return;
            }

            int offset;
            if (replyto != NULL && replyto[0] != '\0') {
                offset = snprintf(buf, buf_size, "HPUB %s %s %lu %lu\r\n",
                        subject, replyto, (unsigned long)header_len, (unsigned long)total_len);
            } else {
                offset = snprintf(buf, buf_size, "HPUB %s %lu %lu\r\n",
                        subject, (unsigned long)header_len, (unsigned long)total_len);
            }

            // Append headers (already includes \r\n\r\n terminator)
            if (headers != NULL && header_len > 0) {
                memcpy(buf + offset, headers, header_len);
                offset += header_len;
            }

            // Append payload
            if (msg != NULL && msg_len > 0) {
                memcpy(buf + offset, msg, msg_len);
                offset += msg_len;
            }

            // Append final CRLF
            memcpy(buf + offset, "\r\n", 2);
            offset += 2;

            // Send entire message atomically
            bool success = send_raw(buf, offset);
            free(buf);

            if (!success) {
                return;
            }
            metrics.msgs_sent++;
            metrics.bytes_sent += offset;
        }
        void publish_fmt(const char* subject, const char* fmt, ...) {
            va_list args;
            va_start(args, fmt);
            char* buf;
            vasprintf(&buf, fmt, args);
            va_end(args);
            publish(subject, buf);
            // Securely zero buffer before freeing (may contain sensitive data)
            NATSUtil::secure_free(buf);
        }
        void publishf(const char* subject, const char* fmt, ...) {
            va_list args;
            va_start(args, fmt);
            char* buf;
            vasprintf(&buf, fmt, args);
            va_end(args);
            publish(subject, buf);
            // Securely zero buffer before freeing (may contain sensitive data)
            NATSUtil::secure_free(buf);
        }

        int subscribe(const char* subject, sub_cb cb, const char* queue = NULL, const int max_wanted = 0) {
            if (!connected) return -1;
            // Validate subject format (Issues #26, #41) - wildcards allowed in subscribe
            if (!validate_subject_format(subject, true)) {  // true = wildcards allowed
                last_error_code = NATS_ERR_INVALID_SUBJECT;
                return -1;
            }
            // Prevent protocol injection via CR/LF in subject or queue
            if (contains_crlf(subject) || contains_crlf(queue)) {
                ESP_LOGE(tag, "Subject or queue contains CR/LF (protocol injection attempt)");
                last_error_code = NATS_ERR_INVALID_ARG;
                return -1;
            }

            xSemaphoreTakeRecursive(mutex, portMAX_DELAY);

            // Enforce subscription limit
            size_t active_subs = subs.size() - free_sids.size();
            if (active_subs >= NATS_MAX_SUBSCRIPTIONS) {
                ESP_LOGE(tag, "Subscription limit reached: %zu (max %d)", active_subs, NATS_MAX_SUBSCRIPTIONS);
                xSemaphoreGiveRecursive(mutex);
                last_error_code = NATS_ERR_TOO_MANY_SUBS;
                return -1;
            }

            // Store subject/queue for reconnection restoration (Issues #39, #55)
            Sub* sub = new Sub(cb, max_wanted, 0, NULL, subject, queue);
            int sid;
            if (free_sids.empty()) {
                sid = subs.push_back(sub);
            } else {
                sid = free_sids.pop();
                subs[sid] = sub;
            }
            // SUB protocol: SUB <subject> [queue group] <sid>
            if (queue != NULL && queue[0] != '\0') {
                send_fmt("SUB %s %s %d", subject, queue, sid);
            } else {
                send_fmt("SUB %s %d", subject, sid);
            }
            xSemaphoreGiveRecursive(mutex);
            return sid;
        }

        void unsubscribe(const int sid) {
            xSemaphoreTakeRecursive(mutex, portMAX_DELAY);

            // Validate SID bounds
            if (sid < 0 || sid >= subs.size() || subs[sid] == NULL) {
                xSemaphoreGiveRecursive(mutex);
                return;
            }

            Sub* sub = subs[sid];

            // Send UNSUB to server if connected (Issue #45: don't fail if disconnected)
            if (connected) {
                send_fmt("UNSUB %d", sid);
            }

            // Use reference counting to safely delete (Issues #1, #7)
            if (sub->get_ref_count() <= 1) {
                // No callbacks running - safe to delete immediately
                delete sub;
                subs[sid] = NULL;
                free_sids.push(sid);
            } else {
                // Callbacks are running - mark for lazy deletion
                sub->mark_for_deletion();
            }
            xSemaphoreGiveRecursive(mutex);
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

            xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
            Sub* sub = new Sub(cb, max_wanted, timeout_ms, on_timeout);
            int sid;
            if (free_sids.empty()) {
                sid = subs.push_back(sub);
            } else {
                sid = free_sids.pop();
                subs[sid] = sub;
            }
            send_fmt("SUB %s %d", inbox, sid);
            xSemaphoreGiveRecursive(mutex);

            publish(subject, msg, inbox);
            free(inbox);
            return sid;
        }

        // JetStream publish with ACK and optional deduplication
        // NOTE: JetStream publish works by publishing directly to the subject (NOT an API endpoint)
        // with a reply-to inbox. JetStream sends an ACK when the message is stored.
        int jetstream_publish(const char* subject, const char* msg, sub_cb ack_cb,
                             timeout_cb on_timeout = NULL, unsigned long timeout_ms = 5000,
                             const char* msg_id = NULL) {
            if (subject == NULL || subject[0] == 0) return -1;
            if (!connected) return -1;

            // Generate inbox for receiving the JetStream ACK
            char* inbox = generate_inbox_subject();
            if (inbox == NULL) return -1;

            // Subscribe to inbox first to receive ACK
            xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
            Sub* sub = new Sub(ack_cb, 1, timeout_ms, on_timeout);
            int sid;
            if (free_sids.empty()) {
                sid = subs.push_back(sub);
            } else {
                sid = free_sids.pop();
                subs[sid] = sub;
            }
            send_fmt("SUB %s %d", inbox, sid);
            xSemaphoreGiveRecursive(mutex);

            if (msg_id != NULL) {
                // Use headers for message deduplication
                char headers[256];
                snprintf(headers, sizeof(headers), "NATS/1.0\r\nNats-Msg-Id: %s\r\n\r\n", msg_id);
                // Publish directly to subject (not an API endpoint) with reply-to inbox
                publish_with_headers(subject, headers, msg, inbox);
            } else {
                // Publish directly to subject with reply-to inbox for ACK
                publish(subject, msg, inbox);
            }

            free(inbox);
            return sid;
        }

        // Create a JetStream stream
        int jetstream_stream_create(const jetstream_stream_config_t* config, sub_cb response_cb,
                                   timeout_cb on_timeout = NULL, unsigned long timeout_ms = 5000) {
            if (config == NULL || config->name == NULL) return -1;
            if (!connected) return -1;

            // Build JSON configuration
            const size_t json_size = 2048;
            char* json = (char*)malloc(json_size);
            if (json == NULL) return -1;

            int offset = snprintf(json, json_size, "{\"name\":\"%s\",\"subjects\":[", config->name);
            if (offset >= (int)json_size) { free(json); return -1; }

            // Add subjects array
            if (config->subjects != NULL) {
                bool first = true;
                for (size_t i = 0; i < NATS_MAX_SUBJECTS && config->subjects[i] != NULL; i++) {
                    if (!first) {
                        offset += snprintf(json + offset, json_size - offset, ",");
                        if (offset >= (int)json_size) { free(json); return -1; }
                    }
                    offset += snprintf(json + offset, json_size - offset, "\"%s\"", config->subjects[i]);
                    if (offset >= (int)json_size) { free(json); return -1; }
                    first = false;
                }
            }
            offset += snprintf(json + offset, json_size - offset, "]");
            if (offset >= (int)json_size) { free(json); return -1; }

            // Add optional parameters
            if (config->max_msgs > 0) {
                offset += snprintf(json + offset, json_size - offset, ",\"max_msgs\":%zu", config->max_msgs);
                if (offset >= (int)json_size) { free(json); return -1; }
            }
            if (config->max_bytes > 0) {
                offset += snprintf(json + offset, json_size - offset, ",\"max_bytes\":%zu", config->max_bytes);
                if (offset >= (int)json_size) { free(json); return -1; }
            }
            if (config->max_age > 0) {
                offset += snprintf(json + offset, json_size - offset, ",\"max_age\":%lld", config->max_age);
                if (offset >= (int)json_size) { free(json); return -1; }
            }
            if (config->max_msg_size >= 0) {
                offset += snprintf(json + offset, json_size - offset, ",\"max_msg_size\":%d", config->max_msg_size);
                if (offset >= (int)json_size) { free(json); return -1; }
            }
            if (config->storage != NULL) {
                offset += snprintf(json + offset, json_size - offset, ",\"storage\":\"%s\"", config->storage);
                if (offset >= (int)json_size) { free(json); return -1; }
            }
            if (config->replicas > 0) {
                offset += snprintf(json + offset, json_size - offset, ",\"num_replicas\":%d", config->replicas);
                if (offset >= (int)json_size) { free(json); return -1; }
            }
            if (config->discard_new) {
                offset += snprintf(json + offset, json_size - offset, ",\"discard\":\"new\"");
                if (offset >= (int)json_size) { free(json); return -1; }
            }

            snprintf(json + offset, json_size - offset, "}");

            // JetStream API uses stream name in subject path
            char api_subject[256];
            snprintf(api_subject, sizeof(api_subject), "$JS.API.STREAM.CREATE.%s", config->name);

            int sid = request_with_timeout(api_subject, json, response_cb, timeout_ms, on_timeout, 1);
            free(json);
            return sid;
        }

        // Delete a JetStream stream
        int jetstream_stream_delete(const char* stream_name, sub_cb response_cb,
                                   timeout_cb on_timeout = NULL, unsigned long timeout_ms = 5000) {
            if (stream_name == NULL) return -1;
            if (!connected) return -1;

            char api_subject[256];
            snprintf(api_subject, sizeof(api_subject), "$JS.API.STREAM.DELETE.%s", stream_name);

            return request_with_timeout(api_subject, "", response_cb, timeout_ms, on_timeout, 1);
        }

        // Get JetStream stream information
        int jetstream_stream_info(const char* stream_name, sub_cb response_cb,
                                 timeout_cb on_timeout = NULL, unsigned long timeout_ms = 5000) {
            if (stream_name == NULL) return -1;
            if (!connected) return -1;

            char api_subject[256];
            snprintf(api_subject, sizeof(api_subject), "$JS.API.STREAM.INFO.%s", stream_name);

            return request_with_timeout(api_subject, "", response_cb, timeout_ms, on_timeout, 1);
        }

        // Create a JetStream consumer
        int jetstream_consumer_create(const jetstream_consumer_config_t* config, sub_cb response_cb,
                                     timeout_cb on_timeout = NULL, unsigned long timeout_ms = 5000) {
            if (config == NULL || config->stream_name == NULL) return -1;
            if (!connected) return -1;

            // Build JSON configuration
            // JetStream API requires config wrapped in a "config" object
            const size_t json_size = 1024;
            char* json = (char*)malloc(json_size);
            if (json == NULL) return -1;

            // Start with stream_name and config wrapper
            int offset = snprintf(json, json_size, "{\"stream_name\":\"%s\",\"config\":{", config->stream_name);
            if (offset >= (int)json_size) { free(json); return -1; }

            if (config->durable_name != NULL) {
                offset += snprintf(json + offset, json_size - offset, "\"durable_name\":\"%s\",", config->durable_name);
                if (offset >= (int)json_size) { free(json); return -1; }
            }
            if (config->filter_subject != NULL) {
                offset += snprintf(json + offset, json_size - offset, "\"filter_subject\":\"%s\",", config->filter_subject);
                if (offset >= (int)json_size) { free(json); return -1; }
            }
            if (config->deliver_all) {
                offset += snprintf(json + offset, json_size - offset, "\"deliver_policy\":\"all\",");
                if (offset >= (int)json_size) { free(json); return -1; }
            } else {
                offset += snprintf(json + offset, json_size - offset, "\"deliver_policy\":\"new\",");
                if (offset >= (int)json_size) { free(json); return -1; }
            }
            if (config->ack_policy != NULL) {
                offset += snprintf(json + offset, json_size - offset, "\"ack_policy\":\"%s\",", config->ack_policy);
                if (offset >= (int)json_size) { free(json); return -1; }
            }
            if (config->ack_wait > 0) {
                offset += snprintf(json + offset, json_size - offset, "\"ack_wait\":%lld,", config->ack_wait);
                if (offset >= (int)json_size) { free(json); return -1; }
            }
            if (config->max_deliver > 0) {
                offset += snprintf(json + offset, json_size - offset, "\"max_deliver\":%d,", config->max_deliver);
                if (offset >= (int)json_size) { free(json); return -1; }
            }
            if (config->replay_policy != NULL) {
                offset += snprintf(json + offset, json_size - offset, "\"replay_policy\":\"%s\",", config->replay_policy);
                if (offset >= (int)json_size) { free(json); return -1; }
            }

            // Remove trailing comma if present and close both objects
            if (json[offset - 1] == ',') offset--;
            offset += snprintf(json + offset, json_size - offset, "}}");
            if (offset >= (int)json_size) { free(json); return -1; }

            // Use correct API endpoint based on consumer type (NATS 2.10+ format):
            // - Named (durable): $JS.API.CONSUMER.CREATE.<stream>.<consumer_name>
            // - Ephemeral: $JS.API.CONSUMER.CREATE.<stream>
            char api_subject[512];
            if (config->durable_name != NULL) {
                snprintf(api_subject, sizeof(api_subject), "$JS.API.CONSUMER.CREATE.%s.%s",
                        config->stream_name, config->durable_name);
            } else {
                snprintf(api_subject, sizeof(api_subject), "$JS.API.CONSUMER.CREATE.%s", config->stream_name);
            }

            int sid = request_with_timeout(api_subject, json, response_cb, timeout_ms, on_timeout, 1);
            free(json);
            return sid;
        }

        // Delete a JetStream consumer
        int jetstream_consumer_delete(const char* stream_name, const char* consumer_name,
                                     sub_cb response_cb, timeout_cb on_timeout = NULL,
                                     unsigned long timeout_ms = 5000) {
            if (stream_name == NULL || consumer_name == NULL) return -1;
            if (!connected) return -1;

            char api_subject[256];
            snprintf(api_subject, sizeof(api_subject), "$JS.API.CONSUMER.DELETE.%s.%s",
                    stream_name, consumer_name);

            return request_with_timeout(api_subject, "", response_cb, timeout_ms, on_timeout, 1);
        }

        // Pull messages from a JetStream consumer
        int jetstream_pull(const char* stream_name, const char* consumer_name, int batch_size,
                          sub_cb message_cb, timeout_cb on_timeout = NULL,
                          unsigned long timeout_ms = 5000) {
            if (stream_name == NULL || consumer_name == NULL || batch_size <= 0) return -1;
            if (!connected) return -1;

            // Build pull request
            char json[128];
            snprintf(json, sizeof(json), "{\"batch\":%d,\"no_wait\":true}", batch_size);

            char api_subject[256];
            snprintf(api_subject, sizeof(api_subject), "$JS.API.CONSUMER.MSG.NEXT.%s.%s",
                    stream_name, consumer_name);

            return request_with_timeout(api_subject, json, message_cb, timeout_ms, on_timeout, batch_size);
        }

        // Acknowledge a JetStream message
        void jetstream_ack(const char* reply_subject) {
            if (reply_subject == NULL || reply_subject[0] == 0) return;
            if (!connected) return;
            publish(reply_subject, "+ACK");
        }

        // Negative acknowledge (redeliver message)
        void jetstream_nak(const char* reply_subject) {
            if (reply_subject == NULL || reply_subject[0] == 0) return;
            if (!connected) return;
            publish(reply_subject, "-NAK");
        }

        // Acknowledge with delay (redeliver after delay)
        void jetstream_ack_delay(const char* reply_subject, unsigned long delay_ms) {
            if (reply_subject == NULL || reply_subject[0] == 0) return;
            if (!connected) return;

            // Convert milliseconds to nanoseconds
            int64_t delay_ns = delay_ms * 1000000LL;
            char msg[64];
            snprintf(msg, sizeof(msg), "+NAK {\"delay\":%lld}", delay_ns);
            publish(reply_subject, msg);
        }

        // ==================== JetStream Fetch Operations ====================

        // Fetch messages with advanced options
        int jetstream_fetch(const char* stream_name, const char* consumer_name,
                           const jetstream_fetch_request_t* fetch_config,
                           sub_cb message_cb, timeout_cb on_timeout = NULL,
                           unsigned long timeout_ms = 5000) {
            if (stream_name == NULL || consumer_name == NULL || fetch_config == NULL) return -1;
            if (fetch_config->batch <= 0) return -1;
            if (!connected) return -1;

            // Build fetch request JSON
            char json[256];
            int offset = snprintf(json, sizeof(json), "{\"batch\":%d", fetch_config->batch);

            if (fetch_config->max_bytes > 0) {
                offset += snprintf(json + offset, sizeof(json) - offset, ",\"max_bytes\":%zu", fetch_config->max_bytes);
            }
            if (fetch_config->expires > 0) {
                offset += snprintf(json + offset, sizeof(json) - offset, ",\"expires\":%lld", fetch_config->expires);
            }
            if (fetch_config->heartbeat > 0) {
                offset += snprintf(json + offset, sizeof(json) - offset, ",\"idle_heartbeat\":%lld", fetch_config->heartbeat);
            }
            if (fetch_config->no_wait) {
                offset += snprintf(json + offset, sizeof(json) - offset, ",\"no_wait\":true");
            }

            snprintf(json + offset, sizeof(json) - offset, "}");

            char api_subject[256];
            snprintf(api_subject, sizeof(api_subject), "$JS.API.CONSUMER.MSG.NEXT.%s.%s",
                    stream_name, consumer_name);

            return request_with_timeout(api_subject, json, message_cb, timeout_ms, on_timeout, fetch_config->batch);
        }

        // ==================== JetStream Account Info ====================

        // Get JetStream account information
        int jetstream_account_info(sub_cb response_cb, timeout_cb on_timeout = NULL,
                                   unsigned long timeout_ms = 5000) {
            if (!connected) return -1;
            return request_with_timeout("$JS.API.INFO", "", response_cb, timeout_ms, on_timeout, 1);
        }

        // ==================== JetStream Ordered Consumers ====================

        // Create an ordered consumer (simplified API for guaranteed in-order delivery)
        int jetstream_consumer_create_ordered(const char* stream_name, const char* filter_subject,
                                              sub_cb response_cb, timeout_cb on_timeout = NULL,
                                              unsigned long timeout_ms = 5000) {
            if (stream_name == NULL) return -1;
            if (!connected) return -1;

            // Build JSON configuration for ordered consumer
            // JetStream API requires config wrapped in a "config" object
            const size_t json_size = 512;
            char* json = (char*)malloc(json_size);
            if (json == NULL) return -1;

            int offset = snprintf(json, json_size,
                "{\"stream_name\":\"%s\",\"config\":{"
                "\"deliver_policy\":\"all\","
                "\"ack_policy\":\"none\","
                "\"flow_control\":true,"
                "\"idle_heartbeat\":5000000000",
                stream_name);
            if (offset >= (int)json_size) { free(json); return -1; }

            if (filter_subject != NULL) {
                offset += snprintf(json + offset, json_size - offset, ",\"filter_subject\":\"%s\"", filter_subject);
                if (offset >= (int)json_size) { free(json); return -1; }
            }

            // Close config object and outer object
            offset += snprintf(json + offset, json_size - offset, "}}");
            if (offset >= (int)json_size) { free(json); return -1; }

            char api_subject[256];
            snprintf(api_subject, sizeof(api_subject), "$JS.API.CONSUMER.CREATE.%s", stream_name);

            int sid = request_with_timeout(api_subject, json, response_cb, timeout_ms, on_timeout, 1);
            free(json);
            return sid;
        }

        // ==================== Key-Value Store ====================

        // Create a Key-Value bucket
        int kv_create_bucket(const kv_config_t* config, sub_cb response_cb,
                            timeout_cb on_timeout = NULL, unsigned long timeout_ms = 5000) {
            if (config == NULL || config->bucket == NULL) return -1;
            if (!connected) return -1;

            // KV bucket is implemented as a JetStream stream with specific configuration
            const size_t json_size = 2048;
            char* json = (char*)malloc(json_size);
            if (json == NULL) return -1;

            // Stream name is "KV_<bucket>" - DON'T close the object yet with ]}
            int offset = snprintf(json, json_size, "{\"name\":\"KV_%s\",\"subjects\":[\"$KV.%s.>\"]",
                                config->bucket, config->bucket);
            if (offset >= (int)json_size) { free(json); return -1; }

            // KV-specific stream configuration
            offset += snprintf(json + offset, json_size - offset, ",\"discard\":\"new\",\"allow_rollup_hdrs\":true");
            if (offset >= (int)json_size) { free(json); return -1; }
            offset += snprintf(json + offset, json_size - offset, ",\"deny_delete\":true,\"deny_purge\":false");
            if (offset >= (int)json_size) { free(json); return -1; }

            // History (max_msgs_per_subject)
            int64_t history = (config->history > 0 && config->history <= 64) ? config->history : 1;
            offset += snprintf(json + offset, json_size - offset, ",\"max_msgs_per_subject\":%lld", history);
            if (offset >= (int)json_size) { free(json); return -1; }

            // TTL (max_age)
            if (config->ttl > 0) {
                offset += snprintf(json + offset, json_size - offset, ",\"max_age\":%lld", config->ttl);
                if (offset >= (int)json_size) { free(json); return -1; }
            }

            // Max value size (max_msg_size)
            if (config->max_value_size > 0) {
                offset += snprintf(json + offset, json_size - offset, ",\"max_msg_size\":%zu", config->max_value_size);
                if (offset >= (int)json_size) { free(json); return -1; }
            }

            // Storage type
            if (config->storage != NULL) {
                offset += snprintf(json + offset, json_size - offset, ",\"storage\":\"%s\"", config->storage);
                if (offset >= (int)json_size) { free(json); return -1; }
            }

            // Replicas
            if (config->replicas > 0) {
                offset += snprintf(json + offset, json_size - offset, ",\"num_replicas\":%d", config->replicas);
                if (offset >= (int)json_size) { free(json); return -1; }
            }

            // Max bucket bytes
            if (config->max_bytes > 0) {
                offset += snprintf(json + offset, json_size - offset, ",\"max_bytes\":%zu", config->max_bytes);
                if (offset >= (int)json_size) { free(json); return -1; }
            }

            // Close the JSON object
            offset += snprintf(json + offset, json_size - offset, "}");
            if (offset >= (int)json_size) { free(json); return -1; }

            // Use correct API endpoint with stream name in subject
            char api_subject[256];
            snprintf(api_subject, sizeof(api_subject), "$JS.API.STREAM.CREATE.KV_%s", config->bucket);

            int sid = request_with_timeout(api_subject, json, response_cb, timeout_ms, on_timeout, 1);
            free(json);
            return sid;
        }

        // Get value for a key from KV bucket
        int kv_get(const char* bucket, const char* key, sub_cb response_cb,
                  timeout_cb on_timeout = NULL, unsigned long timeout_ms = 5000) {
            if (bucket == NULL || key == NULL) return -1;
            if (!connected) return -1;

            // Request last message for subject $KV.<bucket>.<key>
            const size_t json_size = 512;
            char* json = (char*)malloc(json_size);
            if (json == NULL) return -1;

            int offset = snprintf(json, json_size, "{\"last_by_subj\":\"$KV.%s.%s\"}", bucket, key);
            if (offset >= (int)json_size) { free(json); return -1; }

            char stream_name[512];
            snprintf(stream_name, sizeof(stream_name), "KV_%s", bucket);

            char api_subject[512];
            int ret = snprintf(api_subject, sizeof(api_subject), "$JS.API.STREAM.MSG.GET.%s", stream_name);
            if (ret >= (int)sizeof(api_subject)) { free(json); return -1; }

            int sid = request_with_timeout(api_subject, json, response_cb, timeout_ms, on_timeout, 1);
            free(json);
            return sid;
        }

        // Put a key-value pair into KV bucket
        int kv_put(const char* bucket, const char* key, const char* value,
                  sub_cb ack_cb, timeout_cb on_timeout = NULL, unsigned long timeout_ms = 5000) {
            if (bucket == NULL || key == NULL) return -1;
            if (!connected) return -1;

            // Publish to subject $KV.<bucket>.<key>
            char subject[256];
            snprintf(subject, sizeof(subject), "$KV.%s.%s", bucket, key);

            return jetstream_publish(subject, value, ack_cb, on_timeout, timeout_ms, NULL);
        }

        // Delete a key from KV bucket (soft delete with tombstone)
        int kv_delete(const char* bucket, const char* key, sub_cb ack_cb,
                     timeout_cb on_timeout = NULL, unsigned long timeout_ms = 5000) {
            if (bucket == NULL || key == NULL) return -1;
            if (!connected) return -1;

            // Publish with special headers indicating delete
            char subject[256];
            snprintf(subject, sizeof(subject), "$KV.%s.%s", bucket, key);

            const char* headers = "NATS/1.0\r\nKV-Operation: DEL\r\n\r\n";

            char* inbox = generate_inbox_subject();
            if (inbox == NULL) return -1;

            xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
            Sub* sub = new Sub(ack_cb, 1, timeout_ms, on_timeout);
            int sid;
            if (free_sids.empty()) {
                sid = subs.push_back(sub);
            } else {
                sid = free_sids.pop();
                subs[sid] = sub;
            }
            send_fmt("SUB %s %d", inbox, sid);
            xSemaphoreGiveRecursive(mutex);

            publish_with_headers(subject, headers, "", inbox);
            free(inbox);
            return sid;
        }

        // Purge all revisions of a key from KV bucket
        int kv_purge(const char* bucket, const char* key, sub_cb ack_cb,
                    timeout_cb on_timeout = NULL, unsigned long timeout_ms = 5000) {
            if (bucket == NULL || key == NULL) return -1;
            if (!connected) return -1;

            // Publish with special headers indicating purge
            char subject[256];
            snprintf(subject, sizeof(subject), "$KV.%s.%s", bucket, key);

            const char* headers = "NATS/1.0\r\nKV-Operation: PURGE\r\nNats-Rollup: sub\r\n\r\n";

            char* inbox = generate_inbox_subject();
            if (inbox == NULL) return -1;

            xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
            Sub* sub = new Sub(ack_cb, 1, timeout_ms, on_timeout);
            int sid;
            if (free_sids.empty()) {
                sid = subs.push_back(sub);
            } else {
                sid = free_sids.pop();
                subs[sid] = sub;
            }
            send_fmt("SUB %s %d", inbox, sid);
            xSemaphoreGiveRecursive(mutex);

            publish_with_headers(subject, headers, "", inbox);
            free(inbox);
            return sid;
        }

        // List all keys in KV bucket
        int kv_keys(const char* bucket, sub_cb response_cb,
                   timeout_cb on_timeout = NULL, unsigned long timeout_ms = 5000) {
            if (bucket == NULL) return -1;
            if (!connected) return -1;

            // Get stream info to list keys
            char stream_name[256];
            snprintf(stream_name, sizeof(stream_name), "KV_%s", bucket);

            return jetstream_stream_info(stream_name, response_cb, on_timeout, timeout_ms);
        }

        // Watch a key or key pattern for changes
        int kv_watch(const char* bucket, const char* key_pattern, sub_cb message_cb,
                    const kv_watch_options_t* options = NULL) {
            if (bucket == NULL || key_pattern == NULL) return -1;
            if (!connected) return -1;

            // Subscribe to $KV.<bucket>.<key_pattern>
            char subject[256];
            snprintf(subject, sizeof(subject), "$KV.%s.%s", bucket, key_pattern);

            return subscribe(subject, message_cb, NULL, 0);
        }

        // Get history of a key (all revisions)
        int kv_history(const char* bucket, const char* key, sub_cb response_cb,
                      timeout_cb on_timeout = NULL, unsigned long timeout_ms = 5000) {
            if (bucket == NULL || key == NULL) return -1;
            if (!connected) return -1;

            // Create a consumer to get all messages for this key
            char stream_name[256];
            snprintf(stream_name, sizeof(stream_name), "KV_%s", bucket);

            char filter_subject[256];
            snprintf(filter_subject, sizeof(filter_subject), "$KV.%s.%s", bucket, key);

            jetstream_consumer_config_t consumer_config = {
                .stream_name = stream_name,
                .durable_name = NULL,  // Ephemeral
                .filter_subject = filter_subject,
                .deliver_all = true,
                .ack_policy = "none",
                .ack_wait = 0,
                .max_deliver = -1,
                .replay_policy = "instant",
                .ordered = false
            };

            return jetstream_consumer_create(&consumer_config, response_cb, on_timeout, timeout_ms);
        }

        // ========================================================================
        // Object Store Methods
        // ========================================================================

        // Create object store bucket
        int obj_create_bucket(const object_store_config_t* config, sub_cb response_cb,
                             timeout_cb on_timeout = NULL, unsigned long timeout_ms = 5000) {
            if (config == NULL || config->bucket == NULL) return -1;
            if (!connected) return -1;

            // Object store is a JetStream stream named OBJ_<bucket>
            char stream_name[256];
            snprintf(stream_name, sizeof(stream_name), "OBJ_%s", config->bucket);

            // Build stream configuration JSON
            char payload[1024];
            int len = snprintf(payload, sizeof(payload),
                "{\"name\":\"%s\","
                "\"subjects\":[\"$O.%s.C.>\",\"$O.%s.M.>\"],"
                "\"storage\":\"%s\","
                "\"num_replicas\":%d,"
                "\"discard\":\"new\"",
                stream_name,
                config->bucket,
                config->bucket,
                config->storage ? config->storage : "file",
                config->replicas > 0 ? config->replicas : 1
            );

            if (config->description != NULL) {
                len += snprintf(payload + len, sizeof(payload) - len,
                    ",\"description\":\"%s\"", config->description);
            }

            if (config->max_bytes > 0) {
                len += snprintf(payload + len, sizeof(payload) - len,
                    ",\"max_bytes\":%zu", config->max_bytes);
            }

            if (config->ttl > 0) {
                len += snprintf(payload + len, sizeof(payload) - len,
                    ",\"max_age\":%lld", (long long)config->ttl);
            }

            snprintf(payload + len, sizeof(payload) - len, "}");

            // Use correct API endpoint with stream name in subject
            char api_subject[512];
            snprintf(api_subject, sizeof(api_subject), "$JS.API.STREAM.CREATE.%s", stream_name);

            return request_with_timeout(api_subject, payload, response_cb, timeout_ms, on_timeout);
        }

        // Put object in object store (complete upload in one call)
        int obj_put(const char* bucket, const char* name, const uint8_t* data, size_t data_len,
                   const char* description, sub_cb ack_cb,
                   timeout_cb on_timeout = NULL, unsigned long timeout_ms = 5000) {
            if (bucket == NULL || name == NULL || data == NULL || data_len == 0) return -1;
            if (!connected) return -1;

            const size_t CHUNK_SIZE = 131072; // 128KB chunks

            // Prevent integer overflow in chunk calculations
            if (data_len > SIZE_MAX - CHUNK_SIZE + 1) return -1;
            uint64_t num_chunks = (data_len + CHUNK_SIZE - 1) / CHUNK_SIZE;

            // Generate NUID for this object
            char nuid[23];
            generate_nuid(nuid, sizeof(nuid));

            // Publish chunks using simple publish (fire-and-forget)
            // The metadata ACK at the end confirms everything was stored
            for (uint64_t i = 0; i < num_chunks; i++) {
                size_t chunk_offset = i * CHUNK_SIZE;
                size_t chunk_len = (chunk_offset + CHUNK_SIZE > data_len) ?
                                   (data_len - chunk_offset) : CHUNK_SIZE;

                char chunk_subject[256];
                snprintf(chunk_subject, sizeof(chunk_subject),
                         "$O.%s.C.%s", bucket, nuid);

                // Publish chunk directly using binary-safe publish (no strlen issues with null bytes)
                publish_binary(chunk_subject, data + chunk_offset, chunk_len);
            }

            // Publish metadata
            char meta_subject[256];
            snprintf(meta_subject, sizeof(meta_subject), "$O.%s.M.%s", bucket, name);

            char meta_payload[512];
            int len = snprintf(meta_payload, sizeof(meta_payload),
                "{\"name\":\"%s\","
                "\"size\":%zu,"
                "\"chunks\":%llu,"
                "\"nuid\":\"%s\"",
                name,
                data_len,
                (unsigned long long)num_chunks,
                nuid
            );

            if (description != NULL) {
                len += snprintf(meta_payload + len, sizeof(meta_payload) - len,
                    ",\"description\":\"%s\"", description);
            }

            // Add timestamp
            len += snprintf(meta_payload + len, sizeof(meta_payload) - len,
                ",\"mtime\":%llu}", (unsigned long long)(NATSUtil::millis() * 1000000ULL));

            return jetstream_publish(meta_subject, meta_payload, ack_cb, on_timeout, timeout_ms, NULL);
        }

        // Get object metadata/info
        int obj_get_info(const char* bucket, const char* name, sub_cb response_cb,
                        timeout_cb on_timeout = NULL, unsigned long timeout_ms = 5000) {
            if (bucket == NULL || name == NULL) return -1;
            if (!connected) return -1;

            // Get the metadata message
            char stream_name[256];
            snprintf(stream_name, sizeof(stream_name), "OBJ_%s", bucket);

            char meta_subject[512];
            int ret = snprintf(meta_subject, sizeof(meta_subject), "$O.%s.M.%s", bucket, name);
            if (ret >= (int)sizeof(meta_subject)) return -1;

            // Use direct get for low latency
            char payload[512];
            ret = snprintf(payload, sizeof(payload),
                "{\"last_by_subj\":\"%s\"}", meta_subject);
            if (ret >= (int)sizeof(payload)) return -1;

            char api_subject[512];
            ret = snprintf(api_subject, sizeof(api_subject),
                     "$JS.API.STREAM.MSG.GET.%s", stream_name);
            if (ret >= (int)sizeof(api_subject)) return -1;

            return request_with_timeout(api_subject, payload, response_cb, timeout_ms, on_timeout);
        }

        // Get object data (download)
        int obj_get(const char* bucket, const char* name, sub_cb chunk_cb,
                   timeout_cb on_timeout = NULL, unsigned long timeout_ms = 30000) {
            if (bucket == NULL || name == NULL) return -1;
            if (!connected) return -1;

            // First get metadata to find NUID and chunk count
            // In a real implementation, we'd parse the metadata response
            // and then fetch each chunk. For this simplified version,
            // we subscribe to the chunk pattern and let the callback handle assembly

            char chunk_subject[256];
            snprintf(chunk_subject, sizeof(chunk_subject), "$O.%s.C.>", bucket);

            // Subscribe to get chunks (caller must handle chunk assembly)
            return subscribe(chunk_subject, chunk_cb, NULL, 0);
        }

        // List objects in bucket
        int obj_list(const char* bucket, sub_cb response_cb,
                    timeout_cb on_timeout = NULL, unsigned long timeout_ms = 5000) {
            if (bucket == NULL) return -1;
            if (!connected) return -1;

            char stream_name[256];
            snprintf(stream_name, sizeof(stream_name), "OBJ_%s", bucket);

            // Get stream info which includes metadata subjects
            return jetstream_stream_info(stream_name, response_cb, on_timeout, timeout_ms);
        }

        // Delete object from bucket
        int obj_delete(const char* bucket, const char* name, sub_cb ack_cb,
                      timeout_cb on_timeout = NULL, unsigned long timeout_ms = 5000) {
            if (bucket == NULL || name == NULL) return -1;
            if (!connected) return -1;

            // Delete metadata message (chunks will be purged by retention policy)
            char meta_subject[256];
            snprintf(meta_subject, sizeof(meta_subject), "$O.%s.M.%s", bucket, name);

            // Publish delete marker with headers
            const char* headers = "NATS/1.0\r\nNats-Rollup: sub\r\n\r\n";

            char* inbox = generate_inbox_subject();
            if (inbox == NULL) return -1;

            xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
            Sub* sub = new Sub(ack_cb, 1, timeout_ms, on_timeout);
            int sid;
            if (free_sids.empty()) {
                sid = subs.push_back(sub);
            } else {
                sid = free_sids.pop();
                subs[sid] = sub;
            }
            send_fmt("SUB %s %d", inbox, sid);
            xSemaphoreGiveRecursive(mutex);

            publish_with_headers(meta_subject, headers, "", inbox);
            free(inbox);
            return sid;
        }

        // Watch for object changes in bucket
        int obj_watch(const char* bucket, sub_cb message_cb) {
            if (bucket == NULL) return -1;
            if (!connected) return -1;

            // Subscribe to all metadata messages
            char subject[256];
            snprintf(subject, sizeof(subject), "$O.%s.M.>", bucket);

            return subscribe(subject, message_cb, NULL, 0);
        }

        // ========================================================================
        // Direct Get - Low-latency JetStream message retrieval
        // ========================================================================

        // Direct get message from stream (bypasses stream leader)
        int jetstream_direct_get(const char* stream_name, uint64_t seq, sub_cb response_cb,
                                timeout_cb on_timeout = NULL, unsigned long timeout_ms = 5000) {
            if (stream_name == NULL) return -1;
            if (!connected) return -1;

            char subject[256];
            snprintf(subject, sizeof(subject), "$JS.API.DIRECT.GET.%s", stream_name);

            char payload[128];
            snprintf(payload, sizeof(payload), "{\"seq\":%llu}", (unsigned long long)seq);

            return request_with_timeout(subject, payload, response_cb, timeout_ms, on_timeout);
        }

        // Direct get last message for subject
        int jetstream_direct_get_last(const char* stream_name, const char* subject, sub_cb response_cb,
                                      timeout_cb on_timeout = NULL, unsigned long timeout_ms = 5000) {
            if (stream_name == NULL || subject == NULL) return -1;
            if (!connected) return -1;

            char api_subject[256];
            snprintf(api_subject, sizeof(api_subject), "$JS.API.DIRECT.GET.%s", stream_name);

            char payload[256];
            snprintf(payload, sizeof(payload), "{\"last_by_subj\":\"%s\"}", subject);

            return request_with_timeout(api_subject, payload, response_cb, timeout_ms, on_timeout);
        }

        // ========================================================================
        // Consumer Pause - Flow control
        // ========================================================================

        // Pause consumer until specified time
        int jetstream_consumer_pause(const char* stream_name, const char* consumer_name,
                                    uint64_t pause_until_ns, sub_cb response_cb,
                                    timeout_cb on_timeout = NULL, unsigned long timeout_ms = 5000) {
            if (stream_name == NULL || consumer_name == NULL) return -1;
            if (!connected) return -1;

            char subject[256];
            snprintf(subject, sizeof(subject),
                     "$JS.API.CONSUMER.PAUSE.%s.%s", stream_name, consumer_name);

            char payload[128];
            snprintf(payload, sizeof(payload),
                     "{\"pause_until\":%llu}", (unsigned long long)pause_until_ns);

            return request_with_timeout(subject, payload, response_cb, timeout_ms, on_timeout);
        }

        // Resume paused consumer (convenience method)
        int jetstream_consumer_resume(const char* stream_name, const char* consumer_name,
                                     sub_cb response_cb,
                                     timeout_cb on_timeout = NULL, unsigned long timeout_ms = 5000) {
            // Resume by setting pause_until to 0
            return jetstream_consumer_pause(stream_name, consumer_name, 0,
                                           response_cb, on_timeout, timeout_ms);
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
                case NATS_ERR_INVALID_CONFIG: return "Invalid configuration";
                case NATS_ERR_OUT_OF_MEMORY: return "Out of memory";
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
            xSemaphoreTakeRecursive(state_mutex, portMAX_DELAY);
            int initial_pongs = outstanding_pings;
            outstanding_pings++;
            xSemaphoreGiveRecursive(state_mutex);
            send(NATS_CTRL_PING);

            unsigned long start_time = NATSUtil::millis();
            while ((NATSUtil::millis() - start_time) < timeout_ms) {
                // Handle transport-based connections (WebSocket)
                if (transport != NULL) {
                    if (transport->has_data_available(10)) {
                        recv();
                    }
                    // Yield to prevent watchdog
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                // Handle legacy sockfd-based connections (TCP/TLS)
                else if (sockfd >= 0) {
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
                    }
                }
                else {
                    // No valid connection method, yield and continue
                    vTaskDelay(pdMS_TO_TICKS(10));
                }

                // Check if we got the PONG
                xSemaphoreTakeRecursive(state_mutex, portMAX_DELAY);
                bool pong_received = (outstanding_pings <= initial_pongs);
                xSemaphoreGiveRecursive(state_mutex);
                if (pong_received) {
                    return true;
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
            xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
            for (size_t i = 0; i < subs.size(); i++) {
                if (subs[i] != NULL) {
                    send_fmt("UNSUB %d", i);
                    delete subs[i];  // Fix: Use delete for objects created with new
                    subs[i] = NULL;
                    free_sids.push(i);
                }
            }
            xSemaphoreGiveRecursive(mutex);

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
            bool has_connection = (transport != NULL && transport->is_connected()) || sockfd >= 0;
            if (connect_state == CONNECTING && async_connect_cb != NULL && !has_connection) {
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

            // Transport-based processing
            if (transport != NULL && transport->is_connected()) {
                // Check for data using transport
                if (transport->has_data_available(0)) {
                    recv();
                }

                // Check for request timeouts
                NATSUtil::Array<int> timed_out_sids;
                NATSUtil::Array<timeout_cb> timeout_callbacks;

                xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
                for (size_t i = 0; i < subs.size(); i++) {
                    if (subs[i] != NULL && subs[i]->timed_out()) {
                        timed_out_sids.push_back(i);
                        timeout_callbacks.push_back(subs[i]->on_timeout);
                    }
                }
                xSemaphoreGiveRecursive(mutex);

                for (size_t i = 0; i < timed_out_sids.size(); i++) {
                    if (timeout_callbacks[i] != NULL) {
                        timeout_callbacks[i]();
                    }
                    unsubscribe(timed_out_sids[i]);
                }

                if (ping_timer.process())
                    ping();
                return;
            }

            // Legacy socket-based processing
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

                xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
                for (size_t i = 0; i < subs.size(); i++) {
                    if (subs[i] != NULL && subs[i]->timed_out()) {
                        timed_out_sids.push_back(i);
                        timeout_callbacks.push_back(subs[i]->on_timeout);
                    }
                }
                xSemaphoreGiveRecursive(mutex);

                // Now process timeouts without holding the mutex
                for (size_t i = 0; i < timed_out_sids.size(); i++) {
                    if (timeout_callbacks[i] != NULL) {
                        timeout_callbacks[i]();
                    }
                    unsubscribe(timed_out_sids[i]);
                }

                if (ping_timer.process())
                    ping();
            } else if (transport == NULL) {
                // Only handle reconnection in legacy mode (no transport)
                disconnect();
                // Protect reconnect_attempts read with mutex
                if (state_mutex != NULL) xSemaphoreTakeRecursive(state_mutex, portMAX_DELAY);
                int attempts = reconnect_attempts;
                if (state_mutex != NULL) xSemaphoreGiveRecursive(state_mutex);

                if (max_reconnect_attempts == -1 || attempts < max_reconnect_attempts) {
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
