#ifndef OUTGOING_MESSAGES_H
#define OUTGOING_MESSAGES_H

#include <stdint.h>
#include "torrent_parser.h"   // for Peer and TorrentState

// ----------------------
// Basic Protocol Messages
// ----------------------

/**
 * KEEP-ALIVE
 * length = 0 (no ID)
 */
int send_keepalive(Peer *peer);

/**
 * CHOKE message
 * length = 1, id = 0
 */
int send_choke(Peer *peer);

/**
 * UNCHOKE message
 * length = 1, id = 1
 */
int send_unchoke(Peer *peer);

/**
 * INTERESTED message
 * length = 1, id = 2
 */
int send_interested(Peer *peer);

/**
 * NOT_INTERESTED message
 * length = 1, id = 3
 */
int send_not_interested(Peer *peer);

/**
 * HAVE message
 * length = 5, id = 4
 * payload: 4-byte piece index
 */
int send_have(Peer *peer, uint32_t piece_index);

/**
 * BITFIELD message
 * length = 1 + bitfield_size, id = 5
 * payload: raw bitfield bytes
 */
int send_bitfield(Peer *peer, TorrentState *ts);

// --------------------------------------------------
// Uploading messages (PIECE) - called by send_piece()
// --------------------------------------------------

/**
 * PIECE message
 * length = 9 + block_length
 * id = 7
 * payload:
 *   index   (4 bytes)
 *   begin   (4 bytes)
 *   block   (variable)
 */
int send_piece(Peer *peer, TorrentState *ts,
               int index, int begin, int length);

// --------------------------------------------------
// For broadcasting HAVE messages to all connected peers
// --------------------------------------------------
int broadcast_have(TorrentState *ts, int piece_index);

#endif