// tests/test_sbcp.c -- automated tests for the SBCP server and client.
//
// Runs the real ./server and ./client over real sockets. Raw SBCP peers send
// hand-built bytes; client processes are driven through their stdin/stdout:
//   U*  unit tests of the server's codec (encoder, validator, reassembler)
//   A*  the bonus features' server side
//   B*  protocol conformance, byte for byte
//   C*  malformed input: discarded, never fatal
//   D*  disconnects and resource exhaustion (RST, SIGPIPE, slow readers, EMFILE)
//   F*  server command-line validation
//   I*  the client, end to end: against the real server or a scripted fake one
// Children get SIGPIPE back at its default before exec, so a program that
// forgot to ignore it dies here exactly as it would for a grader.
//
// Usage: ./tests/test_sbcp [-v] [--quick] [--only ID,ID,...] [--group LETTERS]
//                          [--list] [--server PATH] [--client PATH] [--wrap "CMD"]

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700                // posix_openpt() and friends for I33
#endif

#include "net.h"
#include "sbcp_codec.h"
#include "util.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_SRV     4
#define MAX_CLI     8
#define MAX_PEER    1300
#define CLI_BUF     (1 << 17)

static const char *server_path = "./server";
static const char *client_path = "./client";
static const char *wrap;            // e.g. "valgrind -q --error-exitcode=99"
static int         verbose, quick, slowdown = 1;
static int         n_pass, n_fail, n_skip;
static int         failed_now, skipped_now;
static char        fail_msg[2048];
static char        detail[512];
static const char *cur_id = "init";

// Scales a timeout when the server runs under valgrind.
#define T(ms) ((ms) * slowdown)

static void msleep(long ms)
{
    struct timespec ts;

    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

static void note(const char *fmt, ...) PRINTF_LIKE(1, 2);
static void test_fail(int line, const char *fmt, ...) PRINTF_LIKE(2, 3);
static void test_skip(const char *fmt, ...) PRINTF_LIKE(1, 2);

static void note(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
}

static void test_fail(int line, const char *fmt, ...)
{
    char    msg[512];
    va_list ap;

    if (failed_now)
        return;
    failed_now = 1;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (detail[0] != '\0')
        snprintf(fail_msg, sizeof(fail_msg), "line %d: %s (%s)", line, msg, detail);
    else
        snprintf(fail_msg, sizeof(fail_msg), "line %d: %s", line, msg);
}

static void test_skip(const char *fmt, ...)
{
    va_list ap;

    skipped_now = 1;
    va_start(ap, fmt);
    vsnprintf(fail_msg, sizeof(fail_msg), fmt, ap);
    va_end(ap);
}

#define CHECK(cond, ...) \
    do { if (!(cond)) { test_fail(__LINE__, __VA_ARGS__); return; } } while (0)
#define SKIP(...) \
    do { test_skip(__VA_ARGS__); return; } while (0)

// Deterministic PRNG for the fuzz case (xorshift64*).
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (uint32_t)((rng_state * 0x2545F4914F6CDD1Dull) >> 32);
}
static uint32_t rnd_below(uint32_t n) { return n ? rnd() % n : 0; }

// ---- processes ----------------------------------------------------------

static void close_from(int lo)
{
    struct rlimit rl;
    long          max = 4096;
    long          fd;

    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
        max = (long)rl.rlim_cur;
    if (max > 65536)
        max = 65536;
    for (fd = lo; fd < max; fd++)
        close((int)fd);
}

