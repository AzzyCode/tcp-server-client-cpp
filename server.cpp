#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <vector>
#include <map>


// Constants
const size_t k_max_msg = 4096; // Maximum message size
const size_t k_max_args = 1024; // Maximum number of arguments

// Connection states
enum {
    STATE_REQ = 0, // Request
    STATE_RES = 1, // Response
    STATE_END = 2 // End, mark the connection for deletion
};

// Response codes
enum {
    RES_OK = 0, // Success
    RES_ERR = 1, // Error
    RES_NX = 2 // Not Found
};

// Utility functions for error logging and error handling
static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}


static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}


// Set a file descriptor to non-blocking mode
static void fd_set_nb(int fd) {
    errno = 0;
    int flags = fcntl(fd, F_GETFL, 0); // Get current fd file status flag
    if (errno) {
        die("fcntl error");
        return;
    }

    flags |= O_NONBLOCK; // Add argument to a flag (non-blocking mode)

    errno = 0;
    static_cast<void>(fcntl(fd, F_SETFL, flags)); // Set modified flag on the fd
    if(errno){
        die("fcntl error");
    }
}


// Connection structure representing a single client connection
struct Conn {
    int fd = -1;
    uint32_t state = 0; // Either STATE_REQ or STATE_RES

    // buffer for reading
    size_t rbuf_size = 0;
    uint8_t rbuf[4 + k_max_msg];

    // buffer for writing
    size_t wbuf_size = 0;
    size_t wbuf_sent = 0;
    uint8_t wbuf[4 + k_max_msg];
};


// Global key-value store using a map
static std::map<std::string, std::string> g_map;


// Associate a connection object with a file descriptor in a vector
static void conn_put(std::vector<Conn *> &fd2conn, struct Conn *conn) {
    if (fd2conn.size() <= static_cast<size_t>(conn->fd)) {
        fd2conn.resize(conn->fd + 1);
    }
    fd2conn[conn->fd] = conn;
}


// Accept new connections on a listening socket
static int32_t accept_new_conn(std::vector<Conn *> &fd2conn, int fd) {
    // Accept
    struct sockaddr_in client_addr = {};
    socklen_t socklen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &socklen);
    if (connfd < 0) {
        msg("accept() error");
        return -1; // error
    }

    // Set the new connection fd to nonblocking mode
    fd_set_nb(connfd);

    // Creating the struct Conn
    struct Conn *conn = (struct Conn *)malloc(sizeof(struct Conn));
    if (!conn) {
        close(connfd);
        return -1;
    }

    conn->fd = connfd;
    conn->state = STATE_REQ;
    conn->rbuf_size = 0;
    conn->wbuf_size = 0;
    conn->wbuf_sent = 0;
    
    conn_put(fd2conn, conn);
    
    return 0;
} 

// Prototypes for state handling functions
static void state_req(Conn *conn);
static void state_res(Conn *conn);


// Parses a request and extracts strings based on prefixed lengths
static int32_t parseRequest(const uint8_t *data, size_t len, std::vector<std::string> &out){
    // First 4 bytes are the number of strings to be extracted
    if (len < 4) {
        return -1; // Not enough data to read the initial cound of strings
    }

    uint32_t numStrings = 0;
    memcpy(&numStrings, &data[0], 4);

    if (numStrings > k_max_args) {
        return -1; // More strings than allowed
    }

    size_t pos = 4;
    while (numStrings--) {
        if (pos + 4 > len) {
            return -1; // Not enough data to read the size of the next sting
        }
        uint32_t sz = 0;
        memcpy(&sz, &data[pos], 4);

        if (pos + 4 + sz > len) {
            return -1;
        }
        out.push_back(std::string((char *)&data[pos + 4], sz));
        pos += 4 + sz;
    }

    if (pos != len) {
        return -1; // Trailing garbage
    }
    
    return 0;
}

// Get the value associated with a key from the g_map
static uint32_t do_get(const std::vector<std::string> &cmd, uint8_t *res, uint32_t *reslen) {
    if (!g_map.count(cmd[1])) {
        return RES_NX;
    }

    std::string &val = g_map[cmd[1]];
    assert(val.size() <= k_max_msg);
    memcpy(res, val.data(), val.size());
    *reslen = static_cast<uint32_t>(val.size());

    return RES_OK;
}

