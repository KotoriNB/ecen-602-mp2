// server.c -- SBCP chat server. A single process multiplexes the listening
// socket(s) and every client connection with select(); nothing forks and
// nothing blocks, so one misbehaving client can never stall or kill the rest.
//
// Usage: ./server <server_ip> <server_port> <max_clients>

#include "net.h"
#include "sbcp_codec.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_CLIENTS_LIMIT   1000            // select() watches at most FD_SETSIZE (1024) descriptors
#define LISTENQ             128
#define MAX_LISTEN          8               // one listening socket per resolved address
#define ACCEPT_BATCH        64              // accepts per wake-up, so clients are not starved
#define RX_CAP              2048            // largest client message accepted (a SEND is <= 524)
#define OUTQ_LIMIT          (1024 * 1024)   // backlog at which a non-reading client is dropped
#define PARTIAL_TIMEOUT_MS  3000            // an incomplete message is discarded after this
#define LINGER_MS           3000            // a rejected client gets this long to read its NAK

enum cstate {
    ST_NEW,         // connected, not joined yet
    ST_JOINED,      // in the chat (online or idle)
    ST_REJECTED     // NAK sent; draining until the client closes
};

// Bytes accepted for sending but not yet taken by the kernel.
struct outq {
    uint8_t *data;
    size_t   head, tail, cap;
};

struct client {
    int                 fd;
    enum cstate         state;
    enum sbcp_wire      wire;           // layout this peer speaks; we answer in kind
    int                 wire_known;
    int                 idle;           // bonus: sent IDLE and no SEND since
    int                 dead;           // closed at the end of this round by reap()
    int                 offline_due;    // OFFLINE still to be broadcast for it
    int                 shut_after_flush;
    int64_t             linger_until;
    unsigned long       join_seq;       // order of joining, for the ACK list
    char                name[MAX_USERNAME_LEN + 1];
    char                peer[NET_ADDRSTRLEN];
    struct sbcp_reader  rd;
    struct outq         out;
};

static struct client *clients[FD_SETSIZE];      // indexed by descriptor
static int            hi_fd = -1;               // highest descriptor in clients[]
static int            listen_fds[MAX_LISTEN];
static int            n_listen;
static int            max_clients;
static int            n_joined;                 // live ST_JOINED clients
static unsigned long  joins_so_far;
static int            spare_fd = -1;            // given up to shed a connection at EMFILE
static int64_t        accept_paused_until;
static int            sig_pipe[2] = { -1, -1 };
static volatile sig_atomic_t stop_signal;

// SIGINT/SIGTERM. The byte in sig_pipe wakes select() even if the signal lands
// between the loop's flag check and the select() call.
static void on_stop(int signo)
{
    int     saved_errno = errno;
    ssize_t ignored;

    stop_signal = signo;
    ignored = write(sig_pipe[1], "", 1);
    (void)ignored;
    errno = saved_errno;
}

static const char *who(const struct client *c)
{
    return c->state == ST_JOINED ? c->name : c->peer;
}

// Marks c for removal. The descriptor is closed by reap() at the end of the
// round, so no loop is ever left holding a pointer to freed memory.
static void kill_client(struct client *c, const char *why)
{
    if (c->dead)
        return;
    c->dead = 1;

    if (c->state == ST_JOINED) {
        n_joined--;
        c->offline_due = 1;
        log_info("%s (%s) left: %s [%d/%d in chat]", c->name, c->peer, why,
                 n_joined, max_clients);
    } else {
        log_info("%s disconnected: %s", c->peer, why);
    }
}

// ---- Output ---------------------------------------------------------------

