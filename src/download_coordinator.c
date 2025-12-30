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

#include "torrent_parser.h"
#include "contact_tracker.h"
#include "handshake_with_peer.h"
#include "manage_peers.h"
#include "parse_message.h"
#include "receive_message.h"
#include "sendRequest.h"
#include "outgoingMessages.h"
#include "init_torrent_state.h"
#include "requestPayload.h"

#define MAX_PEER_CONNECTIONS 50
#define TRACKER_RECONTACT_INTERVAL 1800  // 30 minutes

static unsigned char CLIENT_ID[20] = "-TC0001-123456789012";

// Update our bitfield when we complete a piece
static void update_my_bitfield(TorrentState *ts, int piece_index) {
    if (!ts->my_bitfield || piece_index < 0 || piece_index >= ts->total_pieces) {
        return;
    }
    
    int byte = piece_index / 8;
    int bit = 7 - (piece_index % 8);
    
    if (byte < ts->my_bitfield_len) {
        ts->my_bitfield[byte] |= (1 << bit);
        printf("[BITFIELD] Updated: we now have piece %d\n", piece_index);
    }
}

// Manage upload slots - allow some peers to download from us
static void manage_upload_slots(TorrentState *ts) {
    int unchoked_count = 0;
    
    for (int i = 0; i < ts->peer_count && unchoked_count < 4; i++) {
        Peer *p = ts->peers[i];
        
        if (p->state != PEER_ACTIVE || p->socket_fd < 0) {
            continue;
        }
        
        // If peer is interested and we're choking them, unchoke
        if (p->is_interested && p->am_choking) {
            send_unchoke(p);
            p->am_choking = false;
            unchoked_count++;
            printf("[UPLOAD] Unchoked %s:%d for uploading\n", p->ip, p->port);
        } else if (!p->am_choking) {
            unchoked_count++;
        }
    }
}

