#ifndef ESPIDF_NATS_TYPES_H
#define ESPIDF_NATS_TYPES_H

#include <stdint.h>
#include <stddef.h>

/**
 * TLS/SSL configuration structure
 */
typedef struct {
    bool enabled;                      // Enable TLS/SSL
    const char* ca_cert;               // CA certificate (PEM format)
    size_t ca_cert_len;                // CA certificate length
    const char* client_cert;           // Client certificate for mTLS (optional)
    size_t client_cert_len;            // Client certificate length
    const char* client_key;            // Client private key for mTLS (optional)
    size_t client_key_len;             // Client private key length
    bool skip_cert_verification;       // Skip certificate validation (dev only)
    const char* server_name;           // SNI server name
} nats_tls_config_t;

/**
 * Server definition for multi-server support
 */
typedef struct {
    const char* hostname;              // Server hostname or IP
    int port;                          // Server port
} nats_server_t;

/**
 * Connection metrics and statistics
 */
typedef struct {
    uint64_t msgs_sent;                // Total messages published
    uint64_t msgs_received;            // Total messages received
    uint64_t bytes_sent;               // Total bytes sent (including protocol)
    uint64_t bytes_received;           // Total bytes received
    uint32_t reconnections;            // Number of reconnections
    unsigned long last_connect_time;   // Timestamp of last connection
    unsigned long uptime;              // Milliseconds since connection
} nats_connection_metrics_t;

/**
 * Error codes for diagnostics
 */
typedef enum {
    NATS_ERR_NONE = 0,                 // No error
    NATS_ERR_CONNECTION_FAILED,        // TCP connection failed
    NATS_ERR_DNS_RESOLUTION_FAILED,    // DNS lookup failed
    NATS_ERR_TLS_INIT_FAILED,          // TLS initialization failed
    NATS_ERR_TLS_CONNECTION_FAILED,    // TLS handshake failed
    NATS_ERR_SOCKET_FAILED,            // Socket operation failed
    NATS_ERR_PROTOCOL_ERROR,           // NATS protocol error
    NATS_ERR_MAX_PINGS_EXCEEDED,       // Too many outstanding pings
    NATS_ERR_DISCONNECTED,             // Disconnected from server
    NATS_ERR_INVALID_SUBJECT,          // Invalid subject name
    NATS_ERR_NOT_CONNECTED             // Operation requires connection
} nats_error_code_t;

/**
 * Pending message for offline buffering
 */
typedef struct {
    char* subject;                     // Message subject
    char* message;                     // Message payload
    char* reply_to;                    // Reply-to subject
    char* headers;                     // NATS headers (HPUB)
} pending_msg_t;

#endif // ESPIDF_NATS_TYPES_H