// 0 on success, -1 if the backlog limit would be passed, -2 if out of memory.
static int outq_push(struct outq *q, const uint8_t *p, size_t n)
{
    if (q->tail - q->head + n > OUTQ_LIMIT)
        return -1;

    if (q->tail + n > q->cap) {
        if (q->head > 0) {
            memmove(q->data, q->data + q->head, q->tail - q->head);
            q->tail -= q->head;
            q->head  = 0;
        }
        if (q->tail + n > q->cap) {
            size_t   cap = q->cap ? q->cap : 4096;
            uint8_t *d;

            while (cap < q->tail + n)
                cap *= 2;
            if ((d = realloc(q->data, cap)) == NULL)
                return -2;
            q->data = d;
            q->cap  = cap;
        }
    }
    memcpy(q->data + q->tail, p, n);
    q->tail += n;
    return 0;
}

// Hands queued bytes to the kernel until it would block. The sockets are
// non-blocking, so a client that stops reading costs memory, never time.
static void flush(struct client *c)
{
    struct outq *q = &c->out;

    while (q->head < q->tail) {
        ssize_t k = send(c->fd, q->data + q->head, q->tail - q->head,
                         NET_SEND_FLAGS);

        if (k > 0) {
            q->head += (size_t)k;
            continue;
        }
        if (k < 0 && errno == EINTR)
            continue;
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;                     // select() reports when there is room

        // EPIPE / ECONNRESET: the client is gone. MSG_NOSIGNAL and the ignored
        // SIGPIPE are why this is an error return and not a dead server.
        kill_client(c, k < 0 ? strerror(errno) : "send() returned 0");
        return;
    }

    q->head = q->tail = 0;
    if (q->cap > 65536) {               // give back what a burst grew
        free(q->data);
        q->data = NULL;
        q->cap  = 0;
    }

    // After a NAK: FIN only once the NAK is out, so the client reads it first.
    if (c->shut_after_flush) {
        c->shut_after_flush = 0;
        shutdown(c->fd, SHUT_WR);
    }
}

static void client_send(struct client *c, const uint8_t *msg, size_t len)
{
    int rc;

    if (c->dead)
        return;
    if (len > 0 && (rc = outq_push(&c->out, msg, len)) < 0) {
        kill_client(c, rc == -1 ? "not reading: over 1 MiB of output backed up"
                                : "out of memory for its output");
        return;
    }
    flush(c);
}

// A server-to-client message with an optional Message and Username
// attribute, Message first as in the spec's attribute table for FWD.
static size_t build(uint8_t *buf, size_t cap, enum sbcp_wire wire,
                    unsigned type, const char *user,
                    const void *text, size_t textlen)
{
    struct sbcp_builder b;

    sbcp_begin(&b, buf, cap, wire, type);
    if (text != NULL)
        sbcp_add_attr(&b, ATTR_MESSAGE, text, textlen);
    if (user != NULL)
        sbcp_add_attr(&b, ATTR_USERNAME, user, strlen(user));
    return sbcp_finish(&b);
}

// Sends to every joined client except `except`; returns how many.
static int broadcast(const struct client *except, unsigned type,
                     const char *user, const void *text, size_t textlen)
{
    uint8_t buf[2][SBCP_HDR_LEN + 2 * SBCP_ATTR_HDR_LEN +
                   MAX_MESSAGE_LEN + MAX_USERNAME_LEN];
    size_t  len[2] = { 0, 0 };          // built lazily, once per layout
    int     fd, count = 0;

    for (fd = 0; fd <= hi_fd; fd++) {
        struct client *c = clients[fd];

        if (c == NULL || c == except || c->dead || c->state != ST_JOINED)
            continue;
        if (len[c->wire] == 0)
            len[c->wire] = build(buf[c->wire], sizeof(buf[c->wire]), c->wire,
                                 type, user, text, textlen);
        client_send(c, buf[c->wire], len[c->wire]);
        count++;
    }
    return count;
}

// OFFLINE for every client that left. Sending can uncover more dead
// clients, so repeat until a pass finds none.
static void flush_offlines(void)
{
    int fd, again;

    do {
        again = 0;
        for (fd = 0; fd <= hi_fd; fd++) {
            struct client *c = clients[fd];
            int            n;

            if (c == NULL || !c->offline_due)
                continue;
            c->offline_due = 0;
            n = broadcast(NULL, TYPE_OFFLINE, c->name, NULL, 0);
            log_info("OFFLINE %s -> %d client(s)", c->name, n);
            again = 1;
        }
    } while (again);
}

