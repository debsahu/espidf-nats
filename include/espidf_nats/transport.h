/**
 * @file transport.h
 * @brief Abstract transport interface for NATS client
 *
 * This file defines the NatsTransport abstract class that provides a common
 * interface for different transport implementations (TCP, WebSocket, etc.).
 */

#ifndef ESPIDF_NATS_TRANSPORT_H
#define ESPIDF_NATS_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>
#include "types.h"
#include "config.h"

/**
 * @brief Abstract transport interface for NATS connections
 *
 * All transport implementations must inherit from this class and implement
 * the pure virtual methods. This provides a clean abstraction that separates
 * the NATS protocol logic from the underlying transport mechanism.
 *
 * Implementations:
 * - TcpTransport: TCP/TLS transport using BSD sockets and esp-tls
 * - WebSocketTransport: WebSocket transport using esp_websocket_client
 */
class NatsTransport {
public:
    virtual ~NatsTransport() {}

    // ========================================================================
    // Connection Lifecycle
    // ========================================================================

    /**
     * @brief Establish a connection to the server
     *
     * @param config Transport configuration including hostname, port, TLS settings
     * @return true if connection established successfully
     * @return false if connection failed (check get_last_error())
     */
    virtual bool connect(const nats_transport_config_t* config) = 0;

    /**
     * @brief Close the connection and release resources
     */
    virtual void disconnect() = 0;

    /**
     * @brief Get current transport state
     *
     * @return nats_transport_state_t Current state (DISCONNECTED, CONNECTING, CONNECTED, ERROR)
     */
    virtual nats_transport_state_t get_state() const = 0;

    /**
     * @brief Check if transport is connected
     *
     * @return true if connected and ready for I/O
     * @return false if not connected
     */
    virtual bool is_connected() const = 0;

    // ========================================================================
    // Data Transmission
    // ========================================================================

    /**
     * @brief Send raw data without modification
     *
     * @param data Data to send
     * @param len Length of data in bytes
     * @return int Number of bytes sent, or -1 on error
     */
    virtual int send_data(const char* data, size_t len) = 0;

    /**
     * @brief Send a line of text, appending CRLF
     *
     * @param line Line to send (without CRLF)
     * @return int Total bytes sent including CRLF, or -1 on error
     */
    virtual int send_line(const char* line) = 0;

    // ========================================================================
    // Data Reception
    // ========================================================================

    /**
     * @brief Read a complete line terminated by LF
     *
     * Reads bytes until a newline character is encountered.
     * The returned string is null-terminated and does not include CR/LF.
     * Caller is responsible for freeing the returned buffer.
     *
     * @param initial_cap Initial buffer capacity (will grow if needed)
     * @return char* Malloc'd null-terminated string, or empty string on error
     */
    virtual char* read_line(size_t initial_cap = 128) = 0;

    /**
     * @brief Read exactly n bytes
     *
     * Blocks until all bytes are received or an error occurs.
     * Caller is responsible for freeing the returned buffer.
     *
     * @param n Number of bytes to read
     * @return char* Malloc'd buffer with n bytes plus null terminator, or NULL on error
     */
    virtual char* read_bytes(size_t n) = 0;

    /**
     * @brief Skip (discard) n bytes from the input stream
     *
     * Used to consume trailing CRLF after message payloads.
     *
     * @param n Number of bytes to skip
     */
    virtual void skip_bytes(size_t n) = 0;

    // ========================================================================
    // Polling
    // ========================================================================

    /**
     * @brief Check if data is available for reading
     *
     * @param timeout_ms Timeout in milliseconds (0 = non-blocking)
     * @return true if data is available
     * @return false if no data available or timeout
     */
    virtual bool has_data_available(int timeout_ms = 0) = 0;

    /**
     * @brief Get underlying file descriptor for select()
     *
     * @return int File descriptor, or -1 if not applicable (e.g., WebSocket)
     */
    virtual int get_fd() const = 0;

    // ========================================================================
    // Error Handling
    // ========================================================================

    /**
     * @brief Get the last error code
     *
     * @return nats_error_code_t Most recent error code
     */
    virtual nats_error_code_t get_last_error() const = 0;

    /**
     * @brief Get human-readable error string for last error
     *
     * @return const char* Error description
     */
    virtual const char* get_error_string() const = 0;

    // ========================================================================
    // Transport-Specific Features
    // ========================================================================

    /**
     * @brief Upgrade connection to TLS (for STARTTLS support)
     *
     * Only applicable to TCP transport. WebSocket transport uses TLS from start.
     *
     * @param tls_config TLS configuration
     * @return true if upgrade successful
     * @return false if upgrade failed or not supported
     */
    virtual bool upgrade_to_tls(const nats_tls_config_t* tls_config) {
        // Default implementation: not supported
        (void)tls_config;
        return false;
    }
};

#endif // ESPIDF_NATS_TRANSPORT_H
