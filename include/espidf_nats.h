#ifndef ESPIDF_NATS_H
#define ESPIDF_NATS_H

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <esp_log.h>
#include <esp_random.h>
#include <inttypes.h>
#include <esp_tls.h>

#define NATS_CLIENT_LANG "espidf"
#define NATS_CLIENT_VERSION "1.0.0"

#ifndef NATS_CONF_VERBOSE
#define NATS_CONF_VERBOSE false
#endif

#ifndef NATS_CONF_PEDANTIC
#define NATS_CONF_PEDANTIC false
#endif

#ifndef NATS_PING_INTERVAL
#define NATS_PING_INTERVAL 120000UL
#endif

#ifndef NATS_RECONNECT_INTERVAL
#define NATS_RECONNECT_INTERVAL 5000UL
#endif

#define NATS_DEFAULT_PORT 4222

#define NATS_INBOX_PREFIX "_INBOX."
#define NATS_INBOX_ID_LENGTH 22

#define NATS_MAX_ARGV 5

#define NATS_CR_LF "\r\n"
#define NATS_CTRL_MSG "MSG"
#define NATS_CTRL_OK "+OK"
#define NATS_CTRL_ERR "-ERR"
#define NATS_CTRL_PING "PING"
#define NATS_CTRL_PONG "PONG"
#define NATS_CTRL_INFO "INFO"

static const char* tag = "espidf-nats";

namespace NATSUtil {

    static const char alphanums[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

    inline unsigned long millis() {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        return (tv.tv_sec * 1000UL) + (tv.tv_usec / 1000UL);
    }

    inline int random(int max) {
        return esp_random() % max;
    }

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
                    delete n;  // Use delete for objects created with new
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
                delete root;  // Use delete for objects created with new
                root = n.next;
                len--;
                return n.data;
            }
            T peek() { return root->data; }
    };
};

typedef struct {
    bool enabled;
    const char* ca_cert;
    size_t ca_cert_len;
    const char* client_cert;
    size_t client_cert_len;
    const char* client_key;
    size_t client_key_len;
    bool skip_cert_verification;
    const char* server_name;
} nats_tls_config_t;

class NATS {

    public:
        typedef struct {
            const char* subject;
            const int sid;
            const char* reply;
            const char* data;
            const int size;
        } msg;

    private:
        typedef void (*sub_cb)(msg e);
        typedef void (*event_cb)();

        class Sub {
            public:
            sub_cb cb;
            int received;
            int max_wanted;
            Sub(sub_cb cb, int max_wanted = 0) :
                cb(cb), received(0), max_wanted(max_wanted) {}
            void call(msg& e) {
                received++;
                cb(e);
            }
            bool maxed() {
                return (max_wanted == 0)? false : received >= max_wanted;
            }
        };

    private:
        int sockfd;
        esp_tls_t* tls;
        nats_tls_config_t tls_config;
        const char* hostname;
        const int port;
        const char* user;
        const char* pass;

        NATSUtil::Array<Sub*> subs;
        NATSUtil::Queue<size_t> free_sids;

        NATSUtil::MillisTimer ping_timer;
        NATSUtil::MillisTimer reconnect_timer;

        int outstanding_pings;
        int reconnect_attempts;

    public:
        bool connected;
        int max_outstanding_pings;
        int max_reconnect_attempts;

        event_cb on_connect;
        event_cb on_disconnect;
        event_cb on_error;

    public:
        NATS(const char* hostname,
                int port = NATS_DEFAULT_PORT,
                const char* user = NULL,
                const char* pass = NULL,
                const nats_tls_config_t* tls_cfg = NULL) :
            sockfd(-1),
            tls(NULL),
            hostname(hostname),
            port(port),
            user(user),
            pass(pass),
            ping_timer(NATS_PING_INTERVAL),
            reconnect_timer(NATS_RECONNECT_INTERVAL),
            outstanding_pings(0),
            reconnect_attempts(0),
            connected(false),
            max_outstanding_pings(3),
            max_reconnect_attempts(-1),
            on_connect(NULL),
            on_disconnect(NULL),
            on_error(NULL) {
                if (tls_cfg != NULL) {
                    tls_config = *tls_cfg;
                } else {
                    memset(&tls_config, 0, sizeof(nats_tls_config_t));
                }
            }

