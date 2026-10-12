// net.h -- protocol-independent socket helpers (IPv4 and IPv6 through
// getaddrinfo), shared by the server and the client.

#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>

// "[2001:db8::1%eth0]:65535" plus room to spare.
#define NET_ADDRSTRLEN 80

// Binds and listens on every address host resolves to (up to max sockets),
// so "localhost" covers both ::1 and 127.0.0.1. "::" alone is dual-stack.
// Returns the number of listening sockets; 0 means none could be opened and
// err holds the reason.
int net_listen(const char *host, const char *port, int backlog,
               int *fds, int max, char *err, size_t errlen);

// Tries each address of host in turn, giving each timeout_ms to connect.
// Returns a blocking connected socket, or -1 with the reason in err.
int net_connect(const char *host, const char *port, int timeout_ms,
                char *peer, size_t peerlen, char *err, size_t errlen);

// "1.2.3.4:5" or "[::1]:5"; IPv4-mapped IPv6 addresses print as IPv4.
void net_addr_str(const struct sockaddr *sa, socklen_t len,
                  char *out, size_t outlen);

// Local and remote address of a connected socket, formatted as above.
void net_sock_str(int fd, char *out, size_t outlen);
void net_peer_str(int fd, char *out, size_t outlen);

int net_set_nonblock(int fd, int on);

// Writes all n bytes to a blocking socket. Never raises SIGPIPE: a dead peer
// comes back as -1 with errno EPIPE or ECONNRESET.
int net_send_all(int fd, const void *buf, size_t n);

// Flags for send(): MSG_NOSIGNAL where the platform has it.
#ifdef MSG_NOSIGNAL
#define NET_SEND_FLAGS MSG_NOSIGNAL
#else
#define NET_SEND_FLAGS 0
#endif

#endif // NET_H
