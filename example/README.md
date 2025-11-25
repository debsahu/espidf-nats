# NATS Client Test Application

This is a comprehensive test application for the ESP-IDF NATS client library. It validates all critical functionality including thread safety, deadlock prevention, and memory management.

## Purpose

This test application was created to validate the critical fixes made to the espidf-nats component:
- ✅ Thread safety with FreeRTOS mutexes
- ✅ Deadlock prevention (callbacks calling NATS methods)
- ✅ Memory safety (new/delete matching, overflow protection)
- ✅ Race condition fixes
- ✅ Code duplication cleanup

## Test Scenarios

### Test 1: Basic Pub/Sub
Tests fundamental publish/subscribe functionality:
- Subscribes to a subject
- Publishes multiple messages
- Validates message reception
- Verifies callback execution

### Test 2: Callback Publishing (Deadlock Prevention)
Critical test for deadlock prevention:
- Callback publishes messages (recursive NATS calls)
- Tests ping/pong pattern with callbacks calling `publish()`
- Validates that mutex is released before callback execution
- **This would deadlock without the fixes**

### Test 3: Request/Reply Pattern
Tests request/reply messaging:
- Creates a service responder
- Sends request and receives reply
- Tests inbox subject generation
- Validates reply-to mechanism

### Test 4: Thread Safety (Concurrent Publishing)
Tests multi-threaded access:
- Creates multiple FreeRTOS tasks
- Each task publishes messages concurrently
- Validates mutex protection of shared state
- Tests subscription array thread safety
- **This would crash without mutex protection**

### Test 5: Connection Metrics
Validates metrics tracking:
- Messages sent/received counters
- Bytes sent/received counters
- Reconnection count
- Connection uptime
- Compares with test statistics

## Prerequisites

1. **ESP-IDF**: Version 4.4 or later
   ```bash
   . $HOME/esp/esp-idf/export.sh
   ```

2. **WiFi Access**: Working WiFi network with internet access

3. **NATS Server**: Either:
   - **Local Docker NATS** (recommended): See [DOCKER.md](DOCKER.md) for setup
     ```bash
     make nats-up          # Start all NATS servers
     make nats-basic       # Start basic NATS only (port 4222)
     ```
   - Public demo server: `demo.nats.io:4222` (default in code)
   - Your own NATS server

4. **Docker** (optional, for local NATS server):
   - Docker Desktop (macOS/Windows)
   - Docker Engine (Linux)

## Configuration

### WiFi Settings

Edit [main/main.cpp](main/main.cpp) and update:
```cpp
#define WIFI_SSID      "YOUR_WIFI_SSID"
#define WIFI_PASS      "YOUR_WIFI_PASSWORD"
```

### NATS Server Settings

Edit [main/main.cpp](main/main.cpp) and update:
```cpp
#define NATS_SERVER    "demo.nats.io"  // or your server hostname/IP
#define NATS_PORT      4222
```

**For Local Docker NATS Server:**
```cpp
// If ESP32 and Docker are on same machine (macOS/Windows):
#define NATS_SERVER    "host.docker.internal"
#define NATS_PORT      4222

// Or use your machine's local IP:
#define NATS_SERVER    "192.168.1.100"  // Your machine's IP
#define NATS_PORT      4222
```

📖 **See [DOCKER.md](DOCKER.md) for complete Docker setup instructions.**

## Quick Start with Docker (Recommended)

The easiest way to test is using the local Docker NATS server:

```bash
cd /Users/debsahu/Workspace/nats-test-app

# 1. Start local NATS server
make nats-up

# 2. Update main/main.cpp with your WiFi credentials and use "host.docker.internal" or your IP

# 3. Build and flash
make build
make flash-monitor
```

## Building

```bash
cd /Users/debsahu/Workspace/nats-test-app

# Configure the project (first time only)
idf.py menuconfig

# Build the project
idf.py build
# Or use: make build
```

## Flashing and Running

