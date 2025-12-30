#ifndef VERIFY_PIECES_H
#define VERIFY_PIECES_H

#include <stdbool.h>
#include "torrent_parser.h"

bool verify_piece(TorrentState *ts, int index, unsigned char *data, int length);

#endif
