# NATS - ESP-IDF Client

An ESP-IDF and FreeRTOS (probably) compatible C++ library for communicating with a [NATS](http://nats.io) server.

This module is was ported from https://github.com/isobit/arduino-nats and aims to remain compatible with it.

## Features

- Header-only library
- Compatible with Ethernet and WiFi-capable ESP32s
- Familiar C++ object-oriented API, similar usage to the official NATS client
  APIs
- Automatically attempts to reconnect to NATS server if the connection is dropped
- **NEW:** TLS/SSL support with server certificate validation and mutual TLS (mTLS)

### Manual

Just download [`espidf_nats.h`](https://raw.githubusercontent.com/daed/espidf-nats/master/espidf_nats.h) and include it in your code.

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
