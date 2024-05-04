#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <vector>

#define MAX_EVENTS 10

// Print messages to stderr
static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

// Print errors and abort program
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


const size_t k_max_msg = 4096; // Maximum message size

enum {
    STATE_REQ = 0, // Request
    STATE_RES = 1, // Response
    STATE_END = 2  // End, Mark the connection for deletion
};

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

// Associate a connection with its file descriptor
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

    // Got one request, do something with it
    printf("Client says: %.*s\n", len, &conn->rbuf[4]); 

    // Generation echoing response
    memcpy(&conn->wbuf[0], &len, 4);
    memcpy(&conn->wbuf[4], &conn->rbuf[4], len);
    conn->wbuf_size = 4 + len;

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