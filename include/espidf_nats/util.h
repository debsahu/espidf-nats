#ifndef ESPIDF_NATS_UTIL_H
#define ESPIDF_NATS_UTIL_H

#include <sys/time.h>
#include <esp_random.h>
#include <stdlib.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_log.h>

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

    /**
     * Thread-safe ring buffer for WebSocket event bridging
     *
     * Used to bridge event-based WebSocket callbacks to the polling-based
     * NATS client model. The producer (WebSocket event handler) writes
     * to the buffer, and the consumer (read_line/read_bytes) reads from it.
     */
    class RingBuffer {
    private:
        static constexpr const char* TAG = "RingBuffer";

        uint8_t* buffer;
        size_t capacity;
        volatile size_t head;  // Write position (producer)
        volatile size_t tail;  // Read position (consumer)
        SemaphoreHandle_t mutex;
        SemaphoreHandle_t data_available;  // Signaled when data is written

    public:
        /**
         * Create a ring buffer with specified capacity
         * @param cap Buffer capacity in bytes (default: 8192)
         */
        RingBuffer(size_t cap = 8192) : capacity(cap), head(0), tail(0) {
            buffer = (uint8_t*)malloc(cap);
            mutex = xSemaphoreCreateMutex();
            data_available = xSemaphoreCreateBinary();

            if (buffer == NULL || mutex == NULL || data_available == NULL) {
                ESP_LOGE(TAG, "Failed to allocate resources");
                if (buffer) { free(buffer); buffer = NULL; }
                if (mutex) { vSemaphoreDelete(mutex); mutex = NULL; }
                if (data_available) { vSemaphoreDelete(data_available); data_available = NULL; }
                capacity = 0;
            }
        }

        ~RingBuffer() {
            if (buffer) free(buffer);
            if (mutex) vSemaphoreDelete(mutex);
            if (data_available) vSemaphoreDelete(data_available);
        }

        /**
         * Write data to the buffer (producer)
         * Called from WebSocket event handler (different task context)
         *
         * @param data Data to write
         * @param len Length of data in bytes
         * @return Number of bytes written
         */
        size_t write(const uint8_t* data, size_t len) {
            if (buffer == NULL || mutex == NULL || len == 0 || data == NULL) return 0;

            xSemaphoreTake(mutex, portMAX_DELAY);

            size_t written = 0;
            while (written < len) {
                size_t next_head = (head + 1) % capacity;

                if (next_head == tail) {
                    // Buffer full - drop oldest data (move tail forward)
                    // This prevents blocking the WebSocket event handler
                    tail = (tail + 1) % capacity;
                    ESP_LOGW(TAG, "Buffer overflow, dropping oldest data");
                }

                buffer[head] = data[written];
                head = next_head;
                written++;
            }

            xSemaphoreGive(mutex);

            // Signal that data is available
            xSemaphoreGive(data_available);

            return written;
        }

        /**
         * Read data from the buffer (consumer)
         * Called from client_readline/client_read_bytes in main task
         *
         * @param data Buffer to read into
         * @param len Maximum bytes to read
         * @return Number of bytes read
         */
        size_t read(uint8_t* data, size_t len) {
            if (buffer == NULL || mutex == NULL || len == 0 || data == NULL) return 0;

            xSemaphoreTake(mutex, portMAX_DELAY);

            size_t read_count = 0;
            while (read_count < len && tail != head) {
                data[read_count] = buffer[tail];
                tail = (tail + 1) % capacity;
                read_count++;
            }

            xSemaphoreGive(mutex);
            return read_count;
        }

        /**
         * Read a single byte from the buffer
         * Useful for line-by-line reading
         *
         * @param byte Pointer to store the byte
         * @return 1 if byte read, 0 if buffer empty
         */
        size_t read_byte(uint8_t* byte) {
            return read(byte, 1);
        }

        /**
         * Get number of bytes available for reading
         * @return Bytes available in buffer
         */
        size_t available() const {
            if (buffer == NULL) return 0;

            // Note: This is safe without mutex for single reader/writer
            // as head and tail are volatile
            if (head >= tail) {
                return head - tail;
            }
            return capacity - tail + head;
        }

        /**
         * Wait for data to be available
         * Blocks until data is written or timeout occurs
         *
         * @param timeout_ms Timeout in milliseconds
         * @return true if data available, false on timeout
         */
        bool wait_for_data(uint32_t timeout_ms) {
            if (available() > 0) return true;
            if (data_available == NULL) return false;

            return xSemaphoreTake(data_available, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
        }

        /**
         * Clear all data in the buffer
         * Called on disconnect to reset state
         */
        void clear() {
            if (mutex == NULL) return;

            xSemaphoreTake(mutex, portMAX_DELAY);
            head = 0;
            tail = 0;
            xSemaphoreGive(mutex);

            // Reset the semaphore
            if (data_available) {
                xSemaphoreTake(data_available, 0);  // Clear without waiting
            }
        }

        /**
         * Check if buffer is empty
         * @return true if buffer is empty
         */
        bool empty() const {
            return available() == 0;
        }

        /**
         * Get buffer capacity
         * @return Total capacity in bytes
         */
        size_t get_capacity() const {
            return capacity;
        }
    };

}; // namespace NATSUtil

#endif // ESPIDF_NATS_UTIL_H
