/**
 * @file ws_transport.h
 * @brief WebSocket transport implementation for NATS client
 *
 * This file implements the NatsTransport interface using ESP-IDF's
 * esp_websocket_client component. It bridges the event-based WebSocket
 * API to the polling-based NATS client model using a ring buffer.
 */

#ifndef ESPIDF_NATS_WS_TRANSPORT_H
#define ESPIDF_NATS_WS_TRANSPORT_H

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

#include <string.h>
#include <stdlib.h>
#include <esp_log.h>
#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "transport.h"
#include "config.h"
#include "util.h"

/**
 * @brief WebSocket transport implementation
 *
 * Implements NatsTransport using esp_websocket_client. Uses a ring buffer
 * to bridge event-driven WebSocket callbacks to the polling-based NATS
 * client model.
 *
 * Architecture:
 * - WebSocket DATA events write to ring buffer (producer, WS task context)
 * - read_line()/read_bytes() read from ring buffer (consumer, main task)
 * - Thread-safe synchronization via FreeRTOS primitives
 */
class WebSocketTransport : public NatsTransport {
private:
    static constexpr const char* TAG = "ws_transport";

    esp_websocket_client_handle_t client;
    nats_transport_state_t state;
    nats_error_code_t last_error;

    // Event-to-polling bridge
    NATSUtil::RingBuffer* rx_buffer;
    SemaphoreHandle_t state_mutex;
    volatile bool ws_connected;

    // Connection signaling
    SemaphoreHandle_t connect_sem;
    bool connect_result;

    // TLS config storage
    nats_tls_config_t tls_config;

public:
    WebSocketTransport() :
        client(NULL),
        state(TRANSPORT_DISCONNECTED),
        last_error(NATS_ERR_NONE),
        rx_buffer(NULL),
        ws_connected(false),
        connect_result(false)
    {
        memset(&tls_config, 0, sizeof(nats_tls_config_t));
        state_mutex = xSemaphoreCreateMutex();
        connect_sem = xSemaphoreCreateBinary();

        if (state_mutex == NULL || connect_sem == NULL) {
            ESP_LOGE(TAG, "Failed to create semaphores");
            last_error = NATS_ERR_OUT_OF_MEMORY;
            if (state_mutex) { vSemaphoreDelete(state_mutex); state_mutex = NULL; }
            if (connect_sem) { vSemaphoreDelete(connect_sem); connect_sem = NULL; }
        }
    }

