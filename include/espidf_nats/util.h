#ifndef ESPIDF_NATS_UTIL_H
#define ESPIDF_NATS_UTIL_H

#include <sys/time.h>
#include <esp_random.h>
#include <stdlib.h>

namespace NATSUtil {

    static const char alphanums[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

    /**
     * Securely zero memory to prevent credential disclosure
     * Uses volatile to prevent compiler optimization
     */
    inline void secure_zero(void* ptr, size_t len) {
        if (ptr == NULL || len == 0) return;
        volatile unsigned char* p = (volatile unsigned char*)ptr;
        while (len--) {
            *p++ = 0;
        }
    }

    /**
     * Securely zero a C string before freeing
     * Prevents credential disclosure from freed memory
     */
    inline void secure_free(char* str) {
        if (str == NULL) return;
        secure_zero(str, strlen(str));
        free(str);
    }

    /**
     * Get current time in milliseconds
     */
    inline unsigned long millis() {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        return (tv.tv_sec * 1000UL) + (tv.tv_usec / 1000UL);
    }

    /**
     * Generate random number from 0 to max-1
     * Uses rejection sampling to avoid modulo bias (Issue #17)
     */
    inline int random(int max) {
        if (max <= 0) return 0;
        if (max == 1) return 0;

        // Calculate threshold to reject biased values
        // We reject values >= (UINT32_MAX - (UINT32_MAX % max) + 1) when that doesn't wrap
        uint32_t threshold = UINT32_MAX - (UINT32_MAX % (uint32_t)max);

        uint32_t r;
        do {
            r = esp_random();
        } while (r >= threshold);  // Rejection sampling

        return (int)(r % (uint32_t)max);
    }

    /**
     * Interval timer for periodic tasks
     */
    class MillisTimer {
        const unsigned long interval;
        unsigned long t;
        public:
        MillisTimer(const unsigned long interval) :
            interval(interval), t(millis()) {}

        bool process() {
            unsigned long ms = millis();
            if (ms < t || (ms - t) > interval) {
                t = ms;
                return true;
            }
            return false;
        }
    };

    /**
     * Dynamic array with automatic resizing
     */
    template <typename T>
    class Array {
        private:
            T* data;
            size_t len;
            size_t cap;

        public:
            Array(size_t cap = 32) : len(0), cap(cap) {
                data = (T*)malloc(cap * sizeof(T));
                if (data == NULL) {
                    ESP_LOGE("Array", "Failed to allocate array");
                    this->cap = 0;
                }
            }

            ~Array() { free(data); }

        private:
            void resize() {
                if (cap == 0) cap = 1;
                else {
                    // Check for overflow before doubling capacity
                    if (cap > SIZE_MAX / (2 * sizeof(T))) {
                        // Would overflow - keep current capacity
                        ESP_LOGE("Array", "Array resize would overflow, keeping current size");
                        return;
                    }
                    cap *= 2;
                }

                T* new_data = (T*)realloc(data, cap * sizeof(T));
                if (new_data == NULL) {
                    // Realloc failed - restore old capacity and keep old data
                    ESP_LOGE("Array", "Failed to resize array from %zu to %zu elements", len, cap);
                    cap /= 2;  // Restore old capacity
                    if (cap == 0) cap = 1;
                    return;
                }
                data = new_data;
            }

        public:
            size_t size() const { return len; }

            void erase(size_t idx) {
                if (idx >= len) return;  // Bounds check
                for (size_t i = idx; i < len - 1; i++) {
                    data[i] = data[i+1];
                }
                len--;
            }

            void empty() {
                len = 0;
                cap = 32;
                free(data);
                data = NULL;  // Set to NULL before malloc to avoid dangling pointer
                data = (T*)malloc(cap * sizeof(T));
                if (data == NULL) {
                    ESP_LOGE("Array", "Failed to allocate array in empty()");
                    cap = 0;
                }
            }

            T const& operator[](size_t i) const { return data[i]; }

            T& operator[](size_t i) {
                while (i >= cap) resize();
                return data[i];
            }

            size_t push_back(T v) {
                size_t i = len++;
                if (len > cap) resize();
                data[i] = v;
                return i;
            }

            T* ptr() { return data; }
    };

    /**
     * Simple queue (FIFO) for message buffering and ID recycling
     */
    template <typename T>
    class Queue {
        private:
            class Node {
                public:
                    T data;
                    Node* next;
                    Node(T data, Node* next = NULL) : data(data), next(next) {}
            };
            Node* head;  // Front of queue (for dequeue)
            Node* tail;  // Back of queue (for enqueue)
            size_t len;

        public:
            Queue() : head(NULL), tail(NULL), len(0) {}

            ~Queue() {
                Node* tmp;
                Node* n = head;
                while (n != NULL) {
                    tmp = n->next;
                    delete n;  // Fix: Use delete for objects created with new
                    n = tmp;
                }
            }

            bool empty() const { return head == NULL; }
            size_t size() const { return len; }

            void push(T data) {
                Node* newNode = new Node(data, NULL);
                if (tail != NULL) {
                    tail->next = newNode;
                } else {
                    head = newNode;
                }
                tail = newNode;
                len++;
            }

            T pop() {
                if (head == NULL) {
                    // Should not happen if caller checks empty()
                    return T();
                }
                Node* oldHead = head;
                T data = oldHead->data;
                head = oldHead->next;
                if (head == NULL) {
                    tail = NULL;
                }
                delete oldHead;  // Fix: Use delete for objects created with new
                len--;
                return data;
            }

            T peek() {
                if (head != NULL) {
                    return head->data;
                }
                return T();
            }
    };

}; // namespace NATSUtil

#endif // ESPIDF_NATS_UTIL_H
