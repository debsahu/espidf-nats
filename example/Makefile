# Makefile for NATS Test Application
# Provides convenient commands for Docker NATS servers and ESP-IDF build/flash

.PHONY: help nats-up nats-down nats-logs nats-status certs-gen build flash monitor clean

# Default target
help:
	@echo "╔════════════════════════════════════════════════════════════╗"
	@echo "║       NATS Test Application - Make Commands               ║"
	@echo "╚════════════════════════════════════════════════════════════╝"
	@echo ""
	@echo "Docker NATS Servers:"
	@echo "  make nats-up          - Start all NATS servers"
	@echo "  make nats-basic       - Start basic NATS server only (port 4222)"
	@echo "  make nats-jetstream   - Start JetStream NATS server (port 4223)"
	@echo "  make nats-tls         - Start TLS NATS server (port 4224)"
	@echo "  make nats-down        - Stop all NATS servers"
	@echo "  make nats-logs        - Show logs from all NATS servers"
	@echo "  make nats-status      - Show status of NATS servers"
	@echo "  make certs-gen        - Generate TLS certificates"
	@echo ""
	@echo "ESP-IDF Build/Flash:"
	@echo "  make build            - Build the ESP-IDF project"
	@echo "  make flash            - Flash to ESP32"
	@echo "  make monitor          - Open serial monitor"
	@echo "  make flash-monitor    - Flash and monitor"
	@echo "  make clean            - Clean build artifacts"
	@echo ""
	@echo "Testing:"
	@echo "  make test-local       - Run with local NATS (start server + flash + monitor)"
	@echo ""
	@echo "Default NATS ports:"
	@echo "  - Basic NATS:     localhost:4222"
	@echo "  - JetStream NATS: localhost:4223"
	@echo "  - TLS NATS:       localhost:4224"
	@echo "  - Monitoring:     http://localhost:8222, 8223, 8224"
	@echo ""

# Docker NATS Server Commands (using modern 'docker compose' syntax)
nats-up:
	@echo "🚀 Starting all NATS servers..."
	docker compose up -d
	@echo "✅ NATS servers started"
	@echo ""
	@make nats-status

nats-basic:
	@echo "🚀 Starting basic NATS server..."
	docker compose up -d nats-basic
	@sleep 2
	@docker compose ps nats-basic

nats-jetstream:
	@echo "🚀 Starting JetStream NATS server..."
	docker compose up -d nats-jetstream
	@sleep 2
	@docker compose ps nats-jetstream

nats-tls:
	@echo "🚀 Starting TLS NATS server (generating certificates first)..."
	docker compose up -d generate-certs
	@sleep 2
	docker compose up -d nats-tls
	@sleep 2
	@docker compose ps nats-tls

nats-down:
	@echo "🛑 Stopping all NATS servers..."
	docker compose down
	@echo "✅ NATS servers stopped"

nats-logs:
	docker compose logs -f

nats-status:
	@echo "📊 NATS Server Status:"
	@docker compose ps
	@echo ""
	@echo "🌐 Monitoring URLs:"
	@echo "  - Basic NATS:     http://localhost:8222"
	@echo "  - JetStream NATS: http://localhost:8223"
	@echo "  - TLS NATS:       http://localhost:8224"

certs-gen:
	@echo "🔐 Generating TLS certificates..."
	docker compose up generate-certs
	@echo "✅ Certificates generated in docker/certs/"
	@ls -lh docker/certs/

# ESP-IDF Commands
build:
	@echo "🔨 Building ESP-IDF project..."
	idf.py build

flash:
	@echo "⚡ Flashing to ESP32..."
	idf.py flash

monitor:
	@echo "📺 Starting serial monitor..."
	idf.py monitor

flash-monitor:
	@echo "⚡ Flashing and monitoring..."
	idf.py flash monitor

clean:
	@echo "🧹 Cleaning build artifacts..."
	idf.py fullclean
	@echo "✅ Clean complete"

# Combined testing workflow
test-local: nats-basic
	@echo ""
	@echo "✅ NATS server running at localhost:4222"
	@echo "📝 Update main/main.cpp to use: localhost or host.docker.internal"
	@echo ""
	@echo "Press Enter when ready to flash and monitor..."
	@read dummy
	@make flash-monitor

# Check if Docker is running
docker-check:
	@docker info > /dev/null 2>&1 || (echo "❌ Docker is not running" && exit 1)
