// util.h: logging, strict argument parsing, clocks and display helpers,
// shared by the server and the client.

#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdint.h>

// Lets gcc/clang check printf-style arguments at compile time.
#if defined(__GNUC__)
#define PRINTF_LIKE(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define PRINTF_LIKE(fmt_idx, arg_idx)
#endif

// Timestamped logging, one write() per line: log_info to stdout, log_err to
// stderr with strerror(errno) appended, log_msg to stderr without it.
// die_sys and die_msg log like log_err and log_msg, then exit(1).
void log_info(const char *fmt, ...) PRINTF_LIKE(1, 2);
void log_err(const char *fmt, ...) PRINTF_LIKE(1, 2);
void log_msg(const char *fmt, ...) PRINTF_LIKE(1, 2);
void die_sys(const char *fmt, ...) PRINTF_LIKE(1, 2);
void die_msg(const char *fmt, ...) PRINTF_LIKE(1, 2);

// Whole-string decimal conversion in [lo, hi]. Returns 0, or -1 for junk,
// trailing characters, overflow or a value out of range.
int parse_long(const char *s, long lo, long hi, long *out);

// Milliseconds from CLOCK_MONOTONIC: never jumps when the wall clock is set.
int64_t now_ms(void);

// Copies n bytes of untrusted text into out for printing. Control characters
// become caret notation (^[, ^J, ^?) so a peer cannot drive the terminal.
// Bytes >= 0x80 pass through, so UTF-8 still displays. Always NUL-terminates.
void sanitize(const void *in, size_t n, char *out, size_t outsz);

// Installs handler for signo with sigaction(); SIG_IGN and SIG_DFL also work.
void set_signal(int signo, void (*handler)(int), int flags);

#endif // UTIL_H
