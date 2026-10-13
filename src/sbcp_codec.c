// sbcp_codec.c: implementation of sbcp_codec.h. Every field is packed and unpacked
// byte by byte with shifts, never through a C bit-field struct: bit-field
// layout is implementation-defined (C11 6.7.2.1), byte shifts are not.

#include "sbcp_codec.h"

#include <stdlib.h>
#include <string.h>

#define LEGACY(w) (((w) & SBCP_WIRE_LEGACY) != 0)
#define HDRLEN(w) (((w) & SBCP_WIRE_HDRLEN) != 0)
#define LENGTH_MAX 0xFFFFu

// 16-bit fields: big-endian on the NET layout, little-endian on LEGACY.
static unsigned get16(const uint8_t *p, enum sbcp_wire w)
{
    if (LEGACY(w))
        return (unsigned)p[0] | (unsigned)p[1] << 8;
    return (unsigned)p[0] << 8 | (unsigned)p[1];
}

static void put16(uint8_t *p, unsigned v, enum sbcp_wire w)
{
    if (LEGACY(w)) {
        p[0] = (uint8_t)(v & 0xFF);
        p[1] = (uint8_t)(v >> 8 & 0xFF);
    } else {
        p[0] = (uint8_t)(v >> 8 & 0xFF);
        p[1] = (uint8_t)(v & 0xFF);
    }
}

// NET: first 16 bits = Vrsn << 7 | Type, so version 3 always starts 0x01 then
// a byte with its top bit set.
// LEGACY: first 16 bits = Vrsn | Type << 9, little-endian, so version 3 always
// starts 0x03 then an even byte.
// The two can never be confused.
int sbcp_decode_header(const uint8_t *h, enum sbcp_wire *wire, unsigned *type, size_t *len)
{
    if (h[0] == 0x01 && (h[1] & 0x80) != 0) {
        *wire = SBCP_WIRE_NET;
        *type = h[1] & 0x7Fu;
        *len = get16(h + 2, SBCP_WIRE_NET);
        return 0;
    }
    if (h[0] == 0x03 && (h[1] & 0x01) == 0) {
        *wire = SBCP_WIRE_LEGACY;
        *type = h[1] >> 1;
        *len = get16(h + 2, SBCP_WIRE_LEGACY);
        return 0;
    }
    return -1;
}

static void put_header(uint8_t *h, enum sbcp_wire w, unsigned type, size_t len)
{
    if (LEGACY(w))
        put16(h, SBCP_VERSION | (type & 0x7Fu) << 9, w);
    else
        put16(h, SBCP_VERSION << 7 | (type & 0x7Fu), w);
    put16(h + 2, (unsigned)len, w);
}

void sbcp_begin(struct sbcp_builder *b, uint8_t *buf, size_t cap, enum sbcp_wire wire, unsigned type)
{
    size_t most = LENGTH_MAX + (HDRLEN(wire) ? 0 : SBCP_HDR_LEN);

    b->buf = buf;
    b->cap = cap < most ? cap : most;
    b->len = SBCP_HDR_LEN;
    b->wire = wire;
    b->type = type;
    b->overflow = cap < SBCP_HDR_LEN;
}

void sbcp_add_attr(struct sbcp_builder *b, unsigned type, const void *val, size_t len)
{
    size_t total = SBCP_ATTR_HDR_LEN + len;

    if (b->overflow || len > b->cap || total > b->cap - b->len) {
        b->overflow = 1;
        return;
    }
    put16(b->buf + b->len, type, b->wire);
    put16(b->buf + b->len + 2, (unsigned)(HDRLEN(b->wire) ? total : len), b->wire);
    if (len > 0)
        memcpy(b->buf + b->len + SBCP_ATTR_HDR_LEN, val, len);
    b->len += total;
}

void sbcp_add_u16(struct sbcp_builder *b, unsigned type, unsigned value)
{
    uint8_t v[2];

    put16(v, value > 0xFFFF ? 0xFFFF : value, b->wire);
    sbcp_add_attr(b, type, v, sizeof(v));
}

