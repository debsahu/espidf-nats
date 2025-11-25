# NATS Test App - Quick Start Guide

## 🚀 One-Line Setup

```bash
make nats-up && make build && make flash-monitor
```

## 📋 Step-by-Step Setup

### 1. Start NATS Server
```bash
make nats-up          # Starts all NATS servers
# or
make nats-basic       # Just basic NATS (port 4222)
```

### 2. Configure WiFi & NATS
Edit `main/main.cpp`:
```cpp
#define WIFI_SSID      "YourWiFiName"
#define WIFI_PASS      "YourPassword"
#define NATS_SERVER    "host.docker.internal"  // or your machine's IP
#define NATS_PORT      4222
```

### 3. Build & Flash
```bash
make build            # Build project
make flash            # Flash to ESP32
make monitor          # Open serial monitor
# or
make flash-monitor    # Flash and monitor in one command
```

## 🎯 Common Commands

### Docker NATS Servers
```bash
make nats-up          # Start all NATS servers
make nats-down        # Stop all NATS servers
make nats-status      # Check status
make nats-logs        # View logs
make nats-basic       # Start basic NATS only (4222)
make nats-jetstream   # Start JetStream NATS (4223)
make nats-tls         # Start TLS NATS (4224)
```

### Build & Flash
```bash
make build            # Build the project
make flash            # Flash to ESP32
make monitor          # Open serial monitor
make flash-monitor    # Flash and monitor
make clean            # Clean build artifacts
```

### Complete Testing Workflow
```bash
make test-local       # Start NATS + flash + monitor (interactive)
```

## 🌐 NATS Server Ports

| Server      | Client Port | Monitor URL              |
|-------------|-------------|--------------------------|
| Basic       | 4222        | http://localhost:8222    |
| JetStream   | 4223        | http://localhost:8223    |
| TLS         | 4224        | http://localhost:8224    |

## 🔧 Troubleshooting

### Can't connect to NATS?
```bash
# Check NATS is running
make nats-status

# View NATS logs
make nats-logs

# Find your machine's IP
ifconfig | grep "inet "

# Update NATS_SERVER in main.cpp to your IP
```

### Port conflicts?
```bash
# Stop existing NATS servers
make nats-down

# Check what's using port 4222
lsof -i :4222

# Restart
make nats-up
```

### Build issues?
```bash
# Source ESP-IDF environment
. $HOME/esp/esp-idf/export.sh

# Clean and rebuild
make clean
make build
```

## 📊 What Tests Run?

1. **Basic Pub/Sub** - Simple message passing
2. **Callback Publishing** - Tests deadlock prevention
3. **Request/Reply** - Service pattern
4. **Thread Safety** - Concurrent access from multiple tasks
5. **Connection Metrics** - Statistics tracking

## ✅ Success Indicators

Watch for these in serial monitor:
```
✅ Connected to NATS server
✅ Test 1 completed
✅ Test 2 completed (no deadlock)
✅ Test 3 completed
✅ Test 4 completed
✅ Test 5 completed
```

## ❌ Failure Indicators

- Task watchdog timeout → Deadlock issue
- Guru Meditation Error → Memory corruption
- Missing messages → Race condition
- Connection refused → NATS server not running or wrong IP

## 📚 More Information

- **Full Documentation**: [README.md](README.md)
- **Docker Setup**: [DOCKER.md](DOCKER.md)
- **Component Docs**: [../espidf-nats/README.md](../espidf-nats/README.md)

## 🆘 Quick Help

```bash
make help             # Show all make commands
make nats-status      # Check NATS status
curl http://localhost:8222/varz  # NATS server info
```

## 💡 Pro Tips

1. **Use Local NATS**: Much faster than public demo server
2. **Check Logs First**: `make nats-logs` when connection fails
3. **Monitor NATS**: Open http://localhost:8222 in browser
4. **Test with CLI**: `nats sub ">" --server=localhost:4222`
5. **IP vs Hostname**: Use IP address if `host.docker.internal` doesn't work

---

**Need more help?** See [README.md](README.md) or [DOCKER.md](DOCKER.md)
