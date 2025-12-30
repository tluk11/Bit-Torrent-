#define BLOCK_SIZE 16384

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "torrent_parser.h"
#include "store_pieces.h"
#include "verify_pieces.h"
#include "outgoingMessages.h"
#include "file_writer.h"

void print_progress_if_needed(TorrentState *ts) {
    int total = ts->total_pieces;

    if (total <= 0) return;

    int completed = 0;
    for (int i = 0; i < total; i++) {
        if (ts->piece_complete[i]) {
            completed++;
        }
    }

    int percent = (completed * 100) / total;

    /* print only when crossing a new 10% point */
    if (percent >= ts->last_progress_shown + 10) {
        ts->last_progress_shown = (percent / 10) * 10;
        printf("[PROGRESS] %d%% complete (%d/%d pieces)\n",
               ts->last_progress_shown, completed, total);
    }
}

int init_piece_storage(TorrentState *ts) {
    if (!ts || !ts->meta) {
        return -1;
    }

    /* array of piece buffers */
    ts->pieces = calloc(ts->total_pieces, sizeof(PieceBuffer));
    if (!ts->pieces) {
        return -1;
    }

    /* setup each piece’s buffer */
    for (int i = 0; i < ts->total_pieces; i++) {
        PieceBuffer *pb = &ts->pieces[i];

        long piece_start = (long)i * ts->piece_length;
        int piece_len = ts->piece_length;

        /* handle final shorter piece */
        if (piece_start + piece_len > ts->meta->file_length) {
            piece_len = (int)(ts->meta->file_length - piece_start);
        }

        pb->length = piece_len;
        pb->num_blocks = (piece_len + BLOCK_SIZE - 1) / BLOCK_SIZE;
        pb->blocks_done = 0;
        pb->verified = false;

        /* allocate data buffer */
        pb->data = calloc(pb->length, 1);
        if (!pb->data) {
            free_piece_storage(ts);
            return -1;
        }

        /* track which blocks were requested */
        pb->block_requested = calloc(pb->num_blocks, sizeof(bool));
        if (!pb->block_requested) {
            fprintf(stderr, "[STORE] Failed to allocate request tracking for piece %d\n", i);
            free_piece_storage(ts);
            return -1;
        }

        /* track received blocks */
        pb->block_received = calloc(pb->num_blocks, sizeof(bool));
        if (!pb->block_received) {
            free_piece_storage(ts);
            return -1;
        }
    }

    printf("[STORE] Piece storage initialized for %d pieces\n", ts->total_pieces);
    return 0;
}

void free_piece_storage(TorrentState *ts) {
    if (!ts || !ts->pieces) return;

    for (int i = 0; i < ts->total_pieces; i++) {
        if (ts->pieces[i].data) {
            free(ts->pieces[i].data);
        }
        if (ts->pieces[i].block_received) {
            free(ts->pieces[i].block_received);
        }
        if (ts->pieces[i].block_requested) {
            free(ts->pieces[i].block_requested);
        }
    }

    free(ts->pieces);
    ts->pieces = NULL;
}

int store_received_block(TorrentState *ts, int index, int begin, unsigned char *data, int len) {

    if (!ts || !ts->pieces) return -1;
    if (index < 0 || index >= ts->total_pieces) return -1;

    PieceBuffer *pb = &ts->pieces[index];

    if (!pb->data || !pb->block_received) return -1;
    if (begin < 0 || len <= 0 || begin + len > pb->length) return -1;

    int block_idx = begin / BLOCK_SIZE;
    if (block_idx < 0 || block_idx >= pb->num_blocks) return -1;

    /* avoid counting the same block twice */
    bool first_time = !pb->block_received[block_idx];

    memcpy(pb->data + begin, data, len);

    pb->block_received[block_idx] = true;
    if (pb->block_requested)
        pb->block_requested[block_idx] = false;

    if (first_time)
        pb->blocks_done++;

    /* piece completed? */
    if (pb->blocks_done == pb->num_blocks) {

        printf("[STORE] All blocks received for piece %d. Verifying...\n", index);

        if (verify_piece(ts, index, pb->data, pb->length)) {

            ts->piece_complete[index] = true;
            pb->verified = true;

            if (file_writer_write_piece(ts, index, pb->data, pb->length) == 0)
                printf("[STORE] Piece %d written to disk\n", index);

            broadcast_have(ts, index);
            print_progress_if_needed(ts);

            /* update local bitfield */
            if (ts->my_bitfield) {
                int byte = index / 8;
                int bit  = 7 - (index % 8);
                ts->my_bitfield[byte] |= (1 << bit);
            }

        } else {
            printf("[STORE] Piece %d FAILED verification. Resetting.\n", index);

            memset(pb->data, 0, pb->length);
            memset(pb->block_received, 0, pb->num_blocks);
            if (pb->block_requested)
                memset(pb->block_requested, 0, pb->num_blocks);

            pb->blocks_done = 0;
            pb->verified = false;
        }
    }

    return 0;
}

int get_piece_block(TorrentState *ts, int index, int begin, int length, unsigned char *out) {
    if (!ts || !ts->pieces || !out) {
        return -1;
    }

    if (index < 0 || index >= ts->total_pieces) {
        return -1;
    }

    if (!ts->piece_complete[index]) {
        return -1;
    }

    PieceBuffer *pb = &ts->pieces[index];

    if (begin < 0 || length < 0 || begin + length > pb->length) {
        return -1;
    }

    memcpy(out, pb->data + begin, length);
    return 0;
}

bool is_piece_complete(TorrentState *ts, int index) {
    if (!ts || !ts->piece_complete) {
        return false;
    }

    if (index < 0 || index >= ts->total_pieces) {
        return false;
    }

    return ts->piece_complete[index];
}
