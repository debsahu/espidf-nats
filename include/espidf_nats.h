#ifndef ESPIDF_NATS_H
#define ESPIDF_NATS_H

/**
 * ESP-IDF NATS Client Library
 *
 * A header-only C++ library for ESP32 to communicate with NATS messaging servers.
 *
 * Features:
 * - TLS/SSL support with server certificate validation and mutual TLS (mTLS)
 * - DNS resolution - Connect using hostnames, not just IP addresses
 * - Multiple server URLs with automatic failover
 * - NATS 2.0 Headers support (HPUB/HMSG)
 * - Request timeouts - Prevent hanging requests
 * - Async/non-blocking API
 * - Comprehensive JetStream support (streams, consumers, pull, ACK controls, deduplication)
 * - Key-Value Store (distributed config/state with revision history, TTL, watchers)
 * - Ordered Consumers (guaranteed in-order delivery)
 * - Fetch Operations (efficient batch message retrieval)
 * - Account Info (quota and resource monitoring)
 * - Connection metrics tracking
 * - Detailed error codes
 * - Message buffering for offline support
 * - Exponential backoff reconnection
 * - Flush and drain for graceful shutdown
 *
 * This library is modularized into separate headers for better maintainability:
 * - config.h: Configuration defines and constants
 * - util.h: Utility classes (Array, Queue, MillisTimer)
 * - types.h: Type definitions (structs, enums)
 * - subscription.h: Subscription management
 * - nats_client.h: Main NATS client class
 */

// Include all modular headers
#include "espidf_nats/config.h"
#include "espidf_nats/util.h"
#include "espidf_nats/types.h"
#include "espidf_nats/subscription.h"
#include "espidf_nats/nats_client.h"

#endif // ESPIDF_NATS_H