size_t sbcp_finish(struct sbcp_builder *b)
{
    if (b->overflow)
        return 0;
    put_header(b->buf, b->wire, b->type, HDRLEN(b->wire) ? b->len : b->len - SBCP_HDR_LEN);
    return b->len;
}

// Bytes an attribute takes up, from its raw Length field.
static size_t attr_span(size_t alen, enum sbcp_wire w)
{
    return HDRLEN(w) ? alen : SBCP_ATTR_HDR_LEN + alen;
}

int sbcp_parse(const uint8_t *f, size_t n, struct sbcp_msg *m, const char **why)
{
    size_t declared, pos;

    if (n < SBCP_HDR_LEN) {
        *why = "shorter than the 4-byte header";
        return -1;
    }
    if (sbcp_decode_header(f, &m->wire, &m->type, &declared) < 0) {
        *why = "not SBCP version 3";
        return -1;
    }
    if (declared + SBCP_HDR_LEN == n) {
        // the reference server's format
    } else if (declared == n) {
        m->wire = (enum sbcp_wire)(m->wire | SBCP_WIRE_HDRLEN);
    } else {
        *why = "Length field does not match the bytes received";
        return -1;
    }

    m->len = n;
    m->payload = f + SBCP_HDR_LEN;
    m->payload_len = n - SBCP_HDR_LEN;

    // Check every attribute once, so later lookups can trust the lengths.
    pos = 0;
    while (m->payload_len - pos >= SBCP_ATTR_HDR_LEN) {
        size_t alen = get16(m->payload + pos + 2, m->wire);

        if (HDRLEN(m->wire) && alen < SBCP_ATTR_HDR_LEN) {
            *why = "attribute Length smaller than its header";
            return -1;
        }
        if (attr_span(alen, m->wire) > m->payload_len - pos) {
            *why = "attribute runs past the end of the message";
            return -1;
        }
        pos += attr_span(alen, m->wire);
    }

    // 1-3 left over: tolerate zero padding, reject anything else.
    for (; pos < m->payload_len; pos++) {
        if (m->payload[pos] != 0) {
            *why = "stray bytes after the last attribute";
            return -1;
        }
    }
    return 0;
}

int sbcp_next_attr(const struct sbcp_msg *m, size_t *pos, struct sbcp_field *a)
{
    const uint8_t *p;
    size_t alen;

    if (*pos > m->payload_len || m->payload_len - *pos < SBCP_ATTR_HDR_LEN)
        return 0;

    p = m->payload + *pos;
    alen = get16(p + 2, m->wire);
    if ((HDRLEN(m->wire) && alen < SBCP_ATTR_HDR_LEN) || attr_span(alen, m->wire) > m->payload_len - *pos)
        return 0;

    a->type = get16(p, m->wire);
    a->len = attr_span(alen, m->wire) - SBCP_ATTR_HDR_LEN;
    a->val = p + SBCP_ATTR_HDR_LEN;
    *pos += attr_span(alen, m->wire);
    return 1;
}

int sbcp_find_attr(const struct sbcp_msg *m, unsigned type, struct sbcp_field *a)
{
    size_t pos = 0;

    while (sbcp_next_attr(m, &pos, a))
        if (a->type == type)
            return 1;
    return 0;
}

int sbcp_attr_text(const struct sbcp_field *a, char *out, size_t outsz, size_t maxlen)
{
    size_t n = a->len;

    while (n > 0 && a->val[n - 1] == '\0')
        n--;
    if (n == 0 || n > maxlen || n >= outsz)
        return -1;

    memcpy(out, a->val, n);
    out[n] = '\0';
    return (int)n;
}

int sbcp_attr_count(const struct sbcp_field *a, enum sbcp_wire wire, unsigned long *count)
{
    switch (a->len) {
        case 1:
            *count = a->val[0];
            return 0;
        case 2:
            *count = get16(a->val, wire);
            return 0;
        case 4:
            if (LEGACY(wire))
                *count = (unsigned long)get16(a->val + 2, wire) << 16 | get16(a->val, wire);
            else
                *count = (unsigned long)get16(a->val, wire) << 16 | get16(a->val + 2, wire);
            return 0;
        default:
            return -1;
    }
}

