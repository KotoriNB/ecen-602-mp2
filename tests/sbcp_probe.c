// tests/sbcp_probe.c: which message format does an SBCP server speak?
//
// Joins the server once per format: the campus format (network byte order,
// Length fields that leave out their own header, as ./client sends), Length
// fields that also count the header, and the little-endian bit-field layout
// some implementations send. Prints each exchange in hex with a verdict. Use
// it on the course's reference server or another team's server.
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
#define QUIET_MS 500

static const struct {
    enum sbcp_wire wire;
    const char *label;
} formats[] = {
    { SBCP_WIRE_NET, "campus format (Length leaves out the header)" },
    { SBCP_WIRE_HDRLEN, "Length counts the header" },
    { SBCP_WIRE_LEGACY, "little-endian bit-field layout" },
};

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

static void describe(const struct sbcp_msg *m)
{
    struct sbcp_field a;
    size_t pos = 0;
    char text[256];

    printf("    = %s", sbcp_type_name(m->type));
    while (sbcp_next_attr(m, &pos, &a)) {
        unsigned long count;

        if (a.type == ATTR_CLIENT_COUNT && sbcp_attr_count(&a, m->wire, &count) == 0) {
            printf(", count %lu", count);
        } else {
            const char *kind = "attr";

            if (a.type == ATTR_USERNAME)
                kind = "user";
            else if (a.type == ATTR_REASON)
                kind = "reason";
            else if (a.type == ATTR_MESSAGE)
                kind = "message";
            sanitize(a.val, a.len < 64 ? a.len : 64, text, sizeof(text));
            printf(", %s \"%s\"", kind, text);
        }
    }
    printf("\n");
}

// The first message of a reply, in whichever Length format it parses.
static int parse_reply(const uint8_t *in, size_t got, struct sbcp_msg *m)
{
    enum sbcp_wire w;
    unsigned type;
    size_t len;
    const char *why;

    if (got < SBCP_HDR_LEN || sbcp_decode_header(in, &w, &type, &len) < 0)
        return -1;
    if (got >= len + SBCP_HDR_LEN && sbcp_parse(in, len + SBCP_HDR_LEN, m, &why) == 0)
        return 0;
    if (got >= len && sbcp_parse(in, len, m, &why) == 0)
        return 0;
    return -1;
}

// Returns 1 if the server answered with ACK or NAK in the format it was sent.
static int probe(const char *host, const char *port, const char *name, int i)
{
    uint8_t out[64], in[4096];
    struct sbcp_builder b;
    struct sbcp_msg m;
    char peer[NET_ADDRSTRLEN], err[256];
    size_t n, got = 0;
    int fd;
    int64_t deadline, quiet = 0;

    printf("%s:\n", formats[i].label);
    if ((fd = net_connect(host, port, 5000, peer, sizeof(peer), err, sizeof(err))) < 0) {
        printf("    cannot connect: %s\n", err);
        return 0;
    }

    sbcp_begin(&b, out, sizeof(out), formats[i].wire, TYPE_JOIN);
    sbcp_add_attr(&b, ATTR_USERNAME, name, strlen(name));
    n = sbcp_finish(&b);
    hexdump("sent JOIN", out, n);
    if (net_send_all(fd, out, n) < 0) {
        printf("    send failed: %s\n", strerror(errno));
        close(fd);
        return 0;
    }

    // Everything until the server goes quiet, hangs up, or WAIT_MS passes.
    deadline = now_ms() + WAIT_MS;
    while (got < sizeof(in)) {
        fd_set rset;
        struct timeval tv;
        int64_t left = (quiet ? quiet : deadline) - now_ms();
        ssize_t k;

        if (left <= 0)
            break;
        FD_ZERO(&rset);
        FD_SET(fd, &rset);
        tv.tv_sec = (time_t)(left / 1000);
        tv.tv_usec = (suseconds_t)(left % 1000) * 1000;
        if (select(fd + 1, &rset, NULL, NULL, &tv) <= 0)
            break;
        k = recv(fd, in + got, sizeof(in) - got, 0);
        if (k <= 0) {
            printf("    server closed the connection%s\n", k < 0 ? " (reset)" : "");
            break;
        }
        got += (size_t)k;
        quiet = now_ms() + QUIET_MS;
    }
    close(fd);

    if (got == 0) {
        printf("    no reply\n");
        return 0;
    }
    hexdump("reply", in, got);
    if (parse_reply(in, got, &m) < 0) {
        char text[128];

        sanitize(in, got < 60 ? got : 60, text, sizeof(text));
        printf("    not SBCP; as text: \"%s\"\n", text);
        return 0;
    }
    describe(&m);
    return m.wire == formats[i].wire && (m.type == TYPE_ACK || m.type == TYPE_NAK);
}

int main(int argc, char **argv)
{
    char name[MAX_USERNAME_LEN + 1];
    int ok[3], i;

    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: %s <host> <port> [username]\n", argv[0]);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);

    for (i = 0; i < 3; i++) {
        // A different name each time, in case the server has not yet noticed the last one left.
        if (argc == 4)
            snprintf(name, sizeof(name), "%.14s_%c", argv[3], "123"[i]);
        else
            snprintf(name, sizeof(name), "probe%ld_%c", (long)getpid() % 100000, "123"[i]);
        ok[i] = probe(argv[1], argv[2], name, i);
        printf("\n");
    }

    printf("Verdict: ");
    if (ok[0])
        printf("the server speaks the campus format%s; ./client works with it as is.\n",
            ok[1] || ok[2] ? " (and answers the other formats too)" : "");
    else if (ok[1])
        printf("the server's Length fields count the header. ./server understands such\n"
            "         clients, but ./client sends the campus format.\n");
    else if (ok[2])
        printf("the server speaks the little-endian bit-field layout only.\n");
    else
        printf("no SBCP answer in any format; check host and port, or compare the\n"
            "         hex above with the message format in the handout.\n");
    return ok[0] || ok[1] || ok[2] ? 0 : 1;
}