```bash
# Flash to ESP32 (replace /dev/ttyUSB0 with your port)
idf.py -p /dev/ttyUSB0 flash

# Monitor output
idf.py -p /dev/ttyUSB0 monitor

# Or combine flash and monitor
idf.py -p /dev/ttyUSB0 flash monitor
```

## Expected Output

```
╔════════════════════════════════════════════════╗
║   ESP-IDF NATS Client Test Application        ║
║   Testing Thread Safety & Deadlock Prevention  ║
╚════════════════════════════════════════════════╝

I (xxx) nats_test: WiFi connected, IP: 192.168.1.xxx
I (xxx) nats_test: Connecting to NATS server at demo.nats.io:4222...
I (xxx) nats_test: ✅ Connected to NATS server

=== Test 1: Basic Pub/Sub ===
...

=== Test 2: Callback Publishing (Deadlock Prevention) ===
I (xxx) nats_test: 📨 PING received, sending PONG
I (xxx) nats_test: 📨 PONG received: PONG
I (xxx) nats_test: ✅ Test 2 completed (no deadlock)

=== Test 3: Request/Reply ===
...

=== Test 4: Thread Safety (Concurrent Publishing) ===
...

=== Test 5: Connection Metrics ===
📊 NATS Connection Metrics:
  Messages sent: 50
  Messages received: 45
  Bytes sent: 1234
  Bytes received: 987
  Reconnections: 0
  Uptime: 12345 ms

✅ All tests completed
```

## What to Watch For

### Success Indicators
- ✅ No crashes or reboots
- ✅ All messages are sent and received
- ✅ Callbacks execute without deadlock
- ✅ Multiple tasks publish without crashes
- ✅ Metrics match expected values

### Failure Indicators
- ❌ Task watchdog timeouts (deadlock)
- ❌ Crashes in FreeRTOS (memory corruption)
- ❌ Missing messages (race conditions)
- ❌ Assertion failures (mutex errors)

## Troubleshooting

### WiFi Connection Issues
- Check SSID and password are correct
- Ensure WiFi network is 2.4GHz (ESP32 doesn't support 5GHz)
- Check WiFi signal strength

### NATS Connection Issues
- Verify NATS server is running and accessible
- Check firewall rules (port 4222)
- Try `demo.nats.io` for public demo server
- Check DNS resolution if using hostname

### Build Issues
- Ensure ESP-IDF is properly installed and sourced
- Check ESP-IDF version is 4.4 or later
- Verify component symlink is correct: `ls -la components/`

### Runtime Issues
- Check ESP-IDF monitor for stack traces
- Increase task stack sizes if seeing stack overflow
- Enable verbose logging in espidf-nats

## Component Structure

```
nats-test-app/
├── CMakeLists.txt              # Main project CMakeLists
├── Makefile                    # Convenient make commands
├── sdkconfig.defaults          # Default configuration
├── docker-compose.yml          # Docker NATS servers
├── README.md                   # This file
├── DOCKER.md                   # Docker setup guide
├── components/
│   └── espidf-nats/           # Symlink to ../espidf-nats
├── docker/
│   ├── config/                # NATS server configurations
│   ├── certs/                 # TLS certificates (auto-generated)
│   └── scripts/               # Helper scripts
└── main/
    ├── CMakeLists.txt          # Main component CMakeLists
    └── main.cpp                # Test application code
```

## Next Steps

After running these tests successfully:

1. **Extend Tests**: Add your own test scenarios
2. **TLS Testing**: Add TLS connection tests
3. **JetStream**: Test JetStream publish with ACK
4. **Headers**: Test NATS 2.0 headers (HPUB/HMSG)
5. **Stress Testing**: Increase message volume and concurrent tasks
6. **Reconnection**: Test disconnect/reconnect scenarios

## Component Source

The espidf-nats component source is at:
- **GitHub**: https://github.com/daed/espidf-nats
- **Local**: [../espidf-nats/](../espidf-nats/)
- **Documentation**: [../espidf-nats/README.md](../espidf-nats/README.md)

## License

This test application is MIT licensed, same as the espidf-nats component.