// Announces departures, then closes and frees every dead client.
static void reap(void)
{
    int fd;

    flush_offlines();

    for (fd = 0; fd <= hi_fd; fd++) {
        struct client *c = clients[fd];

        if (c == NULL || !c->dead)
            continue;
        if (close(fd) < 0)
            log_err("close fd %d", fd);
        sbcp_reader_free(&c->rd);
        free(c->out.data);
        free(c);
        clients[fd] = NULL;
    }
    while (hi_fd >= 0 && clients[hi_fd] == NULL)
        hi_fd--;
}

// ---- Protocol ---------------------------------------------------------------

static struct client *find_joined(const char *name)
{
    int fd;

    for (fd = 0; fd <= hi_fd; fd++) {
        struct client *c = clients[fd];

        if (c != NULL && !c->dead && c->state == ST_JOINED &&
            strcmp(c->name, name) == 0)
            return c;
    }
    return NULL;
}

// NAK with a Reason, then a graceful close: FIN after the NAK is flushed and
// input drained until the client hangs up. Closing at once while its later
// bytes sat unread would send RST, which can destroy the NAK in flight.
static void reject(struct client *c, const char *attempted, const char *reason)
{
    uint8_t             buf[SBCP_HDR_LEN + SBCP_ATTR_HDR_LEN + MAX_REASON_LEN];
    struct sbcp_builder b;
    size_t              rlen = strlen(reason);

    if (rlen > MAX_REASON_LEN)
        rlen = MAX_REASON_LEN;

    log_info("%s: JOIN as \"%s\" rejected: %s", c->peer, attempted, reason);

    c->state            = ST_REJECTED;
    c->linger_until     = now_ms() + LINGER_MS;
    c->shut_after_flush = 1;
    sbcp_reader_drop(&c->rd);

    sbcp_begin(&b, buf, sizeof(buf), c->wire, TYPE_NAK);
    sbcp_add_attr(&b, ATTR_REASON, reason, rlen);
    client_send(c, buf, sbcp_finish(&b));
}

static int by_join_order(const void *a, const void *b)
{
    const struct client *x = *(const struct client *const *)a;
    const struct client *y = *(const struct client *const *)b;

    return (x->join_seq > y->join_seq) - (x->join_seq < y->join_seq);
}

// Bonus ACK: Client Count including the new client, then the usernames of
// everyone else, oldest member first.
static void send_ack(struct client *c)
{
    static uint8_t        buf[SBCP_FRAME_MAX];
    static struct client *others[FD_SETSIZE];
    struct sbcp_builder   b;
    size_t                n = 0, i;
    int                   fd;

    for (fd = 0; fd <= hi_fd; fd++) {
        struct client *o = clients[fd];

        if (o != NULL && o != c && !o->dead && o->state == ST_JOINED)
            others[n++] = o;
    }
    qsort(others, n, sizeof(others[0]), by_join_order);

    sbcp_begin(&b, buf, sizeof(buf), c->wire, TYPE_ACK);
    sbcp_add_u16(&b, ATTR_CLIENT_COUNT, (unsigned)n_joined);
    for (i = 0; i < n; i++)
        sbcp_add_attr(&b, ATTR_USERNAME, others[i]->name, strlen(others[i]->name));
    client_send(c, buf, sbcp_finish(&b));
}

