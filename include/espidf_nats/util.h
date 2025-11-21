#ifndef ESPIDF_NATS_UTIL_H
#define ESPIDF_NATS_UTIL_H

#include <sys/time.h>
#include <esp_random.h>
#include <stdlib.h>

namespace NATSUtil {

    static const char alphanums[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

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
     */
    inline int random(int max) {
        return esp_random() % max;
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
            }

            ~Array() { free(data); }

        private:
            void resize() {
                if (cap == 0) cap = 1;
                else cap *= 2;
                data = (T*)realloc(data, cap * sizeof(T));
            }

        public:
            size_t size() const { return len; }

            void erase(size_t idx) {
                for (size_t i = idx; i < len; i++) {
                    data[i] = data[i+1];
                }
                len--;
            }

            void empty() {
                len = 0;
                cap = 32;
                free(data);
                data = (T*)malloc(cap * sizeof(T));
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
     * Simple queue (LIFO stack) for recycling IDs
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
            Node* root;
            size_t len;

        public:
            Queue() : root(NULL), len(0) {}

            ~Queue() {
                Node* tmp;
                Node* n = root;
                while (n != NULL) {
                    tmp = n->next;
                    free(n);
                    n = tmp;
                }
            }

            bool empty() const { return root == NULL; }
            size_t size() const { return len; }

            void push(T data) {
                root = new Node(data, root);
                len++;
            }

            T pop() {
                Node n = *root;
                free(root);
                root = n.next;
                len--;
                return n.data;
            }

            T peek() { return root->data; }
    };

}; // namespace NATSUtil

#endif // ESPIDF_NATS_UTIL_H
