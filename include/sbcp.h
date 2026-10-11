#ifndef SBCP_H
#define SBCP_H

#include <stdint.h>

/* 1. Protocol Parameters and Limits */
#define SBCP_VERSION        3

#define MAX_USERNAME_LEN    16
#define MAX_MESSAGE_LEN     512
#define MAX_REASON_LEN      32

/* 2. Message Types (7 bits) */
/* Mandatory */
#define TYPE_JOIN           2   /* Client -> Server */
#define TYPE_FWD            3   /* Server -> Client */
#define TYPE_SEND           4   /* Client -> Server */

/* Bonus */
#define TYPE_NAK            5   /* Server -> Client */
#define TYPE_OFFLINE        6   /* Server -> Client */
#define TYPE_ACK            7   /* Server -> Client */
#define TYPE_ONLINE         8   /* Server -> Client */
#define TYPE_IDLE           9   /* Client <-> Server */

/* 3. Attribute Types (16 bits) */
#define ATTR_REASON         1   /* 1-32 bytes: failure reason */
#define ATTR_USERNAME       2   /* 1-16 bytes: nickname */
#define ATTR_CLIENT_COUNT   3   /* 2 bytes: number of clients */
#define ATTR_MESSAGE        4   /* 1-512 bytes: chat text */

/* 4. Packet Data Structures */
/*
 * SBCP Message Header (Total 4 Bytes)
 * 0                   1                   2                   3
 * 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |  Vrsn(9 bits) | Type (7 bits) |        Length (2 bytes)       |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
struct sbcp_header {
    uint16_t vrsn_type;  /* 9-bit Version + 7-bit Type (Network Byte Order) */
    uint16_t length;     /* Total message length in bytes including header */
} __attribute__((packed));

/*
 * SBCP Attribute Header (Total 4 Bytes + Payload)
 * 0                   1                   2                   3
 * 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |          Type (2 bytes)       |        Length (2 bytes)       |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                       Payload (variable)                      |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
struct sbcp_attr {
    uint16_t type;       /* Attribute type (Network Byte Order) */
    uint16_t length;     /* Total attribute length in bytes including header */
    char payload[];      /* Flexible array member pointing to payload data */
} __attribute__((packed));

/* 5. Helper Macros for Bitwise Operations */
#define MAKE_VRSN_TYPE(v, t)  (uint16_t)((((v) & 0x01FF) << 7) | ((t) & 0x007F))
#define GET_VERSION(vt)       (uint16_t)(((vt) >> 7) & 0x01FF)
#define GET_TYPE(vt)          (uint16_t)((vt) & 0x007F)

#endif /* SBCP_H */