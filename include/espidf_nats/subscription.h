#ifndef ESPIDF_NATS_SUBSCRIPTION_H
#define ESPIDF_NATS_SUBSCRIPTION_H

#include "util.h"

// Forward declaration for message structure
struct nats_msg_t {
    const char* subject;
    const int sid;
    const char* reply;
    const char* headers;
    const int header_size;
    const char* data;
    const int size;
};

// Callback types
typedef void (*sub_cb)(nats_msg_t e);
typedef void (*timeout_cb)();

/**
 * Subscription management class
 * Tracks message count, limits, and timeouts for each subscription
 */
class Sub {
    public:
        sub_cb cb;                          // Callback function
        int received;                       // Messages received count
        int max_wanted;                     // Maximum messages to receive (0 = unlimited)
        unsigned long timeout_ms;           // Timeout in milliseconds (0 = no timeout)
        unsigned long request_time;         // Time when subscription was created
        timeout_cb on_timeout;              // Timeout callback

        /**
         * Create a new subscription
         * @param cb Callback function for received messages
         * @param max_wanted Maximum messages to receive (0 for unlimited)
         * @param timeout_ms Timeout in milliseconds (0 for no timeout)
         * @param on_timeout Callback when timeout occurs
         */
        Sub(sub_cb cb, int max_wanted = 0, unsigned long timeout_ms = 0, timeout_cb on_timeout = NULL) :
            cb(cb), received(0), max_wanted(max_wanted),
            timeout_ms(timeout_ms), request_time(NATSUtil::millis()), on_timeout(on_timeout) {}

        /**
         * Invoke callback with message
         */
        void call(nats_msg_t& e) {
            received++;
            cb(e);
        }

        /**
         * Check if maximum messages received
         */
        bool maxed() {
            return (max_wanted == 0)? false : received >= max_wanted;
        }

        /**
         * Check if subscription has timed out
         */
        bool timed_out() {
            if (timeout_ms == 0) return false;
            return (NATSUtil::millis() - request_time) > timeout_ms;
        }
};

#endif // ESPIDF_NATS_SUBSCRIPTION_H
