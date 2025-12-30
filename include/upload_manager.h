// upload_manager.h
// Header file for seeding/upload functionality

#ifndef UPLOAD_MANAGER_H
#define UPLOAD_MANAGER_H

#include "init_torrent_state.h"

typedef struct {
    uint32_t index;      // piece index
    uint32_t begin;     // block offset within piece
    uint32_t length;     // requested block length
} piece_request;

/**
 * Start seeding mode after download completes
 * 
 * This function takes over after download_torrent() returns successfully.
 * It handles:
 * - Accepting incoming peer connections
 * - Sending bitfields to new peers
 * - Managing upload slots (unchoking interested peers)
 * - Responding to piece requests
 * - Periodic tracker announcements
 * 
 * @param ts Pointer to the TorrentState with completed download
 * @return 0 on clean shutdown, -1 on error (though typically runs until Ctrl+C)
 */
int start_seeding(TorrentState *ts);
int parse_request_payload(const unsigned char *msg, piece_request *req);

#endif // UPLOAD_MANAGER_H