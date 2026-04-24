#include "hashtable.h"
#include <assert.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#define container_of(ptr, T, member) ((T *)((char *)ptr - offsetof(T, member)))
#define MAX_EVENTS 64
#define BUF_SIZE 4096

constexpr long MAX_MSG_SIZE{32 << 20};
constexpr int PORT{8080};
constexpr size_t K_MAX_ARGS = 200 * 1000;

static void msg(const char *msg) { fprintf(stderr, "%s\n", msg); }

static void msg_errno(const char *msg) {
    fprintf(stderr, "[errno:%d] %s\n", errno, msg);
}

// append to the back
static void buf_append(std::vector<uint8_t> &buf, const uint8_t *data,
                       std::size_t len) {
    buf.insert(buf.end(), data, data + len);
}

// remove from the front
static void buf_consume(std::vector<uint8_t> &buf, size_t n) {
    buf.erase(buf.begin(), buf.begin() + n);
}

enum class State {
    READING,
    WRITING,
    WANT_CLOSE,
};

struct Conn {
    int fd{-1};
    State state{State::READING};

    std::vector<uint8_t> incoming{};
    std::vector<uint8_t> outgoing{};
};

enum {
    RES_OK = 0,
    RES_ERR = 1,
    RES_NX = 2,
    ERR_TOO_BIG = 3,
};

enum {
    TAG_NIL = 0,
    TAG_ERR = 1,
    TAG_STR = 2,
    TAG_INT = 3,
    TAG_DBL = 4,
    TAG_ARR = 5,
};

using Buffer = std::vector<uint8_t>;

static void buf_append_u8(Buffer &buf, uint8_t data) { buf.push_back(data); }

static void buf_append_u32(Buffer &buf, uint32_t data) {
    buf_append(buf, (const uint8_t *)&data, 4);
}

static void buf_append_i64(Buffer &buf, int64_t data) {
    buf_append(buf, (const uint8_t *)&data, sizeof(int64_t));
}

static void buf_append_dbl(Buffer &buf, double data) {
    buf_append(buf, (const uint8_t *)&data, sizeof(double));
}

static void out_nil(Buffer &out) { buf_append_u8(out, TAG_NIL); }

static void out_str(Buffer &out, const char *data, std::size_t len) {
    // append tag
    buf_append_u8(out, TAG_STR);
    // append len
    buf_append_u32(out, len);
    // append data
    buf_append(out, (const uint8_t *)data, len);
}

static void out_int(Buffer &out, int64_t val) {
    buf_append_u8(out, TAG_INT);
    buf_append_i64(out, val);
}

static void out_arr(Buffer &out, std::size_t len) {
    buf_append_u8(out, TAG_ARR);
    buf_append_u32(out, len);
}

static void out_err(Buffer &out, uint32_t error_code) {
    buf_append_u8(out, TAG_ERR);
    buf_append_u32(out, error_code);
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// application callback when the listening socket is ready
static void handle_accept(int listener_fd, int epoll_fd) {
    for (;;) {
        // accept
        struct sockaddr_in client_addr = {};
        socklen_t addrlen = sizeof(client_addr);
        int connfd =
            accept(listener_fd, (struct sockaddr *)&client_addr, &addrlen);
        if (connfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            if (errno == EINTR)
                return;
            msg_errno("accept() error");
            return;
        }
        uint32_t ip = client_addr.sin_addr.s_addr;
        fprintf(stderr, "new client from %u.%u.%u.%u:%u\n", ip & 255,
                (ip >> 8) & 255, (ip >> 16) & 255, ip >> 24,
                ntohs(client_addr.sin_port));

        // set the new connection fd to nonblocking mode
        set_nonblocking(connfd);

        // create a `struct Conn`
        Conn *conn = new Conn();
        conn->fd = connfd;
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
        ev.data.ptr = conn;
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn->fd, &ev);
    }
}

static bool read_u32(const uint8_t *&cur, const uint8_t *end, uint32_t &out) {
    if (cur + 4 > end) {
        return false;
    }
    memcpy(&out, cur, 4);
    cur += 4;
    return true;
}

