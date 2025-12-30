// upload_manager.c
// Handles seeding/upload behavior for peers.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include "upload_manager.h"
#include "torrent_parser.h"
#include "contact_tracker.h"
#include "manage_peers.h"
#include "parse_message.h"
#include "receive_message.h"
#include "outgoingMessages.h"
#include "requestPayload.h"

#define TRACKER_RECONTACT_INTERVAL 1800  // re-announce every 30 mins
#define KEEP_ALIVE_INTERVAL 120          // keep-alives every 2 mins
#define STATUS_PRINT_INTERVAL 60         // periodic status prints

static unsigned char CLIENT_ID[20] = "-TC0001-123456789012";

// basic helper for reading network-order 32-bit integers
static uint32_t read_u32_be(const unsigned char *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return ntohl(v);
}

// parse piece-request payload: index, begin, length
int parse_request_payload(const unsigned char *msg, piece_request *req) {
    if (!msg || !req)
        return -1;

    printf("Bytes as received %u %u %u\n", msg[1], msg[5], msg[9]);

    // NOTE: The offsets here assume msg[0] is index[0]
    req->index  = read_u32_be(msg);
    req->begin  = read_u32_be(msg + 4);
    req->length = read_u32_be(msg + 8);

    printf("Parsed values: index=%u, begin=%u, length=%u\n",
           req->index, req->begin, req->length);

    return 0;
}

// Decide which peers get unchoked so they can download
static void manage_upload_slots(TorrentState *ts) {
    int unchoked_count = 0;
    const int MAX_UNCHOKED = 4;  // typical cap

    for (int i = 0; i < ts->peer_count && unchoked_count < MAX_UNCHOKED; i++) {
        Peer *p = ts->peers[i];

        if (p->state != PEER_ACTIVE || p->socket_fd < 0)
            continue;

        if (p->is_interested && p->am_choking) {
            printf("[UPLOAD] <<< SENDING: UNCHOKE to %s:%d \n", p->ip, p->port);
            send_unchoke(p);
            p->am_choking = false;
            unchoked_count++;

            printf("[UPLOAD]  Unchoked %s:%d (slot %d/%d)\n",
                   p->ip, p->port, unchoked_count, MAX_UNCHOKED);
        } else if (!p->am_choking) {
            unchoked_count++;
        }
    }
}

// Handle any message from a peer while seeding
static void handle_seed_message(TorrentState *ts, Peer *peer) {
    unsigned char *raw_buf = receive_message(peer->socket_fd);
    if (!raw_buf) {
        printf("[SEED %s:%d]  Connection closed\n", peer->ip, peer->port);
        peer->socket_fd = -1;
        return;
    }

    ParsedMessage msg;
    if (parse_message(raw_buf, &msg) < 0) {
        fprintf(stderr, "[SEED %s:%d]  Failed to parse message\n",
                peer->ip, peer->port);
        free(raw_buf);
        peer->socket_fd = -1;
        return;
    }

    switch (msg.id) {
        case MSG_CHOKE:
            printf("[SEED %s:%d] >>> RECEIVED: CHOKE\n", peer->ip, peer->port);
            break;

        case MSG_UNCHOKE:
            printf("[SEED %s:%d] >>> RECEIVED: UNCHOKE\n", peer->ip, peer->port);
            break;

        case MSG_INTERESTED:
            peer->is_interested = true;
            printf("[SEED %s:%d] >>> RECEIVED: INTERESTED \n", peer->ip, peer->port);
            manage_upload_slots(ts);
            break;

        case MSG_NOT_INTERESTED:
            peer->is_interested = false;
            printf("[SEED %s:%d] >>> RECEIVED: NOT_INTERESTED\n",
                   peer->ip, peer->port);

            // if they're not interested, we can choke them again
            if (!peer->am_choking) {
                printf("[SEED %s:%d] <<< SENDING: CHOKE\n",
                       peer->ip, peer->port);
                send_choke(peer);
                peer->am_choking = true;
            }
            break;

        case MSG_HAVE: {
            if (msg.payload_len == 4) {
                uint32_t idx = ntohl(*(uint32_t*)msg.payload);
                printf("[SEED %s:%d] >>> RECEIVED: HAVE piece=%u\n",
                       peer->ip, peer->port, idx);
            }
            break;
        }

        case MSG_BITFIELD:
            printf("[SEED %s:%d] >>> RECEIVED: BITFIELD (%u bytes)\n",
                   peer->ip, peer->port, msg.payload_len);
            break;

        case MSG_REQUEST: {
            printf("Received piece request\n");
            piece_request req;

            // quick dump of raw request bytes
            for (int i = 0; i < 12; i++)
                printf("%02X ", ((unsigned char*)msg.payload)[i]);
            printf("\n");

            int res = parse_request_payload((unsigned char*) msg.payload, &req);
            printf("Parse result: %d\n", res);

            if (res == 0) {
                printf("[SEED %s:%d] >>> REQUEST piece=%u begin=%u len=%u\n",
                       peer->ip, peer->port, req.index, req.begin, req.length);

                // basic safety checks
                if (peer->am_choking)
                    break;

                if (req.index >= ts->total_pieces)
                    break;

                if (req.length > 16384)
                    break;

                printf("[SEED %s:%d] <<< SENDING: PIECE %u %u %u\n",
                       peer->ip, peer->port, req.index, req.begin, req.length);

                send_piece(peer, ts, req.index, req.begin, req.length);
                ts->bytes_uploaded += req.length;
            }
            break;
        }

        case MSG_PIECE:
            // seeders ignore incoming PIECE messages
            printf("[SEED %s:%d] >>> RECEIVED: PIECE (ignored)\n",
                   peer->ip, peer->port);
            break;

        case MSG_KEEP_ALIVE:
            printf("[SEED %s:%d] >>> KEEP_ALIVE\n",
                   peer->ip, peer->port);
            break;

        default:
            printf("[SEED %s:%d] >>> Unknown message ID=%d\n",
                   peer->ip, peer->port, msg.id);
            break;
    }

    free(raw_buf);
}