int sbcp_username_ok(const char *name, size_t len)
{
    size_t i;

    if (len < 1 || len > MAX_USERNAME_LEN)
        return 0;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];

        if (c < 0x20 || c == 0x7F)
            return 0;
    }
    return 1;
}

const char *sbcp_type_name(unsigned type)
{
    switch (type) {
        case TYPE_JOIN:
            return "JOIN";
        case TYPE_FWD:
            return "FWD";
        case TYPE_SEND:
            return "SEND";
        case TYPE_NAK:
            return "NAK";
        case TYPE_OFFLINE:
            return "OFFLINE";
        case TYPE_ACK:
            return "ACK";
        case TYPE_ONLINE:
            return "ONLINE";
        case TYPE_IDLE:
            return "IDLE";
        default:
            return "unknown";
    }
}

// Stream reassembly

int sbcp_reader_init(struct sbcp_reader *r, size_t cap)
{
    memset(r, 0, sizeof(*r));
    if (cap < SBCP_HDR_LEN)
        cap = SBCP_HDR_LEN;
    if ((r->buf = malloc(cap)) == NULL)
        return -1;
    r->cap = cap;
    r->since_ms = -1;
    r->hdrlen = -1;
    return 0;
}

void sbcp_reader_expect(struct sbcp_reader *r, enum sbcp_wire w)
{
    r->hdrlen = HDRLEN(w);
}

void sbcp_reader_free(struct sbcp_reader *r)
{
    free(r->buf);
    r->buf = NULL;
    r->cap = r->start = r->end = r->skip = 0;
}

// Keeps since_ms equal to the arrival time of the oldest unfinished message.
static void touch(struct sbcp_reader *r, int64_t now)
{
    if (r->skip == 0 && r->end == r->start)
        r->since_ms = -1;
    else if (r->since_ms < 0)
        r->since_ms = now;
}

uint8_t *sbcp_reader_space(struct sbcp_reader *r, size_t *room)
{
    if (r->start > 0) {
        memmove(r->buf, r->buf + r->start, r->end - r->start);
        r->end -= r->start;
        r->start = 0;
    }
    *room = r->cap - r->end;
    return r->buf + r->end;
}

void sbcp_reader_commit(struct sbcp_reader *r, size_t n, int64_t now)
{
    if (n > r->cap - r->end)
        n = r->cap - r->end;
    r->end += n;
    r->last_ms = now;

    // Still inside an oversized message: throw its bytes away as they come.
    if (r->skip > 0) {
        size_t k = r->end - r->start;

        if (k > r->skip)
            k = r->skip;
        r->start += k;
        r->skip -= k;
        // the next message starts fresh
        if (r->skip == 0)
            r->since_ms = -1;
    }
    touch(r, now);
}

// Attributes that exactly fill n bytes, with Length fields in either format.
// In the HDRLEN format each must also carry at least one payload byte here.
static int attrs_fill(const uint8_t *q, size_t n, enum sbcp_wire w)
{
    size_t pos = 0;

    while (n - pos >= SBCP_ATTR_HDR_LEN) {
        size_t alen = get16(q + pos + 2, w);

        if ((HDRLEN(w) && alen <= SBCP_ATTR_HDR_LEN) || attr_span(alen, w) > n - pos)
            return 0;
        pos += attr_span(alen, w);
    }
    return pos == n;
}

static int starts_message(const uint8_t *q, enum sbcp_wire layout)
{
    enum sbcp_wire w;
    unsigned type;
    size_t len;

    return sbcp_decode_header(q, &w, &type, &len) == 0 && w == layout && type >= TYPE_JOIN && type <= TYPE_IDLE;
}