static int setup_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket(listen)");
        return -1;
    }

    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind(listen)");
        close(fd);
        return -1;
    }

    if (listen(fd, 128) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

bool all_pieces_downloaded(TorrentState *ts) {
    for (int i = 0; i < ts->total_pieces; i++) {
        if (!ts->piece_complete[i])
            return false;
    }
    return true;
}

static bool peer_can_request_more(Peer *peer, TorrentState *ts) {
    if (!peer || !ts) return false;
    if (peer->socket_fd < 0) return false;

    if (peer->is_choked) {
        return false;
    }

    if (!peer->am_interested) {
        return false;
    }

    if (peer->outstanding_requests >= peer->max_pipeline) {
        return false;
    }

    if (!peer->bitfield || !ts->piece_complete) {
        return false;
    }

    // Check if the peer has at least one piece we still need
    for (int i = 0; i < ts->total_pieces; i++) {
        if (ts->piece_complete[i])
            continue;

        int byte = i / 8;
        int bit  = 7 - (i % 8);

        if (byte < peer->bitfield_len &&
            (peer->bitfield[byte] & (1 << bit))) {
            return true;
        }
    }

    return false;
}

static void maybe_request_more(Peer *peer, TorrentState *ts) {
    if (!peer_can_request_more(peer, ts)) {
        return;
    }

    for (int i = 0; i < 4; i++) {
        if (peer_can_request_more(peer, ts)) {
            request_next_block(peer, ts);
        }
    }
}

// Try to connect to a peer and perform handshake
 int try_connect_peer(TorrentState *ts, const char *ip, int port) {
    printf("[CONNECT] Launching async connect to %s:%d\n", ip, port);

    Peer *peer = add_peer(ts, ip, port);
    if (!peer) {
        fprintf(stderr, "Failed to allocate peer\n");
        return -1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    // set non-blocking
    fcntl(sock, F_SETFL, O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);

    // Start connection — will return immediately
    int r = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    peer->state = PEER_CONNECTING;

    if (r < 0 && errno != EINPROGRESS) {
        perror("connect");
        close(sock);
        peer->state = PEER_DISCONNECTED;
        return -1;
    }

    peer->socket_fd = sock;
    peer->state = PEER_CONNECTING;

    printf("[CONNECT] Started async connect to %s:%d (fd=%d)\n", ip, port, sock);

    return 0;
}

// Handle incoming message from a peer during DOWNLOAD
static void handle_peer_message(TorrentState *ts, Peer *peer) {
    unsigned char *raw_buf = receive_message(peer->socket_fd);
    if (!raw_buf) {
        printf("[PEER %s:%d] Connection closed\n", peer->ip, peer->port);
        peer->socket_fd = -1;
        return;
    }
    
    ParsedMessage msg;
    if (parse_message(raw_buf, &msg) < 0) {
        fprintf(stderr, "[PEER %s:%d] Failed to parse message\n", peer->ip, peer->port);
        free(raw_buf);
        peer->socket_fd = -1;
        return;
    }
    
    // Handle message based on type
    switch (msg.id) {
        case MSG_CHOKE:
            peer->is_choked = true;
            peer->outstanding_requests = 0;
            printf("[PEER %s:%d] CHOKE received\n", peer->ip, peer->port);
            break;
            
        case MSG_UNCHOKE:
            peer->is_choked = false;
            peer->outstanding_requests = 0;
            printf("[PEER %s:%d] UNCHOKE received\n", peer->ip, peer->port);
            maybe_request_more(peer, ts);
            break;
            
        case MSG_INTERESTED:
            peer->is_interested = true;
            printf("[PEER %s:%d] INTERESTED (they want to download from us)\n", peer->ip, peer->port);
            manage_upload_slots(ts);  // Re-evaluate who we unchoke
            break;
            
        case MSG_NOT_INTERESTED:
            peer->is_interested = false;
            printf("[PEER %s:%d] NOT_INTERESTED\n", peer->ip, peer->port);
            break;
            
        case MSG_HAVE: {
            if (msg.payload_len == 4) {
                uint32_t idx = ntohl(*(uint32_t*)msg.payload);
                printf("[PEER %s:%d] HAVE piece %u\n", peer->ip, peer->port, idx);
                if (peer->bitfield && idx < peer->bitfield_len * 8) {
                    peer->bitfield[idx/8] |= (1 << (7 - (idx % 8)));
                }
            }
            break;
        }
        
        case MSG_BITFIELD: {
            printf("[PEER %s:%d] BITFIELD (%u bytes)\n",
                peer->ip, peer->port, msg.payload_len);

            if (peer->bitfield) free(peer->bitfield);

            peer->bitfield_len = msg.payload_len;
            peer->bitfield = malloc(msg.payload_len);

            if (!peer->bitfield) {
                printf("[ERROR] Failed to allocate bitfield for peer\n");
                break;
            }

            memcpy(peer->bitfield, msg.payload, msg.payload_len);

            // Determine if we're interested
            bool interesting = false;
            for (int i = 0; i < ts->total_pieces; i++) {
                if (ts->piece_complete[i])
                    continue;

                if (peer_has_piece(peer, i)) {
                    interesting = true;
                    break;
                }
            }

            if (interesting) {
                send_interested(peer);
                peer->am_interested = true;
                printf("[PEER %s:%d] Sent INTERESTED\n", peer->ip, peer->port);
            } else {
                send_not_interested(peer);
                peer->am_interested = false;
                printf("[PEER %s:%d] Sent NOT_INTERESTED\n", peer->ip, peer->port);
            }

            break;
        }
            
        case MSG_PIECE: {
            PiecePayload piece;
            if (parse_piece_payload(&msg, &piece) == 0) {
                store_received_block(ts, piece.index, piece.begin, piece.data, piece.data_len);
                peer->outstanding_requests--;
                if (peer->outstanding_requests < 0)
                    peer->outstanding_requests = 0;
                
                PieceState *ps = &ts->piece_states[piece.index];
                int b = piece.begin / BLOCK_SIZE;

                if (b < ps->total_blocks) {
                    ps->have_block[b] = 1;
                    ps->requested_block[b] = 0;
                    ps->received_blocks++;

                    // If piece fully received
                    if (ps->received_blocks == ps->total_blocks) {
                        ts->piece_complete[piece.index] = true;
                        
                        // Update our bitfield
                        update_my_bitfield(ts, piece.index);
                        
                        // Announce to all peers that we have this piece
                        for (int i = 0; i < ts->peer_count; i++) {
                            Peer *p = ts->peers[i];
                            if (p->state == PEER_ACTIVE && p->socket_fd >= 0) {
                                send_have(p, piece.index);
                            }
                        }
                        
                        printf("[PIECE] Completed piece %u\n", piece.index);
                    }
                }

                maybe_request_more(peer, ts);
            }
            break;
        }
        
        case MSG_REQUEST: {
            RequestPayload req;
            if (parse_request_payload(&msg, &req) == 0) {
                printf("[DOWNLOAD] REQUEST from %s:%d idx=%u begin=%u len=%u\n",
                    peer->ip, peer->port, req.index, req.begin, req.length);
                
                // Check if we're choking this peer
                if (peer->am_choking) {
                    printf("[DOWNLOAD] Ignoring REQUEST (peer is choked)\n");
                    break;
                }
                
                // Check if we have the piece
                if (req.index >= ts->total_pieces || !ts->piece_complete[req.index]) {
                    printf("[DOWNLOAD] Don't have piece %u\n", req.index);
                    break;
                }
                
                // Validate request size
                if (req.length > 16384) {
                    printf("[DOWNLOAD] Invalid block size %u\n", req.length);
                    break;
                }
                
                // Send the piece (reciprocal sharing while downloading)
                printf("[DOWNLOAD] Uploading: piece=%u begin=%u len=%u to %s:%d\n",
                    req.index, req.begin, req.length, peer->ip, peer->port);
                send_piece(peer, ts, req.index, req.begin, req.length);
                ts->bytes_uploaded += req.length;
            }
            break;
        }
        
        case MSG_KEEP_ALIVE:
            printf("[PEER %s:%d] KEEP_ALIVE\n", peer->ip, peer->port);
            break;
            
        default:
            printf("[PEER %s:%d] Unknown message ID=%d\n", peer->ip, peer->port, msg.id);
            break;
    }
    
    free(raw_buf);
}

// Main download loop (DOWNLOAD ONLY - no seeding)
int download_torrent(TorrentState *ts) {
    time_t last_tracker_contact = 0;
    int last_progress = -1;

    ts->listen_fd = setup_listen_socket(ts->listen_port);
    if (ts->listen_fd >= 0) {
        printf("[LISTEN] Accepting peers on port %d (fd=%d)\n",
               ts->listen_port, ts->listen_fd);
    } else {
        printf("[LISTEN] Failed to open listen socket, continuing without inbound peers\n");
    }

    printf("\n*****************************************\n");
    printf("*     STARTING DOWNLOAD PHASE            *\n");
    printf("*******************************************\n");
    printf("\n");
    printf("Total pieces: %d\n", ts->total_pieces);
    printf("Piece length: %d bytes\n", ts->piece_length);
    printf("File length: %ld bytes\n\n", ts->meta->file_length);

    // MAIN DOWNLOAD LOOP
    while (1) {
        time_t now = time(NULL);

        // 1. Check if download is complete
        if (all_pieces_downloaded(ts)) {
            double elapsed = get_time_seconds() - ts->download_start_time;
            printf("\n");
            printf("******************************************\n");
            printf("*     DOWNLOAD COMPLETE!                 *\n");
            printf("*     Time: %.2f seconds                 *\n", elapsed);
            printf("******************************************\n");
            printf("\n");
            
            // Return success - main.c will ask about seeding
            return 0;
        }

        // 2. Re-contact tracker for more peers
        if (now - last_tracker_contact > TRACKER_RECONTACT_INTERVAL ||
            last_tracker_contact == 0) {

            if (ts->skip_tracker) {
                printf("\n[PEER MODE] Skipping tracker (peer mode active)\n");
                last_tracker_contact = now;  // Update time 
            } else {
                printf("\n[TRACKER] Contacting tracker...\n");
                TrackerResponse tr;

                if (contact_tracker(ts->meta, &tr) == 0) {
                    printf("[TRACKER] Received %d peers\n", tr.num_peers);

                    int new_connections = 0;
                    for (int i = 0; i < tr.num_peers && ts->peer_count < MAX_PEER_CONNECTIONS; i++) {
                        if (new_connections >= 4) break;
                        if (try_connect_peer(ts, tr.peers[i].ip, tr.peers[i].port) == 0)
                            new_connections++;
                    }
                    tracker_response_free(&tr);
                }
                last_tracker_contact = now;
            }
        }

        // 3. No peers so wait
        if (ts->peer_count == 0) {
            sleep(1);
            continue;
        }

        // 4. SELECT loop setup
        fd_set read_fds;
        fd_set write_fds;
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);

        int max_fd = -1;

        if (ts->listen_fd >= 0) {
            FD_SET(ts->listen_fd, &read_fds);
            max_fd = ts->listen_fd;
        }

        for (int i = 0; i < ts->peer_count; i++) {
            Peer *p = ts->peers[i];
            if (p->socket_fd < 0) continue;

            if (p->state == PEER_CONNECTING) {
                FD_SET(p->socket_fd, &write_fds);
            } else if (p->state == PEER_WAIT_HANDSHAKE_IN ||
                       p->state == PEER_WAIT_HANDSHAKE_OUT ||
                       p->state == PEER_ACTIVE) {
                FD_SET(p->socket_fd, &read_fds);
            }

            if (p->socket_fd > max_fd)
                max_fd = p->socket_fd;
        }

        if (max_fd < 0) {
            cleanup_dead_peers(ts);
            continue;
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 20000;

        int activity = select(max_fd + 1, &read_fds, &write_fds, NULL, &tv);

        if (activity < 0) {
            if (errno == EINTR) continue;
            perror("select");
            continue;
        }

        // 5. idle
        if (activity == 0) {
            int prog = (int)get_download_progress(ts);
            if (prog != last_progress) {
                printf("[PROGRESS] %d%% complete (%d/%d pieces)\n",
                       prog,
                       (prog * ts->total_pieces) / 100,
                       ts->total_pieces);

                last_progress = prog;
            }
            continue;
        }

        // 6. Handle incoming peer connections
        if (ts->listen_fd >= 0 && FD_ISSET(ts->listen_fd, &read_fds)) {
            int new_fd = accept(ts->listen_fd, NULL, NULL);
            if (new_fd >= 0) {
                printf("[LISTEN] Accepted inbound peer (fd=%d)\n", new_fd);

                Peer *p = add_peer(ts, "inbound", 0);
                if (!p) {
                    printf("[LISTEN] Failed to add peer\n");
                    close(new_fd);
                } else {
                    p->socket_fd = new_fd;
                    p->state = PEER_WAIT_HANDSHAKE_OUT;
                    p->am_choking = true;

                    int flags = fcntl(new_fd, F_GETFL, 0);
                    if (flags != -1)
                        fcntl(new_fd, F_SETFL, flags & ~O_NONBLOCK);
                }
            }
        }

        // 7. Handle outgoing connection completion
        for (int i = 0; i < ts->peer_count; i++) {
            Peer *p = ts->peers[i];
            if (p->state != PEER_CONNECTING) continue;
            if (!FD_ISSET(p->socket_fd, &write_fds)) continue;

            int err;
            socklen_t len = sizeof(err);
            getsockopt(p->socket_fd, SOL_SOCKET, SO_ERROR, &err, &len);

            if (err != 0) {
                printf("[CONNECT] Failed %s:%d (%s)\n",
                    p->ip, p->port, strerror(err));
                close(p->socket_fd);
                p->socket_fd = -1;
                p->state = PEER_DISCONNECTED;
                continue;
            }

            printf("[CONNECT] Connected: %s:%d\n", p->ip, p->port);

            unsigned char hs[68] = {0};
            hs[0] = 19;
            memcpy(hs+1, "BitTorrent protocol", 19);
            memcpy(hs+28, ts->meta->info_hash, 20);
            memcpy(hs+48, CLIENT_ID, 20);

            send(p->socket_fd, hs, 68, 0);

            int flags = fcntl(p->socket_fd, F_GETFL, 0);
            if (flags != -1) {
                fcntl(p->socket_fd, F_SETFL, flags & ~O_NONBLOCK);
            }

            p->state = PEER_WAIT_HANDSHAKE_IN;
        }

        // 8. Handle outgoing handshake responses
        for (int i = 0; i < ts->peer_count; i++) {
            Peer *p = ts->peers[i];
            if (p->state != PEER_WAIT_HANDSHAKE_IN ||
                !FD_ISSET(p->socket_fd, &read_fds))
                continue;

            unsigned char hs[68];
            int got = recv(p->socket_fd, hs, 68, 0);

            if (got == 68 &&
                hs[0] == 19 &&
                memcmp(hs+1, "BitTorrent protocol", 19) == 0 &&
                memcmp(hs+28, ts->meta->info_hash, 20) == 0) {

                printf("[HANDSHAKE] OK from %s:%d\n", p->ip, p->port);
                p->state = PEER_ACTIVE;
                p->am_choking = true;  // Start by choking

                // Send  bitfield
                if (ts->my_bitfield_len > 0) {
                    send_bitfield(p, ts);
                    printf("[BITFIELD] Sent to %s:%d\n", p->ip, p->port);
                }

                // Send INTERESTED
                send_interested(p);
                p->am_interested = true;
                printf("[INTEREST] Sent INTERESTED to %s:%d\n", p->ip, p->port);

            } else {
                printf("[HANDSHAKE] Invalid from %s:%d\n", p->ip, p->port);
                close(p->socket_fd);
                p->socket_fd = -1;
                p->state = PEER_DISCONNECTED;
            }
        }

        // 9. Handle inbound handshakes
        for (int i = 0; i < ts->peer_count; i++) {
            Peer *p = ts->peers[i];

            if (p->state != PEER_WAIT_HANDSHAKE_OUT ||
                !FD_ISSET(p->socket_fd, &read_fds))
                continue;

            unsigned char hs[68];
            int got = recv(p->socket_fd, hs, 68, 0);

            if (got == 68 &&
                hs[0] == 19 &&
                memcmp(hs+1, "BitTorrent protocol", 19) == 0 &&
                memcmp(hs+28, ts->meta->info_hash, 20) == 0) {

                printf("[INBOUND-HS] OK from peer\n");

                unsigned char reply[68] = {0};
                reply[0] = 19;
                memcpy(reply+1, "BitTorrent protocol", 19);
                memcpy(reply+28, ts->meta->info_hash, 20);
                memcpy(reply+48, CLIENT_ID, 20);

                send(p->socket_fd, reply, 68, 0);

                p->state = PEER_ACTIVE;
                p->am_choking = true;  // Start by choking

                // Send bitfield
                if (ts->my_bitfield_len > 0) {
                    send_bitfield(p, ts);
                }
            } else {
                printf("[INBOUND-HS] Invalid handshake\n");
                close(p->socket_fd);
                p->socket_fd = -1;
                p->state = PEER_DISCONNECTED;
            }
        }

        // 10. Handle peer messages
        for (int i = 0; i < ts->peer_count; i++) {
            Peer *peer = ts->peers[i];

            if (peer->state == PEER_ACTIVE &&
                FD_ISSET(peer->socket_fd, &read_fds)) {

                handle_peer_message(ts, peer);
            }
        }

        // 11. Request more pieces
        for (int i = 0; i < ts->peer_count; i++) {
            Peer *peer = ts->peers[i];
            if (peer->state == PEER_ACTIVE &&
                peer->socket_fd >= 0 &&
                !peer->is_choked) {

                maybe_request_more(peer, ts);
            }
        }

        // 12. Manage upload slots 
        if (activity > 0) {
            manage_upload_slots(ts);
        }

        // 13. Clean up closed peers
        cleanup_dead_peers(ts);
    }

    return 0;
}
