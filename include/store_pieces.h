#ifndef STORE_PIECES_H
#define STORE_PIECES_H

#include <stdbool.h>

//
// Buffer for each piece being downloaded
//
typedef struct PieceBuffer {
    unsigned char *data;
    int length;
    int num_blocks;
    bool *block_received;
    bool *block_requested;
    int blocks_done;
    bool verified;
} PieceBuffer;

struct TorrentState;
typedef struct TorrentState TorrentState;

int init_piece_storage(TorrentState *ts);
void free_piece_storage(TorrentState *ts);
int store_received_block(TorrentState *ts, int index, int begin, unsigned char *data, int len);
int get_piece_block(TorrentState *ts, int index, int begin, int length, unsigned char *out);
bool is_piece_complete(TorrentState *ts, int index);

#endif
