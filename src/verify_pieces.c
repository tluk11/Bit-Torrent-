#include <openssl/sha.h>
#include <string.h>
#include <stdio.h>
#include "verify_pieces.h"
#include "store_pieces.h"
#include "torrent_parser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * Check if a piece matches the SHA1 hash from the torrent file.
 * Marks the piece as complete on success.
 */
bool verify_piece(TorrentState *ts, int index, unsigned char *data, int length)
{
    PieceBuffer *pb = &ts->pieces[index];

    unsigned char digest[20];
    SHA1(pb->data, pb->length, digest);   // compute hash of stored piece

    unsigned char *expected = ts->meta->pieces + (index * 20);  // expected hash

    // compare hashes
    if (memcmp(digest, expected, 20) == 0) {
        printf("[VERIFY] Piece %d verified successfully.\n", index);
        ts->piece_complete[index] = true;
        pb->verified = true;
        return 1;
    }

    // hash mismatch
    printf("[VERIFY] Piece %d FAILED SHA1 check. Redownloading.\n", index);
    pb->verified = false;
    return 0;
}
