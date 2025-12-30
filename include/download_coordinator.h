#ifndef DOWNLOAD_COORDINATOR_H
#define DOWNLOAD_COORDINATOR_H

#include "torrent_parser.h"

/**
 * Main download coordinator loop.
 * Manages peer connections, handles messages, and coordinates
 * the download of all pieces until completion.
 * 
 * @param ts Pointer to initialized TorrentState
 * @return 0 on successful completion, -1 on error
 */
int download_torrent(TorrentState *ts);

#endif // DOWNLOAD_COORDINATOR_H