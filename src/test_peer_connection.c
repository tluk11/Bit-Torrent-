#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "torrent_parser.h"
#include "contact_tracker.h"
#include "handshake_with_peer.h"
#include "parse_message.h"
#include "sendRequest.h"
#include "store_pieces.h"
#include "receive_message.h"

int main() {
    TorrentInfo ti;
    TrackerResponse tr;

    /* load and parse the .torrent file */
    printf("=== Parsing torrent ===\n");
    if (torrentparser("debian-mac-13.2.0-amd64-netinst.iso.torrent", &ti) != 0) {
        printf("Failed to parse torrent.\n");
        return 1;
    }

    /* request peers from the tracker */
    printf("=== Contacting tracker ===\n");
    if (contact_tracker(&ti, &tr) != 0) {
        printf("Tracker request failed.\n");
        torrent_info_free(&ti);
        return 1;
    }

    if (tr.num_peers == 0) {
        printf("Tracker returned zero peers.\n");
        torrent_info_free(&ti);
        return 1;
    }

    /* attempt connection to the first peer */
    Peer peer = tr.peers[0];
    printf("Connecting to peer %s:%d...\n", peer.ip, peer.port);

    unsigned char client_id[20] = "-TC0001-123456789012";

    int sock = connect_and_handshake(&peer, ti.info_hash, client_id);
    if (sock < 0) {
        printf("Handshake failed.\n");
        tracker_response_free(&tr);
        torrent_info_free(&ti);
        return 1;
    }

    printf("Handshake OK!\n");

    /* wait for the peer's bitfield */
    printf("=== Waiting for BITFIELD ===\n");
    unsigned char *raw = receive_message(sock);
    if (!raw) {
        printf("Peer closed after handshake.\n");
        return 1;
    }

    ParsedMessage msg;
    parse_message(raw, &msg);

    if (msg.id == MSG_BITFIELD)
        printf("Received BITFIELD (%u bytes)\n", msg.payload_len);
    else
        printf("Unexpected first msg ID=%d\n", msg.id);

    /* send INTERESTED message */
    printf("=== Sending INTERESTED ===\n");
    uint32_t len = htonl(1);
    unsigned char interested_msg[5];
    memcpy(interested_msg, &len, 4);
    interested_msg[4] = MSG_INTERESTED;
    send(sock, interested_msg, 5, 0);

    /* wait for UNCHOKE */
    printf("=== Waiting for CHOKE/UNCHOKE ===\n");
    while (1) {
        unsigned char *raw2 = receive_message(sock);
        if (!raw2) {
            printf("Peer closed connection.\n");
            return 1;
        }

        ParsedMessage msg2;
        parse_message(raw2, &msg2);
        free(raw2);

        if (msg2.id == MSG_UNCHOKE) {
            printf("Peer UNCHOKED us!\n");
            break;
        }

        if (msg2.id == MSG_CHOKE) {
            printf("Peer is still choking us...\n");
            continue;
        }

        printf("Ignoring msg ID=%d while waiting for UNCHOKE\n", msg2.id);
    }

    /* request one block */
    printf("=== Requesting one block ===\n");

    TorrentState ts = {0};
    ts.meta = &ti;
    ts.total_pieces = ti.num_pieces;
    ts.piece_length = ti.piece_length;

    peer.socket_fd = sock;

    request_next_block(&peer, &ts);

    /* wait for PIECE reply */
    printf("=== Waiting for PIECE ===\n");
    raw = receive_message(sock);
    parse_message(raw, &msg);

    if (msg.id == MSG_PIECE)
        printf("Received PIECE (%u bytes)!\n", msg.payload_len);
    else
        printf("Unexpected msg ID=%d instead of PIECE\n", msg.id);

    free(raw);
    close(sock);
    tracker_response_free(&tr);
    torrent_info_free(&ti);

    return 0;
}
