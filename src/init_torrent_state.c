#include "init_torrent_state.h"
#include "store_pieces.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/time.h>

double get_time_seconds() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}
int init_torrent_state(TorrentState *ts, TorrentInfo *ti, int port) {
    memset(ts, 0, sizeof(TorrentState));
    ts->skip_tracker = false;
    ts->meta = ti;
    ts->total_pieces = ti->num_pieces;
    ts->piece_length = ti->piece_length;
    ts->is_seeding = false;
    ts->download_announced = false;
    ts->last_progress_shown = 0;
    ts->listen_fd = -1;
    ts->listen_port = port;   

    ts->piece_states = calloc(ts->total_pieces, sizeof(PieceState));

    for (int i = 0; i < ts->total_pieces; i++) {
        int piece_size = ts->piece_length;
        if (i == ts->total_pieces - 1) {
            long last = (long)(ts->total_pieces - 1) * ts->piece_length;
            piece_size = ts->meta->file_length - last;
        }

        int blocks = (piece_size + BLOCK_SIZE - 1) / BLOCK_SIZE;

        PieceState *ps = &ts->piece_states[i];
        ps->total_blocks = blocks;
        ps->received_blocks = 0;

        ps->have_block = calloc(blocks, 1);
        ps->requested_block = calloc(blocks, 1);
    }

    // Allocate piece tracking arrays
    ts->piece_complete = calloc(ts->total_pieces, sizeof(bool));
    ts->piece_bytes_have = calloc(ts->total_pieces, sizeof(int));
    
    if (!ts->piece_complete || !ts->piece_bytes_have) {
        fprintf(stderr, "[INIT] Failed to allocate piece tracking arrays\n");
        goto error;
    }
    
    // Initialize piece storage using store_pieces.c functions
    if (init_piece_storage(ts) != 0) {
        fprintf(stderr, "[INIT] Failed to initialize piece storage\n");
        goto error;
    }
    
    // Initialize bitfield
    ts->my_bitfield_len = (ts->total_pieces + 7) / 8;
    ts->my_bitfield = calloc(ts->my_bitfield_len, 1);  
    if (!ts->my_bitfield) {
        fprintf(stderr, "Failed to allocate my_bitfield\n");
        return -1;
    }
    
    // Initialize peer management
    ts->peer_capacity = 50;  
    ts->peer_count = 0;
    ts->peers = calloc(ts->peer_capacity, sizeof(Peer *));
    if (!ts->peers) {
        fprintf(stderr, "[INIT] Failed to allocate peer array\n");
        goto error;
    }
    
    // Open output file
    ts->output_file = fopen(ti->name, "wb");
    if (!ts->output_file) {
        fprintf(stderr, "[INIT] Failed to open output file: %s\n", ti->name);
        perror("fopen");
        goto error;
    }
    
    // Pre-allocate file space
    if (fseek(ts->output_file, ti->file_length - 1, SEEK_SET) == 0) {
        fputc(0, ts->output_file);
        fseek(ts->output_file, 0, SEEK_SET);
    }
    
    printf("[INIT] TorrentState initialized:\n");
    printf("  - Total pieces: %d\n", ts->total_pieces);
    printf("  - Piece length: %d bytes\n", ts->piece_length);
    printf("  - File length: %ld bytes\n", ti->file_length);
    printf("  - Output file: %s\n", ti->name);
    
    return 0;

error:
    cleanup_torrent_state(ts);
    return -1;
}

void cleanup_torrent_state(TorrentState *ts) {
    if (!ts) return;
    
    // Free piece storage
    free_piece_storage(ts);
    
    // Free piece tracking arrays
    if (ts->piece_complete) {
        free(ts->piece_complete);
        ts->piece_complete = NULL;
    }

    
    if (ts->piece_bytes_have) {
        free(ts->piece_bytes_have);
        ts->piece_bytes_have = NULL;
    }
    
    if (ts->my_bitfield) {
        free(ts->my_bitfield);
        ts->my_bitfield = NULL;
    }
    
    // Free peers
    if (ts->peers) {
        for (int i = 0; i < ts->peer_count; i++) {
            if (ts->peers[i]) {
                if (ts->peers[i]->socket_fd >= 0) {
                    close(ts->peers[i]->socket_fd);
                }
                if (ts->peers[i]->bitfield) {
                    free(ts->peers[i]->bitfield);
                }
                free(ts->peers[i]);
            }
        }
        free(ts->peers);
        ts->peers = NULL;
    }
    if (ts->listen_fd >= 0) {
        close(ts->listen_fd);
        ts->listen_fd = -1;
    }

    // Close output file
    if (ts->output_file) {
        fclose(ts->output_file);
        ts->output_file = NULL;
    }
    
    memset(ts, 0, sizeof(TorrentState));
}

bool is_download_complete(TorrentState *ts) {
    if (!ts || !ts->piece_complete) {
        return false;
    }
    
    for (int i = 0; i < ts->total_pieces; i++) {
        if (!ts->piece_complete[i]) {
            return false;
        }
    }
    return true;
}

float get_download_progress(TorrentState *ts) {
    if (!ts || !ts->piece_complete || ts->total_pieces == 0) {
        return 0.0f;
    }
    
    int complete_count = 0;
    for (int i = 0; i < ts->total_pieces; i++) {
        if (ts->piece_complete[i]) {
            complete_count++;
        }
    }
    return (float)complete_count / ts->total_pieces * 100.0f;
}