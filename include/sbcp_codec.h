// sbcp_codec.h: the server's SBCP message builder, validator and TCP stream
// reassembler. Message and attribute numbers and the length limits come from
// sbcp.h, the protocol header shared with the client.
//
// Every field is packed and unpacked with byte shifts in network byte order,
// never by casting a buffer to a struct, so alignment and padding never matter.
// Length fields count the bytes after their own 4-byte header, as the course's
// reference server does: message Length = all attributes, attribute Length =
// its payload. Peers whose Length fields also count the header are understood
// too (SBCP_WIRE_HDRLEN). Strings are sent without a terminating NUL; a
// trailing NUL from a peer is tolerated and stripped.

#ifndef SBCP_CODEC_H
#define SBCP_CODEC_H

#include <stddef.h>
#include <stdint.h>

#include "sbcp.h"

#define SBCP_HDR_LEN 4
#define SBCP_ATTR_HDR_LEN 4
#define SBCP_FRAME_MAX 65539 // 16-bit Length plus the header it does not count
#define SBCP_GUESS_MS 250 // see sbcp_reader_guessing()

// How a peer writes messages. NET is the spec's byte order with Length
// fields that leave out their own header; it is what we send by default and
// what the course's reference server speaks. LEGACY is what a C bit-field
// struct {vrsn:9; type:7; length:16} looks like when an x86 program write()s
// it without byte-order conversion. HDRLEN is a flag for peers whose Length
// fields also count the 4-byte header (another reading of the handout).
// Accepting these lets such peers interoperate with us; we answer in kind.
enum sbcp_wire {
    SBCP_WIRE_NET = 0,
    SBCP_WIRE_LEGACY = 1,
    SBCP_WIRE_HDRLEN = 2
};

// Building messages

struct sbcp_builder {
    uint8_t *buf;
    size_t cap;
    size_t len;
    enum sbcp_wire wire;
    unsigned type;
    int overflow; // set if anything did not fit
};

void sbcp_begin(struct sbcp_builder *b, uint8_t *buf, size_t cap, enum sbcp_wire wire, unsigned type);
void sbcp_add_attr(struct sbcp_builder *b, unsigned type, const void *val, size_t len);
void sbcp_add_u16(struct sbcp_builder *b, unsigned type, unsigned value);
// Writes the header. Returns the message length, or 0 if it overflowed.
size_t sbcp_finish(struct sbcp_builder *b);

// Validating and reading messages

struct sbcp_msg {
    enum sbcp_wire wire;
    unsigned type;
    size_t len; // whole message, header included
    const uint8_t *payload; // the attributes
    size_t payload_len;
};

struct sbcp_field {
    unsigned type;
    size_t len; // payload bytes, attribute header excluded
    const uint8_t *val;
};

// Decodes a 4-byte header in either byte layout; *len is the raw Length
// field. Returns 0, or -1 if the version is not 3 (which also rejects text,
// HTTP and other garbage).
int sbcp_decode_header(const uint8_t *h, enum sbcp_wire *wire, unsigned *type, size_t *len);

// Validates one complete n-byte message: header, Length (n - 4, or n for an
// HDRLEN peer; m->wire says which), and every attribute inside the message.
// Returns 0, or -1 with *why describing the defect.
int sbcp_parse(const uint8_t *frame, size_t n, struct sbcp_msg *m, const char **why);

// Attribute iteration over a validated message; *pos starts at 0.
int sbcp_next_attr(const struct sbcp_msg *m, size_t *pos, struct sbcp_field *a);

// First attribute of the given type. Returns 1 if found, 0 if absent.
int sbcp_find_attr(const struct sbcp_msg *m, unsigned type, struct sbcp_field *a);

// Text payload with trailing NULs stripped, copied to out and NUL-terminated.
// Returns its length (embedded NULs possible), or -1 if empty, longer than
// maxlen, or too big for out.
int sbcp_attr_text(const struct sbcp_field *a, char *out, size_t outsz, size_t maxlen);

// Client Count payload. Returns 0, or -1 if it is not 1, 2 or 4 bytes.
int sbcp_attr_count(const struct sbcp_field *a, enum sbcp_wire wire, unsigned long *count);

// 1-16 bytes and no ASCII control characters.
int sbcp_username_ok(const char *name, size_t len);

const char *sbcp_type_name(unsigned type);

// Reassembling messages from a TCP byte stream
//
// TCP delivers bytes, not messages: one recv() may hold half a message or
// several. Usage per readable socket:
//
// p = sbcp_reader_space(&rd, &room);
// n = recv(fd, p, room, 0);
// sbcp_reader_commit(&rd, n, now);
// while ((r = sbcp_reader_next(&rd, &f, &flen, &why, now)) != SBCP_RD_MORE) {
//     if (r == SBCP_RD_FRAME)
//         handle(f, flen);
//     else
//         log(why);
// }
//
// A frame pointer stays valid until the next sbcp_reader_space() call.

struct sbcp_reader {
    uint8_t *buf;
    size_t cap; // largest message this side accepts
    size_t start, end; // unread bytes are buf[start, end)
    size_t skip; // bytes of an oversized message left to drop
    int64_t since_ms; // when the pending partial message began, -1 if none
    int64_t last_ms; // when bytes last arrived
    int hdrlen; // peer's Length fields count the header: 1, 0, -1 unknown
};

enum sbcp_rd {
    SBCP_RD_MORE, // need more bytes
    SBCP_RD_FRAME, // one complete message
    SBCP_RD_JUNK // bytes were discarded, see *why
};

int sbcp_reader_init(struct sbcp_reader *r, size_t cap);
void sbcp_reader_free(struct sbcp_reader *r);
// Fixes the peer's Length format (the HDRLEN bit of w) instead of learning it
// from the first message.
void sbcp_reader_expect(struct sbcp_reader *r, enum sbcp_wire w);
// Nonzero while the first message is complete in one format but could still
// be the start of a longer message in the other. sbcp_reader_next() settles
// it when more bytes arrive, or after SBCP_GUESS_MS without any.
int sbcp_reader_guessing(const struct sbcp_reader *r);
uint8_t *sbcp_reader_space(struct sbcp_reader *r, size_t *room);
void sbcp_reader_commit(struct sbcp_reader *r, size_t n, int64_t now);
enum sbcp_rd sbcp_reader_next(struct sbcp_reader *r, const uint8_t **frame, size_t *len, const char **why, int64_t now);
// Nonzero while part of a message is buffered (or being skipped).
int sbcp_reader_pending(const struct sbcp_reader *r);
// Drops a partial message that never completed. Returns bytes dropped.
size_t sbcp_reader_drop(struct sbcp_reader *r);

#endif // SBCP_CODEC_H