static bool read_str(const uint8_t *&cur, const uint8_t *end, size_t n,
                     std::string &out) {

    if (cur + n > end) {
        return false;
    }
    out.assign(cur, cur + n);
    cur += n;
    return true;
}

static int32_t parse_req(const uint8_t *data, size_t size,
                         std::vector<std::string> &out) {

    const uint8_t *end = data + size;
    uint32_t nstr{0};
    if (!read_u32(data, end, nstr)) {
        return -1;
    }

    if (nstr > K_MAX_ARGS) {
        return -1;
    }

    while (out.size() < nstr) {

        uint32_t len{0};
        if (!read_u32(data, end, len)) {
            return -1;
        }
        out.push_back(std::string());
        if (!read_str(data, end, len, out.back())) {
            return -1;
        }
    }
    if (data != end) {
        return -1;
    }
    return 0;
}

static uint64_t str_hash(const uint8_t *data, std::size_t len) {
    uint32_t h{0x811C9DC5};
    for (std::size_t i = 0; i < len; i++) {
        h = (h + data[i]) * 0x01000193;
    }
    return h;
}

static struct {
    HMap db;
} g_data;

struct Entry {
    struct HNode node;
    std::string key{};
    std::string val{};
};

static bool entry_eq(HNode *lhs, HNode *rhs) {
    struct Entry *le = container_of(lhs, struct Entry, node);
    struct Entry *re = container_of(rhs, struct Entry, node);
    return le->key == re->key;
}

static void do_get(std::vector<std::string> &cmd, Buffer &out) {
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hash_value =
        str_hash((const uint8_t *)key.key.data(), key.key.size());
    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if (!node) {
        return out_err(out, RES_NX);
    }

    const std::string &val = container_of(node, Entry, node)->val;
    assert(val.size() <= MAX_MSG_SIZE);
    return out_str(out, val.data(), val.size());
}

static void do_set(std::vector<std::string> &cmd, Buffer &out) {
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hash_value =
        str_hash((const uint8_t *)key.key.data(), key.key.size());
    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if (node) {
        container_of(node, Entry, node)->val.swap(cmd[2]);
    } else {
        Entry *ent = new Entry();
        ent->key.swap(key.key);
        ent->val.swap(cmd[2]);
        ent->node.hash_value = key.node.hash_value;
        hm_insert(&g_data.db, &ent->node);
    }
    return out_nil(out);
}

static void do_del(std::vector<std::string> &cmd, Buffer &out) {
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hash_value =
        str_hash((const uint8_t *)key.key.data(), key.key.size());
    HNode *node = hm_delete(&g_data.db, &key.node, &entry_eq);
    if (node) {
        delete container_of(node, Entry, node);
    }
    return out_int(out, node ? 1 : 0);
}

static bool cb_keys(HNode *node, void *arg) {
    Buffer &out = *(Buffer *)arg;
    const std::string &key = container_of(node, struct Entry, node)->key;
    out_str(out, key.data(), key.size());
    return true;
}

static void do_keys(std::vector<std::string> &, Buffer &out) {
    out_arr(out, (uint32_t)hm_size(&g_data.db));
    hm_foreach(&g_data.db, &cb_keys, (void *)&out);
}

static void do_request(std::vector<std::string> &cmd, Buffer &out) {

    if (cmd.size() == 2 && cmd[0] == "get") {
        return do_get(cmd, out);
    } else if (cmd.size() == 3 && cmd[0] == "set") {
        return do_set(cmd, out);
    } else if (cmd.size() == 2 && cmd[0] == "del") {
        return do_del(cmd, out);
    } else if (cmd.size() == 1 && cmd[0] == "keys") {
        return do_keys(cmd, out);
    } else {
        return out_err(out, RES_ERR);
    }
}

static void response_begin(Buffer &out, std::size_t &header) {
    header = out.size();
    buf_append_u32(out, 0);
}

static std::size_t response_size(Buffer &out, std::size_t header) {
    return out.size() - header - 4;
}

static void response_end(Buffer &out, size_t header) {
    std::size_t msg_size{response_size(out, header)};
    if (msg_size > MAX_MSG_SIZE) {
        out.resize(header + 4);
        out_err(out, ERR_TOO_BIG);
        msg_size = response_size(out, header);
    }
    uint32_t len{static_cast<uint32_t>(msg_size)};
    memcpy(&out[header], &len, 4);
}

