#ifndef ESPIDF_NATS_SUBSCRIPTION_H
#define ESPIDF_NATS_SUBSCRIPTION_H

#include "util.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

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
 * Uses reference counting to prevent use-after-free in callbacks (Issues #1, #7)
 * Stores subject/queue for reconnection restoration (Issues #39, #55)
 */
class Sub {
    public:
        sub_cb cb;                          // Callback function
        int received;                       // Messages received count
        int max_wanted;                     // Maximum messages to receive (0 = unlimited)
        unsigned long timeout_ms;           // Timeout in milliseconds (0 = no timeout)
        unsigned long request_time;         // Time when subscription was created
        timeout_cb on_timeout;              // Timeout callback
        char* subject;                      // Subject for reconnection restoration (owned)
        char* queue;                        // Queue group for reconnection restoration (owned, nullable)

    private:
        volatile int ref_count;             // Reference count for safe callback execution (atomic)
        volatile bool marked_for_deletion;  // Flag to mark subscription for lazy deletion (atomic)

    public:
        /**
         * Create a new subscription
         * @param cb Callback function for received messages
         * @param max_wanted Maximum messages to receive (0 for unlimited)
         * @param timeout_ms Timeout in milliseconds (0 for no timeout)
         * @param on_timeout Callback when timeout occurs
         * @param subj Subject to subscribe to (will be copied)
         * @param q Queue group (will be copied, can be NULL)
         */
        Sub(sub_cb cb, int max_wanted = 0, unsigned long timeout_ms = 0, timeout_cb on_timeout = NULL,
            const char* subj = NULL, const char* q = NULL) :
            cb(cb), received(0), max_wanted(max_wanted),
            timeout_ms(timeout_ms), request_time(NATSUtil::millis()), on_timeout(on_timeout),
            subject(NULL), queue(NULL),
            ref_count(1), marked_for_deletion(false) {
            // Deep copy subject and queue for reconnection restoration
            if (subj != NULL) {
                subject = strdup(subj);
            }
            if (q != NULL) {
                queue = strdup(q);
            }
        }

        ~Sub() {
            // Free owned strings
            if (subject != NULL) {
                free(subject);
                subject = NULL;
            }
            if (queue != NULL) {
                free(queue);
                queue = NULL;
            }
        }

        /**
         * Add a reference (call before invoking callback)
         * Uses atomic increment for thread safety
         */
        void add_ref() {
            __atomic_fetch_add(&ref_count, 1, __ATOMIC_SEQ_CST);
        }

        /**
         * Release a reference (call after callback returns)
         * Uses atomic decrement for thread safety
         * @return true if ref_count is now 0 and object can be deleted
         */
        bool release() {
            int prev = __atomic_fetch_sub(&ref_count, 1, __ATOMIC_SEQ_CST);
            return prev <= 1;  // prev was the value before decrement
        }

        /**
         * Get current reference count (atomic load)
         */
        int get_ref_count() const {
            return __atomic_load_n(&ref_count, __ATOMIC_SEQ_CST);
        }

        /**
         * Mark subscription for deletion (will be deleted when ref_count reaches 0)
         * Uses atomic store for thread safety
         */
        void mark_for_deletion() {
            __atomic_store_n(&marked_for_deletion, true, __ATOMIC_SEQ_CST);
        }

        /**
         * Check if subscription is marked for deletion (atomic load)
         */
        bool is_marked_for_deletion() const {
            return __atomic_load_n(&marked_for_deletion, __ATOMIC_SEQ_CST);
        }

        /**
         * Check if subscription can be safely deleted (atomic reads)
         */
        bool can_delete() const {
            return __atomic_load_n(&marked_for_deletion, __ATOMIC_SEQ_CST) &&
                   __atomic_load_n(&ref_count, __ATOMIC_SEQ_CST) <= 1;
        }

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
         * Returns false if already marked for deletion (response was received)
         */
        bool timed_out() {
            if (timeout_ms == 0) return false;
            if (marked_for_deletion) return false;  // Already handled
            return (NATSUtil::millis() - request_time) > timeout_ms;
        }

        /**
         * Clear the timeout (call when response is received)
         */
        void clear_timeout() {
            timeout_ms = 0;
        }
};

#endif // ESPIDF_NATS_SUBSCRIPTION_H
