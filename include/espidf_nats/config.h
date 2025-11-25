#ifndef ESPIDF_NATS_CONFIG_H
#define ESPIDF_NATS_CONFIG_H

// NATS client identification
#define NATS_CLIENT_LANG "espidf"
#define NATS_CLIENT_VERSION "1.1.0"

// Configuration options (can be overridden before including)
#ifndef NATS_CONF_VERBOSE
#define NATS_CONF_VERBOSE false
#endif

#ifndef NATS_CONF_PEDANTIC
#define NATS_CONF_PEDANTIC false
#endif

#ifndef NATS_PING_INTERVAL
#define NATS_PING_INTERVAL 120000UL
#endif

#ifndef NATS_RECONNECT_INTERVAL
#define NATS_RECONNECT_INTERVAL 1000UL
#endif

#ifndef NATS_MAX_RECONNECT_DELAY
#define NATS_MAX_RECONNECT_DELAY 30000UL
#endif

#ifndef NATS_MAX_PENDING_MESSAGES
#define NATS_MAX_PENDING_MESSAGES 100
#endif

// Default connection settings
#define NATS_DEFAULT_PORT 4222

// Inbox settings for request/reply
#define NATS_INBOX_PREFIX "_INBOX."
#define NATS_INBOX_ID_LENGTH 22

// Protocol parsing and limits
#define NATS_MAX_ARGV 5
#define NATS_MAX_LINE_SIZE 65536        // Max control line size (64KB)
#define NATS_MAX_PAYLOAD_SIZE 10485760  // Max message payload (10MB)
#define NATS_MAX_SUBJECT_LEN 1024       // Max subject length
#define NATS_MAX_HEADER_LEN 8192        // Max header size (8KB)
#define NATS_MAX_SUBJECTS 256           // Max subjects in stream config
#define NATS_MAX_CREDENTIAL_LEN 1024    // Max username/password length
#define NATS_MAX_SUBSCRIPTIONS 1024     // Max active subscriptions

// Protocol constants
#define NATS_CR_LF "\r\n"
#define NATS_CTRL_MSG "MSG"
#define NATS_CTRL_HMSG "HMSG"
#define NATS_CTRL_OK "+OK"
#define NATS_CTRL_ERR "-ERR"
#define NATS_CTRL_PING "PING"
#define NATS_CTRL_PONG "PONG"
#define NATS_CTRL_INFO "INFO"

// WebSocket configuration
#ifndef NATS_WEBSOCKET_BUFFER_SIZE
#define NATS_WEBSOCKET_BUFFER_SIZE 8192
#endif

#ifndef NATS_WEBSOCKET_PATH
#define NATS_WEBSOCKET_PATH "/nats"
#endif

#ifndef NATS_WEBSOCKET_SUBPROTOCOL
#define NATS_WEBSOCKET_SUBPROTOCOL "nats"
#endif

#ifndef NATS_WEBSOCKET_RECONNECT_TIMEOUT
#define NATS_WEBSOCKET_RECONNECT_TIMEOUT 10000
#endif

// Logging tag (inline to avoid ODR violations)
inline const char* tag = "espidf-nats";

#endif // ESPIDF_NATS_CONFIG_H
