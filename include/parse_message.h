#ifndef PARSE_MESSAGE_H
#define PARSE_MESSAGE_H

#include <stdint.h>
#include "contact_tracker.h"     // for Peer type
#include "torrent_parser.h"      // for TorrentState type

// Message IDs
typedef enum {
    MSG_CHOKE = 0,
    MSG_UNCHOKE = 1,
    MSG_INTERESTED = 2,
    MSG_NOT_INTERESTED = 3,
    MSG_HAVE = 4,
    MSG_BITFIELD = 5,
    MSG_REQUEST = 6,
    MSG_PIECE = 7,
    MSG_CANCEL = 8,
    MSG_KEEP_ALIVE = 255
} MessageId;

// Generic parsed message
typedef struct {
    MessageId id;
    uint32_t payload_len;
    unsigned char *payload;
} ParsedMessage;

// PIECE payload structure
typedef struct {
    uint32_t index;
    uint32_t begin;
    unsigned char *data;
    uint32_t data_len;
} PiecePayload;

// Functions implemented in parse_message.c
int parse_message(unsigned char *buffer, ParsedMessage *out_msg);
int parse_piece_payload(ParsedMessage *msg, PiecePayload *out_piece);

// Optional: handler function if needed by test
void handle_peer_communication(Peer *peer, TorrentState *ts);

#endif
