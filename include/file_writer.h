#ifndef FILE_WRITER_H
#define FILE_WRITER_H

#include "torrent_parser.h"

int file_writer_write_piece(TorrentState *ts, int index, unsigned char *data, int length);

#endif