// Accept a new inbound peer connection
static void accept_incoming_peer(TorrentState *ts) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    int new_fd = accept(ts->listen_fd, (struct sockaddr *)&addr, &addr_len);
    if (new_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            perror("accept");
        return;
    }

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ip_str, INET_ADDRSTRLEN);
    int port = ntohs(addr.sin_port);

    printf("\nNEW PEER\n");
    printf("  IP:   %s\n", ip_str);
    printf("  Port: %d\n\n", port);

    Peer *p = add_peer(ts, ip_str, port);
    if (!p) {
        printf("[SEED]  Failed to allocate peer\n");
        close(new_fd);
        return;
    }

    p->socket_fd = new_fd;
    p->state = PEER_WAIT_HANDSHAKE_OUT;  // expect their handshake first
    p->am_choking = true;

    // set blocking mode for the handshake
    int flags = fcntl(new_fd, F_GETFL, 0);
    if (flags != -1)
        fcntl(new_fd, F_SETFL, flags & ~O_NONBLOCK);

    printf("[SEED %s:%d]  Waiting for handshake...\n", ip_str, port);
}

// Handle inbound handshake from a peer
static void handle_inbound_handshake(TorrentState *ts, Peer *peer) {
    unsigned char hs[68];
    int got = recv(peer->socket_fd, hs, 68, 0);

    printf("[SEED %s:%d] >>> Handshake (%d bytes)\n",
           peer->ip, peer->port, got);

    if (got != 68) {
        printf("[SEED %s:%d]  Bad handshake length\n",
               peer->ip, peer->port);
        close(peer->socket_fd);
        peer->socket_fd = -1;
        peer->state = PEER_DISCONNECTED;
        return;
    }

    // basic protocol validation
    if (hs[0] != 19 ||
        memcmp(hs + 1, "BitTorrent protocol", 19) != 0 ||
        memcmp(hs + 28, ts->meta->info_hash, 20) != 0) {

        printf("[SEED %s:%d]  Invalid handshake\n",
               peer->ip, peer->port);
        close(peer->socket_fd);
        peer->socket_fd = -1;
        peer->state = PEER_DISCONNECTED;
        return;
    }

    printf("[SEED %s:%d]  Valid handshake\n",
           peer->ip, peer->port);

    // reply handshake
    unsigned char reply[68] = {0};
    reply[0] = 19;
    memcpy(reply + 1, "BitTorrent protocol", 19);
    memcpy(reply + 28, ts->meta->info_hash, 20);
    memcpy(reply + 48, CLIENT_ID, 20);

    printf("[SEED %s:%d] <<< Sending handshake response\n",
           peer->ip, peer->port);

    if (send(peer->socket_fd, reply, 68, 0) != 68) {
        printf("[SEED %s:%d]  Handshake send error\n",
               peer->ip, peer->port);
        close(peer->socket_fd);
        peer->socket_fd = -1;
        peer->state = PEER_DISCONNECTED;
        return;
    }

    peer->state = PEER_ACTIVE;

    // we have all pieces -> send bitfield
    printf("[SEED %s:%d] <<< Sending bitfield (%d bytes)\n",
           peer->ip, peer->port, ts->my_bitfield_len);
    send_bitfield(peer, ts);

    printf("[SEED %s:%d] Waiting for INTERESTED\n",
           peer->ip, peer->port);
}