// process 1 request if there is enough data
static bool try_one_request(Conn *conn) {
    // try to parse the protocol: message header
    if (conn->incoming.size() < 4) {
        return false; // want read
    }
    uint32_t len{0};
    memcpy(&len, conn->incoming.data(), 4);
    if (len > MAX_MSG_SIZE) {
        msg("too long");
        conn->state = State::WANT_CLOSE;
        return false; // want close
    }
    // message body
    if (4 + len > conn->incoming.size()) {
        return false; // want read
    }
    const uint8_t *request{&conn->incoming[4]};

    std::vector<std::string> cmd;
    if (parse_req(request, len, cmd) < 0) {
        msg("bad request");
        conn->state = State::WANT_CLOSE;
        return false;
    }

    std::size_t header_pos{0};
    response_begin(conn->outgoing, header_pos);
    do_request(cmd, conn->outgoing);
    response_end(conn->outgoing, header_pos);

    // got one request, do some application logic
    printf("client says: len:%d data:%.*s\n", len, len < 100 ? len : 100,
           request);

    // application logic done! remove the request message.
    buf_consume(conn->incoming, 4 + len);
    return true; // success
}

// application callback when the socket is writable
static void handle_write(Conn *conn) {
    assert(conn->outgoing.size() > 0);
    for (;;) {
        ssize_t rv = write(conn->fd, &conn->outgoing[0], conn->outgoing.size());
        if (rv < 0 && errno == EAGAIN) {
            return; // actually not ready
        }
        if (rv < 0) {
            msg_errno("write() error");
            conn->state = State::WANT_CLOSE; // error handling
            return;
        }

        // remove written data from `outgoing`
        buf_consume(conn->outgoing, (size_t)rv);

        // update the readiness intention
        if (conn->outgoing.size() == 0) { // all data written
            conn->state = State::READING;
            return;
        } // else: want write
    }
}

// application callback when the socket is readable
static void handle_read(Conn *conn) {
    // read some data
    uint8_t buf[64 * 1024];
    for (;;) {
        ssize_t rv = read(conn->fd, buf, sizeof(buf));
        if (rv < 0 && errno == EAGAIN) {
            break; // no more data to read from this connection
        }
        // handle IO error
        if (rv < 0) {
            msg_errno("read() error");
            conn->state = State::WANT_CLOSE;
            return; // want close
        }
        // handle EOF
        if (rv == 0) {
            if (conn->incoming.size() == 0) {
                msg("client closed");
            } else {
                msg("unexpected EOF");
            }
            conn->state = State::WANT_CLOSE;
            break; // want close
        }
        // got some new data
        buf_append(conn->incoming, buf, (size_t)rv);
    }
    // parse requests and generate responses
    while (try_one_request(conn))
        ;

    // update the readiness intention
    if (conn->outgoing.size() > 0) { // has a response
        conn->state = State::WRITING;
        // The socket is likely ready to write in a request-response
        // protocol, try to write it without waiting for the next iteration.
        return handle_write(conn);
    } // else: want read
}

int main() {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(listen_fd, SOMAXCONN) < 0) {
        perror("listen");
        return 1;
    }

    set_nonblocking(listen_fd);

    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        return 1;
    }

    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

    struct epoll_event events[MAX_EVENTS];
    printf("Echo server listening on port %d\n", PORT);

    for (;;) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == listen_fd) {
                // Accept all pending connections
                handle_accept(listen_fd, epoll_fd);
            } else {
                // handle requests from the existing connections
                Conn *conn = static_cast<Conn *>(events[i].data.ptr);
                uint32_t mask = events[i].events;
                const State &state{conn->state};
                using enum State;
                if ((mask & EPOLLIN) && state == READING) {
                    handle_read(conn);
                }

                if ((mask & EPOLLOUT) && state == WRITING) {
                    handle_write(conn);
                }

                if ((mask & EPOLLERR) || state == WANT_CLOSE) {
                    (void)close(conn->fd);
                    delete conn;
                }
            }
        }
    }

    close(listen_fd);
    close(epoll_fd);
    return 0;
}