static void handle_join(struct client *c, const struct sbcp_msg *m)
{
    struct sbcp_field a;
    char             name[MAX_USERNAME_LEN + 1];
    char             shown[64];
    char             reason[MAX_REASON_LEN + 1];
    int              len, n;

    // Only unknown clients may JOIN; a second JOIN is not a rename.
    if (c->state == ST_JOINED) {
        log_info("%s: another JOIN ignored (already in the chat)", c->name);
        return;
    }

    if (!sbcp_find_attr(m, ATTR_USERNAME, &a)) {
        reject(c, "", "Missing username");
        return;
    }
    len = sbcp_attr_text(&a, name, sizeof(name), MAX_USERNAME_LEN);
    if (len < 0 || !sbcp_username_ok(name, (size_t)len)) {
        sanitize(a.val, a.len < 24 ? a.len : 24, shown, sizeof(shown));
        reject(c, shown, "Invalid username (1-16 chars)");
        return;
    }

    // Announce departures first, so everyone sees OFFLINE before this ONLINE
    // (a departed client's name is already free).
    flush_offlines();

    if (find_joined(name) != NULL) {
        reject(c, name, "Username already in use");
        return;
    }
    if (n_joined >= max_clients) {
        snprintf(reason, sizeof(reason), "Server full (max %d clients)",
                 max_clients);
        reject(c, name, reason);
        return;
    }
    if (c->dead)
        return;

    memcpy(c->name, name, (size_t)len + 1);
    c->state    = ST_JOINED;
    c->idle     = 0;
    c->join_seq = ++joins_so_far;
    n_joined++;

    send_ack(c);
    n = broadcast(c, TYPE_ONLINE, c->name, NULL, 0);
    log_info("%s joined as \"%s\" [%d/%d in chat]: ACK sent, ONLINE -> %d client(s)",
             c->peer, c->name, n_joined, max_clients, n);
}

static void handle_send(struct client *c, const struct sbcp_msg *m)
{
    struct sbcp_field a;
    char             shown[256];
    size_t           len;
    int              n;

    if (c->state != ST_JOINED) {
        log_info("%s: SEND before JOIN discarded", c->peer);
        return;
    }
    if (!sbcp_find_attr(m, ATTR_MESSAGE, &a)) {
        log_info("%s: SEND without a Message attribute discarded", c->name);
        return;
    }

    len = a.len;
    while (len > 0 && a.val[len - 1] == '\0')
        len--;
    if (len == 0 || len > MAX_MESSAGE_LEN) {
        log_info("%s: SEND with a %zu-byte message discarded (allowed 1-%d)",
                 c->name, len, MAX_MESSAGE_LEN);
        return;
    }

    if (c->idle) {
        c->idle = 0;
        log_info("%s is active again", c->name);
    }

    // The Username comes from the JOIN, never from the SEND: no spoofing.
    n = broadcast(c, TYPE_FWD, c->name, a.val, len);

    sanitize(a.val, len < 80 ? len : 80, shown, sizeof(shown));
    log_info("SEND %s: \"%s%s\" -> FWD to %d client(s)",
             c->name, shown, len > 80 ? "..." : "", n);
}

static void handle_idle(struct client *c)
{
    int n;

    if (c->state != ST_JOINED) {
        log_info("%s: IDLE before JOIN discarded", c->peer);
        return;
    }
    if (c->idle) {
        log_info("%s: repeated IDLE ignored (already idle)", c->name);
        return;
    }
    c->idle = 1;
    n = broadcast(c, TYPE_IDLE, c->name, NULL, 0);
    log_info("IDLE %s -> %d client(s)", c->name, n);
}

static void dispatch(struct client *c, const uint8_t *frame, size_t len)
{
    struct sbcp_msg m;
    const char     *why;

    if (sbcp_parse(frame, len, &m, &why) < 0) {
        log_info("%s: discarded a %zu-byte message: %s", who(c), len, why);
        return;
    }

    if (!c->wire_known) {
        c->wire_known = 1;
        c->wire       = m.wire;
        if (m.wire == SBCP_WIRE_LEGACY)
            log_info("%s uses the little-endian bit-field layout; answering in kind",
                     c->peer);
    }

    switch (m.type) {
    case TYPE_JOIN:
        handle_join(c, &m);
        break;
    case TYPE_SEND:
        handle_send(c, &m);
        break;
    case TYPE_IDLE:
        handle_idle(c);
        break;
    default:
        log_info("%s: discarded a %s message (type %u): not something a client sends",
                 who(c), sbcp_type_name(m.type), m.type);
        break;
    }
}