    virtual ~WebSocketTransport() {
        disconnect();

        if (state_mutex != NULL) {
            vSemaphoreDelete(state_mutex);
            state_mutex = NULL;
        }
        if (connect_sem != NULL) {
            vSemaphoreDelete(connect_sem);
            connect_sem = NULL;
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

        state = TRANSPORT_CONNECTING;

        // Build WebSocket URI
        char uri[512];
        const char* scheme = tls_config.enabled ? "wss" : "ws";
        const char* path = config->ws_path ? config->ws_path : NATS_WEBSOCKET_PATH;

        // Ensure path starts with /
        if (path[0] != '/') {
            snprintf(uri, sizeof(uri), "%s://%s:%d/%s", scheme, config->hostname, config->port, path);
        } else {
            snprintf(uri, sizeof(uri), "%s://%s:%d%s", scheme, config->hostname, config->port, path);
        }

        ESP_LOGI(TAG, "Connecting to WebSocket: %s", uri);

        // Create ring buffer
        rx_buffer = new NATSUtil::RingBuffer(NATS_WEBSOCKET_BUFFER_SIZE);
        if (rx_buffer == NULL) {
            ESP_LOGE(TAG, "Failed to create receive buffer");
            last_error = NATS_ERR_OUT_OF_MEMORY;
            state = TRANSPORT_ERROR;
            return false;
        }

        // Configure WebSocket client
        esp_websocket_client_config_t ws_cfg = {};
        ws_cfg.uri = uri;
        // Note: Don't set subprotocol - NATS server may not require it
        // ws_cfg.subprotocol = config->ws_subprotocol ? config->ws_subprotocol : NATS_WEBSOCKET_SUBPROTOCOL;
        ws_cfg.disable_auto_reconnect = true;  // We handle reconnection in NATS client
        ws_cfg.reconnect_timeout_ms = NATS_WEBSOCKET_RECONNECT_TIMEOUT;
        ws_cfg.buffer_size = 4096;  // Ensure buffer is large enough for NATS INFO
        ws_cfg.task_stack = 6144;   // Adequate stack for WebSocket task

        ESP_LOGI(TAG, "WebSocket config: uri=%s", uri);

        // TLS configuration
        if (tls_config.enabled) {
            ws_cfg.cert_pem = tls_config.ca_cert;
            ws_cfg.cert_len = tls_config.ca_cert_len;
            ws_cfg.client_cert = tls_config.client_cert;
            ws_cfg.client_cert_len = tls_config.client_cert_len;
            ws_cfg.client_key = tls_config.client_key;
            ws_cfg.client_key_len = tls_config.client_key_len;
            ws_cfg.skip_cert_common_name_check = tls_config.skip_cert_verification;
        }

        // Create WebSocket client
        client = esp_websocket_client_init(&ws_cfg);
        if (client == NULL) {
            ESP_LOGE(TAG, "Failed to initialize WebSocket client");
            delete rx_buffer;
            rx_buffer = NULL;
            last_error = NATS_ERR_WEBSOCKET_INIT_FAILED;
            state = TRANSPORT_ERROR;
            return false;
        }

        // Register event handler
        esp_err_t err = esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY,
                                                       ws_event_handler, this);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register event handler: %d", err);
            esp_websocket_client_destroy(client);
            client = NULL;
            delete rx_buffer;
            rx_buffer = NULL;
            last_error = NATS_ERR_WEBSOCKET_INIT_FAILED;
            state = TRANSPORT_ERROR;
            return false;
        }

        // Reset connection state
        ws_connected = false;
        connect_result = false;

