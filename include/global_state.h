#ifndef GLOBAL_STATE_H
#define GLOBAL_STATE_H

#include "torrent_parser.h"

// A single global torrent state pointer used by listener threads
extern TorrentState *g_torrent_state;

#endif