static void on_readable(struct client *c)
{
    const uint8_t *frame;
    const char    *why;
    size_t         room, flen;
    uint8_t       *p;
    ssize_t        n;
    int64_t        now;
    enum sbcp_rd   r;

    p = sbcp_reader_space(&c->rd, &room);
    if (room == 0) {                    // cannot happen: a full buffer holds a whole message
        sbcp_reader_drop(&c->rd);
        p = sbcp_reader_space(&c->rd, &room);
    }

    n = recv(c->fd, p, room, 0);
    if (n == 0) {
        kill_client(c, "closed the connection");
        return;
    }
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        kill_client(c, strerror(errno));    // ECONNRESET after an RST, ETIMEDOUT, ...
        return;
    }

    // After a NAK nothing more is processed: the bytes are simply not committed.
    if (c->state == ST_REJECTED)
        return;

    now = now_ms();
    sbcp_reader_commit(&c->rd, (size_t)n, now);

    while (!c->dead && c->state != ST_REJECTED &&
           (r = sbcp_reader_next(&c->rd, &frame, &flen, &why, now)) != SBCP_RD_MORE) {
        if (r == SBCP_RD_JUNK)
            log_info("%s: %s", who(c), why);
        else
            dispatch(c, frame, flen);
    }
}

// ---- Connections ------------------------------------------------------------

// A client-side failure seen through accept(): skip it and carry on.
static int accept_error_is_transient(int e)
{
    switch (e) {
    case ECONNABORTED:  // client reset the connection before we accepted it
    case EPROTO:
    case EPERM:         // refused by a firewall rule
    case ENOPROTOOPT:
    case ENETDOWN:
    case ENETUNREACH:
    case EHOSTUNREACH:
    case EOPNOTSUPP:
#ifdef EHOSTDOWN
    case EHOSTDOWN:
#endif
#ifdef ENONET
    case ENONET:
#endif
        return 1;
    default:
        return 0;
    }
}

// Out of descriptors: the queued connection keeps the listening socket
// readable, and select() would spin on it. Free the spare descriptor, accept
// and close that one connection, then take the spare back.
static void shed_connection(int lfd)
{
    int fd;

    if (spare_fd >= 0) {
        close(spare_fd);
        spare_fd = -1;
    }
    if ((fd = accept(lfd, NULL, NULL)) >= 0) {
        close(fd);
        log_msg("out of file descriptors: turned one connection away");
    }
    if ((spare_fd = open("/dev/null", O_RDONLY)) < 0)
        accept_paused_until = now_ms() + 100;   // no spare: back off instead of spinning
}

static void enable_keepalive(int fd)
{
    int on = 1;

    // A client whose host vanishes (no FIN, no RST) is found and cleaned up.
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
#if defined(TCP_KEEPIDLE) && defined(TCP_KEEPINTVL) && defined(TCP_KEEPCNT)
    {
        int idle = 60, intvl = 10, cnt = 3;

        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    }
#endif
}

