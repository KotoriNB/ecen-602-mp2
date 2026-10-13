#ifndef SBCP_H
#define SBCP_H

#include <stdint.h>

// Protocol parameters and limits
#define SBCP_VERSION 3

#define MAX_USERNAME_LEN 16
#define MAX_MESSAGE_LEN 512
#define MAX_REASON_LEN 32

// Message types (7 bits), mandatory
#define TYPE_JOIN 2 // client to server
#define TYPE_FWD 3 // server to client
#define TYPE_SEND 4 // client to server

// Message types, bonus
#define TYPE_NAK 5 // server to client
#define TYPE_OFFLINE 6 // server to client
#define TYPE_ACK 7 // server to client
#define TYPE_ONLINE 8 // server to client
#define TYPE_IDLE 9 // both directions

// Attribute types (16 bits)
#define ATTR_REASON 1 // 1 to 32 bytes: failure reason
#define ATTR_USERNAME 2 // 1 to 16 bytes: nickname
#define ATTR_CLIENT_COUNT 3 // 2 bytes: number of clients
#define ATTR_MESSAGE 4 // 1 to 512 bytes: chat text

// Message header, 4 bytes: 9-bit version and 7-bit type, then a 16-bit Length
struct sbcp_header {
    uint16_t vrsn_type; // version and type, network byte order
    uint16_t length; // bytes after this header (the attributes), network byte order
} __attribute__((packed));

// Attribute, 4 bytes plus its payload: 16-bit type, then a 16-bit Length
struct sbcp_attr {
    uint16_t type; // attribute type, network byte order
    uint16_t length; // payload bytes, header not counted, network byte order
    char payload[]; // the payload itself
} __attribute__((packed));

// Packing and unpacking the version and type field
#define MAKE_VRSN_TYPE(v, t) (uint16_t)((((v) & 0x01FF) << 7) | ((t) & 0x007F))
#define GET_VERSION(vt) (uint16_t)(((vt) >> 7) & 0x01FF)
#define GET_TYPE(vt) (uint16_t)((vt) & 0x007F)

#endif // SBCP_H