// The first message tells how this peer counts Length: L bytes in all if it
// counts the header, L + 4 if not. Returns 1 or 0, or -1 while it cannot tell
// yet. After a whole L-byte message come the next message's header or nothing;
// inside an (L + 4)-byte one come payload bytes, which never look like a header.
static int guess_hdrlen(const struct sbcp_reader *r, size_t L, enum sbcp_wire layout, int64_t now)
{
    const uint8_t *p = r->buf + r->start;
    size_t avail = r->end - r->start;
    int whole;

    if (L < SBCP_HDR_LEN || L + SBCP_HDR_LEN > r->cap)
        return 0;
    if (avail < L)
        return -1;
    whole = attrs_fill(p + SBCP_HDR_LEN, L - SBCP_HDR_LEN, (enum sbcp_wire)(layout | SBCP_WIRE_HDRLEN));
    if (avail >= L + SBCP_HDR_LEN) {
        if (whole && starts_message(p + L, layout))
            return 1;
        if (attrs_fill(p + SBCP_HDR_LEN, L, layout))
            return 0;
        return whole;
    }
    if (!whole)
        return -1;
    if (avail > L)
        return p[L] == p[0] ? 1 : -1;
    return now - r->last_ms >= SBCP_GUESS_MS ? 1 : -1;
}

int sbcp_reader_guessing(const struct sbcp_reader *r)
{
    enum sbcp_wire wire;
    unsigned type;
    size_t L;

    return r->hdrlen < 0 && r->skip == 0 && r->end - r->start >= SBCP_HDR_LEN &&
        sbcp_decode_header(r->buf + r->start, &wire, &type, &L) == 0 && guess_hdrlen(r, L, wire, r->last_ms) < 0 &&
        guess_hdrlen(r, L, wire, r->last_ms + SBCP_GUESS_MS) == 1;
}

enum sbcp_rd sbcp_reader_next(struct sbcp_reader *r, const uint8_t **frame, size_t *len, const char **why, int64_t now)
{
    size_t avail = r->end - r->start;
    enum sbcp_wire wire;
    unsigned type;
    size_t flen;

    if (r->skip > 0 || avail < SBCP_HDR_LEN) {
        touch(r, now);
        return SBCP_RD_MORE;
    }

    // Once a header is bad there is no way to find the next message boundary,
    // so everything buffered is dropped.
    if (sbcp_decode_header(r->buf + r->start, &wire, &type, &flen) < 0) {
        r->start = r->end;
        r->since_ms = -1;
        *why = "not an SBCP version 3 header; dropped the buffered bytes";
        return SBCP_RD_JUNK;
    }
    if (r->hdrlen < 0) {
        int g = guess_hdrlen(r, flen, wire, now);

        if (g < 0) {
            touch(r, now);
            return SBCP_RD_MORE;
        }
        r->hdrlen = g;
    }
    if (r->hdrlen && flen < SBCP_HDR_LEN) {
        r->start = r->end;
        r->since_ms = -1;
        *why = "Length field smaller than the header; dropped the buffered bytes";
        return SBCP_RD_JUNK;
    }
    if (!r->hdrlen)
        flen += SBCP_HDR_LEN;

    // Too big to be valid here, but correctly framed: skip exactly its bytes
    // so the message after it is still found.
    if (flen > r->cap) {
        r->skip = flen - avail;
        r->start = r->end;
        r->since_ms = now;
        *why = "message longer than this side accepts; skipping it";
        return SBCP_RD_JUNK;
    }

    if (avail < flen) {
        touch(r, now);
        return SBCP_RD_MORE;
    }

    *frame = r->buf + r->start;
    *len = flen;
    r->start += flen;

    // Whatever follows arrived with this recv(), so its clock starts now.
    r->since_ms = -1;
    touch(r, now);
    return SBCP_RD_FRAME;
}

int sbcp_reader_pending(const struct sbcp_reader *r)
{
    return r->skip > 0 || r->end > r->start;
}

size_t sbcp_reader_drop(struct sbcp_reader *r)
{
    size_t dropped = r->end - r->start + r->skip;

    r->start = r->end = 0;
    r->skip = 0;
    r->since_ms = -1;
    return dropped;
}