static void accept_clients(int lfd)
{
    int i;

    for (i = 0; i < ACCEPT_BATCH; i++) {
        struct client *c;
        int            fd = accept(lfd, NULL, NULL);

        if (fd < 0) {
            if (errno == EINTR || accept_error_is_transient(errno))
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            if (errno == EMFILE || errno == ENFILE) {
                shed_connection(lfd);
                return;
            }
            log_err("accept");          // ENOBUFS, ENOMEM: retry next round
            return;
        }

        // select() cannot watch a descriptor this high; FD_SET would corrupt memory.
        if (fd >= FD_SETSIZE) {
            log_msg("descriptor %d is beyond FD_SETSIZE (%d): connection refused",
                    fd, FD_SETSIZE);
            close(fd);
            continue;
        }

        if (net_set_nonblock(fd, 1) < 0 || (c = calloc(1, sizeof(*c))) == NULL) {
            log_err("setting up descriptor %d", fd);
            close(fd);
            continue;
        }
        if (sbcp_reader_init(&c->rd, RX_CAP) < 0) {
            log_err("buffer for descriptor %d", fd);
            free(c);
            close(fd);
            continue;
        }

        c->fd    = fd;
        c->state = ST_NEW;
        c->wire  = SBCP_WIRE_NET;
        net_peer_str(fd, c->peer, sizeof(c->peer));
        enable_keepalive(fd);

        clients[fd] = c;
        if (fd > hi_fd)
            hi_fd = fd;
        log_info("%s connected (fd %d)", c->peer, fd);
    }
}

// Rejected clients that never hang up, and messages that never complete.
// Returns milliseconds until the next deadline, or -1 if there is none.
static int64_t run_timers(int64_t now)
{
    int64_t next = -1;
    int     fd;

    for (fd = 0; fd <= hi_fd; fd++) {
        struct client *c = clients[fd];
        int64_t        due;

        if (c == NULL || c->dead)
            continue;

        if (c->state == ST_REJECTED) {
            due = c->linger_until;
            if (now >= due) {
                kill_client(c, "closed by the server after its NAK");
                continue;
            }
        } else if (sbcp_reader_pending(&c->rd)) {
            due = c->rd.since_ms + PARTIAL_TIMEOUT_MS;
            if (now >= due) {
                size_t k = sbcp_reader_drop(&c->rd);

                log_info("%s: discarded an incomplete message (%zu byte%s never "
                         "arrived within %d s)", who(c), k, k == 1 ? "" : "s",
                         PARTIAL_TIMEOUT_MS / 1000);
                continue;
            }
        } else {
            continue;
        }

        if (next < 0 || due - now < next)
            next = due - now;
    }
    return next;
}

// select() said EBADF: some descriptor is not open any more. Should never
// happen; if it does, drop that client instead of looping on the error.
static void drop_bad_descriptors(void)
{
    int fd;

    for (fd = 0; fd <= hi_fd; fd++)
        if (clients[fd] != NULL && fcntl(fd, F_GETFD) < 0)
            kill_client(clients[fd], "descriptor no longer valid");
}

static void serve(void)
{
    while (!stop_signal) {
        fd_set          rset, wset;
        struct timeval  tv, *tvp = NULL;
        int64_t         now = now_ms();
        int64_t         wait;
        int             maxfd, nready, fd, i;

        wait = run_timers(now);
        reap();

        FD_ZERO(&rset);
        FD_ZERO(&wset);
        FD_SET(sig_pipe[0], &rset);
        maxfd = sig_pipe[0];

        if (now >= accept_paused_until) {
            for (i = 0; i < n_listen; i++) {
                FD_SET(listen_fds[i], &rset);
                if (listen_fds[i] > maxfd)
                    maxfd = listen_fds[i];
            }
        } else if (wait < 0 || accept_paused_until - now < wait) {
            wait = accept_paused_until - now;
        }

        for (fd = 0; fd <= hi_fd; fd++) {
            struct client *c = clients[fd];

            if (c == NULL)
                continue;
            FD_SET(fd, &rset);
            if (c->out.tail > c->out.head)
                FD_SET(fd, &wset);      // only while output is backed up
            if (fd > maxfd)
                maxfd = fd;
        }

        if (wait >= 0) {
            tv.tv_sec  = (time_t)(wait / 1000);
            tv.tv_usec = (suseconds_t)(wait % 1000) * 1000;
            tvp = &tv;
        }

        nready = select(maxfd + 1, &rset, &wset, NULL, tvp);
        if (nready < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EBADF) {
                drop_bad_descriptors();
                continue;
            }
            log_err("select");
            continue;
        }
        if (nready == 0 || stop_signal)
            continue;

        for (i = 0; i < n_listen; i++)
            if (FD_ISSET(listen_fds[i], &rset))
                accept_clients(listen_fds[i]);

        // Descriptors accepted just now were not in the sets, so FD_ISSET is
        // false for them until the next round.
        for (fd = 0; fd <= hi_fd; fd++) {
            struct client *c = clients[fd];

            if (c == NULL || c->dead)
                continue;
            if (FD_ISSET(fd, &wset))
                flush(c);
            if (!c->dead && FD_ISSET(fd, &rset))
                on_readable(c);
        }

        reap();
    }
}

