#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "torrent_parser.h"
#include "contact_tracker.h"
#include "handshake_with_peer.h"
#include "receive_message.h"
#include "parse_message.h"

int main() {
    TorrentInfo ti;
    TrackerResponse tr;

    /* parse the .torrent file */
    printf("=== Parsing torrent ===\n");
    if (torrentparser("debian-mac-13.2.0-amd64-netinst.iso.torrent", &ti) != 0) {
        printf("Failed to parse torrent.\n");
        return 1;
    }

    /* request peers from tracker */
    printf("=== Contacting tracker ===\n");
    if (contact_tracker(&ti, &tr) != 0 || tr.num_peers == 0) {
        printf("Tracker request failed / no peers.\n");
        torrent_info_free(&ti);
        return 1;
    }

    /* connect to first peer returned by tracker */
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

    peer.socket_fd = sock;
    printf("Handshake OK!\n");

    /* wait for peer's bitfield */
    printf("=== Waiting for BITFIELD ===\n");

    unsigned char *raw = receive_message(sock);
    if (!raw) {
        printf("Peer closed or error while waiting for BITFIELD.\n");
        return 1;
    }

    ParsedMessage msg;
    parse_message(raw, &msg);

    if (msg.id != MSG_BITFIELD) {
        printf("Unexpected first msg ID=%d\n", msg.id);
        free(raw);
        return 1;
    }

    printf("Received BITFIELD (%u bytes)\n", msg.payload_len);

    /* store bitfield in peer struct */
    peer.bitfield_len = msg.payload_len;
    peer.bitfield = malloc(peer.bitfield_len);
    memcpy(peer.bitfield, msg.payload, peer.bitfield_len);

    free(raw);

    /* send INTERESTED */
    printf("=== Sending INTERESTED ===\n");
    unsigned char interested_msg[5];
    uint32_t len = htonl(1);
    memcpy(interested_msg, &len, 4);
    interested_msg[4] = 2; // INTERESTED

    send(sock, interested_msg, 5, 0);
    peer.am_interested = true;

    /* wait for choke/unchoke response */
    printf("=== Waiting for CHOKE/UNCHOKE ===\n");

    raw = receive_message(sock);
    if (!raw) {
        printf("Peer closed or error waiting for choke/unchoke.\n");
        return 1;
    }

    parse_message(raw, &msg);

    if (msg.id == MSG_UNCHOKE) {
        printf("Peer UNCHOKED us! (OK)\n");
        peer.is_choked = false;
    } else if (msg.id == MSG_CHOKE) {
        printf("Peer CHOKED us.\n");
        peer.is_choked = true;
    } else {
        printf("Unexpected msg ID=%d instead of CHOKE/UNCHOKE\n", msg.id);
    }

    free(raw);

    /* end test here, do not request blocks */
    printf("=== TEST COMPLETED: No piece requested ===\n");

    close(sock);
    tracker_response_free(&tr);
    torrent_info_free(&ti);

    return 0;
}
