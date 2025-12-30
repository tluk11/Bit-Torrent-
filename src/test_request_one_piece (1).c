#define BLOCK_SIZE 16384

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "torrent_parser.h"
#include "contact_tracker.h"
#include "handshake_with_peer.h"
#include "receive_message.h"
#include "parse_message.h"
#include "sendRequest.h"
#include "store_pieces.h"

int main() {
    TorrentInfo ti;
    TrackerResponse tr;

    /* parse .torrent file */
    printf("=== Parsing torrent ===\n");
    if (torrentparser("debian-mac-13.2.0-amd64-netinst.iso.torrent", &ti) != 0) {
        printf("Failed to parse torrent.\n");
        return 1;
    }
    printf(" Parsed: %d pieces, piece_length=%d\n", ti.num_pieces, ti.piece_length);

    /* query the tracker */
    printf("\n=== Contacting tracker ===\n");
    if (contact_tracker(&ti, &tr) != 0 || tr.num_peers == 0) {
        printf("Tracker request failed / no peers.\n");
        torrent_info_free(&ti);
        return 1;
    }
    printf(" Got %d peers\n", tr.num_peers);

    /* connect to the first peer */
    Peer peer = tr.peers[0];
    printf("\n=== Connecting to peer %s:%d ===\n", peer.ip, peer.port);

    unsigned char client_id[20] = "-TC0001-123456789012";

    int sock = connect_and_handshake(&peer, ti.info_hash, client_id);
    if (sock < 0) {
        printf("Handshake failed.\n");
        tracker_response_free(&tr);
        torrent_info_free(&ti);
        return 1;
    }

    peer.socket_fd = sock;
    printf(" Handshake OK!\n");

    /* initialize torrent state for downloading */
    printf("\n=== Initializing TorrentState ===\n");
    TorrentState ts;
    memset(&ts, 0, sizeof(ts));

    ts.meta = &ti;
    ts.total_pieces = ti.num_pieces;
    ts.piece_length = ti.piece_length;

    printf("  total_pieces: %d\n", ts.total_pieces);
    printf("  piece_length: %d\n", ts.piece_length);

    /* per-piece tracking */
    printf("  Allocating tracking arrays...\n");
    ts.piece_complete = calloc(ts.total_pieces, sizeof(bool));
    ts.piece_bytes_have = calloc(ts.total_pieces, sizeof(int));

    if (!ts.piece_complete || !ts.piece_bytes_have) {
        fprintf(stderr, " Failed to allocate tracking arrays\n");
        return 1;
    }
    printf("   Tracking arrays allocated\n");

    /* allocate storage for piece buffers and block flags */
    printf("  Calling init_piece_storage()...\n");
    if (init_piece_storage(&ts) != 0) {
        fprintf(stderr, " Failed to initialize piece storage\n");
        return 1;
    }
    printf("   Piece storage initialized\n");

    /* sanity check first piece */
    printf("\n=== Verifying piece 0 initialization ===\n");
    printf("  pieces[0].data: %p\n", (void*)ts.pieces[0].data);
    printf("  pieces[0].block_received: %p\n", (void*)ts.pieces[0].block_received);
    printf("  pieces[0].length: %d\n", ts.pieces[0].length);
    printf("  pieces[0].num_blocks: %d\n", ts.pieces[0].num_blocks);

    if (ts.pieces[0].data == NULL) {
        fprintf(stderr, " ERROR: pieces[0].data is NULL!\n");
        return 1;
    }
    if (ts.pieces[0].block_received == NULL) {
        fprintf(stderr, " ERROR: pieces[0].block_received is NULL!\n");
        return 1;
    }
    printf("   Piece 0 is properly initialized\n");

    /* wait for peer's bitfield */
    printf("\n=== Waiting for BITFIELD ===\n");

    unsigned char *raw = receive_message(sock);
    if (!raw) {
        printf("Peer closed on BITFIELD.\n");
        return 1;
    }

    ParsedMessage msg;
    parse_message(raw, &msg);

    if (msg.id != MSG_BITFIELD) {
        printf("Unexpected message ID %d\n", msg.id);
        free(raw);
        return 1;
    }

    printf(" BITFIELD received (%u bytes)\n", msg.payload_len);

    /* store remote peer's bitfield */
    peer.bitfield_len = msg.payload_len;
    peer.bitfield = malloc(peer.bitfield_len);
    memcpy(peer.bitfield, msg.payload, peer.bitfield_len);

    free(raw);

    /* send INTERESTED */
    printf("\n=== Sending INTERESTED ===\n");

    unsigned char msg_interested[5];
    uint32_t len = htonl(1);
    memcpy(msg_interested, &len, 4);
    msg_interested[4] = 2; // INTERESTED

    send(sock, msg_interested, 5, 0);
    peer.am_interested = true;
    printf(" INTERESTED sent\n");

    /* wait for UNCHOKE */
    printf("\n=== Waiting for UNCHOKE ===\n");

    raw = receive_message(sock);
    if (!raw) {
        printf("Peer closed while waiting for UNCHOKE.\n");
        return 1;
    }

    parse_message(raw, &msg);

    if (msg.id == MSG_UNCHOKE) {
        printf(" Peer UNCHOKED us!\n");
        peer.is_choked = false;
    } else if (msg.id == MSG_CHOKE) {
        printf(" Peer CHOKED us.\n");
        peer.is_choked = true;
    } else {
        printf("Unexpected msg ID=%d\n", msg.id);
    }

    free(raw);

    if (peer.is_choked) {
        printf("Cannot request blocks - peer is choking us.\n");
        return 1;
    }

    /* send a single block request */
    printf("\n=== Requesting ONE block ===\n");

    peer.current_piece = -1;
    peer.current_offset = 0;

    if (request_next_block(&peer, &ts) < 0) {
        printf(" Request failed.\n");
        return 1;
    }
    printf(" Request sent\n");

    /* wait for PIECE */
    printf("\n=== Waiting for PIECE ===\n");

    while (1) {
        raw = receive_message(sock);
        if (!raw) {
            printf("Peer closed.\n");
            return 1;
        }

        if (parse_message(raw, &msg) < 0) {
            printf("Failed to parse message. Probably partial segment.\n");
            free(raw);
            continue;
        }

        if (msg.id == MSG_KEEP_ALIVE) {
            printf("[KEEP-ALIVE]\n");
            free(raw);
            continue;
        }

        if (msg.id == MSG_HAVE) {
            printf("[HAVE]\n");
            free(raw);
            continue;
        }

        if (msg.id == MSG_BITFIELD) {
            printf("[BITFIELD ignore duplicate]\n");
            free(raw);
            continue;
        }

        if (msg.id == MSG_UNCHOKE) {
            printf("[UNCHOKE AGAIN]\n");
            free(raw);
            continue;
        }

        if (msg.id == MSG_PIECE) {
            printf(" Received PIECE\n");
            break;
        }

        printf("RAW: ");
        for (int i = 0; i < 20; i++) printf("%02x ", raw[i]);
        printf("\n");

        printf("[UNKNOWN MSG: %d]\n", msg.id);
        free(raw);
    }

    parse_message(raw, &msg);

    if (msg.id != MSG_PIECE) {
        printf("Unexpected msg ID %d instead of PIECE.\n", msg.id);
        free(raw);
        return 1;
    }

    PiecePayload piece;
    parse_piece_payload(&msg, &piece);

    printf(" [PIECE] idx=%u begin=%u size=%u\n",
           piece.index, piece.begin, piece.data_len);

    /* quick sanity check before writing the block */
    printf("\n=== About to call store_received_block ===\n");
    printf("  ts.pieces: %p\n", (void*)ts.pieces);
    printf("  ts.pieces[%u].data: %p\n", piece.index, (void*)ts.pieces[piece.index].data);
    printf("  ts.pieces[%u].block_received: %p\n", piece.index, (void*)ts.pieces[piece.index].block_received);

    if (ts.pieces[piece.index].data == NULL) {
        fprintf(stderr, " FATAL: pieces[%u].data is NULL before storing!\n", piece.index);
        return 1;
    }

    /* write the received block */
    printf("  Calling store_received_block()...\n");
    int store_result = store_received_block(&ts, piece.index, piece.begin, piece.data, piece.data_len);

    if (store_result == 0) {
        printf(" Block stored successfully!\n");
    } else {
        printf(" Failed to store block (returned %d)\n", store_result);
    }

    free(raw);

    printf("\n=== TEST COMPLETED ===\n");
    printf("Piece 0: %d/%d blocks received\n",
           ts.pieces[0].blocks_done, ts.pieces[0].num_blocks);

    /* cleanup */
    close(sock);
    tracker_response_free(&tr);
    torrent_info_free(&ti);

    free_piece_storage(&ts);
    free(ts.piece_complete);
    free(ts.piece_bytes_have);

    if (peer.bitfield)
        free(peer.bitfield);

    return 0;
}
