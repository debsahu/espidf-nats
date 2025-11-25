# Docker Setup for NATS Testing

This directory contains Docker Compose configuration to run NATS servers locally for testing the ESP-IDF NATS client library.

## Quick Start

```bash
# Start all NATS servers
make nats-up

# Or start specific servers
make nats-basic        # Basic NATS (port 4222)
make nats-jetstream    # JetStream enabled (port 4223)
make nats-tls          # TLS/SSL enabled (port 4224)

# Stop all servers
make nats-down

# View logs
make nats-logs

# Check status
make nats-status
```

## Available NATS Servers

### 1. Basic NATS Server (nats-basic)

**Port:** 4222
**Monitoring:** http://localhost:8222

Simple NATS server for basic pub/sub testing. No authentication, no TLS.

```bash
# Start
docker-compose up -d nats-basic

# Or use make
make nats-basic
```

**Connect from ESP32:**
```cpp
NATS nats("host.docker.internal", 4222);  // If ESP32 on same machine
// or
NATS nats("192.168.1.100", 4222);         // Your machine's IP
```

### 2. JetStream NATS Server (nats-jetstream)

**Port:** 4223
**Monitoring:** http://localhost:8223

NATS server with JetStream enabled for guaranteed message delivery testing.

```bash
# Start
docker-compose up -d nats-jetstream

# Or use make
make nats-jetstream
```

**Connect from ESP32:**
```cpp
NATS nats("host.docker.internal", 4223);
```

**Test JetStream:**
```cpp
nats.jetstream_publish(
    "orders",
    "{\"id\":123}",
    [](NATS::msg e) {
        ESP_LOGI(TAG, "ACK: %.*s", e.size, e.data);
    }
);
```

### 3. TLS NATS Server (nats-tls)

**Port:** 4224
**Monitoring:** http://localhost:8224

NATS server with TLS/SSL encryption for secure connection testing.

```bash
# Generate certificates (first time only)
make certs-gen

# Start TLS server
docker-compose up -d nats-tls

# Or use make (auto-generates certs)
make nats-tls
```

**Connect from ESP32:**
```cpp
// Load CA certificate
extern const uint8_t ca_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t ca_cert_pem_end[] asm("_binary_ca_cert_pem_end");

nats_tls_config_t tls_config = {
    .enabled = true,
    .ca_cert = (const char*)ca_cert_pem_start,
    .ca_cert_len = ca_cert_pem_end - ca_cert_pem_start,
    .client_cert = NULL,
    .client_cert_len = 0,
    .client_key = NULL,
    .client_key_len = 0,
    .skip_cert_verification = false,
    .server_name = "localhost"
};

NATS nats("host.docker.internal", 4224, NULL, NULL, &tls_config);
```

## TLS Certificates

Certificates are automatically generated when starting the TLS server for the first time.

### Manual Certificate Generation

```bash
make certs-gen
```

Generated certificates are stored in `docker/certs/`:
- `ca-cert.pem` - CA certificate (embed in ESP32 firmware)
- `ca-key.pem` - CA private key
- `server-cert.pem` - Server certificate
- `server-key.pem` - Server private key
- `client-cert.pem` - Client certificate (for mTLS)
- `client-key.pem` - Client private key (for mTLS)

### Using Certificates in ESP-IDF

Add to your `CMakeLists.txt`:
```cmake
target_add_binary_data(${COMPONENT_TARGET} "../docker/certs/ca-cert.pem" TEXT)
```

## Connecting ESP32 to Docker NATS

### Method 1: Using host.docker.internal (macOS/Windows)

If your ESP32 and computer are on the same network:
```cpp
NATS nats("host.docker.internal", 4222);
```

### Method 2: Using Host Machine IP

Find your machine's IP address:
```bash
# macOS/Linux
ifconfig | grep "inet "

# Windows
ipconfig
```

Then use the IP in your ESP32 code:
```cpp
NATS nats("192.168.1.100", 4222);  // Your machine's IP
```

### Method 3: Docker Host Network (Linux only)

Modify `docker-compose.yml`:
```yaml
services:
  nats-basic:
    network_mode: host
```

Then connect to `localhost:4222` from ESP32.

## Monitoring NATS Servers

### HTTP Monitoring Endpoints

