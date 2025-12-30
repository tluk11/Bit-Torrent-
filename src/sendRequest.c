#define BLOCK_SIZE 16384
#include <stdbool.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdio.h>
#include "torrent_parser.h"

static inline bool we_have_piece(TorrentState *ts, int index) {
    return ts->piece_complete[index];
}

bool peer_has_piece(Peer *peer, int index) {
    if (!peer->bitfield) return false;

    int byte = index / 8;
    int bit  = 7 - (index % 8);

    if (byte >= peer->bitfield_len) return false;

    return (peer->bitfield[byte] & (1 << bit)) != 0;
}

int send_request(Peer *peer, int index, int begin, int length) {
    uint32_t payload_len = htonl(13);
    uint32_t net_index   = htonl(index);
    uint32_t net_begin   = htonl(begin);
    uint32_t net_length  = htonl(length);

    unsigned char msg[17];
    memcpy(msg, &payload_len, 4);
    msg[4] = 6;  /* request message ID */
    memcpy(msg + 5,  &net_index, 4);
    memcpy(msg + 9,  &net_begin, 4);
    memcpy(msg + 13, &net_length, 4);

    int sent = send(peer->socket_fd, msg, sizeof(msg), 0);
    return (sent == (int)sizeof(msg)) ? 0 : -1;
}

int request_multiple_blocks(Peer *peer, TorrentState *ts) {
    if (!peer || peer->socket_fd < 0) return -1;
    if (peer->is_choked) return -1;

    int requests_sent = 0;

    /* issue requests until pipeline is full */
    while (peer->outstanding_requests < peer->max_pipeline) {

        int selected_piece = -1;
        int selected_block = -1;

        /* search all pieces for something useful to request */
        for (int p = 0; p < ts->total_pieces; p++) {

            if (ts->piece_complete[p])
                continue;
            if (!peer_has_piece(peer, p))
                continue;

            PieceBuffer *pb = &ts->pieces[p];
            if (!pb || !pb->data || !pb->block_received)
                continue;

            /* allocate block_requested array if needed */
            if (!pb->block_requested) {
                pb->block_requested = calloc(pb->num_blocks, 1);
                if (!pb->block_requested) continue;
            }

            /* find the first block this peer can help with */
            for (int b = 0; b < pb->num_blocks; b++) {
                if (!pb->block_received[b] && !pb->block_requested[b]) {
                    selected_piece = p;
                    selected_block = b;
                    goto GOT_BLOCK;
                }
            }
        }

GOT_BLOCK:
        if (selected_piece == -1)
            break;  /* nothing available */

        PieceBuffer *pb = &ts->pieces[selected_piece];

        int offset = selected_block * BLOCK_SIZE;
        int remaining = pb->length - offset;
        if (remaining <= 0)
            continue;

        int request_len = (remaining >= BLOCK_SIZE) ? BLOCK_SIZE : remaining;

        /* mark block as requested before sending */
        pb->block_requested[selected_block] = true;

        if (send_request(peer, selected_piece, offset, request_len) < 0) {
            pb->block_requested[selected_block] = false;
            return requests_sent;
        }

        peer->outstanding_requests++;
        requests_sent++;
    }

    return requests_sent;
}

int request_next_block(Peer *peer, TorrentState *ts) {
    return request_multiple_blocks(peer, ts);
}