// Send keep-alives to active peers
static void send_keep_alives(TorrentState *ts) {
    for (int i = 0; i < ts->peer_count; i++) {
        Peer *p = ts->peers[i];
        if (p->state == PEER_ACTIVE && p->socket_fd >= 0) {
            unsigned char keep_alive[4] = {0, 0, 0, 0};
            send(p->socket_fd, keep_alive, 4, 0);
        }
    }
}

// Main seeding loop
int start_seeding(TorrentState *ts) {
    printf("\nSEEDING MODE\n");
    printf("Listening on port %d\n", ts->listen_port);

    ts->is_seeding = true;
    time_t last_tracker_contact = 0;
    time_t last_keep_alive = time(NULL);
    time_t last_status_print = time(NULL);
    time_t start_time = time(NULL);

    // initial tracker announce (completed download)
    printf("[SEED] Announcing completion to tracker...\n");
    TrackerResponse tr;
    if (contact_tracker(ts->meta, &tr) == 0) {
        printf("[SEED] Tracker ok. Waiting for peers...\n");
        tracker_response_free(&tr);
    }
    last_tracker_contact = time(NULL);

    if (ts->listen_fd < 0) {
        fprintf(stderr, "[SEED] ERROR: listen socket not set\n");
        return -1;
    }

    printf("[SEED] Ready. Accepting connections\n");

    while (1) {
        time_t now = time(NULL);

        // periodic tracker announce
        if (now - last_tracker_contact > TRACKER_RECONTACT_INTERVAL) {
            printf("[SEED] Re-announcing to tracker...\n");
            TrackerResponse tr2;
            if (contact_tracker(ts->meta, &tr2) == 0) {
                printf("[SEED] Tracker updated\n");
                tracker_response_free(&tr2);
            }
            last_tracker_contact = now;
        }

        // keep-alives
        if (now - last_keep_alive > KEEP_ALIVE_INTERVAL) {
            send_keep_alives(ts);
            last_keep_alive = now;
        }

        // periodic status print
        if (now - last_status_print > STATUS_PRINT_INTERVAL) {
            int active_peers = 0;
            int unchoked_peers = 0;

            for (int i = 0; i < ts->peer_count; i++) {
                if (ts->peers[i]->state == PEER_ACTIVE &&
                    ts->peers[i]->socket_fd >= 0) {

                    active_peers++;
                    if (!ts->peers[i]->am_choking)
                        unchoked_peers++;
                }
            }

            int uptime = now - start_time;
            double mb_uploaded = ts->bytes_uploaded / (1024.0 * 1024.0);

            printf("\n[SEED] Uptime: %d:%02d:%02d\n",
                   uptime / 3600, (uptime % 3600) / 60, uptime % 60);
            printf("[SEED] Active peers: %d (%d unchoked)\n",
                   active_peers, unchoked_peers);
            printf("[SEED] Uploaded: %.2f MB\n\n", mb_uploaded);

            last_status_print = now;
        }

        // build fd_set for select()
        fd_set read_fds;
        FD_ZERO(&read_fds);

        int max_fd = -1;

        if (ts->listen_fd >= 0) {
            FD_SET(ts->listen_fd, &read_fds);
            max_fd = ts->listen_fd;
        }

        for (int i = 0; i < ts->peer_count; i++) {
            Peer *p = ts->peers[i];
            if (p->socket_fd < 0) continue;

            FD_SET(p->socket_fd, &read_fds);
            if (p->socket_fd > max_fd)
                max_fd = p->socket_fd;
        }

        if (max_fd < 0) {
            sleep(5);
            cleanup_dead_peers(ts);
            continue;
        }

        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;

        int activity = select(max_fd + 1, &read_fds, NULL, NULL, &tv);

        if (activity < 0) {
            if (errno == EINTR) continue;
            perror("select");
            continue;
        }

        if (activity == 0)
            continue;

        if (ts->listen_fd >= 0 && FD_ISSET(ts->listen_fd, &read_fds))
            accept_incoming_peer(ts);

        for (int i = 0; i < ts->peer_count; i++) {
            Peer *peer = ts->peers[i];
            if (peer->socket_fd < 0) continue;

            if (!FD_ISSET(peer->socket_fd, &read_fds))
                continue;

            if (peer->state == PEER_WAIT_HANDSHAKE_OUT)
                handle_inbound_handshake(ts, peer);
            else if (peer->state == PEER_ACTIVE)
                handle_seed_message(ts, peer);
        }

        cleanup_dead_peers(ts);
    }

    return 0;
}
