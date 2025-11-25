# NATS - ESP-IDF Client

An ESP-IDF and FreeRTOS compatible C++ library for communicating with a [NATS](http://nats.io) server.

> **Note:** This is an actively maintained fork of [daed/espidf-nats](https://github.com/daed/espidf-nats), which was originally ported from [arduino-nats](https://github.com/isobit/arduino-nats). This fork adds comprehensive JetStream support, Key-Value Store, Object Store, TLS/SSL, and many reliability improvements.

## Features

- ESP-IDF component with header-only C++ implementation
- Compatible with Ethernet and WiFi-capable ESP32s
- Familiar C++ object-oriented API, similar usage to the official NATS client
  APIs
- Automatically attempts to reconnect to NATS server if the connection is dropped
- **Transport abstraction layer** - Supports TCP, TLS, and WebSocket transports
- **WebSocket support (ws:// and wss://)** - Connect through firewalls and load balancers using ESP-IDF's esp_websocket_client
- **TLS/SSL support** with server certificate validation and mutual TLS (mTLS)
- **DNS resolution** - Connect using hostnames, not just IP addresses
- **Multiple server URLs with automatic failover** - High availability support (TCP and WebSocket)
- **NATS 2.0 Headers** - Publish and receive messages with headers (HPUB/HMSG)
- **Request timeouts** - Prevent hanging requests with configurable timeouts
- **Async/non-blocking API** - connect_async() for better FreeRTOS integration
- **Comprehensive JetStream support** - Streams, consumers, pull-based delivery, message deduplication, and ACK controls
- **Key-Value Store** - Distributed configuration and state management with revision history, TTL, and watchers
- **Object Store** - Large binary storage (firmware, logs, images) with automatic 128KB chunking
- **Ordered Consumers** - Guaranteed in-order message delivery for sequential processing
- **Fetch Operations** - Efficient batch message retrieval with configurable limits and heartbeats
- **Direct Get** - Low-latency JetStream message retrieval bypassing stream leader
- **Consumer Pause** - Temporary flow control for resource management and maintenance windows
- **Account Info** - Monitor JetStream resource usage and quotas
- **Connection metrics** - Track messages/bytes sent/received, reconnections, and uptime
- **Error handling** - Detailed error codes with last_error() for diagnostics
- **Message buffering** - Automatic offline message queuing and replay on reconnect
- **Exponential backoff** - Smart reconnection delays (1s, 2s, 4s, 8s, 16s, 30s max)
- **Flush and drain** - Graceful shutdown with pending message delivery guarantees

### Installation

**Via ESP-IDF Component Registry (recommended):**

```bash
idf.py add-dependency "debsahu/espidf-nats^1.1.0"
```

**Or clone the repository as an ESP-IDF component:**

```bash
cd your-esp-idf-project/components
git clone https://github.com/debsahu/espidf-nats.git
```

**Or add as a git submodule:**

```bash
cd your-esp-idf-project
git submodule add https://github.com/debsahu/espidf-nats.git components/espidf-nats
```

Then include in your code:

```cpp
#include "espidf_nats.h"
```

### Example Project

A complete working example is available in the `example/` directory. It includes:

- Full ESP-IDF project structure ready to build
- Docker Compose setup for running a local NATS server
- Test configurations for basic NATS, JetStream, TLS, and more
- Makefile with convenient build and flash commands

To try the example:

```bash
cd example
idf.py set-target esp32  # or esp32s3, esp32c3, etc.
idf.py build
idf.py flash monitor
```

See `example/README.md` for detailed instructions.

### Library Structure

The library is modularized into separate headers for better maintainability and readability:

- **`include/espidf_nats.h`** - Main header that includes all modules (use this in your code)
- **`include/espidf_nats/config.h`** - Configuration defines, constants, and WebSocket settings
- **`include/espidf_nats/util.h`** - Utility classes (Array, Queue, MillisTimer, RingBuffer)
- **`include/espidf_nats/types.h`** - Type definitions (transport types, structs, enums)
- **`include/espidf_nats/subscription.h`** - Subscription management
- **`include/espidf_nats/transport.h`** - Abstract transport interface
- **`include/espidf_nats/tcp_transport.h`** - TCP/TLS transport implementation
- **`include/espidf_nats/ws_transport.h`** - WebSocket transport implementation (optional)
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

## WebSocket Support

Connect to NATS servers using WebSocket transport (ws:// and wss://), allowing connections through firewalls and load balancers that only allow HTTP/HTTPS traffic.

### Requirements

1. Enable WebSocket client in ESP-IDF menuconfig:
   ```
   Component config → ESP-TLS → [*] Enable WebSocket Client
   ```

2. Or add to your project's `sdkconfig.defaults`:
   ```
   CONFIG_ESP_WEBSOCKET_CLIENT_ENABLE=y
   ```

### Basic WebSocket Connection

```c
// Simple WebSocket connection (ws://)
NATS* nats = NATS::create_websocket("nats.example.com", 9222);
nats->on_connect = []() { ESP_LOGI(TAG, "WebSocket connected!"); };

if (nats->connect()) {
    nats->subscribe("events.*", [](NATS::msg e) {
        ESP_LOGI(TAG, "Received: %s", e.data);
    });

    while(1) {
        nats->process();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
delete nats;  // Factory methods return pointers
```

### Secure WebSocket (wss://)

```c
nats_tls_config_t tls_config = {
    .enabled = true,
    .ca_cert = (const char*)ca_cert_pem_start,
    .ca_cert_len = ca_cert_pem_end - ca_cert_pem_start,
    .skip_cert_verification = false,
    .server_name = "nats.example.com"
};

NATS* nats = NATS::create_websocket("nats.example.com", 443, "user", "pass", &tls_config, "/nats");
```

### WebSocket from URI

```c
// Auto-detects TLS from wss:// scheme
NATS* nats = NATS::create_websocket_uri("wss://nats.example.com:443/nats", "user", "pass");
```

### Multiple WebSocket Servers with Failover

```c
nats_server_t ws_servers[] = {
    {"ws1.example.com", 9222, NATS_TRANSPORT_WEBSOCKET, "/nats"},
    {"ws2.example.com", 9222, NATS_TRANSPORT_WEBSOCKET, "/nats"},
    {"ws3.example.com", 9222, NATS_TRANSPORT_WEBSOCKET, "/nats"}
};

NATS* nats = NATS::create_websocket(ws_servers, 3, "user", "pass", &tls_config);
// Automatically connects to first available server
// Falls over to next server on disconnect
```

### WebSocket Configuration

WebSocket settings can be customized in `config.h`:

```c
#define NATS_WEBSOCKET_BUFFER_SIZE 8192        // Ring buffer size
#define NATS_WEBSOCKET_PATH "/nats"            // Default path
#define NATS_WEBSOCKET_SUBPROTOCOL "nats"      // WebSocket subprotocol
#define NATS_WEBSOCKET_RECONNECT_TIMEOUT 10000 // Reconnect timeout (ms)
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

Comprehensive JetStream support for guaranteed message delivery, persistence, and advanced streaming patterns.

#### Creating Streams

```c
const char* subjects[] = {"orders.*", "inventory.*", NULL};

jetstream_stream_config_t stream_config = {
    .name = "ORDERS",
    .subjects = subjects,
    .max_msgs = 10000,
    .max_bytes = 10485760,  // 10 MB
    .max_age = 86400000000000LL,  // 24 hours in nanoseconds
    .max_msg_size = 1048576,  // 1 MB
    .storage = "file",
    .replicas = 1,
    .discard_new = false
};

nats.jetstream_stream_create(
    &stream_config,
    [](NATS::msg e) {
        ESP_LOGI(TAG, "Stream created: %s", e.data);
    },
    []() { ESP_LOGW(TAG, "Timeout creating stream"); },
    5000
);
```

#### Publishing with Acknowledgment

```c
// Basic publish with ACK
nats.jetstream_publish(
    "orders.new",
    "{\"id\":123,\"item\":\"widget\"}",
    [](NATS::msg e) {
        ESP_LOGI(TAG, "Message acknowledged: %s", e.data);
    },
    []() { ESP_LOGW(TAG, "JetStream ACK timeout!"); },
    5000
);

// Publish with message deduplication
nats.jetstream_publish(
    "orders.new",
    "{\"id\":123,\"item\":\"widget\"}",
    [](NATS::msg e) {
        ESP_LOGI(TAG, "Deduplicated ACK: %s", e.data);
    },
    []() { ESP_LOGW(TAG, "Timeout"); },
    5000,
    "order-123"  // Message ID for deduplication
);
```

#### Creating Consumers

```c
jetstream_consumer_config_t consumer_config = {
    .stream_name = "ORDERS",
    .durable_name = "order-processor",
    .filter_subject = "orders.new",
    .deliver_all = true,
    .ack_policy = "explicit",
    .ack_wait = 30000000000LL,  // 30 seconds in nanoseconds
    .max_deliver = 3,
    .replay_policy = "instant"
};

nats.jetstream_consumer_create(
    &consumer_config,
    [](NATS::msg e) {
        ESP_LOGI(TAG, "Consumer created: %s", e.data);
    },
    []() { ESP_LOGW(TAG, "Timeout creating consumer"); },
    5000
);
```

#### Pull-Based Message Consumption

```c
// Pull batch of messages from consumer
nats.jetstream_pull(
    "ORDERS",                  // stream name
    "order-processor",         // consumer name
    10,                        // batch size
    [](NATS::msg e) {
        ESP_LOGI(TAG, "Received: %.*s", e.size, e.data);

        // Process message...

        // Acknowledge successful processing
        nats.jetstream_ack(e.reply);

        // Or negative acknowledge to redeliver
        // nats.jetstream_nak(e.reply);

        // Or delay redelivery by 5 seconds
        // nats.jetstream_ack_delay(e.reply, 5000);
    },
    []() { ESP_LOGW(TAG, "Pull timeout"); },
    5000
);
```

#### Message Acknowledgment

```c
// Process message from JetStream consumer
nats.subscribe("orders.new", [](NATS::msg e) {
    // Acknowledge successful processing
    nats.jetstream_ack(e.reply);

    // Or negative acknowledge (request redelivery)
    // nats.jetstream_nak(e.reply);

    // Or acknowledge with delayed redelivery (5 seconds)
    // nats.jetstream_ack_delay(e.reply, 5000);
});
```

#### Stream Management

```c
// Get stream information
nats.jetstream_stream_info(
    "ORDERS",
    [](NATS::msg e) {
        ESP_LOGI(TAG, "Stream info: %s", e.data);
    },
    []() { ESP_LOGW(TAG, "Timeout"); },
    5000
);

// Delete stream
nats.jetstream_stream_delete(
    "ORDERS",
    [](NATS::msg e) {
        ESP_LOGI(TAG, "Stream deleted: %s", e.data);
    },
    []() { ESP_LOGW(TAG, "Timeout"); },
    5000
);

// Delete consumer
nats.jetstream_consumer_delete(
    "ORDERS",
    "order-processor",
    [](NATS::msg e) {
        ESP_LOGI(TAG, "Consumer deleted: %s", e.data);
    },
    []() { ESP_LOGW(TAG, "Timeout"); },
    5000
);
```

## Advanced JetStream Features

### Ordered Consumers

Guaranteed in-order message delivery for sequential processing:

```c
// Create ordered consumer (simplified API)
nats.jetstream_consumer_create_ordered(
    "ORDERS",           // stream name
    "orders.new",       // filter subject (optional)
    [](NATS::msg e) {
        ESP_LOGI(TAG, "Consumer created: %s", e.data);
    },
    []() { ESP_LOGW(TAG, "Timeout"); },
    5000
);
```

**Features:**
- Ephemeral consumers (no durable name)
- Server-side flow control and heartbeats
- Guaranteed in-order delivery
- Perfect for sequential command execution, firmware updates, time-series data

### Fetch Operations

Efficient batch message retrieval:

```c
// Configure fetch request
jetstream_fetch_request_t fetch_config = {
    .batch = 10,                        // Fetch 10 messages
    .max_bytes = 1048576,               // Max 1 MB
    .expires = 5000000000LL,            // 5 second timeout (nanoseconds)
    .heartbeat = 1000000000LL,          // 1 second heartbeat
    .no_wait = true                     // Don't wait if no messages
};

// Fetch messages
nats.jetstream_fetch(
    "ORDERS",                           // stream name
    "order-processor",                  // consumer name
    &fetch_config,
    [](NATS::msg e) {
        ESP_LOGI(TAG, "Message: %.*s", e.size, e.data);
        nats.jetstream_ack(e.reply);
    },
    []() { ESP_LOGW(TAG, "Fetch timeout"); },
    5000
);
```

**Benefits:**
- More efficient than individual pulls
- Reduces protocol overhead
- Configurable batch size, byte limits, and heartbeats
- Ideal for batch processing queued messages

### Account Information

Monitor JetStream resource usage and quotas:

```c
nats.jetstream_account_info(
    [](NATS::msg e) {
        ESP_LOGI(TAG, "Account info: %s", e.data);
        // Parse JSON response:
        // {"memory": 1024, "storage": 2048, "streams": 5, "consumers": 10,
        //  "limits": {"max_memory": 1073741824, "max_storage": 10737418240, ...}}
    },
    []() { ESP_LOGW(TAG, "Timeout"); },
    5000
);
```

**Use Cases:**
- Monitor ESP32 device quota usage
- Prevent resource exhaustion
- Warn before hitting limits
- Capacity planning for IoT fleet

## Key-Value Store

Distributed key-value storage built on JetStream for device configuration and state management.

### Creating a KV Bucket

```c
kv_config_t kv_config = {
    .bucket = "device-config",
    .description = "Device configuration",
    .max_value_size = 1024,             // 1 KB max value
    .history = 10,                      // Keep 10 revisions per key
    .ttl = 0,                           // No expiration
    .storage = "file",                  // File storage
    .replicas = 1,
    .max_bytes = 10485760               // 10 MB bucket limit
};

nats.kv_create_bucket(
    &kv_config,
    [](NATS::msg e) {
        ESP_LOGI(TAG, "Bucket created: %s", e.data);
    },
    []() { ESP_LOGW(TAG, "Timeout"); },
    5000
);
```

### Putting and Getting Values

```c
// Put a value
nats.kv_put(
    "device-config",                    // bucket name
    "wifi_ssid",                        // key
    "MyNetwork",                        // value
    [](NATS::msg e) {
        ESP_LOGI(TAG, "Value stored: %s", e.data);
    },
    []() { ESP_LOGW(TAG, "Timeout"); },
    5000
);

// Get a value
nats.kv_get(
    "device-config",                    // bucket name
    "wifi_ssid",                        // key
    [](NATS::msg e) {
        // Parse JSON response to extract value
        ESP_LOGI(TAG, "Got value: %.*s", e.size, e.data);
    },
    []() { ESP_LOGW(TAG, "Key not found or timeout"); },
    5000
);
```

### Watching for Changes

Watch a key or key pattern for real-time updates:

```c
// Watch specific key
nats.kv_watch(
    "device-config",
    "wifi_ssid",
    [](NATS::msg e) {
        ESP_LOGI(TAG, "Config changed: %.*s", e.size, e.data);
        // Automatically reload configuration
    }
);

// Watch all keys in bucket (wildcard)
nats.kv_watch(
    "device-config",
    "*",                                // All keys
    [](NATS::msg e) {
        ESP_LOGI(TAG, "Any config changed: %s", e.subject);
    }
);
```

### Deleting and Purging Keys

```c
// Soft delete (preserves history with tombstone)
nats.kv_delete(
    "device-config",
    "old_key",
    [](NATS::msg e) {
        ESP_LOGI(TAG, "Key deleted (soft): %s", e.data);
    },
    []() { ESP_LOGW(TAG, "Timeout"); },
    5000
);

// Hard delete (purge all revisions)
nats.kv_purge(
    "device-config",
    "old_key",
    [](NATS::msg e) {
        ESP_LOGI(TAG, "Key purged (hard): %s", e.data);
    },
    []() { ESP_LOGW(TAG, "Timeout"); },
    5000
);
```

### Getting Key History

```c
// Get all revisions of a key
nats.kv_history(
    "device-config",
    "wifi_ssid",
    [](NATS::msg e) {
        ESP_LOGI(TAG, "Revision: %.*s", e.size, e.data);
    },
    []() { ESP_LOGW(TAG, "Timeout"); },
    5000
);
```

### Listing All Keys

```c
nats.kv_keys(
    "device-config",
    [](NATS::msg e) {
        // Parse JSON response for key list
        ESP_LOGI(TAG, "Keys: %s", e.data);
    },
    []() { ESP_LOGW(TAG, "Timeout"); },
    5000
);
```

### KV Store Use Cases for IoT

**Device Configuration:**
```c
// Store WiFi credentials
nats.kv_put("device-config", "wifi_ssid", "MyNetwork", ...);
nats.kv_put("device-config", "wifi_password", "secret123", ...);

// Store sensor calibration
nats.kv_put("sensors", "temp_offset", "2.5", ...);
nats.kv_put("sensors", "humidity_cal", "1.02", ...);
```

**Device Twin/Shadow State:**
```c
// Update device state
nats.kv_put("device-state", "temperature", "25.5", ...);
nats.kv_put("device-state", "status", "online", ...);

// Watch for desired state changes from cloud
nats.kv_watch("device-state", "desired_temp", [](NATS::msg e) {
    // Update thermostat setpoint
});
```

**Remote Configuration Updates:**
```c
// Watch for config updates from server
nats.kv_watch("device-config", "*", [](NATS::msg e) {
    ESP_LOGI(TAG, "Config updated remotely: %s", e.subject);
    // Reload configuration without reboot
    reload_config();
});
```

## Object Store

Store large binary objects (firmware, logs, images) with automatic chunking and efficient retrieval.

### Creating an Object Store Bucket

```c
object_store_config_t obj_config = {
    .bucket = "firmware",
    .description = "Firmware images",
    .ttl = 86400000000000LL,           // 1 day in nanoseconds
    .storage = "file",
    .replicas = 1,
    .max_bytes = 104857600,            // 100 MB
    .compressed = false                // Not yet implemented
};

nats.obj_create_bucket(
    &obj_config,
    [](NATS::msg e) {
        ESP_LOGI(TAG, "Object store created: %s", e.data);
    },
    []() { ESP_LOGW(TAG, "Timeout"); },
    5000
);
```

### Storing an Object

Objects are automatically chunked into 128KB pieces for efficient storage:

```c
// Example: Store firmware image
uint8_t firmware_data[256000];  // 250 KB firmware
// ... load firmware data ...

nats.obj_put(
    "firmware",                        // bucket name
    "esp32-v1.2.bin",                  // object name
    firmware_data,                     // data
    sizeof(firmware_data),             // size
    "ESP32 firmware v1.2",             // description (optional)
    [](NATS::msg e) {
        ESP_LOGI(TAG, "Firmware stored: %s", e.data);
    },
    []() { ESP_LOGW(TAG, "Timeout"); },
    10000                              // 10 second timeout
);
```

### Getting Object Metadata

```c
nats.obj_get_info(
    "firmware",
    "esp32-v1.2.bin",
    [](NATS::msg e) {
        // Parse JSON response for metadata
        ESP_LOGI(TAG, "Object info: %.*s", e.size, e.data);
        // Response includes: name, size, chunks, nuid, mtime, description
    },
    []() { ESP_LOGW(TAG, "Object not found"); },
    5000
);
```

### Downloading an Object

```c
nats.obj_get(
    "firmware",
    "esp32-v1.2.bin",
    [](NATS::msg e) {
        // Receive chunks - caller must assemble
        ESP_LOGI(TAG, "Received chunk: %d bytes", e.size);
        // Write chunk to flash or buffer
    },
    []() { ESP_LOGW(TAG, "Timeout"); },
    30000                              // 30 second timeout for large objects
);
```

### Listing Objects in Bucket

```c
nats.obj_list(
    "firmware",
    [](NATS::msg e) {
        // Parse stream info to extract object list
        ESP_LOGI(TAG, "Objects: %.*s", e.size, e.data);
    },
    []() { ESP_LOGW(TAG, "Timeout"); },
    5000
);
```

### Deleting an Object

```c
nats.obj_delete(
    "firmware",
    "old-version.bin",
    [](NATS::msg e) {
        ESP_LOGI(TAG, "Object deleted: %s", e.data);
    },
    []() { ESP_LOGW(TAG, "Timeout"); },
    5000
);
```

### Watching for New Objects

Get notified when objects are added or modified:

```c
nats.obj_watch(
    "firmware",
    [](NATS::msg e) {
        ESP_LOGI(TAG, "New object: %s", e.subject);
        // Download and install new firmware
    }
);
```

### Object Store Use Cases for IoT

**Over-the-Air (OTA) Firmware Updates:**
```c
// Server uploads new firmware to object store
// ESP32 watches for new firmware
nats.obj_watch("firmware", [](NATS::msg e) {
    ESP_LOGI(TAG, "New firmware available!");
    // Download and verify firmware
    // Perform OTA update
});
```

**Data Logging:**
```c
// Store sensor logs as objects
uint8_t log_buffer[50000];
// ... fill log buffer ...

nats.obj_put("logs", "device-123-20250121.log",
             log_buffer, sizeof(log_buffer),
             "Daily sensor log", ...);
```

**Image Storage:**
```c
// Store camera snapshots
uint8_t image_data[65536];  // JPEG image
nats.obj_put("images", "snapshot-001.jpg",
             image_data, image_size,
             "Motion detected", ...);
```

## Direct Get

Low-latency message retrieval from JetStream streams, bypassing the stream leader for faster access.

### Get Message by Sequence Number

```c
nats.jetstream_direct_get(
    "SENSORS",                         // stream name
    1234,                              // sequence number
    [](NATS::msg e) {
        ESP_LOGI(TAG, "Message: %.*s", e.size, e.data);
    },
    []() { ESP_LOGW(TAG, "Message not found"); },
    5000
);
```

### Get Last Message for Subject

```c
nats.jetstream_direct_get_last(
    "SENSORS",                         // stream name
    "sensor.temp.room1",               // subject
    [](NATS::msg e) {
        ESP_LOGI(TAG, "Latest temp: %.*s", e.size, e.data);
    },
    []() { ESP_LOGW(TAG, "No messages"); },
    5000
);
```

### Direct Get Use Cases

**Quick Data Lookup:**
```c
// Get latest sensor reading without creating a consumer
nats.jetstream_direct_get_last("SENSORS", "sensor.temp.*", [](NATS::msg e) {
    // Process latest temperature reading
});
```

**Historical Data Access:**
```c
// Retrieve specific historical message by sequence
nats.jetstream_direct_get("EVENTS", historical_seq, [](NATS::msg e) {
    // Analyze historical event
});
```

## Consumer Pause

Temporarily pause message delivery for flow control and resource management.

### Pause Consumer

```c
// Pause for 30 seconds
uint64_t pause_duration_ns = 30000000000ULL;  // 30 seconds in nanoseconds
uint64_t pause_until = (NATSUtil::millis() * 1000000ULL) + pause_duration_ns;

nats.jetstream_consumer_pause(
    "SENSORS",                         // stream name
    "my-consumer",                     // consumer name
    pause_until,                       // pause until timestamp
    [](NATS::msg e) {
        ESP_LOGI(TAG, "Consumer paused: %s", e.data);
    },
    []() { ESP_LOGW(TAG, "Timeout"); },
    5000
);
```

### Resume Consumer

```c
nats.jetstream_consumer_resume(
    "SENSORS",
    "my-consumer",
    [](NATS::msg e) {
        ESP_LOGI(TAG, "Consumer resumed: %s", e.data);
    },
    []() { ESP_LOGW(TAG, "Timeout"); },
    5000
);
```

### Consumer Pause Use Cases

**Resource Management:**
```c
// Pause consumer during high-priority task
nats.jetstream_consumer_pause("SENSORS", "data-processor", pause_time, ...);

// Perform critical operation
perform_ota_update();

// Resume consumer
nats.jetstream_consumer_resume("SENSORS", "data-processor", ...);
```

**Rate Limiting:**
```c
// Pause consumer when buffer is full
if (buffer_full) {
    uint64_t pause_5_sec = (NATSUtil::millis() * 1000000ULL) + 5000000000ULL;
    nats.jetstream_consumer_pause("EVENTS", "processor", pause_5_sec, ...);
}
```

**Scheduled Maintenance:**
```c
// Pause during maintenance window
uint64_t maintenance_end = get_maintenance_window_end();
nats.jetstream_consumer_pause("LOGS", "analyzer", maintenance_end, ...);
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
- `NATS_ERR_SOCKET_FAILED` - Socket operation failed
- `NATS_ERR_PROTOCOL_ERROR` - NATS protocol error from server
- `NATS_ERR_MAX_PINGS_EXCEEDED` - Too many outstanding pings
- `NATS_ERR_DISCONNECTED` - Disconnected from server
- `NATS_ERR_INVALID_SUBJECT` - Invalid subject name
- `NATS_ERR_NOT_CONNECTED` - Operation requires connection
- `NATS_ERR_INVALID_CONFIG` - Invalid configuration
- `NATS_ERR_INVALID_ARG` - Invalid argument
- `NATS_ERR_OUT_OF_MEMORY` - Memory allocation failed
- `NATS_ERR_TOO_MANY_SUBS` - Subscription limit exceeded

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