    private:
        void send(const char* msg) {
            if (msg == NULL) return;
            size_t len = strlen(msg);
            ssize_t ret;
            if (tls_config.enabled && tls != NULL) {
                ret = esp_tls_conn_write(tls, msg, len);
                if (ret < 0 || (size_t)ret != len) {
                    ESP_LOGE(tag, "TLS write failed: %d", (int)ret);
                    disconnect();
                    return;
                }
                ret = esp_tls_conn_write(tls, NATS_CR_LF, strlen(NATS_CR_LF));
                if (ret < 0) {
                    ESP_LOGE(tag, "TLS write CRLF failed: %d", (int)ret);
                    disconnect();
                    return;
                }
            } else {
                if (sockfd < 0) return;
                ret = ::send(sockfd, msg, len, 0);
                if (ret < 0) {
                    ESP_LOGE(tag, "Socket send failed: %d", errno);
                    disconnect();
                    return;
                }
                ret = ::send(sockfd, NATS_CR_LF, strlen(NATS_CR_LF), 0);
                if (ret < 0) {
                    ESP_LOGE(tag, "Socket send CRLF failed: %d", errno);
                    disconnect();
                    return;
                }
            }
        }

        int vasprintf(char** strp, const char* fmt, va_list ap) {
            va_list ap2;
            va_copy(ap2, ap);
            char tmp[1];
            int size = vsnprintf(tmp, 1, fmt, ap2);
            if (size <= 0) return size;
            va_end(ap2);
            size += 1;
            *strp = (char*)malloc(size * sizeof(char));
            return vsnprintf(*strp, size, fmt, ap);
        }

        void send_fmt(const char* fmt, ...) {
            va_list args;
            va_start(args, fmt);
            char* buf;
            vasprintf(&buf, fmt, args);
            va_end(args);
            send(buf);
            free(buf);
        }

        void send_connect() {
            send_fmt(
                    "CONNECT {"
                        "\"verbose\": %s,"
                        "\"pedantic\": %s,"
                        "\"lang\": \"%s\","
                        "\"version\": \"%s\","
                        "\"user\":\"%s\","
                        "\"pass\":\"%s\""
                    "}", 
                    NATS_CONF_VERBOSE? "true" : "false",
                    NATS_CONF_PEDANTIC? "true" : "false",
                    NATS_CLIENT_LANG,
                    NATS_CLIENT_VERSION,
                    (user == NULL)? "null" : user,
                    (pass == NULL)? "null" : pass);
        }

        char* client_readline(size_t cap = 128) {
            char* buf = (char*)malloc(cap * sizeof(char));
            if (buf == NULL) {
                ESP_LOGE(tag, "Failed to allocate readline buffer");
                disconnect();
                return (char*)calloc(1, sizeof(char)); // Return empty string
            }
            int i = 0;
            char c;
            int ret;
            while (true) {
                if (tls_config.enabled && tls != NULL) {
                    ret = esp_tls_conn_read(tls, &c, 1);
                } else {
                    ret = ::recv(sockfd, &c, 1, 0);
                }
                if (ret <= 0) {
                    // Connection closed (0) or error (-1)
                    if (ret < 0) {
                        ESP_LOGE(tag, "Read error: %d", ret);
                    }
                    free(buf);
                    disconnect();
                    return (char*)calloc(1, sizeof(char)); // Return empty string
                }
                if (c == '\r') continue;
                if (c == '\n') break;
                if (i >= cap) {
                    char* newbuf = (char*)realloc(buf, (cap *= 2) * sizeof(char) + 1);
                    if (newbuf == NULL) {
                        ESP_LOGE(tag, "Failed to realloc readline buffer");
                        free(buf);
                        disconnect();
                        return (char*)calloc(1, sizeof(char));
                    }
                    buf = newbuf;
                }
                buf[i++] = c;
            }
            buf[i] = '\0';
            return buf;
        }

        void recv() {
            char* buf = client_readline();
            size_t argc = 0;
            const char* argv[NATS_MAX_ARGV] = {};
            for (int i = 0; i < NATS_MAX_ARGV; i++) {
                argv[i] = strtok((i == 0) ? buf : NULL, " ");
                if (argv[i] == NULL) break;
                argc++;
            }
            if (argc == 0) {}
            if (strcmp(argv[0], NATS_CTRL_MSG) == 0) {
                if (argc != 4 && argc != 5) { free(buf); return; }
                int sid = atoi(argv[2]);
                if (subs[sid] == NULL) { free(buf); return; };
                int payload_size = atoi((argc == 5)? argv[4] : argv[3]) + 1;
                char* payload_buf = client_readline(payload_size);
                msg e = {
                    argv[1],
                    sid,
                    (argc == 5)? argv[3] : "",
                    payload_buf,
                    payload_size
                };
                subs[sid]->call(e);
                if (subs[sid]->maxed()) unsubscribe(sid);
                free(payload_buf);
            }
            else if (strcmp(argv[0], NATS_CTRL_OK) == 0) {
            }
            else if (strcmp(argv[0], NATS_CTRL_ERR) == 0) {
                if (on_error != NULL) on_error();
                disconnect();
            }
            else if (strcmp(argv[0], NATS_CTRL_PING) == 0) {
                send(NATS_CTRL_PONG);
            }
            else if (strcmp(argv[0], NATS_CTRL_PONG) == 0) {
                outstanding_pings--;
            }
            else if (strcmp(argv[0], NATS_CTRL_INFO) == 0) {
                send_connect();
                connected = true;
                if (on_connect != NULL) on_connect();
            }
            free(buf);
        }

