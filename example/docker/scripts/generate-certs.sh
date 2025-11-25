#!/bin/sh
#
# Generate self-signed TLS certificates for NATS testing
# This script creates a CA certificate and server certificate for TLS testing
#

set -e

CERTS_DIR="/certs"
DAYS_VALID=365

echo "🔐 Generating TLS certificates for NATS testing..."

# Check if certificates already exist
if [ -f "$CERTS_DIR/ca-cert.pem" ] && \
   [ -f "$CERTS_DIR/server-cert.pem" ] && \
   [ -f "$CERTS_DIR/server-key.pem" ]; then
    echo "✅ Certificates already exist, skipping generation"
    exit 0
fi

# Install OpenSSL
apk add --no-cache openssl

cd "$CERTS_DIR"

# 1. Generate CA private key
echo "📝 Generating CA private key..."
openssl genrsa -out ca-key.pem 4096

# 2. Generate CA certificate
echo "📝 Generating CA certificate..."
openssl req -new -x509 -days $DAYS_VALID -key ca-key.pem -out ca-cert.pem \
    -subj "/C=US/ST=State/L=City/O=Test/OU=Testing/CN=NATS-Test-CA"

# 3. Generate server private key
echo "📝 Generating server private key..."
openssl genrsa -out server-key.pem 4096

# 4. Generate server certificate signing request
echo "📝 Generating server CSR..."
openssl req -new -key server-key.pem -out server.csr \
    -subj "/C=US/ST=State/L=City/O=Test/OU=Testing/CN=localhost"

# 5. Create OpenSSL extensions config for SAN
# Include common local network IPs for testing
cat > server-ext.cnf <<EOF
subjectAltName = @alt_names
extendedKeyUsage = serverAuth

[alt_names]
DNS.1 = localhost
DNS.2 = nats-tls
DNS.3 = nats-server
DNS.4 = *.local
IP.1 = 127.0.0.1
IP.2 = ::1
# Add your server's IP address here for TLS to work
# IP.3 = 192.168.x.x
EOF

# 6. Generate server certificate signed by CA
echo "📝 Generating server certificate..."
openssl x509 -req -days $DAYS_VALID -in server.csr \
    -CA ca-cert.pem -CAkey ca-key.pem -CAcreateserial \
    -out server-cert.pem -extfile server-ext.cnf

# 7. Generate client certificates for mutual TLS (mTLS) testing
echo "📝 Generating client private key..."
openssl genrsa -out client-key.pem 4096

echo "📝 Generating client CSR..."
openssl req -new -key client-key.pem -out client.csr \
    -subj "/C=US/ST=State/L=City/O=Test/OU=Testing/CN=nats-client"

echo "📝 Generating client certificate..."
openssl x509 -req -days $DAYS_VALID -in client.csr \
    -CA ca-cert.pem -CAkey ca-key.pem -CAcreateserial \
    -out client-cert.pem

# Clean up temporary files
rm -f server.csr client.csr server-ext.cnf ca-cert.srl

# Set permissions
chmod 644 *.pem
chmod 600 *-key.pem

echo ""
echo "✅ Certificate generation complete!"
echo ""
echo "📁 Generated certificates:"
echo "   - CA Certificate: ca-cert.pem"
echo "   - CA Key: ca-key.pem"
echo "   - Server Certificate: server-cert.pem"
echo "   - Server Key: server-key.pem"
echo "   - Client Certificate: client-cert.pem (for mTLS)"
echo "   - Client Key: client-key.pem (for mTLS)"
echo ""
echo "📋 Certificate info:"
openssl x509 -in ca-cert.pem -noout -subject -dates
openssl x509 -in server-cert.pem -noout -subject -dates
echo ""
