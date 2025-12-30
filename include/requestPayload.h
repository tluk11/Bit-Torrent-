#ifndef REQUEST_PAYLOAD_H
#define REQUEST_PAYLOAD_H

#include <stdint.h>

typedef struct {
    uint32_t index;   // piece index
    uint32_t begin;   // byte offset within the piece
    uint32_t length;  // number of bytes requested
} RequestPayload;

#endif