        void ping() {
            if (outstanding_pings > max_outstanding_pings) {
                disconnect();
                return;
            }
            outstanding_pings++;
            send(NATS_CTRL_PING);
        }

        char* generate_inbox_subject() {
            size_t size = strlen(NATS_INBOX_PREFIX) + NATS_INBOX_ID_LENGTH + 1;
            char* buf = (char*)malloc(size);
            if (buf == NULL) {
                ESP_LOGE(tag, "Failed to allocate memory for inbox subject");
                return NULL;
            }
            strcpy(buf, NATS_INBOX_PREFIX);
            int i;
            size_t alphanum_len = sizeof(NATSUtil::alphanums) - 1;
            for (i = strlen(NATS_INBOX_PREFIX); i < size - 1; i++) {
                // Avoid modulo bias by using rejection sampling
                uint32_t random_val;
                uint32_t max_valid = (UINT32_MAX / alphanum_len) * alphanum_len;
                do {
                    random_val = esp_random();
                } while (random_val >= max_valid);
                int random_idx = random_val % alphanum_len;
                buf[i] = NATSUtil::alphanums[random_idx];
            }
            buf[i] = '\0';
            return buf;
        }

    public:
        bool connect() {
            if (tls_config.enabled) {
                // TLS connection
                esp_tls_cfg_t cfg = {};
                cfg.cacert_buf = (const unsigned char*)tls_config.ca_cert;
                cfg.cacert_bytes = tls_config.ca_cert_len;
                cfg.timeout_ms = 30000;  // 30 second timeout
                cfg.non_block = false;    // Use blocking mode for simplicity

                if (tls_config.client_cert != NULL && tls_config.client_key != NULL) {
                    cfg.clientcert_buf = (const unsigned char*)tls_config.client_cert;
                    cfg.clientcert_bytes = tls_config.client_cert_len;
                    cfg.clientkey_buf = (const unsigned char*)tls_config.client_key;
                    cfg.clientkey_bytes = tls_config.client_key_len;
                }

                if (tls_config.skip_cert_verification) {
                    // Skip both CN check and server certificate verification
                    cfg.skip_common_name = true;
                    // Note: esp_tls doesn't have skip_server_cert_verify in older versions
                    // This only skips CN verification, not the full cert chain
                    ESP_LOGW(tag, "Certificate CN verification disabled (chain still validated)");
                }

                if (tls_config.server_name != NULL) {
                    cfg.common_name = tls_config.server_name;
                }

                tls = esp_tls_init();
                if (tls == NULL) {
                    ESP_LOGE(tag, "Failed to initialize TLS");
                    reconnect_attempts++;
                    return false;
                }

                int ret = esp_tls_conn_new_sync(hostname, strlen(hostname), port, &cfg, tls);
                if (ret != 1) {
                    ESP_LOGE(tag, "TLS connection failed: %d", ret);
                    esp_tls_conn_destroy(tls);
                    tls = NULL;
                    reconnect_attempts++;
                    return false;
                }

                sockfd = 1; // Set to valid for compatibility with process()
                outstanding_pings = 0;
                reconnect_attempts = 0;
                return true;
            } else {
                // Non-TLS connection
                struct sockaddr_in server_addr;
                sockfd = socket(AF_INET, SOCK_STREAM, 0);
                if (sockfd < 0) {
                    ESP_LOGE(tag, "Failed to create socket");
                    reconnect_attempts++;
                    return false;
                }

                server_addr.sin_family = AF_INET;
                server_addr.sin_port = htons(port);

                // Try to parse as IP address first
                if (inet_aton(hostname, &server_addr.sin_addr) == 0) {
                    // Not an IP address, try DNS resolution
                    struct hostent *he = gethostbyname(hostname);
                    if (he == NULL) {
                        ESP_LOGE(tag, "DNS resolution failed for %s", hostname);
                        close(sockfd);
                        sockfd = -1;
                        reconnect_attempts++;
                        return false;
                    }
                    memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);
                }

                if (::connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == 0) {
                    outstanding_pings = 0;
                    reconnect_attempts = 0;
                    return true;
                }

                ESP_LOGE(tag, "Connection failed to %s:%d", hostname, port);
                close(sockfd);
                sockfd = -1;
                reconnect_attempts++;
                return false;
            }
        }

        void disconnect() {
            if (!connected) return;
            connected = false;

            if (tls_config.enabled && tls != NULL) {
                esp_tls_conn_destroy(tls);
                tls = NULL;
            }

            if (sockfd >= 0) {
                if (!tls_config.enabled) {
                    close(sockfd);
                }
                sockfd = -1;
            }

            subs.empty();
            if (on_disconnect != NULL) on_disconnect();
        }

        void publish(const char* subject, const char* msg = NULL, const char* replyto = NULL) {
            if (subject == NULL || subject[0] == 0) return;
            if (!connected) return;
            send_fmt("PUB %s %s %lu",
                    subject,
                    (replyto == NULL)? "" : replyto,
                    (unsigned long)strlen(msg));
            send((msg == NULL)? "" : msg);
        }
        void publish(const char* subject, const bool msg) {
            publish(subject, (msg)? "true" : "false");
        }
        void publish_fmt(const char* subject, const char* fmt, ...) {
            va_list args;
            va_start(args, fmt);
            char* buf;
            vasprintf(&buf, fmt, args);
            va_end(args);
            publish(subject, buf);
            free(buf);
        }
        void publishf(const char* subject, const char* fmt, ...) {
            va_list args;
            va_start(args, fmt);
            char* buf;
            vasprintf(&buf, fmt, args);
            va_end(args);
            publish(subject, buf);
            free(buf);
        }

        int subscribe(const char* subject, sub_cb cb, const char* queue = NULL, const int max_wanted = 0) {
            if (!connected) return -1;
            Sub* sub = new Sub(cb, max_wanted);
            int sid;
            if (free_sids.empty()) {
                sid = subs.push_back(sub);
            } else {
                sid = free_sids.pop();
                subs[sid] = sub;
            }
            send_fmt("SUB %s %s %d", 
                    subject, 
                    (queue == NULL)? "" : queue, 
                    sid);
            return sid;
        }

        void unsubscribe(const int sid) {
            if (!connected) return;
            send_fmt("UNSUB %d", sid);
            delete subs[sid];  // Use delete for objects created with new
            subs[sid] = NULL;
            free_sids.push(sid);
        }

        int request(const char* subject, const char* msg, sub_cb cb, const int max_wanted = 1) {
            if (subject == NULL || subject[0] == 0) return -1;
            if (!connected) return -1;
            char* inbox = generate_inbox_subject();
            if (inbox == NULL) {
                ESP_LOGE(tag, "Failed to generate inbox subject");
                return -1;
            }
            int sid = subscribe(inbox, cb, NULL, max_wanted);
            publish(subject, msg, inbox);
            free(inbox);
            return sid;
        }

        int log_tick_count = 0;
        void process() {
            if (log_tick_count++ % 1000 == 0) {
                //ESP_LOGI(tag, "(tick %d) Outstanding pings: %d, Reconnect attempts: %d", log_tick_count, outstanding_pings, reconnect_attempts);
            }
            if (sockfd >= 0) {
                int fd_to_check = sockfd;

                // For TLS connections, get the underlying socket descriptor
                if (tls_config.enabled && tls != NULL) {
                    if (esp_tls_get_conn_sockfd(tls, &fd_to_check) != ESP_OK) {
                        disconnect();
                        return;
                    }
                }

                fd_set rfds;
                struct timeval tv = {0, 0};
                FD_ZERO(&rfds);
                FD_SET(fd_to_check, &rfds);
                int ret = select(fd_to_check + 1, &rfds, NULL, NULL, &tv);
                if (ret > 0 && FD_ISSET(fd_to_check, &rfds)) {
                    recv();
                }
                if (ping_timer.process())
                    ping();
            } else {
                disconnect();
                if (max_reconnect_attempts == -1 || reconnect_attempts < max_reconnect_attempts) {
                    if (reconnect_timer.process())
                        connect();
                }
            }
        }

};

#endif