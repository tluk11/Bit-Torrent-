#ifndef SEND_REQUEST_H
#define SEND_REQUEST_H

#include <stdint.h>
#include <stdbool.h>
#include "contact_tracker.h"
#include "torrent_parser.h"

static inline bool we_have_piece(TorrentState *ts, int index) {
    return ts->piece_complete[index];
}



static inline bool peer_has_piece(Peer *peer, int index) {
    if (!peer->bitfield) return false;
    int byte = index / 8;
    int bit  = 7 - (index % 8);
    if (byte >= peer->bitfield_len) return false;
    return (peer->bitfield[byte] & (1 << bit)) != 0;
}
/**
 * Sends a REQUEST message to a peer.
 * Returns 0 on success, -1 on failure.
 */
int send_request(Peer *peer, int index, int begin, int length);

/**
 * Chooses the next block to request and sends a REQUEST message.
 * Returns 0 on success, -1 on failure or no interesting pieces.
 */
int request_next_block(Peer *peer, TorrentState *ts);

/**
 * Fill the pipeline with multiple block requests (up to MAX_PIPELINE).
 * Returns number of blocks requested.
 */
int request_pipeline_blocks(Peer *peer, TorrentState *ts);

#endif // SEND_REQUEST_H