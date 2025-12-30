#include "file_writer.h"
#include <stdio.h>
#include <stdlib.h>

// Writes one complete piece into the final output file.
int file_writer_write_piece(TorrentState *ts,
                            int piece_index,
                            unsigned char *data,
                            int length)
{
    if (!ts || !ts->meta) {
        fprintf(stderr, "[FILE] Invalid TorrentState.\n");
        return -1;
    }

    if (!ts->output_file) {
        fprintf(stderr, "[FILE] Output file not open.\n");
        return -1;
    }

    long offset = (long) piece_index * ts->piece_length;

    if (fseek(ts->output_file, offset, SEEK_SET) != 0) {
        perror("[FILE] fseek failed");
        return -1;
    }

    size_t written = fwrite(data, 1, length, ts->output_file);

    if (written != (size_t)length) {
        fprintf(stderr, "[FILE] fwrite wrote %zu/%d bytes\n", written, length);
        return -1;
    }

    fflush(ts->output_file);

    printf("[FILE] Wrote piece %d (%d bytes) at offset %ld\n",
           piece_index, length, offset);

    return 0;
}
