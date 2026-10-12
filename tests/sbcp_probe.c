// tests/sbcp_probe.c -- which byte layout does an SBCP server speak?
//
// Joins the server twice, once in the spec's network-byte-order layout and
// once in the little-endian bit-field layout some implementations send,
// then prints both exchanges in hex with a verdict. Use it on the course's
// reference server or another team's server before interoperability tests.
//
// Usage: ./tests/sbcp_probe <host> <port> [username]

#include "net.h"
#include "sbcp_codec.h"
#include "util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define WAIT_MS 3000

static void hexdump(const char *label, const uint8_t *p, size_t n)
{
    size_t i;

    printf("    %s (%zu bytes):", label, n);
    for (i = 0; i < n && i < 48; i++)
        printf(" %02x", p[i]);
    if (n > 48)
        printf(" ...");
    printf("\n");
}

static void describe(const uint8_t *frame, size_t len)
{
    struct sbcp_msg  m;
    struct sbcp_field a;
    const char      *why;
    size_t           pos = 0;
    char             text[256];

    if (sbcp_parse(frame, len, &m, &why) < 0) {
        printf("    not a valid SBCP message: %s\n", why);
        return;
    }
    printf("    = %s in the %s layout", sbcp_type_name(m.type),
           m.wire == SBCP_WIRE_NET ? "network-byte-order" : "little-endian bit-field");
    while (sbcp_next_attr(&m, &pos, &a)) {
        unsigned long count;

        if (a.type == ATTR_CLIENT_COUNT && sbcp_attr_count(&a, m.wire, &count) == 0) {
            printf(", count %lu", count);
        } else {
            sanitize(a.val, a.len < 64 ? a.len : 64, text, sizeof(text));
            printf(", %s \"%s\"", a.type == ATTR_USERNAME ? "user" :
                                  a.type == ATTR_REASON ? "reason" :
                                  a.type == ATTR_MESSAGE ? "message" : "attr", text);
        }
    }
    printf("\n");
}

// Returns 1 if the server answered this layout with valid SBCP in that layout.
static int probe(const char *host, const char *port, const char *name,
                 enum sbcp_wire wire)
{
    uint8_t             out[64], in[4096];
    struct sbcp_builder b;
    char                peer[NET_ADDRSTRLEN], err[256];
    size_t              n, got = 0;
    int                 fd, ok = 0;
    int64_t             deadline;

    printf("%s layout:\n", wire == SBCP_WIRE_NET ? "Network-byte-order (spec)"
                                                 : "Little-endian bit-field");
    if ((fd = net_connect(host, port, 5000, peer, sizeof(peer), err, sizeof(err))) < 0) {
        printf("    cannot connect: %s\n", err);
        return 0;
    }

    sbcp_begin(&b, out, sizeof(out), wire, TYPE_JOIN);
    sbcp_add_attr(&b, ATTR_USERNAME, name, strlen(name));
    n = sbcp_finish(&b);
    hexdump("sent JOIN", out, n);
    if (net_send_all(fd, out, n) < 0) {
        printf("    send failed: %s\n", strerror(errno));
        close(fd);
        return 0;
    }

    deadline = now_ms() + WAIT_MS;
    while (got < sizeof(in)) {
        fd_set         rset;
        struct timeval tv;
        int64_t        left = deadline - now_ms();
        ssize_t        k;

        if (left <= 0)
            break;
        FD_ZERO(&rset);
        FD_SET(fd, &rset);
        tv.tv_sec  = (time_t)(left / 1000);
        tv.tv_usec = (suseconds_t)(left % 1000) * 1000;
        if (select(fd + 1, &rset, NULL, NULL, &tv) <= 0)
            break;
        k = recv(fd, in + got, sizeof(in) - got, 0);
        if (k <= 0) {
            printf("    server closed the connection%s\n", k < 0 ? " (reset)" : "");
            break;
        }
        got += (size_t)k;

        // Stop as soon as one whole message is here.
        if (got >= SBCP_HDR_LEN) {
            enum sbcp_wire w;
            unsigned       type;
            size_t         flen;

            if (sbcp_decode_header(in, &w, &type, &flen) < 0 || got >= flen)
                break;
        }
    }
    close(fd);

    if (got == 0) {
        printf("    no reply within %d s\n", WAIT_MS / 1000);
        return 0;
    }
    hexdump("reply", in, got);
    {
        enum sbcp_wire w;
        unsigned       type;
        size_t         flen;

        if (got >= SBCP_HDR_LEN && sbcp_decode_header(in, &w, &type, &flen) == 0 &&
            flen <= got) {
            describe(in, flen);
            ok = w == wire && (type == TYPE_ACK || type == TYPE_NAK ||
                               type == TYPE_FWD || type == TYPE_ONLINE);
        } else {
            char text[128];

            sanitize(in, got < 60 ? got : 60, text, sizeof(text));
            printf("    not SBCP; as text: \"%s\"\n", text);
        }
    }
    return ok;
}

int main(int argc, char **argv)
{
    char name[MAX_USERNAME_LEN + 1];
    int  net_ok, leg_ok;

    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: %s <host> <port> [username]\n", argv[0]);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);

    if (argc == 4)
        snprintf(name, sizeof(name), "%s", argv[3]);
    else
        snprintf(name, sizeof(name), "probe%ld", (long)getpid() % 100000);

    net_ok = probe(argv[1], argv[2], name, SBCP_WIRE_NET);
    // A different name, in case the server has not yet noticed the first one left.
    name[0] = 'q';
    leg_ok = probe(argv[1], argv[2], name, SBCP_WIRE_LEGACY);

    printf("\nVerdict: ");
    if (net_ok && !leg_ok)
        printf("the server speaks the spec's network-byte-order layout.\n"
               "         ./client works with it as is.\n");
    else if (leg_ok && !net_ok)
        printf("the server speaks the little-endian bit-field layout.\n"
               "         Talk to it with: SBCP_WIRE=legacy ./client <user> <host> <port>\n"
               "         (./server already answers such clients in their own layout).\n");
    else if (net_ok && leg_ok)
        printf("the server accepts both layouts (it detects the layout per client).\n");
    else
        printf("no SBCP answer in either layout; check host/port, or compare the\n"
               "         hex above with the header layout in the spec.\n");
    return net_ok || leg_ok ? 0 : 1;
}
