#ifndef INIT_TORRENT_STATE_H
#define INIT_TORRENT_STATE_H

#include <stdbool.h>
#include <unistd.h>
#include "torrent_parser.h"


double get_time_seconds();

/**
 * Initialize TorrentState structure with all necessary allocations.
 * Creates piece buffers, bitfields, and opens output file.
 * 
 * @param ts Pointer to TorrentState to initialize
 * @param ti Pointer to parsed TorrentInfo
 * @return 0 on success, -1 on error
 */
int init_torrent_state(TorrentState *ts, TorrentInfo *ti, int port);

/**
 * Free all resources associated with TorrentState.
 * Closes file handles, frees memory, closes peer connections.
 * 
 * @param ts Pointer to TorrentState to cleanup
 */
void cleanup_torrent_state(TorrentState *ts);

/**
 * Check if all pieces have been downloaded and verified.
 * 
 * @param ts Pointer to TorrentState
 * @return true if download is complete, false otherwise
 */
bool is_download_complete(TorrentState *ts);

/**
 * Get current download progress as a percentage.
 * 
 * @param ts Pointer to TorrentState
 * @return Progress as float between 0.0 and 100.0
 */
float get_download_progress(TorrentState *ts);

#endif // INIT_TORRENT_STATE_H