static void close_everything(void)
{
    int fd, i, n = 0;

    for (fd = 0; fd <= hi_fd; fd++) {
        struct client *c = clients[fd];

        if (c == NULL)
            continue;
        if (!c->dead)
            flush(c);                   // best effort; never blocks
        close(fd);
        sbcp_reader_free(&c->rd);
        free(c->out.data);
        free(c);
        clients[fd] = NULL;
        n++;
    }
    for (i = 0; i < n_listen; i++)
        close(listen_fds[i]);
    if (spare_fd >= 0)
        close(spare_fd);
    close(sig_pipe[0]);
    close(sig_pipe[1]);

    log_info("closed %d connection(s); server stopped", n);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s <server_ip> <server_port> <max_clients>\n"
            "  e.g. %s 127.0.0.1 9000 3\n"
            "       %s ::1 9000 3        IPv6 loopback\n"
            "       %s :: 9000 3         every interface, IPv4 and IPv6\n",
            prog, prog, prog, prog);
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
    char err[256];
    long port, maxc;
    int  i;

    if (argc != 4)
        usage(argv[0]);
    if (parse_long(argv[2], 1, 65535, &port) < 0)
        die_msg("invalid server_port \"%s\": expected an integer in 1-65535", argv[2]);
    if (parse_long(argv[3], 1, MAX_CLIENTS_LIMIT, &maxc) < 0)
        die_msg("invalid max_clients \"%s\": expected an integer in 1-%d",
                argv[3], MAX_CLIENTS_LIMIT);
    max_clients = (int)maxc;

    if (pipe(sig_pipe) < 0)
        die_sys("pipe");
    net_set_nonblock(sig_pipe[0], 1);
    net_set_nonblock(sig_pipe[1], 1);

    // Writing to a client that already reset the connection must return
    // EPIPE, not deliver SIGPIPE: its default action would end the server.
    set_signal(SIGPIPE, SIG_IGN, 0);
    // Started in the background (./server ... &), logging must not stop it.
    set_signal(SIGTTOU, SIG_IGN, 0);
    // No SA_RESTART, so select() returns EINTR and the loop can exit cleanly.
    set_signal(SIGINT, on_stop, 0);
    set_signal(SIGTERM, on_stop, 0);

    n_listen = net_listen(argv[1], argv[2], LISTENQ, listen_fds, MAX_LISTEN,
                          err, sizeof(err));
    if (n_listen == 0)
        die_msg("cannot listen on %s port %s: %s", argv[1], argv[2], err);

    for (i = 0; i < n_listen; i++) {
        char addr[NET_ADDRSTRLEN];

        // Non-blocking, so accept() cannot hang if the client resets first.
        if (net_set_nonblock(listen_fds[i], 1) < 0)
            die_sys("fcntl on the listening socket");
        net_sock_str(listen_fds[i], addr, sizeof(addr));
        log_info("SBCP v%d server listening on %s (max %d clients, pid %ld)",
                 SBCP_VERSION, addr, max_clients, (long)getpid());
    }

    spare_fd = open("/dev/null", O_RDONLY);

    serve();

    log_info("caught %s, shutting down", stop_signal == SIGINT ? "SIGINT" : "SIGTERM");
    close_everything();
    return EXIT_SUCCESS;
}
