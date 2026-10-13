// net.c: implementation of net.h.

#include "net.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

int net_listen(const char *host, const char *port, int backlog, int *fds, int max, char *err, size_t errlen)
{
    struct addrinfo hints, *res, *ai;
    int n_addr = 0, n = 0, rc;

    memset(&hints, 0, sizeof(hints));
    // IPv4 or IPv6, whatever host is
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;

    if ((rc = getaddrinfo(host, port, &hints, &res)) != 0) {
        snprintf(err, errlen, "cannot resolve \"%s\": %s", host, gai_strerror(rc));
        return 0;
    }
    snprintf(err, errlen, "\"%s\" has no usable address", host);

    for (ai = res; ai != NULL; ai = ai->ai_next)
        n_addr++;

    for (ai = res; ai != NULL && n < max; ai = ai->ai_next) {
        char addr[NET_ADDRSTRLEN];
        int one = 1;
        int fd;

        net_addr_str(ai->ai_addr, ai->ai_addrlen, addr, sizeof(addr));

        if ((fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) < 0) {
            snprintf(err, errlen, "socket for %s: %s", addr, strerror(errno));
            continue;
        }

        // Or a restart while old connections sit in TIME_WAIT fails with EADDRINUSE.
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0)
            log_err("setsockopt(SO_REUSEADDR) on %s", addr);

        // A lone "::" should also take IPv4 clients (as ::ffff:a.b.c.d). With
        // several addresses the IPv4 ones get sockets of their own, so each
        // IPv6 socket must stay IPv6-only or those binds would collide.
        if (ai->ai_family == AF_INET6) {
            int v6only = n_addr > 1;

            if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) < 0)
                log_err("setsockopt(IPV6_V6ONLY) on %s", addr);
        }

        if (bind(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
            snprintf(err, errlen, "bind to %s: %s", addr, strerror(errno));
            close(fd);
            continue;
        }
        if (listen(fd, backlog) < 0) {
            snprintf(err, errlen, "listen on %s: %s", addr, strerror(errno));
            close(fd);
            continue;
        }
        fds[n++] = fd;
    }

    freeaddrinfo(res);
    return n;
}

// Non-blocking connect() bounded by timeout_ms, so a wrong or firewalled
// address fails in seconds instead of the kernel's two minutes of SYN retries.
static int connect_timeout(int fd, const struct sockaddr *sa, socklen_t len, int timeout_ms)
{
    int64_t deadline = now_ms() + timeout_ms;
    int soerr = 0;
    socklen_t elen = sizeof(soerr);

    if (net_set_nonblock(fd, 1) < 0)
        return -1;

    if (connect(fd, sa, len) < 0) {
        if (errno != EINPROGRESS && errno != EINTR)
            return -1;

        for (;;) {
            fd_set wset;
            struct timeval tv;
            int64_t left = deadline - now_ms();
            int nready;

            if (left <= 0) {
                errno = ETIMEDOUT;
                return -1;
            }
            if (fd >= FD_SETSIZE) {
                errno = EMFILE;
                return -1;
            }
            FD_ZERO(&wset);
            FD_SET(fd, &wset);
            tv.tv_sec = (time_t)(left / 1000);
            tv.tv_usec = (suseconds_t)(left % 1000) * 1000;

            nready = select(fd + 1, NULL, &wset, NULL, &tv);
            if (nready < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            if (nready > 0)
                break;
        }

        // Writable means finished, successfully or not; SO_ERROR says which.
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &elen) < 0)
            return -1;
        if (soerr != 0) {
            errno = soerr;
            return -1;
        }
    }

    return net_set_nonblock(fd, 0);
}

int net_connect(const char *host, const char *port, int timeout_ms, char *peer, size_t peerlen, char *err,
    size_t errlen)
{
    struct addrinfo hints, *res, *ai;
    int fd = -1, saved = ENOENT, rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;

    if ((rc = getaddrinfo(host, port, &hints, &res)) != 0) {
        snprintf(err, errlen, "cannot resolve \"%s\": %s", host, gai_strerror(rc));
        return -1;
    }
    snprintf(err, errlen, "\"%s\" has no usable address", host);

    for (ai = res; ai != NULL; ai = ai->ai_next) {
        char addr[NET_ADDRSTRLEN];

        net_addr_str(ai->ai_addr, ai->ai_addrlen, addr, sizeof(addr));

        if ((fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) < 0) {
            saved = errno;
            snprintf(err, errlen, "socket for %s: %s", addr, strerror(saved));
            continue;
        }
        if (connect_timeout(fd, ai->ai_addr, ai->ai_addrlen, timeout_ms) == 0) {
            snprintf(peer, peerlen, "%s", addr);
            break;
        }
        saved = errno;
        snprintf(err, errlen, "connect to %s: %s", addr, strerror(saved));
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    if (fd < 0)
        errno = saved;
    return fd;
}

void net_addr_str(const struct sockaddr *sa, socklen_t len, char *out, size_t outlen)
{
    char host[1025];
    char serv[32];
    struct sockaddr_in v4;

    // Dual-stack sockets report IPv4 peers as ::ffff:a.b.c.d; show a.b.c.d.
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)(const void *)sa;

        if (IN6_IS_ADDR_V4MAPPED(&s6->sin6_addr)) {
            memset(&v4, 0, sizeof(v4));
            v4.sin_family = AF_INET;
            v4.sin_port = s6->sin6_port;
            memcpy(&v4.sin_addr, &s6->sin6_addr.s6_addr[12], 4);
            sa = (const struct sockaddr *)&v4;
            len = sizeof(v4);
        }
    }

    if (getnameinfo(sa, len, host, sizeof(host), serv, sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        snprintf(out, outlen, "(unknown address)");
        return;
    }

    if (sa->sa_family == AF_INET6)
        snprintf(out, outlen, "[%s]:%s", host, serv);
    else
        snprintf(out, outlen, "%s:%s", host, serv);
}

void net_sock_str(int fd, char *out, size_t outlen)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);

    if (getsockname(fd, (struct sockaddr *)&ss, &len) < 0)
        snprintf(out, outlen, "(unknown address)");
    else
        net_addr_str((struct sockaddr *)&ss, len, out, outlen);
}

void net_peer_str(int fd, char *out, size_t outlen)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);

    if (getpeername(fd, (struct sockaddr *)&ss, &len) < 0)
        snprintf(out, outlen, "(unknown address)");
    else
        net_addr_str((struct sockaddr *)&ss, len, out, outlen);
}

int net_set_nonblock(int fd, int on)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0)
        return -1;
    flags = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(fd, F_SETFL, flags);
}

int net_send_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;

    while (n > 0) {
        ssize_t k = send(fd, p, n, NET_SEND_FLAGS);

        if (k < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (k == 0) {
            errno = EPIPE;
            return -1;
        }
        p += k;
        n -= (size_t)k;
    }
    return 0;
}