// Insert a new key-value pair or updates an existing one
static uint32_t do_set(const std::vector<std::string> &cmd, uint8_t *res, uint32_t *reslen) {
    static_cast<void>(res);
    static_cast<void>(reslen);

    g_map[cmd[1]] = cmd[2];

    return RES_OK;
}

// Remove a key and its value from map
static uint32_t do_del(const std::vector<std::string> &cmd, uint8_t *res, uint32_t *reslen) {
    static_cast<void>(res);
    static_cast<void>(reslen);

    g_map.erase((cmd[1]));

    return RES_OK;
}

static bool cmd_is(const std::string &word, const char *cmd) {
    return 0 == strcasecmp(word.c_str(), cmd);
}

// Handles a request by determining which command to execute (get, set, del)
static int32_t do_request(
    const uint8_t *req, uint32_t reqlen,
    uint32_t *rescode, uint8_t *res, uint32_t *reslen) 
{
    std::vector<std::string> cmd;
    if(0 != parseRequest(req, reqlen, cmd)) {
        msg("bad request");
        return -1;
    }

    if (cmd.size() == 2 && cmd_is(cmd[0], "get")) {
        *rescode = do_get(cmd, res, reslen);
    } else if (cmd.size() == 3 && cmd_is(cmd[0], "set")) {
        *rescode = do_set(cmd, res, reslen);
    } else if (cmd.size() == 2 && cmd_is(cmd[0], "del")) {
        *rescode = do_del(cmd, res, reslen);
    } else {
        // Cmd is not recognized
        *rescode = RES_ERR;
        const char *msg = "Unknown cmd";
        strcpy((char*)res, msg);
        *reslen = strlen(msg);

        return 0;
    }

    return 0;
}

// Process a single request from the connection buffer
static bool try_one_request(Conn *conn) {
    // Try to parse a request from the buffer
    if (conn->rbuf_size < 4) {
        // Not enoough data in the buffer. Will retry in the next iteration.
        return false;
    }

    uint32_t len = 0;
    memcpy(&len, &conn->rbuf[0], 4);
    if (len > k_max_msg){
        msg("too long");
        conn->state = STATE_END;
        return false;
    }

    if (4 + len > conn->rbuf_size) {
        // no enough data in the buffer. Will retry in the next iteration
        return false;
    }

    // Got one request, generate the response
    uint32_t rescode = 0;
    uint32_t wlen = 0;
    int32_t err = do_request(&conn->rbuf[4], len, &rescode, &conn->wbuf[4+4], &wlen);

    if(err) {
        conn->state = STATE_END;
        return false;
    }

    // Generation echoing response
    wlen += 4;
    memcpy(&conn->wbuf[0], &wlen, 4);
    memcpy(&conn->wbuf[4], &rescode, 4);
    conn->wbuf_size = 4 + wlen;

    // Remove the request from the buffer
    size_t remain = conn->rbuf_size - 4 - len;
    if (remain) {
        memmove(conn->rbuf, &conn->rbuf[4 + len], remain);
    }
    conn->rbuf_size = remain;

    // Change state
    conn->state = STATE_RES;
    state_res(conn);

    // Continuse the outer loop if the request was fully processed
    return (conn->state == STATE_REQ);

}

// Attempt to fill the conneciton's read buffer
static bool try_fill_buffer(Conn *conn){
    // Try to fil buffer
    assert(conn->rbuf_size < sizeof(conn->rbuf));
    ssize_t rv = 0;

    do {
        size_t cap = sizeof(conn->rbuf) - conn->rbuf_size;
        rv = read(conn->fd, &conn->rbuf[conn->rbuf_size], cap);
    } while (rv < 0 && errno == EINTR);

    if (rv < 0 && errno == EAGAIN) {
        // got EAGAIN stop.
        return false;
    }
    if (rv < 0) {
        msg("reaD() error");
        conn->state = STATE_END;
        return false;
    }
    if (rv == 0) {
        if (conn->rbuf_size > 0){
            msg("unexpected EOF");
        } else {
            msg("EOF");
        }
        conn->state = STATE_END; 
        return false;
    }

    conn->rbuf_size += static_cast<size_t>(rv);
    assert(conn->rbuf_size < sizeof(conn->rbuf));

    // Try to process requests one by one.
    // Why is there a loop? Please read the explanation of "pielining"
    while (try_one_request(conn)) {}
    return (conn->state == STATE_REQ);
}