        // Start WebSocket connection
        err = esp_websocket_client_start(client);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "WebSocket start failed: %d", err);
            esp_websocket_client_destroy(client);
            client = NULL;
            delete rx_buffer;
            rx_buffer = NULL;
            last_error = NATS_ERR_CONNECTION_FAILED;
            state = TRANSPORT_ERROR;
            return false;
        }

        // Wait for connection (with timeout)
        if (xSemaphoreTake(connect_sem, pdMS_TO_TICKS(30000)) != pdTRUE) {
            ESP_LOGE(TAG, "WebSocket connection timeout");
            esp_websocket_client_stop(client);
            esp_websocket_client_destroy(client);
            client = NULL;
            delete rx_buffer;
            rx_buffer = NULL;
            last_error = NATS_ERR_CONNECTION_FAILED;
            state = TRANSPORT_ERROR;
            return false;
        }

        if (!connect_result || !ws_connected) {
            ESP_LOGE(TAG, "WebSocket connection failed");
            esp_websocket_client_stop(client);
            esp_websocket_client_destroy(client);
            client = NULL;
            delete rx_buffer;
            rx_buffer = NULL;
            last_error = NATS_ERR_WEBSOCKET_HANDSHAKE_FAILED;
            state = TRANSPORT_ERROR;
            return false;
        }

        state = TRANSPORT_CONNECTED;
        last_error = NATS_ERR_NONE;
        ESP_LOGI(TAG, "WebSocket connected successfully");
        return true;
    }

    void disconnect() override {
        if (state_mutex != NULL) xSemaphoreTake(state_mutex, portMAX_DELAY);

        ws_connected = false;

        if (client != NULL) {
            esp_websocket_client_stop(client);
            esp_websocket_client_destroy(client);
            client = NULL;
        }

        if (rx_buffer != NULL) {
            rx_buffer->clear();
            delete rx_buffer;
            rx_buffer = NULL;
        }

        if (state_mutex != NULL) xSemaphoreGive(state_mutex);

        state = TRANSPORT_DISCONNECTED;
    }

    nats_transport_state_t get_state() const override {
        return state;
    }

    bool is_connected() const override {
        return state == TRANSPORT_CONNECTED && ws_connected && client != NULL;
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

        // Send as text frame (NATS protocol is text-based)
        int ret = esp_websocket_client_send_text(client, data, len, portMAX_DELAY);
        if (ret < 0) {
            ESP_LOGE(TAG, "WebSocket send failed: %d", ret);
            last_error = NATS_ERR_WEBSOCKET_SEND_FAILED;
            return -1;
        }

        return ret;
    }

    int send_line(const char* line) override {
        if (line == NULL) return 0;

        size_t len = strlen(line);
        size_t total_len = len + 2;  // +2 for CRLF

        // Build message with CRLF
        char* msg = (char*)malloc(total_len + 1);
        if (msg == NULL) {
            ESP_LOGE(TAG, "Failed to allocate send buffer");
            last_error = NATS_ERR_OUT_OF_MEMORY;
            return -1;
        }

        memcpy(msg, line, len);
        memcpy(msg + len, NATS_CR_LF, 2);
        msg[total_len] = '\0';

        int ret = send_data(msg, total_len);
        free(msg);

        if (ret < 0) return ret;
        return (int)total_len;
    }

    // ========================================================================
    // Data Reception
    // ========================================================================

    char* read_line(size_t initial_cap) override {
        if (!is_connected() || rx_buffer == NULL) {
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
        uint8_t c;

        while (true) {
            // Wait for data if buffer is empty
            if (rx_buffer->available() == 0) {
                if (!rx_buffer->wait_for_data(5000)) {
                    // Timeout - check if still connected
                    if (!ws_connected) {
                        free(buf);
                        disconnect();
                        return (char*)calloc(1, sizeof(char));
                    }
                    continue;  // Keep waiting
                }
            }

            // Read one byte
            if (rx_buffer->read_byte(&c) == 0) {
                // Buffer empty after wait - disconnection?
                if (!ws_connected) {
                    free(buf);
                    disconnect();
                    return (char*)calloc(1, sizeof(char));
                }
                continue;
            }

            if (c == '\r') continue;
            if (c == '\n') break;

            if ((size_t)i >= cap - 1) {
                if (cap >= NATS_MAX_LINE_SIZE) {
                    ESP_LOGE(TAG, "Line too long (exceeds %d bytes)", NATS_MAX_LINE_SIZE);
                    free(buf);
                    disconnect();
                    return (char*)calloc(1, sizeof(char));
                }
                if (cap > SIZE_MAX / 2) {
                    ESP_LOGE(TAG, "Readline buffer too large");
                    free(buf);
                    disconnect();
                    return (char*)calloc(1, sizeof(char));
                }
                cap *= 2;
                char* newbuf = (char*)realloc(buf, cap + 1);
                if (newbuf == NULL) {
                    ESP_LOGE(TAG, "Failed to realloc readline buffer");
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

    char* read_bytes(size_t n) override {
        if (n == 0) {
            return (char*)calloc(1, sizeof(char));
        }

        if (!is_connected() || rx_buffer == NULL) {
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

        while (total_read < n) {
            // Wait for data if needed
            if (rx_buffer->available() == 0) {
                if (!rx_buffer->wait_for_data(5000)) {
                    if (!ws_connected) {
                        free(buf);
                        disconnect();
                        return NULL;
                    }
                    continue;
                }
            }

            size_t bytes_read = rx_buffer->read((uint8_t*)(buf + total_read), n - total_read);
            if (bytes_read == 0 && !ws_connected) {
                free(buf);
                disconnect();
                return NULL;
            }
            total_read += bytes_read;
        }

        buf[n] = '\0';
        return buf;
    }

    void skip_bytes(size_t n) override {
        if (n == 0 || !is_connected() || rx_buffer == NULL) return;

        uint8_t discard[16];
        size_t remaining = n;

        while (remaining > 0) {
            if (rx_buffer->available() == 0) {
                if (!rx_buffer->wait_for_data(5000)) {
                    if (!ws_connected) {
                        disconnect();
                        return;
                    }
                    continue;
                }
            }

            size_t to_read = (remaining < sizeof(discard)) ? remaining : sizeof(discard);
            size_t bytes_read = rx_buffer->read(discard, to_read);
            if (bytes_read == 0 && !ws_connected) {
                disconnect();
                return;
            }
            remaining -= bytes_read;
        }
    }

    // ========================================================================
    // Polling
    // ========================================================================

    bool has_data_available(int timeout_ms) override {
        if (!is_connected() || rx_buffer == NULL) return false;

        if (rx_buffer->available() > 0) return true;

        if (timeout_ms > 0) {
            return rx_buffer->wait_for_data(timeout_ms);
        }

        return false;
    }

    int get_fd() const override {
        // WebSocket client doesn't expose file descriptor
        return -1;
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
            case NATS_ERR_CONNECTION_FAILED: return "WebSocket connection failed";
            case NATS_ERR_NOT_CONNECTED: return "Not connected";
            case NATS_ERR_OUT_OF_MEMORY: return "Out of memory";
            case NATS_ERR_INVALID_ARG: return "Invalid argument";
            case NATS_ERR_WEBSOCKET_INIT_FAILED: return "WebSocket initialization failed";
            case NATS_ERR_WEBSOCKET_SEND_FAILED: return "WebSocket send failed";
            case NATS_ERR_WEBSOCKET_HANDSHAKE_FAILED: return "WebSocket handshake failed";
            default: return "Unknown error";
        }
    }

private:
    // ========================================================================
    // WebSocket Event Handler
    // ========================================================================

    static void ws_event_handler(void* handler_args, esp_event_base_t base,
                                  int32_t event_id, void* event_data) {
        WebSocketTransport* transport = (WebSocketTransport*)handler_args;
        esp_websocket_event_data_t* data = (esp_websocket_event_data_t*)event_data;

        // Use explicit enum comparison for compatibility across esp_websocket_client versions
        if (event_id == WEBSOCKET_EVENT_CONNECTED) {
            ESP_LOGD(TAG, "WebSocket connected event");
            transport->handle_connected();
        } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
            ESP_LOGD(TAG, "WebSocket disconnected event");
            transport->handle_disconnected();
        } else if (event_id == WEBSOCKET_EVENT_DATA) {
            ESP_LOGD(TAG, "WebSocket DATA event: len=%d", data ? data->data_len : -1);
            transport->handle_data(data);
        } else if (event_id == WEBSOCKET_EVENT_ERROR) {
            ESP_LOGE(TAG, "WebSocket error event");
            transport->handle_error();
        } else if (event_id == WEBSOCKET_EVENT_CLOSED) {
            ESP_LOGD(TAG, "WebSocket closed event");
            transport->handle_disconnected();
        }
    }

    void handle_connected() {
        if (state_mutex != NULL) xSemaphoreTake(state_mutex, portMAX_DELAY);
        ws_connected = true;
        connect_result = true;
        if (state_mutex != NULL) xSemaphoreGive(state_mutex);

        // Signal connection complete
        if (connect_sem != NULL) {
            xSemaphoreGive(connect_sem);
        }
    }

    void handle_disconnected() {
        if (state_mutex != NULL) xSemaphoreTake(state_mutex, portMAX_DELAY);
        ws_connected = false;
        state = TRANSPORT_DISCONNECTED;
        if (state_mutex != NULL) xSemaphoreGive(state_mutex);

        // Signal if waiting for connection
        if (connect_sem != NULL) {
            xSemaphoreGive(connect_sem);
        }
    }

    void handle_data(esp_websocket_event_data_t* data) {
        if (data == NULL || rx_buffer == NULL) return;

        // Handle all data frames (op_code 1 = text, 2 = binary)
        // NATS protocol uses text frames
        if (data->data_len > 0 && data->data_ptr != NULL) {
            ESP_LOGD(TAG, "WS received %d bytes", data->data_len);
            rx_buffer->write((const uint8_t*)data->data_ptr, data->data_len);
        }
    }

    void handle_error() {
        last_error = NATS_ERR_SOCKET_FAILED;
        state = TRANSPORT_ERROR;

        // Signal if waiting for connection
        if (connect_sem != NULL) {
            xSemaphoreGive(connect_sem);
        }
    }
};

#endif // CONFIG_ESP_WEBSOCKET_CLIENT_ENABLE

#endif // ESPIDF_NATS_WS_TRANSPORT_H
