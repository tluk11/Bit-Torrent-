#ifndef TORRENT_PARSER_H
#define TORRENT_PARSER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "store_pieces.h"

#define BLOCK_SIZE 16384

typedef struct {
    int total_blocks;
    int received_blocks;
    uint8_t *have_block;       // 1 = already downloaded
    uint8_t *requested_block;  // 1 = requested but not received yet
} PieceState;
// Forward declare PieceBuffer
struct PieceBuffer;
typedef struct PieceBuffer PieceBuffer;

// Include peer definition (from contact_tracker.h)
#include "contact_tracker.h"

//
// Torrent metadata parsed from .torrent file
//
typedef struct TorrentInfo {
    char **trackers;
    int num_trackers;

    int piece_length;
    unsigned char *pieces;
    int num_pieces;

    char *name;
    long file_length;

    unsigned char info_hash[20];
} TorrentInfo;


//
// Runtime state for entire torrent
//
typedef struct TorrentState {
    int last_progress_shown;   // 0–100%

    TorrentInfo *meta;

    bool skip_tracker;

    Peer **peers;
    int peer_count;
    int peer_capacity;

    uint8_t *my_bitfield;
    int my_bitfield_len;

    bool *piece_complete;      
    int *piece_bytes_have;

    unsigned char **piece_data;

    int total_pieces;
    int piece_length;
    PieceState *piece_states;

    const unsigned char *client_id;

    PieceBuffer *pieces;

    FILE *output_file; 

    double download_start_time;   // timestamp when download began
    long bytes_downloaded;        // total bytes downloaded so far
    long bytes_uploaded;
    double last_speed_print;      // last time we printed speed

    bool is_seeding;             
    bool download_announced;     

    int listen_fd;
    int listen_port;
    
} TorrentState;

int torrentparser(const char *path, TorrentInfo *ti);
void torrent_info_free(TorrentInfo *ti);

#endif