// Handles state for waiting for request
static void state_req(Conn *conn) {
    while (try_fill_buffer(conn)) {}
} 

// Attempt to flush the connection's write buffer
static bool try_flush_buffer(Conn *conn) {
    ssize_t rv = 0;

    do {
        size_t remain = conn->wbuf_size - conn->wbuf_sent;
        rv = write(conn->fd, &conn->wbuf[conn->wbuf_sent], remain);
    } while (rv < 0 && errno == EINTR);

    if (rv < 0 && errno == EAGAIN) {
        // got EAGAIN, stop.
        return false;
    }
    if (rv < 0) {
        msg("write() error");
        conn->state = STATE_END;
        return false;
    }
    conn->wbuf_sent += static_cast<size_t>(rv);
    assert(conn->wbuf_sent <= conn->wbuf_size);

    if (conn->wbuf_sent <= conn->wbuf_size) {
        // Response was fully sent, change state back
        conn->state = STATE_REQ;
        conn->wbuf_sent = 0;
        conn->wbuf_size = 0;
        
        return false;
    }
    // Still got some data in wbuf, could try to write again
    return true;
}

// Handles state for sending responses
static void state_res(Conn *conn) {
    while (try_flush_buffer(conn)) {}
}

// Main IO handling logic for connection based on state
static void connection_io(Conn *conn) {
    if (conn->state == STATE_REQ) {
        state_req(conn);
    } else if (conn->state == STATE_RES) {
        state_res(conn);
    } else {
        assert(0); // Unexpected state 
    }
}


int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        die("socket()");
    }

    // Set SO_REUSEADDR to quickly restart the server after a crash or shutdown
    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    // Binding socket to port 1234
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = ntohs(1234);
    addr.sin_addr.s_addr = ntohl(0);    // wildcard address 0.0.0.0
    int rv = bind(fd, (const sockaddr *)&addr, sizeof(addr));
    if (rv) {
        die("bind()");
    }

    // listen
    rv = listen(fd, SOMAXCONN);
    if (rv) {
        die("listen()");
    }

    // A map of all clients connections, keyed by fd
    std::vector<Conn *> fd2conn;

    // Set the listen fd to nonblocking mode
    fd_set_nb(fd);

    // Event loop
    std::vector<struct pollfd> poll_args;
    
    while(true) {
        // Prepare the arguments of the poll()
        poll_args.clear();

        // The listening fd is put in the first position
        struct pollfd pfd = {fd, POLLIN, 0};
        poll_args.push_back(pfd);

        // Connection fds
        for (Conn *conn : fd2conn) {
            if(!conn) {
                continue;
            }
            struct pollfd pfd = {};
            pfd.fd = conn->fd;
            pfd.events = (conn->state == STATE_REQ) ? POLLIN : POLLOUT;
            pfd.events = pfd.events | POLLERR;
            poll_args.push_back(pfd);
        }

        // Poll for active fds
        // Time arguments doesn't matter here
        int rv = poll(poll_args.data(), static_cast<nfds_t>(poll_args.size()), 1000);
        if (rv < 0){
            die("poll");
        }

        // Process active connection
        for (size_t i = 1; i < poll_args.size(); ++i) {
            if (poll_args[i].revents) {
                Conn *conn = fd2conn[poll_args[i].fd];
                connection_io(conn);
                if (conn->state == STATE_END) {
                    // Client closed normally or something bad happened.
                    fd2conn[conn->fd] = NULL;
                    static_cast<void>(close(conn->fd));
                    free(conn);
                }
            }
        }

        // Try to accept a new connectio if the listening fd is active
        if(poll_args[0].revents) {
            static_cast<void>(accept_new_conn(fd2conn, fd));
        }
    }

    return 0;
}