Each NATS server exposes an HTTP monitoring endpoint:

- **Basic NATS:** http://localhost:8222
- **JetStream NATS:** http://localhost:8223
- **TLS NATS:** http://localhost:8224

**Available endpoints:**
- `/varz` - General server information
- `/connz` - Connection information
- `/routez` - Routing information
- `/subsz` - Subscription information
- `/gatewayz` - Gateway information
- `/jsz` - JetStream information (JetStream server only)

Example:
```bash
curl http://localhost:8222/varz
curl http://localhost:8223/jsz
```

### Using NATS CLI

Install NATS CLI:
```bash
# macOS
brew install nats-io/nats-tools/nats

# Or download from https://github.com/nats-io/natscli
```

Connect to server:
```bash
# Basic server
nats server info --server=localhost:4222

# JetStream server
nats server info --server=localhost:4223

# Monitor messages
nats sub ">" --server=localhost:4222
```

## Docker Compose Commands

```bash
# Start all services
docker-compose up -d

# Start specific service
docker-compose up -d nats-basic

# View logs
docker-compose logs -f
docker-compose logs -f nats-basic

# Check status
docker-compose ps

# Stop all services
docker-compose down

# Stop and remove volumes
docker-compose down -v

# Restart a service
docker-compose restart nats-basic
```

## Troubleshooting

### Port Already in Use

If ports 4222, 4223, or 4224 are already in use, modify `docker-compose.yml`:
```yaml
ports:
  - "14222:4222"  # Use different external port
```

### ESP32 Can't Connect

1. **Check Docker is running:** `docker ps`
2. **Check NATS is running:** `make nats-status`
3. **Verify IP address:** Ping from ESP32 to host machine
4. **Check firewall:** Ensure ports 4222-4224 are open
5. **Test with NATS CLI:** `nats server info --server=localhost:4222`

### Certificate Issues

Regenerate certificates:
```bash
rm -rf docker/certs/*
make certs-gen
docker-compose restart nats-tls
```

### JetStream Issues

Check JetStream status:
```bash
curl http://localhost:8223/jsz
```

Reset JetStream data:
```bash
docker-compose down -v
docker-compose up -d nats-jetstream
```

## Architecture

```
┌─────────────────┐
│   ESP32 Device  │
│  (NATS Client)  │
└────────┬────────┘
         │
         │ WiFi
         │
┌────────▼────────────────────────────┐
│      Host Machine Network           │
│  ┌────────────────────────────────┐ │
│  │  Docker Bridge Network         │ │
│  │  ┌──────────┐  ┌──────────┐   │ │
│  │  │  nats-   │  │  nats-   │   │ │
│  │  │  basic   │  │jetstream │   │ │
│  │  │  :4222   │  │  :4223   │   │ │
│  │  └──────────┘  └──────────┘   │ │
│  │  ┌──────────┐                 │ │
│  │  │  nats-   │                 │ │
│  │  │   tls    │                 │ │
│  │  │  :4224   │                 │ │
│  │  └──────────┘                 │ │
│  └────────────────────────────────┘ │
└─────────────────────────────────────┘
```

## Advanced Configuration

### Custom NATS Configuration

Edit configuration files in `docker/config/`:
- `nats-basic.conf`
- `nats-jetstream.conf`
- `nats-tls.conf`

Then rebuild:
```bash
docker-compose down
docker-compose up -d
```

### Adding Authentication

Edit `docker/config/nats-basic.conf`:
```conf
authorization {
    users = [
        {user: "esp32", password: "password123"}
    ]
}
```

Restart:
```bash
docker-compose restart nats-basic
```

Connect from ESP32:
```cpp
NATS nats("host.docker.internal", 4222, "esp32", "password123");
```

### Clustering Multiple NATS Servers

See `docker-compose.yml` for cluster configuration examples.

## Resources

- **NATS Documentation:** https://docs.nats.io/
- **NATS Docker Hub:** https://hub.docker.com/_/nats
- **NATS Server Config:** https://docs.nats.io/running-a-nats-service/configuration
- **JetStream Guide:** https://docs.nats.io/nats-concepts/jetstream
- **TLS Configuration:** https://docs.nats.io/running-a-nats-service/configuration/securing_nats/tls