// Children start with the dispositions a grader's shell gives them: SIG_IGN
// survives exec(), and a background run would also hand down an ignored SIGINT.
static void child_signals(void)
{
    signal(SIGPIPE, SIG_DFL);
    signal(SIGALRM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
}

// ---- servers --------------------------------------------------------------

enum outmode { OUT_LOG, OUT_PIPE_CLOSED, OUT_PIPE_UNREAD };

struct srv {
    int   used;
    pid_t pid;
    int   port;
    int   exited, status;
    int   may_die;                      // the test kills it on purpose
    int   out_fd;                       // unread stdout pipe (OUT_PIPE_UNREAD)
    char  host[64];
    char  probe[64];                    // address to connect to
    char  log[256];
};

static struct srv srvs[MAX_SRV];

static int free_port(void)
{
    struct sockaddr_in6 a6;
    struct sockaddr_in  a4;
    socklen_t           len;
    int                 fd, port = 0, off = 0;

    // A port free on [::] with V6ONLY off is free for IPv4 as well.
    if ((fd = socket(AF_INET6, SOCK_STREAM, 0)) >= 0) {
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
        memset(&a6, 0, sizeof(a6));
        a6.sin6_family = AF_INET6;
        a6.sin6_addr   = in6addr_any;
        len = sizeof(a6);
        if (bind(fd, (struct sockaddr *)&a6, sizeof(a6)) == 0 &&
            getsockname(fd, (struct sockaddr *)&a6, &len) == 0)
            port = ntohs(a6.sin6_port);
        close(fd);
        if (port)
            return port;
    }
    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return 0;
    memset(&a4, 0, sizeof(a4));
    a4.sin_family      = AF_INET;
    a4.sin_addr.s_addr = htonl(INADDR_ANY);
    len = sizeof(a4);
    if (bind(fd, (struct sockaddr *)&a4, sizeof(a4)) == 0 &&
        getsockname(fd, (struct sockaddr *)&a4, &len) == 0)
        port = ntohs(a4.sin_port);
    close(fd);
    return port;
}

static int ipv6_loopback_ok(void)
{
    struct sockaddr_in6 a;
    int                 fd = socket(AF_INET6, SOCK_STREAM, 0), ok;

    if (fd < 0)
        return 0;
    memset(&a, 0, sizeof(a));
    a.sin6_family = AF_INET6;
    a.sin6_addr   = in6addr_loopback;
    ok = bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0;
    close(fd);
    return ok;
}

static int srv_alive(struct srv *s)
{
    pid_t r;

    if (s->exited)
        return 0;
    r = waitpid(s->pid, &s->status, WNOHANG);
    if (r == s->pid) {
        s->exited = 1;
        return 0;
    }
    return 1;
}

static int try_connect(const char *host, int port)
{
    char portstr[16], peer[NET_ADDRSTRLEN], err[256];
    int  fd;

    snprintf(portstr, sizeof(portstr), "%d", port);
    fd = net_connect(host, portstr, 2000, peer, sizeof(peer), err, sizeof(err));
    if (fd < 0)
        note("%s", err);
    return fd;
}

// Starts ./server host port maxc. nofile > 0 lowers RLIMIT_NOFILE first.
static struct srv *srv_start_on(const char *host, int port, int maxc,
                                int nofile, enum outmode mode)
{
    struct srv *s = NULL;
    char        portstr[16], maxstr[16];
    int         pfd[2] = { -1, -1 };
    int         i;
    int64_t     deadline;

    for (i = 0; i < MAX_SRV; i++)
        if (!srvs[i].used) {
            s = &srvs[i];
            break;
        }
    if (s == NULL) {
        note("too many servers in one test");
        return NULL;
    }
    memset(s, 0, sizeof(*s));
    s->used   = 1;
    s->out_fd = -1;
    s->port   = port ? port : free_port();
    snprintf(s->host, sizeof(s->host), "%s", host);
    if (strcmp(host, "::") == 0)
        snprintf(s->probe, sizeof(s->probe), "::1");
    else if (strcmp(host, "0.0.0.0") == 0 || strcmp(host, "localhost") == 0)
        snprintf(s->probe, sizeof(s->probe), "127.0.0.1");
    else
        snprintf(s->probe, sizeof(s->probe), "%s", host);
    snprintf(s->log, sizeof(s->log), "tests/logs/%s.server%d.log", cur_id, i);
    snprintf(portstr, sizeof(portstr), "%d", s->port);
    snprintf(maxstr, sizeof(maxstr), "%d", maxc);

    if (mode != OUT_LOG && pipe(pfd) < 0) {
        note("pipe: %s", strerror(errno));
        s->used = 0;
        return NULL;
    }

    if ((s->pid = fork()) < 0) {
        note("fork: %s", strerror(errno));
        s->used = 0;
        return NULL;
    }
    if (s->pid == 0) {
        int out = mode == OUT_LOG ? open(s->log, O_WRONLY | O_CREAT | O_TRUNC, 0644)
                                  : pfd[1];
        int nul = open("/dev/null", O_RDONLY);

        dup2(nul, STDIN_FILENO);
        dup2(out, STDOUT_FILENO);
        dup2(out, STDERR_FILENO);
        close_from(3);
        if (nofile > 0) {
            struct rlimit rl;

            getrlimit(RLIMIT_NOFILE, &rl);
            rl.rlim_cur = (rlim_t)nofile;
            if (rl.rlim_max != RLIM_INFINITY && rl.rlim_cur > rl.rlim_max)
                rl.rlim_cur = rl.rlim_max;
            setrlimit(RLIMIT_NOFILE, &rl);
        }
        child_signals();
        if (wrap != NULL) {
            char cmd[2048];

            snprintf(cmd, sizeof(cmd), "exec %s %s %s %s %s", wrap, server_path,
                     host, portstr, maxstr);
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        } else {
            execl(server_path, server_path, host, portstr, maxstr, (char *)NULL);
        }
        _exit(127);
    }

    if (mode == OUT_PIPE_CLOSED) {
        close(pfd[0]);                  // reader gone before the server writes a byte
        close(pfd[1]);
    } else if (mode == OUT_PIPE_UNREAD) {
        close(pfd[1]);
        s->out_fd = pfd[0];             // kept open, never read
    }

    deadline = now_ms() + T(5000);
    while (now_ms() < deadline) {
        int fd = try_connect(s->probe, s->port);

        if (fd >= 0) {
            close(fd);
            detail[0] = '\0';          // forget the refused attempts before it listened
            return s;
        }
        if (!srv_alive(s)) {
            note("server exited during startup (status 0x%x); see %s", s->status, s->log);
            return NULL;
        }
        msleep(30);
    }
    note("server never started listening");
    return NULL;
}

static struct srv *srv_start(const char *host, int maxc)
{
    return srv_start_on(host, 0, maxc, 0, OUT_LOG);
}

// SIGTERM, then wait; returns the wait status (or -1 if it had to be killed).
static int srv_stop(struct srv *s)
{
    int64_t deadline;

    if (!srv_alive(s))
        return s->status;
    s->may_die = 1;                     // ended on purpose from here on
    kill(s->pid, SIGTERM);
    deadline = now_ms() + T(5000) + (wrap ? 10000 : 0);
    while (now_ms() < deadline) {
        if (!srv_alive(s))
            return s->status;
        msleep(20);
    }
    kill(s->pid, SIGKILL);
    waitpid(s->pid, &s->status, 0);
    s->exited = 1;
    return -1;
}

static int log_has(const struct srv *s, const char *needle)
{
    static char buf[1 << 22];
    FILE       *fp = fopen(s->log, "r");
    size_t      n;

    if (fp == NULL)
        return 0;
    n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

static int server_fd_count(const struct srv *s)
{
    char           path[64];
    DIR           *d;
    struct dirent *e;
    int            n = 0;

    snprintf(path, sizeof(path), "/proc/%ld/fd", (long)s->pid);
    if ((d = opendir(path)) == NULL)
        return -1;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.')
            n++;
    closedir(d);
    return n;
}

// ---- raw peers ------------------------------------------------------------

struct peer {
    int                used;
    int                fd;
    int                rd_ok;
    struct sbcp_reader rd;
};

struct frame {
    uint8_t         buf[SBCP_FRAME_MAX];
    size_t          len;
    int             ok;
    struct sbcp_msg m;
};

static struct peer peers[MAX_PEER];

// Plain blocking connect(), bounded by SO_SNDTIMEO: unlike select() it also
// works for descriptors past FD_SETSIZE, which the D10 flood needs.
static int raw_connect(const char *host, int port, int rcvbuf)
{
    struct addrinfo hints, *res, *ai;
    char            portstr[16];
    int             fd = -1, rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_NUMERICSERV;
    snprintf(portstr, sizeof(portstr), "%d", port);
    if ((rc = getaddrinfo(host, portstr, &hints, &res)) != 0) {
        note("resolve %s: %s", host, gai_strerror(rc));
        return -1;
    }
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        struct timeval tv;

        tv.tv_sec  = 3 * slowdown;
        tv.tv_usec = 0;
        if ((fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) < 0)
            continue;
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (rcvbuf > 0)
            setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        note("connect to %s port %d: %s", host, port, strerror(errno));
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static struct peer *peer_connect_buf(const char *host, int port, int rcvbuf)
{
    struct peer *p = NULL;
    int          i, fd;

    for (i = 0; i < MAX_PEER; i++)
        if (!peers[i].used) {
            p = &peers[i];
            break;
        }
    if (p == NULL) {
        note("peer pool exhausted");
        return NULL;
    }
    if ((fd = raw_connect(host, port, rcvbuf)) < 0)
        return NULL;
    memset(p, 0, sizeof(*p));
    p->used = 1;
    p->fd   = fd;
    return p;
}

static struct peer *peer_connect(const char *host, int port)
{
    return peer_connect_buf(host, port, 0);
}

static void peer_free(struct peer *p)
{
    if (!p->used)
        return;
    if (p->fd >= 0)
        close(p->fd);
    if (p->rd_ok)
        sbcp_reader_free(&p->rd);
    p->used = 0;
    p->fd   = -1;
}

// SO_LINGER {1, 0}: close() sends RST, exactly what a crashed client looks like.
static void peer_rst(struct peer *p)
{
    struct linger lg = { 1, 0 };

    setsockopt(p->fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    peer_free(p);
}

static int peer_write(struct peer *p, const void *buf, size_t n)
{
    if (net_send_all(p->fd, buf, n) < 0) {
        note("send: %s", strerror(errno));
        return -1;
    }
    return 0;
}

// Builds a message: after the type, (attr type, value, length) triples ended
// by attr type 0; length -1 means strlen(value).
static size_t vmk(uint8_t *buf, size_t cap, enum sbcp_wire w, unsigned type,
                  va_list ap)
{
    struct sbcp_builder b;
    unsigned            at;

    sbcp_begin(&b, buf, cap, w, type);
    while ((at = va_arg(ap, unsigned)) != 0) {
        const char *v = va_arg(ap, const char *);
        int         n = va_arg(ap, int);

        sbcp_add_attr(&b, at, v, n < 0 ? strlen(v) : (size_t)n);
    }
    return sbcp_finish(&b);
}

static size_t mk(uint8_t *buf, size_t cap, enum sbcp_wire w, unsigned type, ...)
{
    va_list ap;
    size_t  n;

    va_start(ap, type);
    n = vmk(buf, cap, w, type, ap);
    va_end(ap);
    return n;
}

static int psend(struct peer *p, unsigned type, ...)
{
    uint8_t buf[SBCP_FRAME_MAX];
    va_list ap;
    size_t  n;

    va_start(ap, type);
    n = vmk(buf, sizeof(buf), SBCP_WIRE_NET, type, ap);
    va_end(ap);
    return n ? peer_write(p, buf, n) : -1;
}

// 1 = frame, 0 = timeout, -1 = EOF or error, -2 = bytes that are not SBCP.
static int peer_recv(struct peer *p, struct frame *f, int timeout_ms)
{
    int64_t deadline = now_ms() + timeout_ms;

    if (!p->rd_ok) {
        if (sbcp_reader_init(&p->rd, SBCP_FRAME_MAX) < 0)
            return -1;
        p->rd_ok = 1;
    }
    if (p->fd >= FD_SETSIZE) {
        note("descriptor too high for select()");
        return -1;
    }

    for (;;) {
        const uint8_t *fr;
        const char    *why;
        size_t         flen, room;
        uint8_t       *space;
        enum sbcp_rd   r = sbcp_reader_next(&p->rd, &fr, &flen, &why, now_ms());
        fd_set         rset;
        struct timeval tv;
        int64_t        left;
        ssize_t        n;

        if (r == SBCP_RD_FRAME) {
            memcpy(f->buf, fr, flen);
            f->len = flen;
            f->ok  = sbcp_parse(f->buf, flen, &f->m, &why) == 0;
            return 1;
        }
        if (r == SBCP_RD_JUNK) {
            note("received non-SBCP bytes: %s", why);
            return -2;
        }

        left = deadline - now_ms();
        if (left <= 0)
            return 0;
        FD_ZERO(&rset);
        FD_SET(p->fd, &rset);
        tv.tv_sec  = (time_t)(left / 1000);
        tv.tv_usec = (suseconds_t)(left % 1000) * 1000;
        n = select(p->fd + 1, &rset, NULL, NULL, &tv);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return 0;
        space = sbcp_reader_space(&p->rd, &room);
        n = recv(p->fd, space, room, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        sbcp_reader_commit(&p->rd, (size_t)n, now_ms());
    }
}

// Text attribute (not sanitized, trailing NULs kept); returns its length or -1.
static int fr_attr(const struct frame *f, unsigned type, char *out, size_t outsz)
{
    struct sbcp_field a;

    if (!f->ok || !sbcp_find_attr(&f->m, type, &a) || a.len >= outsz)
        return -1;
    memcpy(out, a.val, a.len);
    out[a.len] = '\0';
    return (int)a.len;
}

static int fr_count(const struct frame *f, unsigned long *count)
{
    struct sbcp_field a;

    return f->ok && sbcp_find_attr(&f->m, ATTR_CLIENT_COUNT, &a) &&
           sbcp_attr_count(&a, f->m.wire, count) == 0;
}

// Usernames in order, comma-separated.
static void fr_names(const struct frame *f, char *out, size_t outsz)
{
    struct sbcp_field a;
    size_t           pos = 0, used = 0;

    out[0] = '\0';
    while (f->ok && sbcp_next_attr(&f->m, &pos, &a)) {
        if (a.type != ATTR_USERNAME)
            continue;
        if (used + a.len + 2 >= outsz)
            break;
        if (used > 0)
            out[used++] = ',';
        memcpy(out + used, a.val, a.len);
        used += a.len;
        out[used] = '\0';
    }
}

static int check_frame(const struct frame *f, unsigned type, const char *user,
                       const char *text)
{
    char got[SBCP_FRAME_MAX];

    if (!f->ok) {
        note("malformed frame from the server");
        return 0;
    }
    if (f->m.type != type) {
        note("got %s, expected %s", sbcp_type_name(f->m.type), sbcp_type_name(type));
        return 0;
    }
    if (user != NULL && (fr_attr(f, ATTR_USERNAME, got, sizeof(got)) < 0 ||
                         strcmp(got, user) != 0)) {
        note("%s username \"%s\", expected \"%s\"", sbcp_type_name(type), got, user);
        return 0;
    }
    if (text != NULL && (fr_attr(f, ATTR_MESSAGE, got, sizeof(got)) < 0 ||
                         strcmp(got, text) != 0)) {
        note("%s message \"%.60s\", expected \"%.60s\"", sbcp_type_name(type), got, text);
        return 0;
    }
    return 1;
}

// The very next frame must be `type` (and carry user/text when given).
static int expect_msg(struct peer *p, unsigned type, const char *user,
                      const char *text, int timeout_ms)
{
    static struct frame f;
    int                 r = peer_recv(p, &f, timeout_ms);

    if (r == 0) {
        note("timed out waiting for %s", sbcp_type_name(type));
        return 0;
    }
    if (r < 0) {
        if (r == -1)
            note("connection closed while waiting for %s", sbcp_type_name(type));
        return 0;
    }
    return check_frame(&f, type, user, text);
}

// Skips other frames until a matching one arrives.
static int wait_for(struct peer *p, unsigned type, const char *user,
                    const char *text, int timeout_ms)
{
    static struct frame f;
    int64_t             deadline = now_ms() + timeout_ms;

    for (;;) {
        int64_t left = deadline - now_ms();
        int     r;

        if (left <= 0) {
            note("timed out waiting for %s", sbcp_type_name(type));
            return 0;
        }
        r = peer_recv(p, &f, (int)left);
        if (r == 0)
            continue;
        if (r < 0) {
            note("connection closed while waiting for %s", sbcp_type_name(type));
            return 0;
        }
        if (f.ok && f.m.type == type) {
            char save[sizeof(detail)];

            memcpy(save, detail, sizeof(save));
            if (check_frame(&f, type, user, text))
                return 1;
            memcpy(detail, save, sizeof(save));
        }
    }
}

static int expect_none(struct peer *p, int ms)
{
    static struct frame f;
    int                 r = peer_recv(p, &f, ms);

    if (r == 1) {
        note("unexpected %s", f.ok ? sbcp_type_name(f.m.type) : "malformed frame");
        return 0;
    }
    if (r == -1) {
        note("connection closed unexpectedly");
        return 0;
    }
    return r == 0;
}

static int expect_eof(struct peer *p, int ms)
{
    static struct frame f;
    int                 r = peer_recv(p, &f, ms);

    if (r == 1)
        note("got %s instead of EOF", f.ok ? sbcp_type_name(f.m.type) : "garbage");
    else if (r == 0)
        note("connection still open after %d ms", ms);
    return r == -1;
}

static char drained_offline[64];        // last OFFLINE name drain_count() saw

// Reads frames until `want` of type have arrived; other frames are counted
// in *others. Returns how many of `type` were seen.
static int drain_count(struct peer *p, unsigned type, int want, int timeout_ms,
                       int *others)
{
    static struct frame f;
    int64_t             deadline = now_ms() + timeout_ms;
    int                 got = 0;

    while (got < want) {
        int64_t left = deadline - now_ms();
        int     r;

        if (left <= 0)
            break;
        r = peer_recv(p, &f, (int)left);
        if (r < 0)
            break;
        if (r == 1) {
            if (f.ok && f.m.type == TYPE_OFFLINE)
                fr_attr(&f, ATTR_USERNAME, drained_offline, sizeof(drained_offline));
            if (f.ok && f.m.type == type)
                got++;
            else if (others != NULL)
                (*others)++;
        }
    }
    return got;
}

// Connects, JOINs and consumes the ACK. NULL if anything went wrong.
static struct peer *join_as_buf(const struct srv *s, const char *name, int rcvbuf)
{
    static struct frame f;
    struct peer        *p = peer_connect_buf(s->probe, s->port, rcvbuf);
    int                 r;

    if (p == NULL)
        return NULL;
    if (psend(p, TYPE_JOIN, ATTR_USERNAME, name, -1, 0) < 0) {
        peer_free(p);
        return NULL;
    }
    r = peer_recv(p, &f, T(3000));
    if (r != 1 || !f.ok || f.m.type != TYPE_ACK) {
        char reason[64] = "";

        fr_attr(&f, ATTR_REASON, reason, sizeof(reason));
        note("JOIN as %s: %s %s", name,
             r != 1 ? "no reply" : sbcp_type_name(f.m.type), reason);
        peer_free(p);
        return NULL;
    }
    return p;
}

static struct peer *join_as(const struct srv *s, const char *name)
{
    return join_as_buf(s, name, 0);
}

// JOIN and return the reply frame instead of insisting on an ACK.
static int join_reply(struct peer *p, const char *name, int len, struct frame *f)
{
    if (psend(p, TYPE_JOIN, ATTR_USERNAME, name, len, 0) < 0)
        return -1;
    return peer_recv(p, f, T(3000));
}

// ---- child processes: the client, and the server run with bad arguments -------

struct cli {
    int    used;
    pid_t  pid;
    int    in, out;
    int    exited, status;
    size_t len, scan;
    char   buf[CLI_BUF];
};

static struct cli clis[MAX_CLI];

static struct cli *cli_spawn(char *const argv[], const char *env)
{
    struct cli *c = NULL;
    int         in[2], out[2], i;

    for (i = 0; i < MAX_CLI; i++)
        if (!clis[i].used) {
            c = &clis[i];
            break;
        }
    if (c == NULL || pipe(in) < 0 || pipe(out) < 0) {
        note("cannot spawn client");
        return NULL;
    }
    memset(c, 0, offsetof(struct cli, buf));
    c->used   = 1;
    c->buf[0] = '\0';

    if ((c->pid = fork()) < 0) {
        c->used = 0;
        return NULL;
    }
    if (c->pid == 0) {
        dup2(in[0], STDIN_FILENO);
        dup2(out[1], STDOUT_FILENO);
        dup2(out[1], STDERR_FILENO);
        close_from(3);
        child_signals();
        if (env != NULL)
            putenv((char *)env);
        execv(argv[0], argv);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    c->in  = in[1];
    c->out = out[0];
    net_set_nonblock(c->out, 1);
    return c;
}

// Pulls whatever output is available within ms. -1 once the pipe hits EOF.
static int cli_fill(struct cli *c, int ms)
{
    fd_set         rset;
    struct timeval tv;
    ssize_t        n;

    if (c->out < 0)
        return -1;
    if (c->len + 4096 > CLI_BUF) {      // keep room: forget what was consumed
        memmove(c->buf, c->buf + c->scan, c->len - c->scan);
        c->len -= c->scan;
        c->scan = 0;
        c->buf[c->len] = '\0';
    }
    FD_ZERO(&rset);
    FD_SET(c->out, &rset);
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    if (select(c->out + 1, &rset, NULL, NULL, &tv) <= 0)
        return 0;
    n = read(c->out, c->buf + c->len, CLI_BUF - 1 - c->len);
    if (n > 0) {
        c->len += (size_t)n;
        c->buf[c->len] = '\0';
        if (verbose)
            fprintf(stderr, "    [client %ld] %.*s", (long)c->pid, (int)n,
                    c->buf + c->len - n);
        return (int)n;
    }
    if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
        close(c->out);
        c->out = -1;
        return -1;
    }
    return 0;
}

static struct cli *cli_start(const char *name, const char *host, int port,
                             const char *env)
{
    char  portstr[16];
    char *argv[5];

    snprintf(portstr, sizeof(portstr), "%d", port);
    argv[0] = (char *)client_path;
    argv[1] = (char *)name;
    argv[2] = (char *)host;
    argv[3] = portstr;
    argv[4] = NULL;
    return cli_spawn(argv, env);
}

static void last_line(const struct cli *c, char *out, size_t outsz)
{
    const char *end = c->buf + c->len, *p;

    while (end > c->buf && (end[-1] == '\n' || end[-1] == '\r'))
        end--;
    p = end;
    while (p > c->buf && p[-1] != '\n')
        p--;
    snprintf(out, outsz, "%.*s", (int)(end - p), p);
}

// Waits for needle in the output after what earlier expects consumed.
static int cli_expect(struct cli *c, const char *needle, int ms)
{
    int64_t deadline = now_ms() + ms;

    for (;;) {
        char *hit = strstr(c->buf + c->scan, needle);
        char  line[200];

        if (hit != NULL) {
            c->scan = (size_t)(hit - c->buf) + strlen(needle);
            return 1;
        }
        if (now_ms() >= deadline || (c->out < 0 && cli_fill(c, 0) < 0)) {
            last_line(c, line, sizeof(line));
            note("client output lacks \"%s\"; last line: \"%s\"", needle, line);
            return 0;
        }
        cli_fill(c, (int)(deadline - now_ms()));
    }
}

// Waits up to ms for needle anywhere in the output; consumes nothing.
static int cli_has(struct cli *c, const char *needle, int ms)
{
    int64_t deadline = now_ms() + ms;

    for (;;) {
        if (strstr(c->buf, needle) != NULL)
            return 1;
        if (now_ms() >= deadline || c->out < 0)
            return 0;
        cli_fill(c, (int)(deadline - now_ms()));
    }
}

// Anywhere in everything the client printed so far.
static int cli_ever(struct cli *c, const char *needle)
{
    cli_fill(c, 0);
    return strstr(c->buf, needle) != NULL;
}

static int cli_write(struct cli *c, const void *buf, size_t n)
{
    return c->in >= 0 && write(c->in, buf, n) == (ssize_t)n ? 0 : -1;
}

static int cli_say(struct cli *c, const char *text)
{
    if (cli_write(c, text, strlen(text)) < 0 || cli_write(c, "\n", 1) < 0) {
        note("write to client stdin: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static void cli_eof(struct cli *c)
{
    if (c->in >= 0)
        close(c->in);
    c->in = -1;
}

// 1 once the process has exited (status in c->status).
static int cli_wait(struct cli *c, int ms)
{
    int64_t deadline = now_ms() + ms;

    while (!c->exited) {
        if (waitpid(c->pid, &c->status, WNOHANG) == c->pid) {
            c->exited = 1;
            break;
        }
        if (now_ms() >= deadline) {
            note("client still running after %d ms", ms);
            return 0;
        }
        cli_fill(c, 20);
    }
    while (cli_fill(c, 0) > 0)
        ;
    return 1;
}

static void cli_free(struct cli *c)
{
    if (!c->used)
        return;
    if (!c->exited) {
        kill(c->pid, SIGKILL);
        waitpid(c->pid, &c->status, 0);
    }
    if (c->in >= 0)
        close(c->in);
    if (c->out >= 0)
        close(c->out);
    c->used = 0;
}

// Runs a program to completion (client or server with bad arguments).
static int run_status(char *const argv[], int ms, char *out, size_t outsz)
{
    struct cli *c = cli_spawn(argv, NULL);
    int         ok;

    if (c == NULL)
        return -1;
    cli_eof(c);
    ok = cli_wait(c, ms);
    snprintf(out, outsz, "%s", c->buf);
    if (!ok) {
        cli_free(c);
        return -1;
    }
    cli_free(c);
    return WIFEXITED(c->status) ? WEXITSTATUS(c->status) : 128 + WTERMSIG(c->status);
}

// ---- fake server for client tests, and a listener that occupies a port --------

static int fake_lfds[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };

static int fake_listen(int *port)
{
    struct sockaddr_in a;
    socklen_t          len = sizeof(a);
    int                fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 8) < 0 ||
        getsockname(fd, (struct sockaddr *)&a, &len) < 0) {
        close(fd);
        return -1;
    }
    *port = ntohs(a.sin_port);
    for (len = 0; len < 8; len++)
        if (fake_lfds[len] < 0) {
            fake_lfds[len] = fd;
            break;
        }
    return fd;
}

static void fake_close(int lfd)
{
    int i;

    for (i = 0; i < 8; i++)
        if (fake_lfds[i] == lfd)
            fake_lfds[i] = -1;
    close(lfd);
}

static struct peer *fake_accept(int lfd, int ms)
{
    struct peer   *p = NULL;
    fd_set         rset;
    struct timeval tv;
    int            i, fd;

    FD_ZERO(&rset);
    FD_SET(lfd, &rset);
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    if (select(lfd + 1, &rset, NULL, NULL, &tv) <= 0 || (fd = accept(lfd, NULL, NULL)) < 0) {
        note("the client never connected");
        return NULL;
    }
    for (i = 0; i < MAX_PEER; i++)
        if (!peers[i].used) {
            p = &peers[i];
            break;
        }
    if (p == NULL) {
        close(fd);
        return NULL;
    }
    memset(p, 0, sizeof(*p));
    p->used = 1;
    p->fd   = fd;
    return p;
}

static int bytes_equal(const struct frame *f, const char *hex_bytes, size_t n)
{
    char   got[3 * 64 + 1];
    size_t i, o = 0;

    if (f->len == n && memcmp(f->buf, hex_bytes, n) == 0)
        return 1;
    for (i = 0; i < f->len && i < 64; i++)
        o += (size_t)snprintf(got + o, sizeof(got) - o, "%02x ", f->buf[i]);
    note("bytes differ; got %s", got);
    return 0;
}

#define GOLDEN(f, lit) bytes_equal((f), (lit), sizeof(lit) - 1)

// ============================================================================
// U: the server's codec
// ============================================================================

static void t_u1_encoder_golden(void)
{
    uint8_t buf[128];
    size_t  n;

    n = mk(buf, sizeof(buf), SBCP_WIRE_NET, TYPE_JOIN, ATTR_USERNAME, "alice", -1, 0);
    CHECK(n == 13 && memcmp(buf, "\x01\x82\x00\x0d\x00\x02\x00\x09" "alice", 13) == 0,
          "JOIN alice encodes wrong");
    n = mk(buf, sizeof(buf), SBCP_WIRE_NET, TYPE_IDLE, 0);
    CHECK(n == 4 && memcmp(buf, "\x01\x89\x00\x04", 4) == 0, "IDLE encodes wrong");
    n = mk(buf, sizeof(buf), SBCP_WIRE_LEGACY, TYPE_JOIN, ATTR_USERNAME, "alice", -1, 0);
    CHECK(n == 13 && memcmp(buf, "\x03\x04\x0d\x00\x02\x00\x09\x00" "alice", 13) == 0,
          "legacy JOIN encodes wrong");
    {
        struct sbcp_builder b;

        sbcp_begin(&b, buf, sizeof(buf), SBCP_WIRE_NET, TYPE_ACK);
        sbcp_add_u16(&b, ATTR_CLIENT_COUNT, 258);
        n = sbcp_finish(&b);
        CHECK(n == 10 && memcmp(buf, "\x01\x87\x00\x0a\x00\x03\x00\x06\x01\x02", 10) == 0,
              "ACK count encodes wrong");
        sbcp_begin(&b, buf, 8, SBCP_WIRE_NET, TYPE_SEND);
        sbcp_add_attr(&b, ATTR_MESSAGE, "too long", 8);
        CHECK(sbcp_finish(&b) == 0, "builder overflow not reported");
    }
}

static void t_u2_parser_rejects(void)
{
    static const struct {
        const char *bytes;
        size_t      n;
        const char *what;
    } bad[] = {
        { "\x01\x84\x00",                        3, "short header" },
        { "\x02\x04\x00\x04",                    4, "version 4" },
        { "\x00\x84\x00\x04",                    4, "version 1" },
        { "\x01\x04\x00\x04",                    4, "version 2" },
        { "\x01\x84\x00\x05",                    4, "Length above bytes" },
        { "\x01\x84\x00\x04\x00",                5, "Length below bytes" },
        { "\x01\x84\x00\x08\x00\x04\x00\x02",    8, "attribute Length 2" },
        { "\x01\x84\x00\x0a\x00\x04\x00\x10hi", 10, "attribute overruns" },
        { "\x01\x84\x00\x08\x00\x04\x00\x05",    8, "attribute Length past end" },
        { "\x01\x84\x00\x09\x00\x04\x00\x04z",   9, "stray byte" },
    };
    struct sbcp_msg m;
    const char     *why;
    size_t          i;

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(sbcp_parse((const uint8_t *)bad[i].bytes, bad[i].n, &m, &why) < 0,
              "parser accepted: %s", bad[i].what);

    CHECK(sbcp_parse((const uint8_t *)"\x01\x84\x00\x0b\x00\x04\x00\x05x\x00\x00", 11,
                     &m, &why) == 0, "zero padding after the last attribute rejected");
    CHECK(sbcp_parse((const uint8_t *)"\x01\x89\x00\x04", 4, &m, &why) == 0 &&
          m.type == TYPE_IDLE, "bare IDLE rejected");
}

static void t_u3_reader(void)
{
    uint8_t            stream[256];
    struct sbcp_reader r;
    const uint8_t     *f;
    const char        *why;
    size_t             n1, n2, flen, room, i;
    uint8_t           *sp;
    int                frames = 0;

    n1 = mk(stream, sizeof(stream), SBCP_WIRE_NET, TYPE_SEND, ATTR_MESSAGE, "first", -1, 0);
    n2 = mk(stream + n1, sizeof(stream) - n1, SBCP_WIRE_NET, TYPE_SEND,
            ATTR_MESSAGE, "second", -1, 0);
    CHECK(sbcp_reader_init(&r, 64) == 0, "reader init");

    // One byte at a time: exactly two frames, each only once complete.
    for (i = 0; i < n1 + n2; i++) {
        sp = sbcp_reader_space(&r, &room);
        CHECK(room > 0, "no room");
        *sp = stream[i];
        sbcp_reader_commit(&r, 1, 0);
        while (sbcp_reader_next(&r, &f, &flen, &why, 0) == SBCP_RD_FRAME) {
            frames++;
            CHECK((frames == 1 && i == n1 - 1 && flen == n1) ||
                  (frames == 2 && i == n1 + n2 - 1 && flen == n2),
                  "frame %d surfaced at byte %zu", frames, i);
        }
    }
    CHECK(frames == 2 && !sbcp_reader_pending(&r), "byte-by-byte gave %d frames", frames);

    // Both in one chunk.
    sp = sbcp_reader_space(&r, &room);
    memcpy(sp, stream, n1 + n2);
    sbcp_reader_commit(&r, n1 + n2, 0);
    CHECK(sbcp_reader_next(&r, &f, &flen, &why, 0) == SBCP_RD_FRAME && flen == n1, "coalesced 1");
    CHECK(sbcp_reader_next(&r, &f, &flen, &why, 0) == SBCP_RD_FRAME && flen == n2, "coalesced 2");
    CHECK(sbcp_reader_next(&r, &f, &flen, &why, 0) == SBCP_RD_MORE, "coalesced 3");

    // Garbage is dropped, the next message is found.
    sp = sbcp_reader_space(&r, &room);
    memcpy(sp, "GET / HTTP/1.0\r\n", 16);
    sbcp_reader_commit(&r, 16, 0);
    CHECK(sbcp_reader_next(&r, &f, &flen, &why, 0) == SBCP_RD_JUNK, "garbage not flagged");
    CHECK(!sbcp_reader_pending(&r), "garbage left behind");

    // Oversized (Length 1000 > cap 64): skipped exactly, then the next frame.
    {
        uint8_t big[1000 + 64];

        memset(big, 'x', sizeof(big));
        big[0] = 0x01; big[1] = 0x84; big[2] = 0x03; big[3] = 0xE8;
        memcpy(big + 1000, stream, n1);
        for (i = 0; i < 1000 + n1; ) {
            size_t k;

            sp = sbcp_reader_space(&r, &room);
            k = room < 1000 + n1 - i ? room : 1000 + n1 - i;
            memcpy(sp, big + i, k);
            sbcp_reader_commit(&r, k, 0);
            i += k;
            while (sbcp_reader_next(&r, &f, &flen, &why, 0) != SBCP_RD_MORE)
                if (flen == n1 && memcmp(f, stream, n1) == 0)
                    frames++;
        }
        CHECK(frames == 3, "frame after an oversized message was lost");
    }

    // Partial message: pending, then dropped.
    sp = sbcp_reader_space(&r, &room);
    memcpy(sp, stream, 5);
    sbcp_reader_commit(&r, 5, 1000);
    CHECK(sbcp_reader_next(&r, &f, &flen, &why, 1000) == SBCP_RD_MORE &&
          sbcp_reader_pending(&r) && r.since_ms == 1000, "partial not pending");
    CHECK(sbcp_reader_drop(&r) == 5 && !sbcp_reader_pending(&r), "drop");
    sbcp_reader_free(&r);
}

static void t_u4_text_rules(void)
{
    struct sbcp_field a;
    char             out[64], shown[64];

    a.val = (const uint8_t *)"dave\0\0";
    a.len = 6;
    CHECK(sbcp_attr_text(&a, out, sizeof(out), 16) == 4 && strcmp(out, "dave") == 0,
          "trailing NULs not stripped");
    a.len = 2;
    a.val = (const uint8_t *)"\0\0";
    CHECK(sbcp_attr_text(&a, out, sizeof(out), 16) < 0, "all-NUL text accepted");
    a.val = (const uint8_t *)"abcdefghijklmnopq";
    a.len = 17;
    CHECK(sbcp_attr_text(&a, out, sizeof(out), 16) < 0, "17-byte username accepted");

    CHECK(sbcp_username_ok("abcdefghijklmnop", 16), "16-byte username rejected");
    CHECK(!sbcp_username_ok("", 0), "empty username accepted");
    CHECK(!sbcp_username_ok("a\x1b" "b", 3), "control character accepted");
    CHECK(sbcp_username_ok("a b", 3), "space rejected");
    CHECK(sbcp_username_ok("\xc3\xa9t\xc3\xa9", 5), "UTF-8 rejected");

    sanitize("a\x1b[31mb\nc\x7f", 10, shown, sizeof(shown));
    CHECK(strcmp(shown, "a^[[31mb^Jc^?") == 0, "sanitize gave \"%s\"", shown);
}

static void t_u5_legacy_header(void)
{
    enum sbcp_wire w;
    unsigned       type;
    size_t         len;
    unsigned       t;

    for (t = 0; t < 128; t++) {
        uint8_t net[4], leg[4];

        net[0] = (uint8_t)((3u << 7 | t) >> 8);
        net[1] = (uint8_t)((3u << 7 | t) & 0xFF);
        net[2] = 0x01;
        net[3] = 0x02;
        leg[0] = (uint8_t)((3u | t << 9) & 0xFF);
        leg[1] = (uint8_t)((3u | t << 9) >> 8);
        leg[2] = 0x02;
        leg[3] = 0x01;
        CHECK(sbcp_decode_header(net, &w, &type, &len) == 0 && w == SBCP_WIRE_NET &&
              type == t && len == 0x0102, "network layout type %u", t);
        CHECK(sbcp_decode_header(leg, &w, &type, &len) == 0 && w == SBCP_WIRE_LEGACY &&
              type == t && len == 0x0102, "legacy layout type %u", t);
    }
}

// ============================================================================
// A: bonus features, server side
// ============================================================================

static void t_a5_ack_nak_online_offline(void)
{
    static struct frame f;
    struct srv         *s = srv_start("127.0.0.1", 3);
    struct peer        *p1, *p2, *p3, *p4, *p5, *p6;
    unsigned long       count;
    char                names[256], reason[64];

    CHECK(s != NULL, "server did not start");
    p1 = peer_connect(s->probe, s->port);
    CHECK(p1 && join_reply(p1, "p1", -1, &f) == 1 && f.ok && f.m.type == TYPE_ACK,
          "p1 got no ACK");
    fr_names(&f, names, sizeof(names));
    CHECK(fr_count(&f, &count) && count == 1 && names[0] == '\0', "p1 ACK: count %lu names \"%s\"",
          count, names);

    p2 = peer_connect(s->probe, s->port);
    CHECK(p2 && join_reply(p2, "p2", -1, &f) == 1 && f.ok && f.m.type == TYPE_ACK, "p2 no ACK");
    fr_names(&f, names, sizeof(names));
    CHECK(fr_count(&f, &count) && count == 2 && strcmp(names, "p1") == 0,
          "p2 ACK: count %lu names \"%s\" (count must include p2, list must not)", count, names);
    CHECK(expect_msg(p1, TYPE_ONLINE, "p2", NULL, T(2000)), "p1 got no ONLINE p2");
    CHECK(expect_none(p2, 200), "p2 got something besides its ACK");

    p3 = peer_connect(s->probe, s->port);
    CHECK(p3 && join_reply(p3, "p3", -1, &f) == 1 && f.ok && f.m.type == TYPE_ACK, "p3 no ACK");
    fr_names(&f, names, sizeof(names));
    CHECK(fr_count(&f, &count) && count == 3 && strcmp(names, "p1,p2") == 0,
          "p3 ACK: count %lu names \"%s\" (expected join order p1,p2)", count, names);
    CHECK(expect_msg(p1, TYPE_ONLINE, "p3", NULL, T(2000)) &&
          expect_msg(p2, TYPE_ONLINE, "p3", NULL, T(2000)), "ONLINE p3 missing");

    p4 = peer_connect(s->probe, s->port);
    CHECK(p4 && join_reply(p4, "p4", -1, &f) == 1 && f.ok && f.m.type == TYPE_NAK, "no NAK when full");
    CHECK(fr_attr(&f, ATTR_REASON, reason, sizeof(reason)) > 0 &&
          strcmp(reason, "Server full (max 3 clients)") == 0, "NAK reason \"%s\"", reason);
    CHECK(expect_eof(p4, T(1000)), "server kept the rejected connection open");

    p5 = peer_connect(s->probe, s->port);
    CHECK(p5 && join_reply(p5, "p1", -1, &f) == 1 && f.ok && f.m.type == TYPE_NAK &&
          fr_attr(&f, ATTR_REASON, reason, sizeof(reason)) > 0 &&
          strcmp(reason, "Username already in use") == 0, "duplicate got no NAK");

    peer_free(p2);
    CHECK(expect_msg(p1, TYPE_OFFLINE, "p2", NULL, T(2000)) &&
          expect_msg(p3, TYPE_OFFLINE, "p2", NULL, T(2000)), "OFFLINE p2 missing");

    p6 = peer_connect(s->probe, s->port);
    CHECK(p6 && join_reply(p6, "p2", -1, &f) == 1 && f.ok && f.m.type == TYPE_ACK &&
          fr_count(&f, &count) && count == 3, "slot or name not freed");
    CHECK(expect_msg(p1, TYPE_ONLINE, "p2", NULL, T(2000)) &&
          expect_msg(p3, TYPE_ONLINE, "p2", NULL, T(2000)), "ONLINE for the new p2 missing");
    CHECK(expect_none(p1, 200) && expect_none(p3, 1), "stray notifications");
}

static void t_a6_idle_relay(void)
{
    struct srv  *s = srv_start("127.0.0.1", 5);
    struct peer *p1, *p2, *p3, *pu;

    CHECK(s != NULL, "server did not start");
    p1 = join_as(s, "p1");
    p2 = join_as(s, "p2");
    CHECK(p1 && p2 && expect_msg(p1, TYPE_ONLINE, "p2", NULL, T(2000)), "setup");
    p3 = join_as(s, "p3");
    CHECK(p3 && expect_msg(p1, TYPE_ONLINE, "p3", NULL, T(2000)) &&
          expect_msg(p2, TYPE_ONLINE, "p3", NULL, T(2000)), "setup p3");

    CHECK(psend(p1, TYPE_IDLE, 0) == 0, "send IDLE");
    CHECK(expect_msg(p2, TYPE_IDLE, "p1", NULL, T(2000)) &&
          expect_msg(p3, TYPE_IDLE, "p1", NULL, T(2000)), "IDLE not relayed with the username");
    CHECK(expect_none(p1, 300), "the idle client got its own IDLE");

    CHECK(psend(p1, TYPE_IDLE, 0) == 0 && expect_none(p2, 300),
          "a repeated IDLE was relayed again");

    CHECK(psend(p1, TYPE_SEND, ATTR_MESSAGE, "back", -1, 0) == 0 &&
          expect_msg(p2, TYPE_FWD, "p1", "back", T(2000)) &&
          expect_msg(p3, TYPE_FWD, "p1", "back", T(2000)), "SEND after IDLE");
    CHECK(psend(p1, TYPE_IDLE, 0) == 0 && expect_msg(p2, TYPE_IDLE, "p1", NULL, T(2000)),
          "IDLE after activity not relayed");
    CHECK(expect_msg(p3, TYPE_IDLE, "p1", NULL, T(2000)), "IDLE after activity (p3)");

    // IDLE carrying someone else's name still reports the sender.
    CHECK(psend(p2, TYPE_IDLE, ATTR_USERNAME, "p3", -1, 0) == 0 &&
          expect_msg(p1, TYPE_IDLE, "p2", NULL, T(2000)), "IDLE spoofing");
    CHECK(expect_msg(p3, TYPE_IDLE, "p2", NULL, T(2000)), "IDLE p2 to p3");

    pu = peer_connect(s->probe, s->port);
    CHECK(pu && psend(pu, TYPE_IDLE, 0) == 0 && expect_none(p1, 300),
          "IDLE from a client that never joined was relayed");
}

static struct frame scratch;

static void t_a9_localhost(void)
{
    struct srv  *s = srv_start("localhost", 3);
    struct peer *p4, *p6;

    CHECK(s != NULL, "server on localhost did not start");
    p4 = peer_connect("127.0.0.1", s->port);
    CHECK(p4 && join_reply(p4, "four", -1, &scratch) == 1,
          "127.0.0.1 unreachable on a localhost server");
    if (log_has(s, "listening on [::1]")) {
        p6 = peer_connect("::1", s->port);
        CHECK(p6 && join_reply(p6, "six", -1, &scratch) == 1,
              "::1 unreachable although the server bound it");
        CHECK(expect_msg(p4, TYPE_ONLINE, "six", NULL, T(2000)), "one room for both");
    }
}

// ============================================================================
// B: protocol conformance
// ============================================================================

static void t_b1_golden_bytes(void)
{
    static struct frame f;
    struct srv         *s = srv_start("127.0.0.1", 3);
    struct peer        *pa, *pb, *pc;

    CHECK(s != NULL, "server did not start");
    pa = peer_connect(s->probe, s->port);
    CHECK(pa && peer_write(pa, "\x01\x82\x00\x0d\x00\x02\x00\x09" "alice", 13) == 0, "JOIN alice");
    CHECK(peer_recv(pa, &f, T(2000)) == 1 && GOLDEN(&f, "\x01\x87\x00\x0a\x00\x03\x00\x06\x00\x01"),
          "ACK for alice");

    pb = peer_connect(s->probe, s->port);
    CHECK(pb && peer_write(pb, "\x01\x82\x00\x0b\x00\x02\x00\x07" "bob", 11) == 0, "JOIN bob");
    CHECK(peer_recv(pb, &f, T(2000)) == 1 &&
          GOLDEN(&f, "\x01\x87\x00\x13\x00\x03\x00\x06\x00\x02\x00\x02\x00\x09" "alice"),
          "ACK for bob");
    CHECK(peer_recv(pa, &f, T(2000)) == 1 &&
          GOLDEN(&f, "\x01\x88\x00\x0b\x00\x02\x00\x07" "bob"), "ONLINE bob");

    CHECK(peer_write(pa, "\x01\x84\x00\x0a\x00\x04\x00\x06" "hi", 10) == 0, "SEND hi");
    CHECK(peer_recv(pb, &f, T(2000)) == 1 &&
          GOLDEN(&f, "\x01\x83\x00\x13\x00\x04\x00\x06" "hi" "\x00\x02\x00\x09" "alice"),
          "FWD hi");

    CHECK(peer_write(pb, "\x01\x89\x00\x04", 4) == 0, "IDLE");
    CHECK(peer_recv(pa, &f, T(2000)) == 1 &&
          GOLDEN(&f, "\x01\x89\x00\x0b\x00\x02\x00\x07" "bob"), "IDLE relay");

    pc = peer_connect(s->probe, s->port);
    CHECK(pc && peer_write(pc, "\x01\x82\x00\x0d\x00\x02\x00\x09" "alice", 13) == 0, "dup JOIN");
    CHECK(peer_recv(pc, &f, T(2000)) == 1 &&
          GOLDEN(&f, "\x01\x85\x00\x1f\x00\x01\x00\x1b" "Username already in use"), "NAK");

    peer_free(pb);
    CHECK(peer_recv(pa, &f, T(2000)) == 1 &&
          GOLDEN(&f, "\x01\x86\x00\x0b\x00\x02\x00\x07" "bob"), "OFFLINE bob");
}

static void t_b2_no_spoofing(void)
{
    struct srv  *s = srv_start("127.0.0.1", 3);
    struct peer *pa, *pb;

    CHECK(s != NULL, "server did not start");
    pa = join_as(s, "alice");
    pb = join_as(s, "bob");
    CHECK(pa && pb && expect_msg(pa, TYPE_ONLINE, "bob", NULL, T(2000)), "setup");
    CHECK(psend(pa, TYPE_SEND, ATTR_USERNAME, "mallory", -1,
                ATTR_MESSAGE, "trust me", -1, 0) == 0, "send");
    CHECK(expect_msg(pb, TYPE_FWD, "alice", "trust me", T(2000)),
          "FWD must name the real sender, not the Username inside SEND");
}

static void t_b3_coalesced(void)
{
    uint8_t      buf[256];
    size_t       n;
    struct srv  *s = srv_start("127.0.0.1", 3);
    struct peer *pa, *pb, *pc;

    CHECK(s != NULL, "server did not start");
    pa = join_as(s, "alice");
    pb = join_as(s, "bob");
    CHECK(pa && pb && expect_msg(pa, TYPE_ONLINE, "bob", NULL, T(2000)), "setup");

    n  = mk(buf, sizeof(buf), SBCP_WIRE_NET, TYPE_SEND, ATTR_MESSAGE, "one", -1, 0);
    n += mk(buf + n, sizeof(buf) - n, SBCP_WIRE_NET, TYPE_SEND, ATTR_MESSAGE, "two", -1, 0);
    CHECK(peer_write(pa, buf, n) == 0, "write");
    CHECK(expect_msg(pb, TYPE_FWD, "alice", "one", T(2000)) &&
          expect_msg(pb, TYPE_FWD, "alice", "two", T(2000)), "two messages in one segment");

    // JOIN and a SEND in a single write from a brand-new client.
    pc = peer_connect(s->probe, s->port);
    n  = mk(buf, sizeof(buf), SBCP_WIRE_NET, TYPE_JOIN, ATTR_USERNAME, "carol", -1, 0);
    n += mk(buf + n, sizeof(buf) - n, SBCP_WIRE_NET, TYPE_SEND, ATTR_MESSAGE, "first!", -1, 0);
    CHECK(pc && peer_write(pc, buf, n) == 0, "write");
    CHECK(expect_msg(pb, TYPE_ONLINE, "carol", NULL, T(2000)) &&
          expect_msg(pb, TYPE_FWD, "carol", "first!", T(2000)), "JOIN+SEND in one segment");
    CHECK(expect_msg(pc, TYPE_ACK, NULL, NULL, T(2000)), "carol's ACK");
}

static void t_b4_split_bytes(void)
{
    uint8_t      buf[128];
    size_t       n, i;
    struct srv  *s = srv_start("127.0.0.1", 3);
    struct peer *pa, *pb;

    CHECK(s != NULL, "server did not start");
    pa = join_as(s, "alice");
    pb = join_as(s, "bob");
    CHECK(pa && pb && expect_msg(pa, TYPE_ONLINE, "bob", NULL, T(2000)), "setup");

    n = mk(buf, sizeof(buf), SBCP_WIRE_NET, TYPE_SEND, ATTR_MESSAGE,
           "delivered one byte at a time", -1, 0);
    for (i = 0; i < n; i++) {
        CHECK(peer_write(pa, buf + i, 1) == 0, "write byte %zu", i);
        if (i + 1 < n)
            CHECK(expect_none(pb, 15), "FWD before the message was complete");
    }
    CHECK(expect_msg(pb, TYPE_FWD, "alice", "delivered one byte at a time", T(2000)),
          "byte-by-byte message not reassembled");
}

static void t_b5_nul_tolerance(void)
{
    static struct frame f;
    struct srv         *s = srv_start("127.0.0.1", 4);
    struct peer        *pa, *pd, *pe;
    char                got[64];

    CHECK(s != NULL, "server did not start");
    pa = join_as(s, "alice");
    CHECK(pa != NULL, "setup");
    pd = peer_connect(s->probe, s->port);
    CHECK(pd && join_reply(pd, "dave", 5, &f) == 1 && f.ok && f.m.type == TYPE_ACK,
          "JOIN with a NUL-terminated username rejected");
    CHECK(peer_recv(pa, &f, T(2000)) == 1 && check_frame(&f, TYPE_ONLINE, "dave", NULL) &&
          fr_attr(&f, ATTR_USERNAME, got, sizeof(got)) == 4,
          "ONLINE should carry \"dave\" without the NUL");
    pe = peer_connect(s->probe, s->port);
    CHECK(pe && join_reply(pe, "eve\0\0\0\0\0\0\0\0\0\0\0\0\0", 16, &f) == 1 && f.ok &&
          f.m.type == TYPE_ACK, "NUL-padded fixed-size username rejected");

    CHECK(expect_msg(pa, TYPE_ONLINE, "eve", NULL, T(2000)), "ONLINE eve (NULs stripped)");
    CHECK(psend(pd, TYPE_SEND, ATTR_MESSAGE, "hi", 3, 0) == 0, "send");
    CHECK(peer_recv(pa, &f, T(2000)) == 1 && check_frame(&f, TYPE_FWD, "dave", "hi") &&
          fr_attr(&f, ATTR_MESSAGE, got, sizeof(got)) == 2,
          "NUL-terminated message should be forwarded without the NUL");
}

static void t_b6_message_lengths(void)
{
    static char  big[MAX_MESSAGE_LEN + 2];
    struct srv  *s = srv_start("127.0.0.1", 3);
    struct peer *pa, *pb;

    CHECK(s != NULL, "server did not start");
    pa = join_as(s, "alice");
    pb = join_as(s, "bob");
    CHECK(pa && pb && expect_msg(pa, TYPE_ONLINE, "bob", NULL, T(2000)), "setup");

    CHECK(psend(pa, TYPE_SEND, ATTR_MESSAGE, "x", -1, 0) == 0 &&
          expect_msg(pb, TYPE_FWD, "alice", "x", T(2000)), "1-byte message");

    memset(big, 'a', MAX_MESSAGE_LEN);
    big[MAX_MESSAGE_LEN] = '\0';
    CHECK(psend(pa, TYPE_SEND, ATTR_MESSAGE, big, MAX_MESSAGE_LEN, 0) == 0 &&
          expect_msg(pb, TYPE_FWD, "alice", big, T(2000)), "512-byte message");
    CHECK(psend(pa, TYPE_SEND, ATTR_MESSAGE, big, MAX_MESSAGE_LEN + 1, 0) == 0 &&
          expect_msg(pb, TYPE_FWD, "alice", big, T(2000)),
          "512 bytes + a NUL terminator should be accepted");

    big[MAX_MESSAGE_LEN] = 'a';
    CHECK(psend(pa, TYPE_SEND, ATTR_MESSAGE, big, MAX_MESSAGE_LEN + 1, 0) == 0 &&
          expect_none(pb, 300), "513-byte message forwarded");
    CHECK(psend(pa, TYPE_SEND, ATTR_MESSAGE, "", 0, 0) == 0 && expect_none(pb, 300),
          "empty message forwarded");
    CHECK(psend(pa, TYPE_SEND, 0) == 0 && expect_none(pb, 300), "SEND with no attributes forwarded");
    CHECK(psend(pa, TYPE_SEND, ATTR_USERNAME, "alice", -1, 0) == 0 && expect_none(pb, 300),
          "SEND with only a Username forwarded");

    CHECK(psend(pa, TYPE_SEND, ATTR_MESSAGE, "still framed", -1, 0) == 0 &&
          expect_msg(pb, TYPE_FWD, "alice", "still framed", T(2000)),
          "stream lost sync after the rejected messages");
}

static void t_b7_username_rules(void)
{
    static struct frame f;
    struct srv         *s = srv_start("127.0.0.1", 10);
    struct peer        *p;
    char                reason[64];
    static const struct {
        const char *name;
        int         len;
        int         ok;
        const char *reason;
    } cases[] = {
        { "abcdefghijklmnop",  -1, 1, NULL },
        { "a b",               -1, 1, NULL },
        { "\xc3\xa9l\xc3\xa8ve", -1, 1, NULL },
        { "abcdefghijklmnopq", -1, 0, "Invalid username (1-16 chars)" },
        { "",                   0, 0, "Invalid username (1-16 chars)" },
        { "bad\x01name",       -1, 0, "Invalid username (1-16 chars)" },
        { "esc\x1b[2J",        -1, 0, "Invalid username (1-16 chars)" },
        { "nul\0inside",       10, 0, "Invalid username (1-16 chars)" },
    };
    size_t i;

    CHECK(s != NULL, "server did not start");
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        p = peer_connect(s->probe, s->port);
        CHECK(p && join_reply(p, cases[i].name, cases[i].len, &f) == 1 && f.ok,
              "case %zu: no reply", i);
        if (cases[i].ok) {
            CHECK(f.m.type == TYPE_ACK, "case %zu: valid username refused", i);
        } else {
            CHECK(f.m.type == TYPE_NAK && fr_attr(&f, ATTR_REASON, reason, sizeof(reason)) > 0 &&
                  strcmp(reason, cases[i].reason) == 0, "case %zu: expected NAK \"%s\"", i,
                  cases[i].reason);
            CHECK(expect_eof(p, T(1000)), "case %zu: connection not closed after NAK", i);
        }
    }

    p = peer_connect(s->probe, s->port);
    CHECK(p && psend(p, TYPE_JOIN, 0) == 0 && peer_recv(p, &f, T(2000)) == 1 &&
          f.ok && f.m.type == TYPE_NAK && fr_attr(&f, ATTR_REASON, reason, sizeof(reason)) > 0 &&
          strcmp(reason, "Missing username") == 0, "JOIN without a Username");
    p = peer_connect(s->probe, s->port);
    CHECK(p && psend(p, TYPE_JOIN, ATTR_MESSAGE, "zed", -1, 0) == 0 &&
          peer_recv(p, &f, T(2000)) == 1 && f.ok && f.m.type == TYPE_NAK,
          "JOIN with a Message instead of a Username");
}

static void t_b8_legacy_layout(void)
{
    static struct frame f;
    uint8_t             buf[128];
    size_t              n;
    struct srv         *s = srv_start("127.0.0.1", 4);
    struct peer        *pl, *pn;

    CHECK(s != NULL, "server did not start");
    pl = peer_connect(s->probe, s->port);
    n  = mk(buf, sizeof(buf), SBCP_WIRE_LEGACY, TYPE_JOIN, ATTR_USERNAME, "leg", -1, 0);
    CHECK(pl && peer_write(pl, buf, n) == 0, "legacy JOIN");
    CHECK(peer_recv(pl, &f, T(2000)) == 1 &&
          GOLDEN(&f, "\x03\x0e\x0a\x00\x03\x00\x06\x00\x01\x00"), "legacy ACK");

    pn = join_as(s, "net");
    CHECK(pn != NULL, "network-order client");
    CHECK(peer_recv(pl, &f, T(2000)) == 1 &&
          GOLDEN(&f, "\x03\x10\x0b\x00\x02\x00\x07\x00" "net"), "legacy ONLINE");

    n = mk(buf, sizeof(buf), SBCP_WIRE_LEGACY, TYPE_SEND, ATTR_MESSAGE, "from leg", -1, 0);
    CHECK(peer_write(pl, buf, n) == 0 && expect_msg(pn, TYPE_FWD, "leg", "from leg", T(2000)),
          "legacy -> network relay");
    CHECK(psend(pn, TYPE_SEND, ATTR_MESSAGE, "from net", -1, 0) == 0 &&
          peer_recv(pl, &f, T(2000)) == 1 && f.ok && f.m.wire == SBCP_WIRE_LEGACY &&
          check_frame(&f, TYPE_FWD, "net", "from net"), "network -> legacy relay");

}

static void t_b10_order(void)
{
    static uint8_t buf[300 * 32];
    size_t         n = 0;
    struct srv    *s = srv_start("127.0.0.1", 3);
    struct peer   *pa, *pb;
    int            i;

    CHECK(s != NULL, "server did not start");
    pa = join_as(s, "alice");
    pb = join_as(s, "bob");
    CHECK(pa && pb && expect_msg(pa, TYPE_ONLINE, "bob", NULL, T(2000)), "setup");
    for (i = 0; i < 300; i++) {
        char m[16];

        snprintf(m, sizeof(m), "m%03d", i);
        n += mk(buf + n, sizeof(buf) - n, SBCP_WIRE_NET, TYPE_SEND, ATTR_MESSAGE, m, -1, 0);
    }
    CHECK(peer_write(pa, buf, n) == 0, "burst");
    for (i = 0; i < 300; i++) {
        char m[16];

        snprintf(m, sizeof(m), "m%03d", i);
        CHECK(expect_msg(pb, TYPE_FWD, "alice", m, T(2000)), "message %d out of order", i);
    }
}

// The ACK lists members oldest first, not in descriptor order.
static void t_b11_ack_join_order(void)
{
    static struct frame f;
    struct srv         *s = srv_start("127.0.0.1", 5);
    struct peer        *u, *m1, *m2, *m3;
    char                names[128];

    CHECK(s != NULL, "server did not start");
    u  = peer_connect(s->probe, s->port);   // holds the lowest descriptor for now
    m1 = join_as(s, "first");
    CHECK(u && m1, "setup");
    peer_free(u);                            // frees it: "second" will get it
    msleep(100);
    m2 = join_as(s, "second");
    CHECK(m2 && expect_msg(m1, TYPE_ONLINE, "second", NULL, T(2000)), "second joined");
    m3 = peer_connect(s->probe, s->port);
    CHECK(m3 && join_reply(m3, "third", -1, &f) == 1 && f.ok && f.m.type == TYPE_ACK, "third");
    fr_names(&f, names, sizeof(names));
    CHECK(strcmp(names, "first,second") == 0, "ACK list \"%s\", expected join order", names);
}

// ============================================================================
// C: malformed input
// ============================================================================

static struct srv *two_joined(struct peer **pa, struct peer **pb)
{
    struct srv *s = srv_start("127.0.0.1", 5);

    if (s == NULL)
        return NULL;
    *pa = join_as(s, "alice");
    *pb = join_as(s, "bob");
    if (*pa == NULL || *pb == NULL || !expect_msg(*pa, TYPE_ONLINE, "bob", NULL, T(2000)))
        return NULL;
    return s;
}

static void t_c1_garbage(void)
{
    static struct frame f;
    struct srv         *s = srv_start("127.0.0.1", 3);
    struct peer        *p;

    CHECK(s != NULL, "server did not start");
    p = peer_connect(s->probe, s->port);
    CHECK(p && peer_write(p, "hello\r\n", 7) == 0, "telnet-style text");
    msleep(150);
    CHECK(peer_write(p, "GET / HTTP/1.1\r\nHost: x\r\n\r\n", 27) == 0, "HTTP");
    msleep(150);
    CHECK(peer_write(p, "\xff\xff\xff\xff\x00\x00", 6) == 0, "binary junk");
    msleep(150);
    CHECK(expect_none(p, 100), "server answered garbage");
    CHECK(join_reply(p, "survivor", -1, &f) == 1 && f.ok && f.m.type == TYPE_ACK,
          "connection unusable after garbage");
    CHECK(log_has(s, "not an SBCP version 3 header"), "garbage not logged");
}

static void t_c2_unknown_type(void)
{
    uint8_t      buf[256];
    size_t       n;
    struct peer *pa, *pb;
    struct srv  *s = two_joined(&pa, &pb);

    CHECK(s != NULL, "setup");
    n  = mk(buf, sizeof(buf), SBCP_WIRE_NET, 13, ATTR_MESSAGE, "secret", -1, 0);
    n += mk(buf + n, sizeof(buf) - n, SBCP_WIRE_NET, 0, 0);
    n += mk(buf + n, sizeof(buf) - n, SBCP_WIRE_NET, 127, ATTR_USERNAME, "x", -1, 0);
    n += mk(buf + n, sizeof(buf) - n, SBCP_WIRE_NET, TYPE_SEND, ATTR_MESSAGE, "after", -1, 0);
    CHECK(peer_write(pa, buf, n) == 0, "write");
    CHECK(expect_msg(pb, TYPE_FWD, "alice", "after", T(2000)),
          "message after unknown types lost (framing must survive)");
    CHECK(expect_none(pb, 200) && expect_none(pa, 1), "unknown types produced output");
}

static void t_c3_server_only_types(void)
{
    uint8_t      buf[512];
    size_t       n;
    struct peer *pa, *pb;
    struct srv  *s = two_joined(&pa, &pb);

    CHECK(s != NULL, "setup");
    n  = mk(buf, sizeof(buf), SBCP_WIRE_NET, TYPE_FWD, ATTR_MESSAGE, "fake", -1,
            ATTR_USERNAME, "bob", -1, 0);
    n += mk(buf + n, sizeof(buf) - n, SBCP_WIRE_NET, TYPE_ACK, 0);
    n += mk(buf + n, sizeof(buf) - n, SBCP_WIRE_NET, TYPE_NAK, ATTR_REASON, "no", -1, 0);
    n += mk(buf + n, sizeof(buf) - n, SBCP_WIRE_NET, TYPE_ONLINE, ATTR_USERNAME, "ghost", -1, 0);
    n += mk(buf + n, sizeof(buf) - n, SBCP_WIRE_NET, TYPE_OFFLINE, ATTR_USERNAME, "bob", -1, 0);
    CHECK(peer_write(pa, buf, n) == 0, "write");
    CHECK(expect_none(pb, 300), "a server-only message from a client was acted on");
    CHECK(psend(pa, TYPE_SEND, ATTR_MESSAGE, "still here", -1, 0) == 0 &&
          expect_msg(pb, TYPE_FWD, "alice", "still here", T(2000)), "client dropped");
}

static void t_c4_bad_lengths(void)
{
    struct peer *pa, *pb;
    struct srv  *s = two_joined(&pa, &pb);
    uint8_t      big[1100];

    CHECK(s != NULL, "setup");

    // Length below the header size.
    CHECK(peer_write(pa, "\x01\x84\x00\x02", 4) == 0, "len 2");
    msleep(150);
    CHECK(peer_write(pa, "\x01\x84\x00\x00", 4) == 0, "len 0");
    msleep(150);
    CHECK(psend(pa, TYPE_SEND, ATTR_MESSAGE, "after short", -1, 0) == 0 &&
          expect_msg(pb, TYPE_FWD, "alice", "after short", T(2000)), "after Length < 4");

    // Length smaller than the bytes sent: attribute overruns the message.
    CHECK(peer_write(pa, "\x01\x84\x00\x08\x00\x04\x00\x0e" "abcdefghij", 18) == 0, "len short");
    msleep(150);
    CHECK(expect_none(pb, 100), "truncated message forwarded");
    CHECK(psend(pa, TYPE_SEND, ATTR_MESSAGE, "after overrun", -1, 0) == 0 &&
          expect_msg(pb, TYPE_FWD, "alice", "after overrun", T(2000)), "after Length < bytes");

    // Attribute-level defects with correct message framing: one write.
    memset(big, 0, sizeof(big));
    memcpy(big, "\x01\x84\x00\x08\x00\x04\x00\x02"                    // attr Length 2
                "\x01\x84\x00\x0a\x00\x04\x00\x10" "hi"               // attr overruns
                "\x01\x84\x00\x0c\x00\x04\x00\x05" "x" "\x07\x07\x07" // stray bytes
                "\x01\x84\x00\x0c\x00\x04\x00\x07" "pad" "\x00"       // zero pad: OK
                "\x01\x84\x00\x17\x00\x04\x00\x09" "first" "\x00\x04\x00\x0a" "second"
                "\x01\x84\x00\x10\x00\x63\x00\x05" "?" "\x00\x04\x00\x07" "unk",
           8 + 10 + 12 + 12 + 23 + 16);
    CHECK(peer_write(pa, big, 8 + 10 + 12 + 12 + 23 + 16) == 0, "write");
    CHECK(expect_msg(pb, TYPE_FWD, "alice", "pad", T(2000)), "zero padding should be tolerated");
    CHECK(expect_msg(pb, TYPE_FWD, "alice", "first", T(2000)), "duplicate attribute: use the first");
    CHECK(expect_msg(pb, TYPE_FWD, "alice", "unk", T(2000)), "unknown attribute should be skipped");
    CHECK(expect_none(pb, 200), "a defective message was forwarded");
}

static void t_c5_incomplete_timeout(void)
{
    struct srv  *s = srv_start("127.0.0.1", 5);
    struct peer *pa, *pb, *pc;

    CHECK(s != NULL, "server did not start");
    pa = join_as(s, "alice");
    pb = join_as(s, "bob");
    pc = join_as(s, "carol");
    CHECK(pa && pb && pc && wait_for(pb, TYPE_ONLINE, "carol", NULL, T(2000)), "setup");

    // Promises 100 bytes, delivers 12, then stalls.
    CHECK(peer_write(pa, "\x01\x84\x00\x64\x00\x04\x00\x60" "abcd", 12) == 0, "partial");
    CHECK(psend(pc, TYPE_SEND, ATTR_MESSAGE, "meanwhile", -1, 0) == 0 &&
          expect_msg(pb, TYPE_FWD, "carol", "meanwhile", T(1000)),
          "server stalled on the incomplete message");
    msleep(3600);
    CHECK(log_has(s, "discarded an incomplete message"), "timeout not logged");
    CHECK(psend(pa, TYPE_SEND, ATTR_MESSAGE, "fresh start", -1, 0) == 0 &&
          expect_msg(pb, TYPE_FWD, "alice", "fresh start", T(2000)),
          "stream not resynchronised after the timeout");
}

static void t_c6_before_join(void)
{
    static struct frame f;
    struct peer        *pa, *pb, *pu;
    struct srv         *s = two_joined(&pa, &pb);
    unsigned long       count;

    CHECK(s != NULL, "setup");
    pu = peer_connect(s->probe, s->port);
    CHECK(pu && psend(pu, TYPE_SEND, ATTR_MESSAGE, "sneaky", -1, 0) == 0 &&
          psend(pu, TYPE_IDLE, 0) == 0, "write");
    CHECK(expect_none(pb, 300) && expect_none(pu, 1), "SEND/IDLE before JOIN was acted on");
    CHECK(join_reply(pu, "late", -1, &f) == 1 && f.ok && f.m.type == TYPE_ACK &&
          fr_count(&f, &count) && count == 3, "JOIN after the discarded messages");
    CHECK(expect_msg(pb, TYPE_ONLINE, "late", NULL, T(2000)), "ONLINE late");
}

static void t_c7_second_join(void)
{
    struct peer *pa, *pb;
    struct srv  *s = two_joined(&pa, &pb);

    CHECK(s != NULL, "setup");
    CHECK(psend(pa, TYPE_JOIN, ATTR_USERNAME, "renamed", -1, 0) == 0 &&
          psend(pa, TYPE_JOIN, ATTR_USERNAME, "bob", -1, 0) == 0, "write");
    CHECK(expect_none(pa, 300) && expect_none(pb, 1), "second JOIN answered or announced");
    CHECK(psend(pa, TYPE_SEND, ATTR_MESSAGE, "who am i", -1, 0) == 0 &&
          expect_msg(pb, TYPE_FWD, "alice", "who am i", T(2000)), "identity changed");
}

static void t_c8_oversized(void)
{
    static uint8_t big[60000 + 64];
    size_t         n;
    struct peer   *pa, *pb;
    struct srv    *s = two_joined(&pa, &pb);

    CHECK(s != NULL, "setup");
    memset(big, 'z', sizeof(big));
    big[0] = 0x01; big[1] = 0x84; big[2] = 0xEA; big[3] = 0x60;     // Length 60000
    n = 60000 + mk(big + 60000, 64, SBCP_WIRE_NET, TYPE_SEND, ATTR_MESSAGE,
                   "after big", -1, 0);
    CHECK(peer_write(pa, big, n) == 0, "write");
    CHECK(expect_msg(pb, TYPE_FWD, "alice", "after big", T(3000)),
          "message after an oversized one was lost");
    CHECK(log_has(s, "longer than this side accepts"), "not logged");
}

static void t_c9_fuzz(void)
{
    static uint8_t buf[2048];
    struct srv    *s = srv_start("127.0.0.1", 30);
    struct peer   *wa, *wb, *p;
    int            conn, i;

    CHECK(s != NULL, "server did not start");
    wa = join_as(s, "watch-a");
    wb = join_as(s, "watch-b");
    CHECK(wa && wb, "watchers");

    for (conn = 0; conn < 12; conn++) {
        p = peer_connect(s->probe, s->port);
        CHECK(p != NULL, "fuzz connection %d refused: server %s", conn,
              srv_alive(s) ? "alive" : "DEAD");
        if (conn % 2 == 0) {
            char name[16];

            snprintf(name, sizeof(name), "fz%d", conn);
            psend(p, TYPE_JOIN, ATTR_USERNAME, name, -1, 0);
        }
        for (i = 0; i < 300; i++) {
            size_t n = 0, k;

            switch (rnd_below(5)) {
            case 0:                     // raw random bytes
                n = 1 + rnd_below(64);
                for (k = 0; k < n; k++)
                    buf[k] = (uint8_t)rnd();
                break;
            case 1:                     // valid version, random type and Length
                n = 4 + rnd_below(80);
                for (k = 0; k < n; k++)
                    buf[k] = (uint8_t)rnd();
                buf[0] = 0x01;
                buf[1] = (uint8_t)(0x80 | rnd_below(128));
                buf[2] = (uint8_t)rnd_below(3);
                buf[3] = (uint8_t)rnd();
                break;
            case 2: {                   // well-framed, random attributes
                struct sbcp_builder b;
                int                 na = (int)rnd_below(4), j;

                sbcp_begin(&b, buf, sizeof(buf), rnd_below(4) ? SBCP_WIRE_NET : SBCP_WIRE_LEGACY,
                           2 + rnd_below(8));
                for (j = 0; j < na; j++) {
                    uint8_t v[40];
                    size_t  vl = rnd_below(40);

                    for (k = 0; k < vl; k++)
                        v[k] = (uint8_t)(rnd_below(2) ? 'a' + rnd_below(26) : rnd());
                    sbcp_add_attr(&b, rnd_below(7), v, vl);
                }
                n = sbcp_finish(&b);
                if (n > 4 && rnd_below(3) == 0)
                    buf[3] = (uint8_t)(buf[3] + (int)rnd_below(7) - 3);   // corrupt Length
                break;
            }
            case 3:                     // a valid SEND
                n = mk(buf, sizeof(buf), SBCP_WIRE_NET, TYPE_SEND, ATTR_MESSAGE,
                       "fuzz", -1, 0);
                break;
            default:                    // truncated valid message
                n = mk(buf, sizeof(buf), SBCP_WIRE_NET, TYPE_JOIN, ATTR_USERNAME,
                       "partial", -1, 0);
                n = 1 + rnd_below((uint32_t)n);
                break;
            }
            if (n > 0 && peer_write(p, buf, n) < 0)
                break;                  // server closed it (e.g. after a NAK)
        }
        switch (conn % 3) {
        case 0:  peer_rst(p);  break;
        case 1:  peer_free(p); break;
        default: shutdown(p->fd, SHUT_WR); peer_free(p); break;
        }
        drain_count(wa, 0xFFFF, 1 << 30, 30, NULL);
        drain_count(wb, 0xFFFF, 1 << 30, 30, NULL);
    }

    msleep(200);
    CHECK(srv_alive(s), "server died during fuzzing");
    drain_count(wa, 0xFFFF, 1 << 30, 300, NULL);
    drain_count(wb, 0xFFFF, 1 << 30, 300, NULL);
    CHECK(psend(wa, TYPE_SEND, ATTR_MESSAGE, "alive?", -1, 0) == 0 &&
          wait_for(wb, TYPE_FWD, "watch-a", "alive?", T(3000)), "chat broken after fuzzing");
    p = join_as(s, "after-fuzz");
    CHECK(p != NULL, "cannot join after fuzzing");
}

// ============================================================================
// D: disconnects and resource exhaustion
// ============================================================================

static void t_d1_rst_before_join(void)
{
    struct srv  *s = srv_start("127.0.0.1", 3);
    struct peer *p;
    int          i;

    CHECK(s != NULL, "server did not start");
    for (i = 0; i < 40; i++) {
        p = peer_connect(s->probe, s->port);
        CHECK(p != NULL, "connect %d: server %s", i, srv_alive(s) ? "alive" : "DEAD");
        if (i % 2)
            peer_write(p, "\x01\x82\x00\x0d\x00\x02", 6);    // half a JOIN
        peer_rst(p);
    }
    msleep(200);
    CHECK(srv_alive(s), "server died after RSTs");
    CHECK(join_as(s, "after") != NULL, "cannot join after RSTs");
}

static void t_d2_rst_after_join(void)
{
    static struct frame f;
    struct peer        *pa, *pb, *pa2;
    struct srv         *s = two_joined(&pa, &pb);
    unsigned long       count;

    CHECK(s != NULL, "setup");
    peer_rst(pa);
    CHECK(expect_msg(pb, TYPE_OFFLINE, "alice", NULL, T(2000)), "no OFFLINE after RST");
    pa2 = peer_connect(s->probe, s->port);
    CHECK(pa2 && join_reply(pa2, "alice", -1, &f) == 1 && f.ok && f.m.type == TYPE_ACK &&
          fr_count(&f, &count) && count == 2, "name/slot not freed after RST");
    CHECK(log_has(s, "Connection reset by peer"), "RST not logged");
}

static void t_d3_rst_with_output_pending(void)
{
    static char  payload[401];
    static uint8_t batch[100 * 420];
    struct srv  *s = srv_start("127.0.0.1", 5);
    struct peer *slow, *obs, *fast;
    size_t       n;
    int          b, i, got;

    CHECK(s != NULL, "server did not start");
    slow = join_as_buf(s, "slow", 4096); // tiny window, never reads again
    obs  = join_as(s, "obs");
    fast = join_as(s, "fast");
    CHECK(slow && obs && fast && expect_msg(obs, TYPE_ONLINE, "fast", NULL, T(2000)), "setup");

    memset(payload, 'p', 400);
    for (n = 0, i = 0; i < 100; i++)
        n += mk(batch + n, sizeof(batch) - n, SBCP_WIRE_NET, TYPE_SEND,
                ATTR_MESSAGE, payload, 400, 0);
    for (b = 0; b < 20; b++) {             // ~0.8 MB: backed up, under the 1 MiB cut-off
        CHECK(peer_write(fast, batch, n) == 0, "flood batch %d", b);
        got = drain_count(obs, TYPE_FWD, 100, T(3000), NULL);
        CHECK(got == 100, "observer got %d of 100 in batch %d", got, b);
    }
    peer_rst(slow);                     // dies with the server's writes backed up
    CHECK(wait_for(obs, TYPE_OFFLINE, "slow", NULL, T(3000)), "no OFFLINE for the reset client");
    CHECK(srv_alive(s), "server died writing to a reset connection (SIGPIPE?)");
    CHECK(join_as(s, "slow") != NULL, "name not freed");
}

static void t_d4_join_then_rst(void)
{
    static struct frame f;
    struct srv         *s = srv_start("127.0.0.1", 60);
    struct peer        *obs, *p;
    unsigned long       count;
    int                 i, on = 0, off = 0;

    CHECK(s != NULL, "server did not start");
    obs = join_as(s, "obs");
    CHECK(obs != NULL, "setup");
    for (i = 0; i < 50; i++) {
        char name[16];

        snprintf(name, sizeof(name), "r%d", i);
        p = peer_connect(s->probe, s->port);
        CHECK(p && psend(p, TYPE_JOIN, ATTR_USERNAME, name, -1, 0) == 0, "join %d", i);
        peer_rst(p);                    // gone before the ACK can be read
    }
    for (;;) {
        int r = peer_recv(obs, &f, 500);

        if (r != 1)
            break;
        if (f.ok && f.m.type == TYPE_ONLINE)
            on++;
        if (f.ok && f.m.type == TYPE_OFFLINE)
            off++;
    }
    CHECK(on == off, "%d ONLINE but %d OFFLINE", on, off);
    p = peer_connect(s->probe, s->port);
    CHECK(p && join_reply(p, "final", -1, &f) == 1 && f.ok && f.m.type == TYPE_ACK &&
          fr_count(&f, &count) && count == 2, "room count wrong after JOIN+RST storm (%lu)", count);
}

static void t_d6_half_close(void)
{
    struct peer *pa, *pb;
    struct srv  *s = two_joined(&pa, &pb);

    CHECK(s != NULL, "setup");
    shutdown(pa->fd, SHUT_WR);
    CHECK(expect_msg(pb, TYPE_OFFLINE, "alice", NULL, T(2000)), "half-close not treated as leaving");
    CHECK(expect_eof(pa, T(2000)), "server did not close its side");
}

static void t_d7_mass_rst(void)
{
    static struct frame f;
    static uint8_t      burst[200 * 24];
    struct srv         *s = srv_start("127.0.0.1", 60);
    struct peer        *talk, *obs, *m[50], *p;
    unsigned long       count;
    size_t              n = 0;
    int                 i, fwd = 0, off = 0;

    CHECK(s != NULL, "server did not start");
    talk = join_as(s, "talker");
    obs  = join_as(s, "obs");
    CHECK(talk && obs, "setup");
    for (i = 0; i < 50; i++) {
        char name[16];

        snprintf(name, sizeof(name), "m%d", i);
        CHECK((m[i] = join_as(s, name)) != NULL, "member %d", i);
    }
    CHECK(drain_count(obs, TYPE_ONLINE, 50, T(3000), NULL) == 50, "ONLINEs");

    for (i = 0; i < 200; i++) {
        char msg[16];

        snprintf(msg, sizeof(msg), "b%d", i);
        n += mk(burst + n, sizeof(burst) - n, SBCP_WIRE_NET, TYPE_SEND, ATTR_MESSAGE, msg, -1, 0);
    }
    CHECK(peer_write(talk, burst, n) == 0, "burst");
    for (i = 0; i < 50; i++)
        peer_rst(m[i]);

    while (fwd < 200 || off < 50) {
        int r = peer_recv(obs, &f, T(3000));

        if (r != 1)
            break;
        if (f.ok && f.m.type == TYPE_FWD)
            fwd++;
        if (f.ok && f.m.type == TYPE_OFFLINE)
            off++;
    }
    CHECK(fwd == 200 && off == 50, "observer saw %d/200 FWD and %d/50 OFFLINE", fwd, off);
    p = peer_connect(s->probe, s->port);
    CHECK(p && join_reply(p, "late", -1, &f) == 1 && f.ok && f.m.type == TYPE_ACK &&
          fr_count(&f, &count) && count == 3, "room count after mass RST is %lu", count);
}

static void t_d8_slow_reader(void)
{
    static char    payload[501];
    static uint8_t batch[200 * 520];
    struct srv    *s = srv_start("127.0.0.1", 5);
    struct peer   *slow, *obs, *fast;
    size_t         n;
    int            b, i, got, others, total = 0;
    int64_t        t0 = now_ms();

    CHECK(s != NULL, "server did not start");
    slow = join_as(s, "slow");
    obs  = join_as(s, "obs");
    fast = join_as(s, "fast");
    CHECK(slow && obs && fast && expect_msg(obs, TYPE_ONLINE, "fast", NULL, T(2000)), "setup");
    drained_offline[0] = '\0';

    memset(payload, 's', 500);
    for (n = 0, i = 0; i < 200; i++)
        n += mk(batch + n, sizeof(batch) - n, SBCP_WIRE_NET, TYPE_SEND,
                ATTR_MESSAGE, payload, 500, 0);

    // ~20 MB through the server while one client never reads a byte.
    others = 0;
    for (b = 0; b < 200; b++) {
        CHECK(peer_write(fast, batch, n) == 0, "flood batch %d", b);
        got = drain_count(obs, TYPE_FWD, 200, T(5000), &others);
        total += got;
        CHECK(got == 200, "observer starved: %d of 200 in batch %d", got, b);
    }
    CHECK(total == 40000, "observer received %d of 40000", total);
    CHECK(strcmp(drained_offline, "slow") == 0 || wait_for(obs, TYPE_OFFLINE, "slow", NULL, T(2000)),
          "the non-reading client was never dropped");
    CHECK(log_has(s, "not reading"), "backlog drop not logged");
    CHECK(now_ms() - t0 < 60000, "flood took too long");
    (void)slow;
}

static void t_d9_emfile(void)
{
    struct srv  *s;
    struct peer *p[40];
    int          i;

    if (wrap != NULL)
        SKIP("descriptor limits are not meaningful under a wrapper");
    s = srv_start_on("127.0.0.1", 0, 50, 16, OUT_LOG);
    CHECK(s != NULL, "server did not start with RLIMIT_NOFILE 16");
    for (i = 0; i < 40; i++)
        p[i] = peer_connect(s->probe, s->port);
    msleep(400);
    CHECK(srv_alive(s), "server died when it ran out of descriptors");
    CHECK(log_has(s, "out of file descriptors"), "EMFILE path not exercised");
    for (i = 0; i < 40; i++)
        if (p[i] != NULL)
            peer_free(p[i]);
    msleep(400);
    CHECK(join_as(s, "after") != NULL, "server unusable after EMFILE");
}

static void t_d10_fd_setsize(void)
{
    struct rlimit rl;
    struct srv   *s;
    int           i, opened = 0;

    if (wrap != NULL)
        SKIP("descriptor limits are not meaningful under a wrapper");
    if (getrlimit(RLIMIT_NOFILE, &rl) < 0)
        SKIP("getrlimit failed");
    if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < 2600)
        SKIP("hard descriptor limit %lu is below 2600", (unsigned long)rl.rlim_max);
    rl.rlim_cur = 2600;
    if (setrlimit(RLIMIT_NOFILE, &rl) < 0)
        SKIP("cannot raise the descriptor limit");

    s = srv_start_on("127.0.0.1", 0, 10, 2048, OUT_LOG);
    CHECK(s != NULL, "server did not start");
    for (i = 0; i < 1150; i++) {
        if (peer_connect(s->probe, s->port) == NULL)
            break;
        opened++;
        if (i % 100 == 99)
            msleep(50);
    }
    msleep(500);
    CHECK(srv_alive(s), "server died past FD_SETSIZE");
    CHECK(opened > 1030, "only %d connections opened", opened);
    CHECK(log_has(s, "beyond FD_SETSIZE"), "FD_SETSIZE guard not exercised");
    for (i = 0; i < MAX_PEER; i++)
        peer_free(&peers[i]);
    msleep(800);
    CHECK(join_as(s, "after") != NULL, "server unusable after the flood");
}

static void t_d11_stdout_trouble(void)
{
    static uint8_t batch[100 * 120];
    struct srv    *sc, *su;
    struct peer   *pa, *pb;
    size_t         n = 0;
    int            i, b;

    sc = srv_start_on("127.0.0.1", 0, 3, 0, OUT_PIPE_CLOSED);
    CHECK(sc != NULL, "server with a closed stdout pipe did not start");
    pa = join_as(sc, "alice");
    pb = join_as(sc, "bob");
    CHECK(pa && pb && expect_msg(pa, TYPE_ONLINE, "bob", NULL, T(2000)), "closed-stdout chat");
    CHECK(psend(pa, TYPE_SEND, ATTR_MESSAGE, "logs go nowhere", -1, 0) == 0 &&
          expect_msg(pb, TYPE_FWD, "alice", "logs go nowhere", T(2000)), "relay");
    CHECK(srv_alive(sc), "SIGPIPE from logging killed the server");

    // stdout is a pipe nobody reads: the log must not block the chat.
    su = srv_start_on("127.0.0.1", 0, 3, 0, OUT_PIPE_UNREAD);
    CHECK(su != NULL, "server with an unread stdout pipe did not start");
    pa = join_as(su, "alice");
    pb = join_as(su, "bob");
    CHECK(pa && pb && expect_msg(pa, TYPE_ONLINE, "bob", NULL, T(2000)), "unread-stdout chat");
    for (i = 0; i < 100; i++)
        n += mk(batch + n, sizeof(batch) - n, SBCP_WIRE_NET, TYPE_SEND, ATTR_MESSAGE,
                "this line is logged by the server and fills its stdout pipe", -1, 0);
    for (b = 0; b < 30; b++) {
        CHECK(peer_write(pa, batch, n) == 0, "batch %d", b);
        CHECK(drain_count(pb, TYPE_FWD, 100, T(3000), NULL) == 100,
              "chat stalled at batch %d: server blocked writing its log", b);
    }
}

static void t_d13_rst_mid_message(void)
{
    struct peer *pa, *pb;
    struct srv  *s = two_joined(&pa, &pb);

    CHECK(s != NULL, "setup");
    CHECK(peer_write(pa, "\x01\x84\x00\x20\x00\x04\x00\x1c" "hal", 11) == 0, "half message");
    msleep(50);
    peer_rst(pa);
    CHECK(expect_msg(pb, TYPE_OFFLINE, "alice", NULL, T(2000)), "no OFFLINE after mid-message RST");
    CHECK(expect_none(pb, 200), "the half message was forwarded");
}

static void t_d14_churn_no_leak(void)
{
    struct srv  *s = srv_start("127.0.0.1", 10);
    struct peer *obs, *p;
    int          before, after, i;

    CHECK(s != NULL, "server did not start");
    obs = join_as(s, "obs");
    CHECK(obs != NULL, "setup");
    msleep(100);
    before = server_fd_count(s);
    if (before < 0)
        SKIP("/proc not available");
    for (i = 0; i < 300; i++) {
        char name[16];

        snprintf(name, sizeof(name), "c%d", i);
        p = peer_connect(s->probe, s->port);
        CHECK(p != NULL, "cycle %d", i);
        psend(p, TYPE_JOIN, ATTR_USERNAME, name, -1, 0);
        if (i % 3 == 1)
            psend(p, TYPE_SEND, ATTR_MESSAGE, "bye", -1, 0);
        if (i % 3 == 2)
            peer_rst(p);
        else
            peer_free(p);
        if (i % 25 == 24)
            drain_count(obs, 0xFFFF, 1 << 30, 20, NULL);
    }
    msleep(500);
    drain_count(obs, 0xFFFF, 1 << 30, 200, NULL);
    after = server_fd_count(s);
    CHECK(after == before, "server holds %d descriptors, %d before the churn", after, before);
}

static void t_d15_restart_same_port(void)
{
    struct srv  *s1, *s2;
    struct peer *pa, *pb;
    int          port, st;

    s1 = srv_start("127.0.0.1", 3);
    CHECK(s1 != NULL, "server did not start");
    port = s1->port;
    pa = join_as(s1, "alice");
    pb = join_as(s1, "bob");
    CHECK(pa && pb && expect_msg(pa, TYPE_ONLINE, "bob", NULL, T(2000)), "setup");
    st = srv_stop(s1);
    CHECK(st != -1 && WIFEXITED(st) && WEXITSTATUS(st) == 0, "SIGTERM did not stop it cleanly");
    CHECK(expect_eof(pa, T(2000)) && expect_eof(pb, T(2000)), "clients not disconnected");
    s2 = srv_start_on("127.0.0.1", port, 3, 0, OUT_LOG);
    CHECK(s2 != NULL, "restart on the same port failed (SO_REUSEADDR?)");
    CHECK(join_as(s2, "alice") != NULL, "restarted server unusable");
}

// ============================================================================
// F: server command line
// ============================================================================

static void t_f1_server_args(void)
{
    static char out[CLI_BUF];
    char        busy[16];
    int         lfd, bport = 0, rc;
    char *const cases[][5] = {
        { (char *)server_path, NULL },
        { (char *)server_path, "127.0.0.1", "9000", NULL },
        { (char *)server_path, "127.0.0.1", "abc", "3", NULL },
        { (char *)server_path, "127.0.0.1", "0", "3", NULL },
        { (char *)server_path, "127.0.0.1", "70000", "3", NULL },
        { (char *)server_path, "127.0.0.1", "9000", "0", NULL },
        { (char *)server_path, "127.0.0.1", "9000", "-1", NULL },
        { (char *)server_path, "127.0.0.1", "9000", "x", NULL },
        { (char *)server_path, "127.0.0.1", "9000", "1001", NULL },
        { (char *)server_path, "999.1.1.1", "9000", "3", NULL },
        { (char *)server_path, "192.0.2.1", "9000", "3", NULL },
    };
    char *bz[] = { (char *)server_path, "127.0.0.1", busy, "3", NULL };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        rc = run_status(cases[i], T(3000), out, sizeof(out));
        CHECK(rc == 1, "case %zu should exit 1, got %d: \"%.100s\"", i, rc, out);
    }
    lfd = fake_listen(&bport);          // port already in use
    snprintf(busy, sizeof(busy), "%d", bport);
    rc = run_status(bz, T(3000), out, sizeof(out));
    fake_close(lfd);
    CHECK(rc == 1 && strstr(out, "in use"), "port in use: rc %d \"%.100s\"", rc, out);
}

// ============================================================================
// I: the client, end to end
// ============================================================================

// The real client against the real server, with raw peers as the other
// members, or against a scripted fake server when a check needs exact bytes
// or malformed input. Expected text is the client's own display format.

static const char HAN[]   = "\xe4\xb8\xad";                     // U+4E2D, 3 bytes
static const char HELLO[] = "\xe4\xbd\xa0\xe5\xa5\xbd, \xe4\xb8\x96\xe7\x95\x8c \xf0\x9f\x99\x82";

// Starts the client as name and waits for its ACK line.
static struct cli *client_joined(const struct srv *s, const char *name)
{
    struct cli *c = cli_start(name, s->probe, s->port, NULL);

    if (c == NULL || !cli_expect(c, "Client count", T(3000)))
        return NULL;
    return c;
}

struct fwds {
    int    n;
    size_t total, longest;
    size_t len[64];
    char   text[64][MAX_MESSAGE_LEN + 1];
};

// Every FWD from user that reaches p within ms, in order.
static void collect_fwd(struct peer *p, const char *user, int ms, struct fwds *out)
{
    static struct frame f;
    int64_t             deadline = now_ms() + ms;
    char                who[MAX_USERNAME_LEN + 1];

    memset(out, 0, sizeof(*out));
    for (;;) {
        int64_t left = deadline - now_ms();
        int     r, n;

        if (left <= 0 || (r = peer_recv(p, &f, (int)left)) < 0)
            return;
        if (r == 0 || !f.ok || f.m.type != TYPE_FWD)
            continue;
        if (fr_attr(&f, ATTR_USERNAME, who, sizeof(who)) < 0 || strcmp(who, user) != 0)
            continue;
        if (out->n == 64 || (n = fr_attr(&f, ATTR_MESSAGE, out->text[out->n], MAX_MESSAGE_LEN + 1)) < 0)
            continue;
        out->len[out->n++] = (size_t)n;
        out->total += (size_t)n;
        if ((size_t)n > out->longest)
            out->longest = (size_t)n;
    }
}

static struct frame join_frame;          // the JOIN fake_join() received

// A scripted server: starts the client as name, accepts it, reads its JOIN.
static struct peer *fake_join(struct cli **c, const char *name)
{
    struct peer        *conn;
    int                 lfd, port;

    if ((lfd = fake_listen(&port)) < 0)
        return NULL;
    if ((*c = cli_start(name, "127.0.0.1", port, NULL)) == NULL)
        return NULL;
    if ((conn = fake_accept(lfd, T(3000))) == NULL)
        return NULL;
    if (peer_recv(conn, &join_frame, T(3000)) != 1 || !check_frame(&join_frame, TYPE_JOIN, name, NULL))
        return NULL;
    return conn;
}

static int send_fwd(struct peer *conn, const char *text, const char *user)
{
    return psend(conn, TYPE_FWD, ATTR_MESSAGE, text, -1, ATTR_USERNAME, user, -1, 0);
}

static const char *signal_note(const struct cli *c)
{
    static char buf[64];

    if (WIFSIGNALED(c->status))
        snprintf(buf, sizeof(buf), "killed by signal %d%s", WTERMSIG(c->status),
                 WTERMSIG(c->status) == SIGPIPE ? " (SIGPIPE)" : "");
    else
        snprintf(buf, sizeof(buf), "exit status %d", WEXITSTATUS(c->status));
    return buf;
}

static void t_i1_ack_first(void)
{
    struct srv *s = srv_start("127.0.0.1", 5);
    struct cli *c;

    CHECK(s != NULL, "server did not start");
    c = cli_start("frank", "127.0.0.1", s->port, NULL);
    CHECK(c && cli_expect(c, "Client count: 1", T(3000)), "no ACK with count 1");
}

static void t_i2_ack_list_online(void)
{
    struct srv  *s = srv_start("127.0.0.1", 5);
    struct peer *a, *b;
    struct cli  *c;

    CHECK(s != NULL, "server did not start");
    a = join_as(s, "alice");
    b = join_as(s, "bob");
    CHECK(a && b, "setup");
    c = cli_start("frank", "127.0.0.1", s->port, NULL);
    CHECK(c && cli_expect(c, "Client count: 3", T(3000)), "count should be 3");
    CHECK(cli_expect(c, "alice, bob", T(1000)), "member list should read \"alice, bob\"");
    CHECK(wait_for(a, TYPE_ONLINE, "frank", NULL, T(2000)), "alice got no ONLINE for frank");
}

static void t_i3_relay_both_ways(void)
{
    struct srv  *s = srv_start("127.0.0.1", 5);
    struct peer *a;
    struct cli  *c;

    CHECK(s != NULL, "server did not start");
    a = join_as(s, "alice");
    c = client_joined(s, "frank");
    CHECK(a && c, "setup");
    CHECK(psend(a, TYPE_SEND, ATTR_MESSAGE, "hello frank", -1, 0) == 0 &&
          cli_expect(c, "alice: hello frank", T(2000)), "FWD not shown as \"alice: hello frank\"");
    CHECK(cli_say(c, "hi alice") == 0 && wait_for(a, TYPE_FWD, "frank", "hi alice", T(2000)),
          "typed line not relayed");
}

static void t_i4_offline_shown(void)
{
    struct srv  *s = srv_start("127.0.0.1", 5);
    struct peer *a;
    struct cli  *c;

    CHECK(s != NULL, "server did not start");
    a = join_as(s, "alice");
    c = client_joined(s, "frank");
    CHECK(a && c, "setup");
    peer_free(a);
    CHECK(cli_expect(c, "<alice> is offline", T(2000)), "no OFFLINE notice for alice");
}

static void t_i5_duplicate_name(void)
{
    struct srv  *s = srv_start("127.0.0.1", 5);
    struct peer *a;
    struct cli  *c;

    CHECK(s != NULL, "server did not start");
    a = join_as(s, "alice");
    CHECK(a != NULL, "setup");
    c = cli_start("alice", "127.0.0.1", s->port, NULL);
    CHECK(c && cli_expect(c, "Username already in use", T(3000)), "NAK reason not shown");
    CHECK(cli_wait(c, T(3000)), "client kept running after the NAK");
    note("%s", signal_note(c));
}

static void t_i6_room_full(void)
{
    struct srv  *s = srv_start("127.0.0.1", 2);
    struct peer *a, *b;
    struct cli  *c;

    CHECK(s != NULL, "server did not start");
    a = join_as(s, "alice");
    b = join_as(s, "bob");
    CHECK(a && b, "setup");
    c = cli_start("frank", "127.0.0.1", s->port, NULL);
    CHECK(c && cli_expect(c, "Server full", T(3000)), "NAK reason not shown");
    CHECK(cli_wait(c, T(3000)), "client kept running after the NAK");
}

static void t_i7_coalesced(void)
{
    struct srv  *s = srv_start("127.0.0.1", 5);
    struct peer *a;
    struct cli  *c;
    int          i, shown = 0;

    CHECK(s != NULL, "server did not start");
    a = join_as(s, "alice");
    c = client_joined(s, "frank");
    CHECK(a && c, "setup");
    for (i = 0; i < 5; i++) {
        uint8_t buf[256];
        char    one[32], two[32];
        size_t  n;

        snprintf(one, sizeof(one), "one%d", i);
        snprintf(two, sizeof(two), "two%d", i);
        n  = mk(buf, sizeof(buf), SBCP_WIRE_NET, TYPE_SEND, ATTR_MESSAGE, one, -1, 0);
        n += mk(buf + n, sizeof(buf) - n, SBCP_WIRE_NET, TYPE_SEND, ATTR_MESSAGE, two, -1, 0);
        CHECK(peer_write(a, buf, n) == 0, "send");
        snprintf(one, sizeof(one), "alice: one%d", i);
        snprintf(two, sizeof(two), "alice: two%d", i);
        shown += cli_has(c, two, T(1000));
        shown += cli_has(c, one, 0);
    }
    CHECK(shown == 10, "%d of 10 messages shown", shown);
}

static void t_i8_two_leave_at_once(void)
{
    struct srv  *s = srv_start("127.0.0.1", 5);
    struct peer *a, *b;
    struct cli  *c;
    int          n;

    CHECK(s != NULL, "server did not start");
    a = join_as(s, "alice");
    b = join_as(s, "bob");
    c = client_joined(s, "frank");
    CHECK(a && b && c, "setup");
    peer_free(a);
    peer_free(b);
    n  = cli_has(c, "<bob> is offline", T(2000));
    n += cli_has(c, "<alice> is offline", T(500));
    CHECK(n == 2, "%d of 2 OFFLINE notices shown", n);
}

static void t_i9_idle_while_others_talk(void)
{
    struct srv  *s = srv_start("127.0.0.1", 5);
    struct peer *a;
    struct cli  *c;
    int64_t      t0, dt;
    int          got = 0;

    CHECK(s != NULL, "server did not start");
    a = join_as(s, "alice");
    c = client_joined(s, "frank");
    t0 = now_ms();
    CHECK(a && c && wait_for(a, TYPE_ONLINE, "frank", NULL, T(2000)), "setup");
    while (!got && now_ms() - t0 < 16000) {
        CHECK(psend(a, TYPE_SEND, ATTR_MESSAGE, "chatter", -1, 0) == 0, "send");
        got = wait_for(a, TYPE_IDLE, "frank", NULL, T(4000));
    }
    detail[0] = '\0';
    CHECK(got, "no IDLE within 16 s while alice talked every 4 s "
               "(receiving messages must not restart the idle clock)");
    dt = now_ms() - t0;
    CHECK(dt >= 9000 && dt <= 12000, "IDLE came after %lld ms, expected about 10 s", (long long)dt);
}

static void t_i10_idle_quiet_room(void)
{
    struct srv  *s = srv_start("127.0.0.1", 5);
    struct peer *a;
    struct cli  *c;
    int64_t      t0, dt;

    CHECK(s != NULL, "server did not start");
    a = join_as(s, "alice");
    c = client_joined(s, "frank");
    t0 = now_ms();
    CHECK(a && c && wait_for(a, TYPE_ONLINE, "frank", NULL, T(2000)), "setup");
    CHECK(wait_for(a, TYPE_IDLE, "frank", NULL, T(14000)), "no IDLE after 14 s of silence");
    dt = now_ms() - t0;
    CHECK(dt >= 9000 && dt <= 12000, "IDLE came after %lld ms, expected about 10 s", (long long)dt);
}

static void t_i11_two_lines_at_once(void)
{
    struct srv  *s = srv_start("127.0.0.1", 5);
    struct peer *a;
    struct cli  *c;

    CHECK(s != NULL, "server did not start");
    a = join_as(s, "alice");
    c = client_joined(s, "frank");
    CHECK(a && c && wait_for(a, TYPE_ONLINE, "frank", NULL, T(2000)), "setup");
    CHECK(cli_write(c, "first\nsecond\n", 13) == 0, "write");
    CHECK(wait_for(a, TYPE_FWD, "frank", "first", T(2000)), "first line not relayed");
    CHECK(wait_for(a, TYPE_FWD, "frank", "second", T(1500)),
          "second line not sent at once (held in a stdio buffer?)");
}

static void t_i12_long_line(void)
{
    static struct fwds m;
    struct srv        *s = srv_start("127.0.0.1", 5);
    struct peer       *a;
    struct cli        *c;
    char               line[601];

    CHECK(s != NULL, "server did not start");
    a = join_as(s, "alice");
    c = client_joined(s, "frank");
    CHECK(a && c && wait_for(a, TYPE_ONLINE, "frank", NULL, T(2000)), "setup");
    memset(line, 'x', 600);
    line[600] = '\0';
    CHECK(cli_say(c, line) == 0, "write");
    collect_fwd(a, "frank", T(2000), &m);
    CHECK(m.total == 600 && m.longest <= MAX_MESSAGE_LEN,
          "%zu of 600 bytes relayed in %d SEND(s), longest %zu", m.total, m.n, m.longest);
}

static void t_i13_ipv6(void)
{
    struct srv *s;
    struct cli *c;

    if (!ipv6_loopback_ok())
        SKIP("no IPv6 loopback on this machine");
    s = srv_start("::1", 5);
    CHECK(s != NULL, "server did not start on ::1");
    c = cli_start("frank6", "::1", s->port, NULL);
    CHECK(c && cli_expect(c, "Client count: 1", T(3000)), "client could not join over IPv6");
}

static void t_i14_localhost(void)
{
    struct srv *s = srv_start("127.0.0.1", 5);
    struct cli *c;

    CHECK(s != NULL, "server did not start");
    c = cli_start("frank", "localhost", s->port, NULL);
    CHECK(c && cli_expect(c, "Client count: 1", T(3000)), "client could not join via \"localhost\"");
}

static void t_i15_server_shutdown(void)
{
    struct srv *s = srv_start("127.0.0.1", 5);
    struct cli *c;

    CHECK(s != NULL, "server did not start");
    c = client_joined(s, "frank");
    CHECK(c != NULL, "setup");
    srv_stop(s);
    CHECK(cli_wait(c, T(3000)), "client kept running after the server shut down");
    CHECK(WIFEXITED(c->status), "client %s", signal_note(c));
    CHECK(cli_has(c, "closed", T(500)), "client did not say the server closed the connection");
}

static void t_i16_eof_leaves(void)
{
    struct srv  *s = srv_start("127.0.0.1", 5);
    struct peer *a;
    struct cli  *c;

    CHECK(s != NULL, "server did not start");
    a = join_as(s, "alice");
    c = client_joined(s, "frank");
    CHECK(a && c, "setup");
    cli_eof(c);
    CHECK(cli_wait(c, T(3000)), "client kept running after end of input");
    CHECK(wait_for(a, TYPE_OFFLINE, "frank", NULL, T(2000)), "no OFFLINE for frank");
}

static void t_i17_piped_input(void)
{
    struct srv  *s = srv_start("127.0.0.1", 5);
    struct peer *a;
    struct cli  *c;

    CHECK(s != NULL, "server did not start");
    a = join_as(s, "alice");
    c = client_joined(s, "frank");
    CHECK(a && c, "setup");
    CHECK(cli_write(c, "l1\nl2\nl3\n", 9) == 0, "write");
    cli_eof(c);
    CHECK(wait_for(a, TYPE_FWD, "frank", "l1", T(2000)) &&
          wait_for(a, TYPE_FWD, "frank", "l2", T(1000)) &&
          wait_for(a, TYPE_FWD, "frank", "l3", T(1000)), "not every piped line was relayed");
}

static void t_i18_big_ack(void)
{
    struct srv *s = srv_start("127.0.0.1", 60);
    struct cli *c;
    int         i;

    CHECK(s != NULL, "server did not start");
    for (i = 0; i < 55; i++) {
        char name[MAX_USERNAME_LEN + 1];

        snprintf(name, sizeof(name), "member_%02d_xxxxx", i);
        CHECK(join_as(s, name) != NULL, "member %d did not join", i);
    }
    c = cli_start("frank", "127.0.0.1", s->port, NULL);
    CHECK(c && cli_expect(c, "Client count: 56", T(3000)), "ACK of more than 1024 bytes not shown");
}

static void t_i19_name_reuse(void)
{
    struct srv  *s = srv_start("127.0.0.1", 5);
    struct peer *b;
    struct cli  *c1, *c2;

    CHECK(s != NULL, "server did not start");
    b = join_as(s, "bob");
    c1 = client_joined(s, "alice");
    CHECK(b && c1, "setup");
    cli_eof(c1);
    CHECK(cli_wait(c1, T(3000)), "first alice did not leave");
    CHECK(wait_for(b, TYPE_OFFLINE, "alice", NULL, T(2000)), "no OFFLINE for alice");
    c2 = cli_start("alice", "127.0.0.1", s->port, NULL);
    CHECK(c2 && cli_expect(c2, "Client count: 2", T(3000)), "the name was not reusable");
    CHECK(wait_for(b, TYPE_ONLINE, "alice", NULL, T(2000)), "no ONLINE for the new alice");
}

static void t_i20_sigkill(void)
{
    struct srv  *s = srv_start("127.0.0.1", 5);
    struct peer *b;
    struct cli  *c1, *c2;

    CHECK(s != NULL, "server did not start");
    b = join_as(s, "bob");
    c1 = client_joined(s, "alice");
    CHECK(b && c1, "setup");
    kill(c1->pid, SIGKILL);
    cli_wait(c1, T(2000));
    CHECK(wait_for(b, TYPE_OFFLINE, "alice", NULL, T(2000)), "no OFFLINE after SIGKILL");
    c2 = cli_start("alice", "127.0.0.1", s->port, NULL);
    CHECK(c2 && cli_expect(c2, "Client count: 2", T(3000)), "the name was not reusable");
}

static void t_i21_server_dies_mid_send(void)
{
    struct srv  *s = srv_start("127.0.0.1", 5);
    struct peer *a;
    struct cli  *c;
    int64_t      t0;
    int          i = 0;

    CHECK(s != NULL, "server did not start");
    a = join_as(s, "alice");
    c = client_joined(s, "frank");
    CHECK(a && c, "setup");
    // Lines keep coming while the server dies, so a send() hits the dead socket.
    t0 = now_ms();
    while (now_ms() - t0 < 800) {
        char line[32];

        snprintf(line, sizeof(line), "burst line %d", i++);
        if (cli_say(c, line) < 0)
            break;
        if (i == 400) {
            s->may_die = 1;
            kill(s->pid, SIGKILL);
        }
        msleep(1);
    }
    if (!s->may_die) {
        s->may_die = 1;
        kill(s->pid, SIGKILL);
    }
    detail[0] = '\0';
    CHECK(cli_wait(c, T(5000)), "client kept running after the server died");
    CHECK(WIFEXITED(c->status), "client %s", signal_note(c));
}

static void t_i22_idle_once_then_again(void)
{
    struct srv  *s = srv_start("127.0.0.1", 5);
    struct peer *a;
    struct cli  *c;
    int64_t      t0, dt;

    CHECK(s != NULL, "server did not start");
    a = join_as(s, "alice");
    c = client_joined(s, "frank");
    t0 = now_ms();
    CHECK(a && c && wait_for(a, TYPE_ONLINE, "frank", NULL, T(2000)), "setup");
    CHECK(expect_msg(a, TYPE_IDLE, "frank", NULL, T(12000)), "no IDLE after 10 s of silence");
    dt = now_ms() - t0;
    CHECK(dt >= 9000 && dt <= 12000, "first IDLE after %lld ms", (long long)dt);
    CHECK(expect_none(a, T(6000)), "IDLE sent more than once in one idle spell");
    CHECK(cli_say(c, "back again") == 0 && expect_msg(a, TYPE_FWD, "frank", "back again", T(2000)),
          "message after the idle spell not relayed");
    t0 = now_ms();
    CHECK(expect_msg(a, TYPE_IDLE, "frank", NULL, T(12000)), "no new IDLE after the next SEND");
    dt = now_ms() - t0;
    CHECK(dt >= 9000 && dt <= 12000, "second IDLE after %lld ms", (long long)dt);
}

static void t_i23_text_handling(void)
{
    static struct fwds m;
    static char        big[5000];
    struct srv        *s = srv_start("127.0.0.1", 5);
    struct peer       *a;
    struct cli        *c, *c2;
    char               line[601];
    int                i;

    CHECK(s != NULL, "server did not start");
    a = join_as(s, "alice");
    c = client_joined(s, "frank");
    CHECK(a && c && wait_for(a, TYPE_ONLINE, "frank", NULL, T(2000)), "setup");
    for (i = 0; i < 200; i++)
        memcpy(line + 3 * i, HAN, 3);
    line[600] = '\0';
    CHECK(cli_say(c, line) == 0 && cli_write(c, "crlf line\r\n", 11) == 0, "write");
    collect_fwd(a, "frank", T(2000), &m);
    CHECK(m.n == 3, "expected 2 SENDs for the 600-byte line and 1 for the CRLF line, got %d", m.n);
    CHECK(m.len[0] + m.len[1] == 600 && m.longest <= MAX_MESSAGE_LEN, "UTF-8 line: %zu + %zu bytes",
          m.len[0], m.len[1]);
    CHECK(m.len[0] % 3 == 0 && ((unsigned char)m.text[1][0] & 0xC0) != 0x80,
          "a 3-byte character was cut in half (first part %zu bytes)", m.len[0]);
    CHECK(strcmp(m.text[2], "crlf line") == 0, "CRLF line arrived as \"%s\"", m.text[2]);

    c2 = client_joined(s, "frank2");
    CHECK(c2 != NULL, "second client");
    memset(big, 'y', sizeof(big));
    CHECK(cli_write(c2, big, sizeof(big)) == 0, "write");
    cli_eof(c2);
    collect_fwd(a, "frank2", T(3000), &m);
    CHECK(m.total == 5000 && m.longest <= MAX_MESSAGE_LEN,
          "5000 bytes without newline: %zu relayed in %d SEND(s), longest %zu", m.total, m.n, m.longest);
}

static void t_i24_exact_bytes(void)
{
    static struct frame f;
    struct peer        *conn;
    struct cli         *c;
    int64_t             t0, dt;

    conn = fake_join(&c, "alice");
    CHECK(conn != NULL, "client did not connect and JOIN");
    CHECK(GOLDEN(&join_frame, "\x01\x82\x00\x0d\x00\x02\x00\x09" "alice"), "JOIN bytes");
    CHECK(psend(conn, TYPE_ACK, ATTR_CLIENT_COUNT, "\x00\x01", 2, 0) == 0 &&
          cli_expect(c, "Client count: 1", T(2000)), "ACK not shown");
    CHECK(cli_say(c, "hi") == 0 && peer_recv(conn, &f, T(2000)) == 1 &&
          GOLDEN(&f, "\x01\x84\x00\x0a\x00\x04\x00\x06" "hi"), "SEND bytes");
    t0 = now_ms();
    CHECK(cli_write(c, "\n", 1) == 0 && expect_none(conn, T(1000)), "an empty line sent something");
    CHECK(peer_recv(conn, &f, T(12000)) == 1 && GOLDEN(&f, "\x01\x89\x00\x04"), "IDLE bytes");
    dt = now_ms() - t0;
    CHECK(dt >= 9000 && dt <= 12000, "IDLE %lld ms after the SEND", (long long)dt);
}

static void t_i25_split_message(void)
{
    uint8_t      buf[128];
    struct peer *conn;
    struct cli  *c;
    size_t       n, i;

    conn = fake_join(&c, "alice");
    CHECK(conn != NULL, "client did not connect and JOIN");
    CHECK(psend(conn, TYPE_ACK, ATTR_CLIENT_COUNT, "\x00\x02", 2, ATTR_USERNAME, "bob", -1, 0) == 0 &&
          cli_expect(c, "Client count", T(2000)), "ACK not shown");
    n = mk(buf, sizeof(buf), SBCP_WIRE_NET, TYPE_FWD, ATTR_MESSAGE, "split works", -1,
           ATTR_USERNAME, "bob", -1, 0);
    for (i = 0; i < n; i++) {
        CHECK(peer_write(conn, buf + i, 1) == 0, "write");
        msleep(10);
    }
    CHECK(cli_expect(c, "bob: split works", T(2000)), "message sent one byte at a time not shown");
}

static void t_i26_discards(void)
{
    static const uint8_t partial[20] = { 0x01, 0x83, 0x00, 0x64, 'x', 'x', 'x', 'x', 'x', 'x',
                                         'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x' };
    uint8_t      buf[256];
    struct peer *conn;
    struct cli  *c;
    size_t       n;

    conn = fake_join(&c, "alice");
    CHECK(conn != NULL, "client did not connect and JOIN");
    CHECK(psend(conn, TYPE_ACK, ATTR_CLIENT_COUNT, "\x00\x02", 2, ATTR_USERNAME, "bob", -1, 0) == 0 &&
          cli_expect(c, "Client count", T(2000)), "ACK not shown");

    n  = mk(buf, sizeof(buf), SBCP_WIRE_NET, 13, ATTR_MESSAGE, "mystery", -1, 0);
    n += mk(buf + n, sizeof(buf) - n, SBCP_WIRE_NET, TYPE_FWD, ATTR_MESSAGE, "after unknown", -1,
            ATTR_USERNAME, "bob", -1, 0);
    CHECK(peer_write(conn, buf, n) == 0 && cli_expect(c, "bob: after unknown", T(2000)),
          "message after an unknown type not shown");
    CHECK(!cli_ever(c, "mystery"), "an unknown message type was displayed");

    CHECK(peer_write(conn, "GET / HTTP/1.0\r\n\r\n", 18) == 0, "write");
    msleep(300);
    CHECK(send_fwd(conn, "after garbage", "bob") == 0 && cli_expect(c, "bob: after garbage", T(2000)),
          "chat did not recover after garbage");

    CHECK(peer_write(conn, partial, sizeof(partial)) == 0, "write");
    msleep(4000);
    CHECK(send_fwd(conn, "after partial", "bob") == 0 && cli_expect(c, "bob: after partial", T(2000)),
          "a message that never completed was not discarded after a few seconds");
    CHECK(waitpid(c->pid, &c->status, WNOHANG) == 0, "client exited");
}

static void t_i27_bad_arguments(void)
{
    static char out[CLI_BUF];
    char        port[16], refused[16], name17[MAX_USERNAME_LEN + 2];
    int         lfd, p, rc, i;
    char       *cases[][5] = {
        { (char *)client_path, NULL },
        { (char *)client_path, "alice", NULL },
        { (char *)client_path, "alice", "127.0.0.1", NULL },
        { (char *)client_path, name17, "127.0.0.1", port, NULL },
        { (char *)client_path, "", "127.0.0.1", port, NULL },
        { (char *)client_path, "alice", "127.0.0.1", "abc", NULL },
        { (char *)client_path, "alice", "127.0.0.1", "0", NULL },
        { (char *)client_path, "alice", "127.0.0.1", "65536", NULL },
        { (char *)client_path, "alice", "999.1.1.1", port, NULL },
        { (char *)client_path, "alice", "127.0.0.1", refused, NULL },
    };

    memset(name17, 'a', MAX_USERNAME_LEN + 1);
    name17[MAX_USERNAME_LEN + 1] = '\0';
    lfd = fake_listen(&p);                  // ports with nothing listening
    fake_close(lfd);
    snprintf(port, sizeof(port), "%d", p);
    snprintf(refused, sizeof(refused), "%d", p);
    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        rc = run_status(cases[i], T(8000), out, sizeof(out));
        CHECK(rc == 1 && out[0] != '\0', "case %d (%s %s %s): status %d, output \"%.80s\"", i,
              cases[i][1] ? cases[i][1] : "", cases[i][1] && cases[i][2] ? cases[i][2] : "",
              cases[i][1] && cases[i][2] && cases[i][3] ? cases[i][3] : "", rc, out);
    }
}

static void t_i28_last_line_at_eof(void)
{
    struct srv  *s = srv_start("127.0.0.1", 5);
    struct peer *a;
    struct cli  *c;

    CHECK(s != NULL, "server did not start");
    a = join_as(s, "alice");
    c = client_joined(s, "frank");
    CHECK(a && c, "setup");
    CHECK(cli_write(c, "no newline at the end", 21) == 0, "write");
    cli_eof(c);
    CHECK(wait_for(a, TYPE_FWD, "frank", "no newline at the end", T(3000)),
          "an unfinished last line was not sent at end of input");
}

static void t_i29_ack_as_text(void)
{
    struct peer *conn;
    struct cli  *c;

    conn = fake_join(&c, "alice");
    CHECK(conn != NULL, "client did not connect and JOIN");
    CHECK(psend(conn, TYPE_ACK, ATTR_MESSAGE, "3 clients: bob, carol", -1, 0) == 0 &&
          cli_expect(c, "3 clients: bob, carol", T(2000)), "ACK text not shown");
}

static void t_i30_four_byte_count(void)
{
    struct peer *conn;
    struct cli  *c;

    conn = fake_join(&c, "alice");
    CHECK(conn != NULL, "client did not connect and JOIN");
    CHECK(psend(conn, TYPE_ACK, ATTR_CLIENT_COUNT, "\x00\x00\x00\x03", 4, ATTR_USERNAME, "bob", -1,
                ATTR_USERNAME, "carol", -1, 0) == 0 &&
          cli_expect(c, "Client count: 3", T(2000)), "a 4-byte Client Count was not read as 3");
}

static void t_i31_long_and_utf8_display(void)
{
    struct srv  *s = srv_start("127.0.0.1", 5);
    struct peer *a;
    struct cli  *c;
    char         text[MAX_MESSAGE_LEN + 1], want[MAX_MESSAGE_LEN + 16];

    CHECK(s != NULL, "server did not start");
    a = join_as(s, "alice");
    c = client_joined(s, "frank");
    CHECK(a && c, "setup");
    memset(text, 'L', MAX_MESSAGE_LEN - 1);
    text[MAX_MESSAGE_LEN - 1] = 'E';
    text[MAX_MESSAGE_LEN] = '\0';
    snprintf(want, sizeof(want), "alice: %s", text);
    CHECK(psend(a, TYPE_SEND, ATTR_MESSAGE, text, -1, 0) == 0 && cli_expect(c, want, T(2000)),
          "a 512-byte message was not shown in full");
    snprintf(want, sizeof(want), "alice: %s", HELLO);
    CHECK(psend(a, TYPE_SEND, ATTR_MESSAGE, HELLO, -1, 0) == 0 && cli_expect(c, want, T(2000)),
          "UTF-8 text not shown intact");
}

static void t_i32_ctrl_c(void)
{
    struct srv  *s = srv_start("127.0.0.1", 5);
    struct peer *b;
    struct cli  *c;

    CHECK(s != NULL, "server did not start");
    b = join_as(s, "bob");
    c = client_joined(s, "alice");
    CHECK(b && c, "setup");
    kill(c->pid, SIGINT);
    CHECK(cli_wait(c, T(3000)), "client ignored Ctrl-C");
    CHECK(wait_for(b, TYPE_OFFLINE, "alice", NULL, T(2000)), "no OFFLINE after Ctrl-C");
}

static void t_i33_prompt_on_terminal(void)
{
    struct srv *s = srv_start("127.0.0.1", 5);
    char        portstr[16], out[4096];
    size_t      len = 0;
    int         master;
    pid_t       pid;
    int64_t     deadline;

    CHECK(s != NULL, "server did not start");
    master = posix_openpt(O_RDWR | O_NOCTTY);
    CHECK(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0,
          "no pseudo-terminal: %s", strerror(errno));
    snprintf(portstr, sizeof(portstr), "%d", s->port);
    if ((pid = fork()) == 0) {
        int slave;

        setsid();
        if ((slave = open(ptsname(master), O_RDWR)) < 0)
            _exit(126);
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        close_from(3);
        child_signals();
        execl(client_path, client_path, "frank", "127.0.0.1", portstr, (char *)NULL);
        _exit(127);
    }
    out[0] = '\0';
    deadline = now_ms() + T(3000);
    while (pid > 0 && now_ms() < deadline && strstr(out, "frank> ") == NULL) {
        fd_set         rset;
        struct timeval tv = { 0, 100000 };
        ssize_t        n;

        FD_ZERO(&rset);
        FD_SET(master, &rset);
        if (select(master + 1, &rset, NULL, NULL, &tv) <= 0)
            continue;
        if ((n = read(master, out + len, sizeof(out) - 1 - len)) <= 0)
            break;
        len += (size_t)n;
        out[len] = '\0';
    }
    if (pid > 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
    close(master);
    CHECK(pid > 0, "fork failed");
    CHECK(strstr(out, "frank> ") != NULL, "no \"frank> \" prompt on a terminal; output \"%.80s\"", out);
}

static void t_i34_no_echo_from_server(void)
{
    struct srv  *s = srv_start("127.0.0.1", 5);
    struct peer *a;
    struct cli  *c;

    CHECK(s != NULL, "server did not start");
    a = join_as(s, "alice");
    c = client_joined(s, "frank");
    CHECK(a && c, "setup");
    CHECK(cli_say(c, "hello all") == 0 && wait_for(a, TYPE_FWD, "frank", "hello all", T(2000)),
          "not relayed");
    msleep(500);
    CHECK(!cli_ever(c, "frank: hello all"), "own message shown back as if someone else sent it");
}

// ============================================================================
// runner
// ============================================================================

struct test {
    const char *id;
    const char *name;
    void      (*fn)(void);
    int         slow;
};

static const struct test TESTS[] = {
    { "U1",  "encoder produces the exact bytes",                        t_u1_encoder_golden, 0 },
    { "U2",  "validator rejects malformed messages",                    t_u2_parser_rejects, 0 },
    { "U3",  "reassembler: split, coalesced, garbage, oversized",       t_u3_reader, 0 },
    { "U4",  "username/text rules and display sanitising",              t_u4_text_rules, 0 },
    { "U5",  "both header layouts decode for all 128 types",            t_u5_legacy_header, 0 },
    { "A5",  "bonus: ACK/NAK/ONLINE/OFFLINE contents",                  t_a5_ack_nak_online_offline, 0 },
    { "A6",  "bonus: IDLE relayed once, with the sender's name",        t_a6_idle_relay, 0 },
    { "A9",  "server on \"localhost\" takes IPv4 and IPv6",             t_a9_localhost, 0 },
    { "B1",  "golden bytes for every message type",                     t_b1_golden_bytes, 0 },
    { "B2",  "sender name cannot be spoofed",                           t_b2_no_spoofing, 0 },
    { "B3",  "several messages in one TCP segment",                     t_b3_coalesced, 0 },
    { "B4",  "one message split into single bytes",                     t_b4_split_bytes, 0 },
    { "B5",  "NUL-terminated strings tolerated",                        t_b5_nul_tolerance, 0 },
    { "B6",  "message length limits 1..512",                            t_b6_message_lengths, 0 },
    { "B7",  "username rules and NAK reasons",                          t_b7_username_rules, 0 },
    { "B8",  "legacy little-endian peers interoperate",                 t_b8_legacy_layout, 0 },
    { "B10", "300 messages arrive in order",                            t_b10_order, 0 },
    { "B11", "ACK lists members in join order",                         t_b11_ack_join_order, 0 },
    { "C1",  "text/HTTP/binary garbage discarded",                      t_c1_garbage, 0 },
    { "C2",  "unknown message types discarded, framing kept",           t_c2_unknown_type, 0 },
    { "C3",  "server-only message types from a client discarded",       t_c3_server_only_types, 0 },
    { "C4",  "bad Length fields and attribute defects",                 t_c4_bad_lengths, 0 },
    { "C5",  "incomplete message discarded after 3 s, no stall",        t_c5_incomplete_timeout, 1 },
    { "C6",  "SEND/IDLE before JOIN discarded",                         t_c6_before_join, 0 },
    { "C7",  "second JOIN on a joined connection ignored",              t_c7_second_join, 0 },
    { "C8",  "oversized message skipped, next one kept",                t_c8_oversized, 0 },
    { "C9",  "fuzz: 3600 random messages, 12 connections",              t_c9_fuzz, 0 },
    { "D1",  "RST before JOIN (40x)",                                   t_d1_rst_before_join, 0 },
    { "D2",  "RST after JOIN: OFFLINE and name freed",                  t_d2_rst_after_join, 0 },
    { "D3",  "RST while the server has output queued (SIGPIPE case)",   t_d3_rst_with_output_pending, 0 },
    { "D4",  "JOIN then immediate RST (50x)",                           t_d4_join_then_rst, 0 },
    { "D6",  "client half-close (shutdown SHUT_WR)",                    t_d6_half_close, 0 },
    { "D7",  "50 clients RST during a broadcast burst",                 t_d7_mass_rst, 0 },
    { "D8",  "non-reading client + 20 MB flood: no stall",              t_d8_slow_reader, 0 },
    { "D9",  "out of descriptors (EMFILE)",                             t_d9_emfile, 0 },
    { "D10", "more connections than FD_SETSIZE",                        t_d10_fd_setsize, 0 },
    { "D11", "stdout closed or never read",                             t_d11_stdout_trouble, 0 },
    { "D13", "RST in the middle of a message",                          t_d13_rst_mid_message, 0 },
    { "D14", "300 connect/leave cycles leak no descriptors",            t_d14_churn_no_leak, 0 },
    { "D15", "SIGTERM shutdown and restart on the same port",           t_d15_restart_same_port, 0 },
    { "F1",  "server argument and bind errors",                         t_f1_server_args, 0 },
    { "I1",  "client: first member gets ACK count 1",                       t_i1_ack_first, 0 },
    { "I2",  "client: ACK lists members; they get ONLINE",                  t_i2_ack_list_online, 0 },
    { "I3",  "client: FWD shown; typed line relayed",                       t_i3_relay_both_ways, 0 },
    { "I4",  "client: OFFLINE shown when a member leaves",                  t_i4_offline_shown, 0 },
    { "I5",  "client: duplicate name NAK shown, client exits",              t_i5_duplicate_name, 0 },
    { "I6",  "client: room-full NAK shown, client exits",                   t_i6_room_full, 0 },
    { "I7",  "client: two messages in one segment both shown",              t_i7_coalesced, 0 },
    { "I8",  "client: two members leaving at once both shown",              t_i8_two_leave_at_once, 0 },
    { "I9",  "client: IDLE after 10 s while others keep talking",           t_i9_idle_while_others_talk, 1 },
    { "I10", "client: IDLE after 10 s in a quiet room",                     t_i10_idle_quiet_room, 1 },
    { "I11", "client: two lines arriving together both sent",               t_i11_two_lines_at_once, 0 },
    { "I12", "client: 600-character line sent in full",                     t_i12_long_line, 0 },
    { "I13", "client: joins over IPv6 (::1)",                               t_i13_ipv6, 0 },
    { "I14", "client: host name localhost accepted",                        t_i14_localhost, 0 },
    { "I15", "client: server shutdown reported, client exits",              t_i15_server_shutdown, 0 },
    { "I16", "client: Ctrl-D leaves; others see OFFLINE",                   t_i16_eof_leaves, 0 },
    { "I17", "client: piped lines all sent before exit",                    t_i17_piped_input, 0 },
    { "I18", "client: ACK over 1024 bytes (55 members) shown",              t_i18_big_ack, 0 },
    { "I19", "client: name reusable after its owner leaves",                t_i19_name_reuse, 0 },
    { "I20", "client: SIGKILL gives OFFLINE; name reusable",                t_i20_sigkill, 0 },
    { "I21", "client: server dies mid-send; no SIGPIPE death",              t_i21_server_dies_mid_send, 0 },
    { "I22", "client: one IDLE per idle spell, again after SEND",           t_i22_idle_once_then_again, 1 },
    { "I23", "client: UTF-8 split safely, CRLF, 5000 bytes",                t_i23_text_handling, 0 },
    { "I24", "client: exact JOIN/SEND/IDLE bytes; empty line",              t_i24_exact_bytes, 1 },
    { "I25", "client: message one byte at a time shown",                    t_i25_split_message, 0 },
    { "I26", "client: unknown, garbage, unfinished discarded",              t_i26_discards, 1 },
    { "I27", "client: bad arguments, refused connection: exit 1",           t_i27_bad_arguments, 0 },
    { "I28", "client: unfinished last line sent at end of input",           t_i28_last_line_at_eof, 0 },
    { "I29", "client: ACK as a text Message attribute shown",               t_i29_ack_as_text, 0 },
    { "I30", "client: 4-byte Client Count read correctly",                  t_i30_four_byte_count, 0 },
    { "I31", "client: 512-byte and UTF-8 messages shown intact",            t_i31_long_and_utf8_display, 0 },
    { "I32", "client: Ctrl-C (SIGINT) gives OFFLINE",                       t_i32_ctrl_c, 0 },
    { "I33", "client: prompt shown on a terminal",                          t_i33_prompt_on_terminal, 0 },
    { "I34", "client: own messages not shown back",                         t_i34_no_echo_from_server, 0 },
};

static void kill_children(void)
{
    int i;

    for (i = 0; i < MAX_SRV; i++)
        if (srvs[i].used && !srvs[i].exited)
            kill(srvs[i].pid, SIGKILL);
    for (i = 0; i < MAX_CLI; i++)
        if (clis[i].used && !clis[i].exited)
            kill(clis[i].pid, SIGKILL);
}

static void on_alarm(int signo)
{
    static const char msg[] = "\n*** test timed out; killing children ***\n";
    ssize_t           ignored;

    (void)signo;
    ignored = write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    (void)ignored;
    kill_children();
    _exit(2);
}

static void finish_test(void)
{
    int i;

    for (i = 0; i < 8; i++)
        if (fake_lfds[i] >= 0) {
            close(fake_lfds[i]);
            fake_lfds[i] = -1;
        }

    for (i = 0; i < MAX_CLI; i++)
        cli_free(&clis[i]);
    for (i = 0; i < MAX_PEER; i++)
        peer_free(&peers[i]);
    msleep(50);

    for (i = 0; i < MAX_SRV; i++) {
        struct srv *s = &srvs[i];
        int         st;

        if (!s->used)
            continue;
        if (!s->may_die && !srv_alive(s)) {
            detail[0] = '\0';
            test_fail(0, "SERVER DIED (%s %d); log: %s",
                      WIFSIGNALED(s->status) ? "signal" : "exit",
                      WIFSIGNALED(s->status) ? WTERMSIG(s->status) : WEXITSTATUS(s->status),
                      s->log);
        } else if (!s->may_die) {
            st = srv_stop(s);           // clean exit on SIGTERM is part of the test
            if (st == -1 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
                detail[0] = '\0';
                test_fail(0, "server did not exit cleanly on SIGTERM (status 0x%x); log: %s",
                          (unsigned)st, s->log);
            }
        } else {
            srv_stop(s);
        }
        if (s->out_fd >= 0)
            close(s->out_fd);
        s->used = 0;
    }
}

// --group: the cases whose ID starts with one of these letters.
static int in_group(const char *group, const char *id)
{
    return group == NULL || strchr(group, id[0]) != NULL;
}

static int selected(const char *only, const char *id)
{
    const char *p = only;
    size_t      n = strlen(id);

    if (only == NULL)
        return 1;
    while ((p = strstr(p, id)) != NULL) {
        if ((p == only || p[-1] == ',') && (p[n] == '\0' || p[n] == ','))
            return 1;
        p += n;
    }
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [-v] [--quick] [--only ID,ID] [--group LETTERS] [--list]\n"
                    "          [--server PATH] [--client PATH]\n"
                    "          [--wrap \"valgrind -q --error-exitcode=99\"]\n"
                    "  --group I runs the client cases, --group UABCDF the server cases\n",
            prog);
    exit(2);
}

int main(int argc, char **argv)
{
    const char *only = NULL, *group = NULL;
    int         need_client = 0;
    size_t      i;
    int64_t     t_all = now_ms();

    for (i = 1; i < (size_t)argc; i++) {
        if (strcmp(argv[i], "-v") == 0)
            verbose = 1;
        else if (strcmp(argv[i], "--quick") == 0)
            quick = 1;
        else if (strcmp(argv[i], "--only") == 0 && i + 1 < (size_t)argc)
            only = argv[++i];
        else if (strcmp(argv[i], "--server") == 0 && i + 1 < (size_t)argc)
            server_path = argv[++i];
        else if (strcmp(argv[i], "--group") == 0 && i + 1 < (size_t)argc)
            group = argv[++i];
        else if (strcmp(argv[i], "--client") == 0 && i + 1 < (size_t)argc)
            client_path = argv[++i];
        else if (strcmp(argv[i], "--wrap") == 0 && i + 1 < (size_t)argc)
            wrap = argv[++i];
        else if (strcmp(argv[i], "--list") == 0) {
            for (i = 0; i < sizeof(TESTS) / sizeof(TESTS[0]); i++)
                printf("%-4s %s%s\n", TESTS[i].id, TESTS[i].name, TESTS[i].slow ? " (slow)" : "");
            return 0;
        } else
            usage(argv[0]);
    }
    if (wrap != NULL)
        slowdown = 4;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGALRM, on_alarm);
    atexit(kill_children);
    mkdir("tests/logs", 0755);
    for (i = 0; i < MAX_PEER; i++)
        peers[i].fd = -1;

    for (i = 0; i < sizeof(TESTS) / sizeof(TESTS[0]); i++)
        if (TESTS[i].id[0] == 'I' && selected(only, TESTS[i].id) && in_group(group, TESTS[i].id))
            need_client = 1;
    if (access(server_path, X_OK) != 0 || (need_client && access(client_path, X_OK) != 0)) {
        fprintf(stderr, "build first: %s%s%s must exist (run make)\n", server_path,
                need_client ? " and " : "", need_client ? client_path : "");
        return 2;
    }
    printf("SBCP tests: server %s%s%s%s%s\n\n", server_path,
           need_client ? ", client " : "", need_client ? client_path : "",
           wrap ? ", server wrapped in " : "", wrap ? wrap : "");

    for (i = 0; i < sizeof(TESTS) / sizeof(TESTS[0]); i++) {
        const struct test *t = &TESTS[i];
        int64_t            t0;
        char               label[80];

        if (!selected(only, t->id) || !in_group(group, t->id))
            continue;
        snprintf(label, sizeof(label), "[%s] %s ", t->id, t->name);
        printf("%-66.66s", label);
        fflush(stdout);
        if (quick && t->slow) {
            printf(" SKIP (slow; drop --quick)\n");
            n_skip++;
            continue;
        }

        cur_id = t->id;
        failed_now = skipped_now = 0;
        fail_msg[0] = detail[0] = '\0';
        t0 = now_ms();
        alarm((unsigned)(t->slow ? 90 : 60) * (unsigned)slowdown);
        t->fn();
        alarm(0);
        finish_test();

        if (skipped_now && !failed_now) {
            printf(" SKIP  %s\n", fail_msg);
            n_skip++;
        } else if (failed_now) {
            printf(" FAIL  (%.1f s)\n        %s\n", (double)(now_ms() - t0) / 1000.0, fail_msg);
            n_fail++;
        } else {
            printf(" PASS  (%.1f s)\n", (double)(now_ms() - t0) / 1000.0);
            n_pass++;
        }
        fflush(stdout);
    }

    printf("\n%d passed, %d failed, %d skipped in %.1f s\n", n_pass, n_fail, n_skip,
           (double)(now_ms() - t_all) / 1000.0);
    return n_fail == 0 ? 0 : 1;
}
