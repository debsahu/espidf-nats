# NATS - ESP-IDF Client

An ESP-IDF and FreeRTOS (probably) compatible C++ library for communicating with a [NATS](http://nats.io) server.

This module is was ported from https://github.com/isobit/arduino-nats and aims to remain compatible with it.

## Features

- Header-only library
- Compatible with Ethernet and WiFi-capable ESP32s
- Familiar C++ object-oriented API, similar usage to the official NATS client
  APIs
- Automatically attempts to reconnect to NATS server if the connection is dropped

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
