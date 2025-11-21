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
    NATS_ERR_NOT_CONNECTED,            // Operation requires connection
    NATS_ERR_INVALID_CONFIG,           // Invalid configuration
    NATS_ERR_OUT_OF_MEMORY             // Out of memory
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

/**
 * JetStream stream configuration
 */
typedef struct {
    const char* name;                  // Stream name
    const char** subjects;             // Array of subjects (NULL-terminated)
    size_t max_msgs;                   // Maximum number of messages
    size_t max_bytes;                  // Maximum total bytes
    int64_t max_age;                   // Maximum age in nanoseconds (0 = unlimited)
    int max_msg_size;                  // Maximum message size (-1 = unlimited)
    const char* storage;               // "file" or "memory"
    int replicas;                      // Number of replicas (1-5)
    bool discard_new;                  // Discard new messages when limits hit
} jetstream_stream_config_t;

/**
 * JetStream consumer configuration
 */
typedef struct {
    const char* stream_name;           // Stream name
    const char* durable_name;          // Durable consumer name (optional)
    const char* filter_subject;        // Filter by subject (optional)
    bool deliver_all;                  // Deliver all messages (vs. new only)
    const char* ack_policy;            // "explicit", "all", "none"
    int64_t ack_wait;                  // Ack wait time in nanoseconds
    int max_deliver;                   // Maximum delivery attempts (-1 = unlimited)
    const char* replay_policy;         // "instant" or "original"
    bool ordered;                      // Create ordered consumer (ephemeral, guaranteed order)
} jetstream_consumer_config_t;

/**
 * JetStream fetch request configuration
 */
typedef struct {
    int batch;                         // Number of messages to fetch
    size_t max_bytes;                  // Maximum bytes to fetch (0 = unlimited)
    int64_t expires;                   // Request expiration in nanoseconds
    int64_t heartbeat;                 // Idle heartbeat interval in nanoseconds
    bool no_wait;                      // Don't wait if no messages available
} jetstream_fetch_request_t;

/**
 * JetStream account information
 */
typedef struct {
    int64_t memory;                    // Memory storage used (bytes)
    int64_t storage;                   // File storage used (bytes)
    int64_t streams;                   // Number of streams
    int64_t consumers;                 // Number of consumers
    int64_t memory_max;                // Memory storage limit (-1 = unlimited)
    int64_t storage_max;               // File storage limit (-1 = unlimited)
    int64_t streams_max;               // Stream count limit (-1 = unlimited)
    int64_t consumers_max;             // Consumer count limit (-1 = unlimited)
} jetstream_account_info_t;

/**
 * Key-Value bucket configuration
 */
typedef struct {
    const char* bucket;                // Bucket name
    const char* description;           // Bucket description (optional)
    size_t max_value_size;             // Maximum value size in bytes (-1 = unlimited)
    int64_t history;                   // Number of historical values per key (1-64, default 1)
    int64_t ttl;                       // Time-to-live for values in nanoseconds (0 = unlimited)
    const char* storage;               // "file" or "memory"
    int replicas;                      // Number of replicas (1-5)
    size_t max_bytes;                  // Maximum total bytes for bucket (0 = unlimited)
} kv_config_t;

/**
 * Key-Value entry
 */
typedef struct {
    const char* bucket;                // Bucket name
    const char* key;                   // Key name
    const char* value;                 // Value data
    size_t value_len;                  // Value length
    uint64_t revision;                 // Entry revision number
    uint64_t created;                  // Creation timestamp (nanoseconds)
    const char* operation;             // Operation type: "PUT", "DEL", "PURGE"
    int64_t delta;                     // Distance from latest value
} kv_entry_t;

/**
 * Key-Value watch options
 */
typedef struct {
    bool include_history;              // Include historical values
    bool ignore_deletes;               // Ignore delete markers
    bool meta_only;                    // Only metadata, no values
    int64_t updates_only;              // Only updates after revision N
} kv_watch_options_t;

#endif // ESPIDF_NATS_TYPES_H
