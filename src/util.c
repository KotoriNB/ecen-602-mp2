// util.c: implementation of the helpers in util.h.

#include "util.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#define LOG_LINE_MAX 1024

// Advances past a *printf() result, clamping so we never run past the buffer.
static size_t bump(size_t off, int ret, size_t cap)
{
    if (ret < 0)
        return off;
    if ((size_t)ret >= cap - off)
        return cap - 1;
    return off + (size_t)ret;
}

// Zero-timeout select(): would a write to fd go through without blocking?
static int writable_now(int fd)
{
    fd_set wset;
    struct timeval tv = { 0, 0 };

    if (fd < 0 || fd >= FD_SETSIZE)
        return 1;
    FD_ZERO(&wset);
    FD_SET(fd, &wset);
    return select(fd + 1, NULL, &wset, NULL, &tv) != 0;
}

static void vlog(int fd, int with_errno, const char *fmt, va_list ap) PRINTF_LIKE(3, 0);

// One write() per line keeps lines whole even when stdout is a pipe. Logging
// must never stop the server: a line is dropped rather than blocking on a
// full pipe (e.g. a test harness that only reads the log at the end) or a
// paused terminal, and a failed write is ignored (SIGPIPE is ignored).
static void vlog(int fd, int with_errno, const char *fmt, va_list ap)
{
    int saved_errno = errno;
    char line[LOG_LINE_MAX];
    char stamp[32];
    struct tm tmv;
    time_t now = time(NULL);
    size_t off = 0;
    ssize_t ignored;

    if (!writable_now(fd)) {
        errno = saved_errno;
        return;
    }

    if (localtime_r(&now, &tmv) != NULL)
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmv);
    else
        snprintf(stamp, sizeof(stamp), "no timestamp");

    off = bump(off, snprintf(line, sizeof(line), "[%s] ", stamp), sizeof(line));

    if (off < sizeof(line) - 1)
        off = bump(off, vsnprintf(line + off, sizeof(line) - off, fmt, ap), sizeof(line));

    if (with_errno && off < sizeof(line) - 1)
        off = bump(off, snprintf(line + off, sizeof(line) - off, ": %s", strerror(saved_errno)), sizeof(line));

    line[off++] = '\n';

    ignored = write(fd, line, off);
    (void)ignored;

    errno = saved_errno;
}

void log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(STDOUT_FILENO, 0, fmt, ap);
    va_end(ap);
}

void log_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(STDERR_FILENO, 1, fmt, ap);
    va_end(ap);
}

void log_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(STDERR_FILENO, 0, fmt, ap);
    va_end(ap);
}

void die_sys(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(STDERR_FILENO, 1, fmt, ap);
    va_end(ap);
    exit(EXIT_FAILURE);
}

void die_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(STDERR_FILENO, 0, fmt, ap);
    va_end(ap);
    exit(EXIT_FAILURE);
}

int parse_long(const char *s, long lo, long hi, long *out)
{
    char *end;
    long v;

    if (s == NULL || *s == '\0')
        return -1;

    errno = 0;
    v = strtol(s, &end, 10);

    // end == s: no digits. *end != '\0': trailing junk such as "80x".
    if (end == s || *end != '\0' || errno != 0 || v < lo || v > hi)
        return -1;

    *out = v;
    return 0;
}

int64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void sanitize(const void *in, size_t n, char *out, size_t outsz)
{
    const unsigned char *p = in;
    size_t o = 0;
    size_t i;

    if (outsz == 0)
        return;

    for (i = 0; i < n && o + 1 < outsz; i++) {
        unsigned char c = p[i];

        if (c == '\t' || (c >= 0x20 && c != 0x7F)) {
            out[o++] = (char)c;
        } else {
            if (o + 2 >= outsz)
                break;
            out[o++] = '^';
            out[o++] = (c == 0x7F) ? '?' : (char)(c + '@');
        }
    }
    out[o] = '\0';
}

void set_signal(int signo, void (*handler)(int), int flags)
{
    struct sigaction act;

    memset(&act, 0, sizeof(act));
    act.sa_handler = handler;
    act.sa_flags = flags;
    sigemptyset(&act.sa_mask);

    if (sigaction(signo, &act, NULL) < 0)
        die_sys("sigaction for signal %d", signo);
}
