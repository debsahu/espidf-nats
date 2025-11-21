# NATS - ESP-IDF Client

An ESP-IDF and FreeRTOS (probably) compatible C++ library for communicating with a [NATS](http://nats.io) server.

This module is was ported from https://github.com/isobit/arduino-nats and aims to remain compatible with it.

## Features

- Header-only library
- Compatible with Ethernet and WiFi-capable ESP32s
- Familiar C++ object-oriented API, similar usage to the official NATS client
  APIs
- Automatically attempts to reconnect to NATS server if the connection is dropped
- **TLS/SSL support** with server certificate validation and mutual TLS (mTLS)
- **DNS resolution** - Connect using hostnames, not just IP addresses
- **Multiple server URLs with automatic failover** - High availability support
- **NATS 2.0 Headers** - Publish and receive messages with headers (HPUB/HMSG)
- **Request timeouts** - Prevent hanging requests with configurable timeouts
- **Async/non-blocking API** - connect_async() for better FreeRTOS integration
- **Basic JetStream support** - Publish with acknowledgment for guaranteed delivery
- **Connection metrics** - Track messages/bytes sent/received, reconnections, and uptime
- **Error handling** - Detailed error codes with last_error() for diagnostics
- **Message buffering** - Automatic offline message queuing and replay on reconnect
- **Exponential backoff** - Smart reconnection delays (1s, 2s, 4s, 8s, 16s, 30s max)
- **Flush and drain** - Graceful shutdown with pending message delivery guarantees

### Installation

Just download [`espidf_nats.h`](https://raw.githubusercontent.com/daed/espidf-nats/master/espidf_nats.h) and include it in your code, or clone the entire repository as an ESP-IDF component.

### Library Structure

The library is modularized into separate headers for better maintainability and readability:

- **`include/espidf_nats.h`** - Main header that includes all modules (use this in your code)
- **`include/espidf_nats/config.h`** - Configuration defines and constants
- **`include/espidf_nats/util.h`** - Utility classes (Array, Queue, MillisTimer)
- **`include/espidf_nats/types.h`** - Type definitions (structs, enums)
- **`include/espidf_nats/subscription.h`** - Subscription management
- **`include/espidf_nats/nats_client.h`** - Main NATS client class

Simply `#include "espidf_nats.h"` in your code - all modules are included automatically.

## API

```c
class NATS {
	typedef struct {
		const char* subject;
		const int sid;
		const char* reply;
		const char* data;
		const int size;
	} msg;

	typedef void (*sub_cb)(msg e);
	typedef void (*event_cb)();

	NATS(
		const char* hostname,
		int port = NATS_DEFAULT_PORT,
		const char* user = NULL,
		const char* pass = NULL
	);

	bool connect();			// initiate the connection
	void disconnect();      // close the connection

	bool connected;			// whether or not the client is connected

	int max_outstanding_pings;	// number of outstanding pings to allow before considering the connection closed (default 3)
	int max_reconnect_attempts; // number of times to attempt reconnects, -1 means no maximum (default -1)

	event_cb on_connect;    // called after NATS finishes connecting to server
	event_cb on_disconnect; // called when a disconnect happens
	event_cb on_error;		// called when an error is received

	void publish(const char* subject, const char* msg = NULL, const char* replyto = NULL);
	void publish(const char* subject, const bool msg);
	void publishf(const char* subject, const char* fmt, ...);

	int subscribe(const char* subject, sub_cb cb, const char* queue = NULL, const int max_wanted = 0);
	void unsubscribe(const int sid);

	int request(const char* subject, const char* msg, sub_cb cb, const int max_wanted = 1);

	void process();			// process pending messages from the buffer, must be called regularly in loop()
}
```

## TLS/SSL Usage

To use TLS/SSL encrypted connections, create a `nats_tls_config_t` structure and pass it to the NATS constructor:

```c
extern const uint8_t ca_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t ca_cert_pem_end[] asm("_binary_ca_cert_pem_end");

nats_tls_config_t tls_config = {
    .enabled = true,
    .ca_cert = (const char*)ca_cert_pem_start,
    .ca_cert_len = ca_cert_pem_end - ca_cert_pem_start,
    .client_cert = NULL,                  // Optional: for mutual TLS
    .client_cert_len = 0,
    .client_key = NULL,                   // Optional: for mutual TLS
    .client_key_len = 0,
    .skip_cert_verification = false,      // Set true for development only
    .server_name = "nats.example.com"     // SNI hostname
};

NATS nats("nats.example.com", 4222, "user", "pass", &tls_config);
```

### TLS Configuration Options

- **enabled**: Set to `true` to enable TLS/SSL encryption
- **ca_cert**: PEM-encoded CA certificate for server verification
- **ca_cert_len**: Length of the CA certificate buffer
- **client_cert**: PEM-encoded client certificate for mutual TLS (optional)
- **client_cert_len**: Length of client certificate buffer
- **client_key**: PEM-encoded private key for mutual TLS (optional)
- **client_key_len**: Length of private key buffer
- **skip_cert_verification**: Skip certificate validation (insecure, for development only)
- **server_name**: Server name for SNI and certificate validation

### Embedding Certificates

To embed certificates in your ESP-IDF project, add them to your `CMakeLists.txt`:

```cmake
target_add_binary_data(your_target.elf "certs/ca_cert.pem" TEXT)
target_add_binary_data(your_target.elf "certs/client_cert.pem" TEXT)
target_add_binary_data(your_target.elf "certs/client_key.pem" TEXT)
```

### Mutual TLS (mTLS) Example

```c
nats_tls_config_t tls_config = {
    .enabled = true,
    .ca_cert = (const char*)ca_cert_pem_start,
    .ca_cert_len = ca_cert_pem_end - ca_cert_pem_start,
    .client_cert = (const char*)client_cert_pem_start,
    .client_cert_len = client_cert_pem_end - client_cert_pem_start,
    .client_key = (const char*)client_key_pem_start,
    .client_key_len = client_key_pem_end - client_key_pem_start,
    .skip_cert_verification = false,
    .server_name = "nats.example.com"
};

// When using mTLS for authentication, user/pass can be NULL
NATS nats("nats.example.com", 4222, NULL, NULL, &tls_config);
```

## Advanced Features

### Multiple Servers with Automatic Failover

Connect to a NATS cluster with automatic failover between nodes:

```c
nats_server_t servers[] = {
    {"nats1.example.com", 4222},
    {"nats2.example.com", 4222},
    {"nats3.example.com", 4222}
};

NATS nats(servers, 3, "user", "pass", &tls_config);
```

The library will automatically try all servers in the list and fail over to the next server if the current connection is lost.

### NATS Headers (NATS 2.0+)

Publish and subscribe to messages with headers:

```c
// Publish with headers
const char* headers = "Content-Type: application/json\r\nPriority: high\r\n\r\n";
nats.publish_with_headers("orders", headers, "{\"id\":123}");

// Subscribe - headers are available in the msg struct
nats.subscribe("orders", [](NATS::msg e) {
    if (e.headers != NULL) {
        ESP_LOGI(TAG, "Headers: %.*s", e.header_size, e.headers);
    }
    ESP_LOGI(TAG, "Data: %.*s", e.size, e.data);
});
```

### Request with Timeout

Prevent hanging requests by specifying a timeout:

```c
nats.request_with_timeout(
    "service.ping",                    // subject
    "PING",                            // request data
    [](NATS::msg e) {                  // response callback
        ESP_LOGI(TAG, "Got response: %s", e.data);
    },
    5000,                              // 5 second timeout
    []() {                             // timeout callback
        ESP_LOGW(TAG, "Request timed out!");
    }
);
```

### Async/Non-blocking Connection

Use async connection for better FreeRTOS task integration:

```c
nats.connect_async([](bool success) {
    if (success) {
        ESP_LOGI(TAG, "Connected!");
        // Start subscribing, publishing, etc.
    } else {
        ESP_LOGE(TAG, "Connection failed!");
    }
});

// Continue with other work while connecting
while (1) {
    nats.process();  // Handles async connection
    vTaskDelay(pdMS_TO_TICKS(10));
}
```

### JetStream Support

Publish with guaranteed delivery using JetStream:

```c
nats.jetstream_publish(
    "orders",                          // subject
    "{\"id\":123,\"item\":\"widget\"}", // message
    [](NATS::msg e) {                  // ACK callback
        ESP_LOGI(TAG, "Message acknowledged: %s", e.data);
    },
    []() {                             // timeout callback
        ESP_LOGW(TAG, "JetStream ACK timeout!");
    },
    5000                               // timeout (ms)
);
```

## Reliability Features

### Connection Metrics

Track connection health and message statistics:

```c
nats_connection_metrics_t metrics = nats.get_metrics();

ESP_LOGI(TAG, "Messages sent: %llu", metrics.msgs_sent);
ESP_LOGI(TAG, "Messages received: %llu", metrics.msgs_received);
ESP_LOGI(TAG, "Bytes sent: %llu", metrics.bytes_sent);
ESP_LOGI(TAG, "Bytes received: %llu", metrics.bytes_received);
ESP_LOGI(TAG, "Reconnections: %u", metrics.reconnections);
ESP_LOGI(TAG, "Uptime: %lu ms", metrics.uptime);
```

### Error Handling

Get detailed error information when operations fail:

```c
if (!nats.connect()) {
    nats_error_code_t err = nats.last_error();
    ESP_LOGE(TAG, "Connection failed: %s", nats.last_error_string());

    // Handle specific error codes
    if (err == NATS_ERR_DNS_RESOLUTION_FAILED) {
        ESP_LOGE(TAG, "Check your DNS settings");
    } else if (err == NATS_ERR_TLS_CONNECTION_FAILED) {
        ESP_LOGE(TAG, "Check your TLS certificates");
    }
}
```

Available error codes:
- `NATS_ERR_NONE` - No error
- `NATS_ERR_CONNECTION_FAILED` - TCP connection failed
- `NATS_ERR_DNS_RESOLUTION_FAILED` - Hostname lookup failed
- `NATS_ERR_TLS_INIT_FAILED` - TLS initialization failed
- `NATS_ERR_TLS_CONNECTION_FAILED` - TLS handshake failed
- `NATS_ERR_PROTOCOL_ERROR` - NATS protocol error from server
- `NATS_ERR_MAX_PINGS_EXCEEDED` - Too many outstanding pings
- `NATS_ERR_DISCONNECTED` - Disconnected from server
- `NATS_ERR_NOT_CONNECTED` - Operation requires connection

### Message Buffering

Automatically buffer messages when offline and replay them on reconnect:

```c
// Enable message buffering (enabled by default)
nats.message_buffering_enabled = true;
nats.max_pending_messages = 100;  // Buffer up to 100 messages

// Messages published while offline are automatically buffered
nats.publish("sensor.data", "{\"temp\":25.5}");
nats.publish("sensor.data", "{\"temp\":26.0}");

// When connection is restored, buffered messages are sent automatically
```

This is particularly useful for IoT devices with intermittent connectivity.

### Flush Pending Writes

Ensure all published messages have been sent to the server:

```c
nats.publish("critical.alert", "System overload");

// Wait for the message to be sent (max 5 seconds)
if (nats.flush(5000)) {
    ESP_LOGI(TAG, "Message delivered");
} else {
    ESP_LOGW(TAG, "Flush timeout");
}
```

### Graceful Shutdown with Drain

Properly close the connection by unsubscribing and flushing:

```c
// Drain will:
// 1. Unsubscribe from all subscriptions
// 2. Flush pending messages
// 3. Close the connection
nats.drain(5000);  // 5 second timeout
```

### Exponential Backoff Reconnect

Smart reconnection with increasing delays to avoid overwhelming the server:

```c
// Exponential backoff is enabled by default
nats.exponential_backoff_enabled = true;

// Reconnection delays: 1s, 2s, 4s, 8s, 16s, 30s (max)
// Configure in espidf_nats.h:
// #define NATS_RECONNECT_INTERVAL 1000UL        // Initial delay
// #define NATS_MAX_RECONNECT_DELAY 30000UL      // Maximum delay
```

The library automatically increases the delay between reconnection attempts, helping to reduce server load during outages.

## Configuration

Common configuration options can be customized by defining them before including the header:

```c
#define NATS_PING_INTERVAL 120000UL              // Ping interval (ms)
#define NATS_RECONNECT_INTERVAL 1000UL           // Initial reconnect delay (ms)
#define NATS_MAX_RECONNECT_DELAY 30000UL         // Max reconnect delay (ms)
#define NATS_MAX_PENDING_MESSAGES 100            // Max buffered messages
#include "espidf_nats.h"
```
