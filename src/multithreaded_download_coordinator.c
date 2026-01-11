// multithreaded_download_coordinator.c
// Thread-safe BitTorrent downloader with worker threads

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
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
#define TRACKER_RECONTACT_INTERVAL 1800
#define NUM_WORKER_THREADS 4  // Number of download threads

// Thread-safe wrapper for critical sections
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t disk_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work_available = PTHREAD_COND_INITIALIZER;
static bool shutdown_flag = false;

// Worker thread data
typedef struct {
    int thread_id;
    TorrentState *ts;
    pthread_t pthread;
} WorkerThread;

static unsigned char CLIENT_ID[20] = "-TC0001-123456789012";

// ============================================================================
// Thread-safe helper functions
// ============================================================================

static void update_my_bitfield_safe(TorrentState *ts, int piece_index) {
    if (!ts->my_bitfield || piece_index < 0 || piece_index >= ts->total_pieces) {
        return;
    }
    
    pthread_mutex_lock(&state_mutex);
    
    int byte = piece_index / 8;
    int bit = 7 - (piece_index % 8);
    
    if (byte < ts->my_bitfield_len) {
        ts->my_bitfield[byte] |= (1 << bit);
        //printf("[THREAD-%d] [BITFIELD] Updated: we now have piece %d\n", 
        //       pthread_self() % 100, piece_index);
    }
    
    pthread_mutex_unlock(&state_mutex);
}

static void broadcast_have_safe(TorrentState *ts, int piece_index) {
    pthread_mutex_lock(&state_mutex);
    
    for (int i = 0; i < ts->peer_count; i++) {
        Peer *p = ts->peers[i];
        if (p->state == PEER_ACTIVE && p->socket_fd >= 0) {
            send_have(p, piece_index);
        }
    }
    
    pthread_mutex_unlock(&state_mutex);
}

static void manage_upload_slots_safe(TorrentState *ts) {
    pthread_mutex_lock(&state_mutex);
    
    int unchoked_count = 0;
    for (int i = 0; i < ts->peer_count && unchoked_count < 4; i++) {
        Peer *p = ts->peers[i];
        
        if (p->state != PEER_ACTIVE || p->socket_fd < 0) {
            continue;
        }
        
        if (p->is_interested && p->am_choking) {
            send_unchoke(p);
            p->am_choking = false;
            unchoked_count++;
        } else if (!p->am_choking) {
            unchoked_count++;
        }
    }
    
    pthread_mutex_unlock(&state_mutex);
}

// ============================================================================
// Core download functions (same as before, but with locking)
// ============================================================================

static int setup_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket(listen)");
        return -1;
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

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

bool all_pieces_downloaded1(TorrentState *ts) {
    pthread_mutex_lock(&state_mutex);
    bool complete = true;
    for (int i = 0; i < ts->total_pieces; i++) {
        if (!ts->piece_complete[i]) {
            complete = false;
            break;
        }
    }
    pthread_mutex_unlock(&state_mutex);
    return complete;
}

static bool peer_can_request_more(Peer *peer, TorrentState *ts) {
    if (!peer || !ts) return false;
    if (peer->socket_fd < 0) return false;
    if (peer->is_choked) return false;
    if (!peer->am_interested) return false;
    if (peer->outstanding_requests >= peer->max_pipeline) return false;
    if (!peer->bitfield || !ts->piece_complete) return false;

    for (int i = 0; i < ts->total_pieces; i++) {
        if (ts->piece_complete[i]) continue;

        int byte = i / 8;
        int bit  = 7 - (i % 8);

        if (byte < peer->bitfield_len &&
            (peer->bitfield[byte] & (1 << bit))) {
            return true;
        }
    }

    return false;
}

static void maybe_request_more(Peer *peer, TorrentState *ts, int thread_id) {
    if (!peer_can_request_more(peer, ts)) return;

    for (int i = 0; i < 4; i++) {
        if (peer_can_request_more(peer, ts)) {
            request_next_block(peer, ts);
        }
    }
}

static int try_connect_peer(TorrentState *ts, const char *ip, int port) {
    Peer *peer = add_peer(ts, ip, port);
    if (!peer) return -1;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    fcntl(sock, F_SETFL, O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);

    int r = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    peer->state = PEER_CONNECTING;

    if (r < 0 && errno != EINPROGRESS) {
        close(sock);
        peer->state = PEER_DISCONNECTED;
        return -1;
    }

    peer->socket_fd = sock;
    peer->state = PEER_CONNECTING;
    return 0;
}

// ============================================================================
// Handle peer message (with thread safety)
// ============================================================================
static void handle_peer_message(TorrentState *ts, Peer *peer, int thread_id) {
    unsigned char *raw_buf = receive_message(peer->socket_fd);
    if (!raw_buf) {
        printf(" [PEER %s:%d] Connection closed\n", 
                peer->ip, peer->port);
        peer->socket_fd = -1;
        return;
    }
    
    ParsedMessage msg;
    if (parse_message(raw_buf, &msg) < 0) {
        free(raw_buf);
        peer->socket_fd = -1;
        return;
    }
    
    switch (msg.id) {
        case MSG_CHOKE:
            peer->is_choked = true;
            peer->outstanding_requests = 0;
            printf(" [PEER %s:%d] CHOKE\n",  peer->ip, peer->port);
            break;
            
        case MSG_UNCHOKE:
            peer->is_choked = false;
            peer->outstanding_requests = 0;
            printf(" [PEER %s:%d] UNCHOKE\n",  peer->ip, peer->port);
            maybe_request_more(peer, ts, thread_id);
            break;
            
        case MSG_INTERESTED:
            peer->is_interested = true;
            printf(" [PEER %s:%d] INTERESTED\n",  peer->ip, peer->port);
            manage_upload_slots_safe(ts);
            break;
            
        case MSG_NOT_INTERESTED:
            peer->is_interested = false;
            break;
            
        case MSG_HAVE: {
            if (msg.payload_len == 4) {
                uint32_t idx = ntohl(*(uint32_t*)msg.payload);
                pthread_mutex_lock(&state_mutex);
                if (peer->bitfield && idx < peer->bitfield_len * 8) {
                    peer->bitfield[idx/8] |= (1 << (7 - (idx % 8)));
                }
                pthread_mutex_unlock(&state_mutex);
            }
            break;
        }
        
        case MSG_BITFIELD: {
            pthread_mutex_lock(&state_mutex);
            
            if (peer->bitfield) free(peer->bitfield);
            peer->bitfield_len = msg.payload_len;
            peer->bitfield = malloc(msg.payload_len);
            
            if (peer->bitfield) {
                memcpy(peer->bitfield, msg.payload, msg.payload_len);
                
                bool interesting = false;
                for (int i = 0; i < ts->total_pieces; i++) {
                    if (ts->piece_complete[i]) continue;
                    if (peer_has_piece(peer, i)) {
                        interesting = true;
                        break;
                    }
                }
                
                pthread_mutex_unlock(&state_mutex);
                
                if (interesting) {
                    send_interested(peer);
                    peer->am_interested = true;
                } else {
                    send_not_interested(peer);
                    peer->am_interested = false;
                }
            } else {
                pthread_mutex_unlock(&state_mutex);
            }
            break;
        }
            
        case MSG_PIECE: {
            PiecePayload piece;
            if (parse_piece_payload(&msg, &piece) == 0) {
                // Lock for disk write
                pthread_mutex_lock(&disk_mutex);
                store_received_block(ts, piece.index, piece.begin, piece.data, piece.data_len);
                pthread_mutex_unlock(&disk_mutex);
                
                pthread_mutex_lock(&state_mutex);
                peer->outstanding_requests--;
                if (peer->outstanding_requests < 0)
                    peer->outstanding_requests = 0;
                
                PieceState *ps = &ts->piece_states[piece.index];
                int b = piece.begin / BLOCK_SIZE;

                if (b < ps->total_blocks) {
                    ps->have_block[b] = 1;
                    ps->requested_block[b] = 0;
                    ps->received_blocks++;

                    if (ps->received_blocks == ps->total_blocks) {
                        ts->piece_complete[piece.index] = true;
                        pthread_mutex_unlock(&state_mutex);
                        
                        update_my_bitfield_safe(ts, piece.index);
                        broadcast_have_safe(ts, piece.index);
                        
                        printf(" [PIECE] Completed piece %u\n", 
                               piece.index);
                    } else {
                        pthread_mutex_unlock(&state_mutex);
                    }
                } else {
                    pthread_mutex_unlock(&state_mutex);
                }

                maybe_request_more(peer, ts, thread_id);
            }
            break;
        }
        
        case MSG_REQUEST: {
            RequestPayload req;
            if (parse_request_payload(&msg, &req) == 0) {
                if (!peer->am_choking && 
                    req.index < ts->total_pieces && 
                    ts->piece_complete[req.index] &&
                    req.length <= 16384) {
                    
                    pthread_mutex_lock(&disk_mutex);
                    send_piece(peer, ts, req.index, req.begin, req.length);
                    pthread_mutex_unlock(&disk_mutex);
                    
                    pthread_mutex_lock(&state_mutex);
                    ts->bytes_uploaded += req.length;
                    pthread_mutex_unlock(&state_mutex);
                }
            }
            break;
        }
        
        default:
            break;
    }
    
    free(raw_buf);
}

// ============================================================================
// Worker thread function
// ============================================================================
void* worker_thread_func(void* arg) {
    WorkerThread *worker = (WorkerThread*)arg;
    TorrentState *ts = worker->ts;
    int thread_id = worker->thread_id;
    
    
    while (!shutdown_flag) {
        pthread_mutex_lock(&state_mutex);
        
        // Find peers for this thread to handle
        fd_set read_fds, write_fds;
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        int max_fd = -1;
        
        // Each thread handles a subset of peers
        for (int i = thread_id; i < ts->peer_count; i += NUM_WORKER_THREADS) {
            Peer *p = ts->peers[i];
            if (p->socket_fd < 0) continue;
            
            if (p->state == PEER_CONNECTING) {
                FD_SET(p->socket_fd, &write_fds);
            } else if (p->state == PEER_WAIT_HANDSHAKE_IN || p->state == PEER_ACTIVE) {
                FD_SET(p->socket_fd, &read_fds);
            }
            
            if (p->socket_fd > max_fd) max_fd = p->socket_fd;
        }
        
        pthread_mutex_unlock(&state_mutex);
        
        if (max_fd < 0) {
            usleep(100000); // 100ms
            continue;
        }
        
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50000; // 50ms timeout
        
        int activity = select(max_fd + 1, &read_fds, &write_fds, NULL, &tv);
        
        if (activity <= 0) continue;
        
        pthread_mutex_lock(&state_mutex);
        
        // Handle peer activity
        for (int i = thread_id; i < ts->peer_count; i += NUM_WORKER_THREADS) {
            Peer *p = ts->peers[i];
            if (p->socket_fd < 0) continue;
            
            // Handle connection completion
            if (p->state == PEER_CONNECTING && FD_ISSET(p->socket_fd, &write_fds)) {
                int err;
                socklen_t len = sizeof(err);
                getsockopt(p->socket_fd, SOL_SOCKET, SO_ERROR, &err, &len);
                
                if (err == 0) {
                    unsigned char hs[68] = {0};
                    hs[0] = 19;
                    memcpy(hs+1, "BitTorrent protocol", 19);
                    memcpy(hs+28, ts->meta->info_hash, 20);
                    memcpy(hs+48, CLIENT_ID, 20);
                    
                    pthread_mutex_unlock(&state_mutex);
                    send(p->socket_fd, hs, 68, 0);
                    pthread_mutex_lock(&state_mutex);
                    
                    int flags = fcntl(p->socket_fd, F_GETFL, 0);
                    if (flags != -1) fcntl(p->socket_fd, F_SETFL, flags & ~O_NONBLOCK);
                    
                    p->state = PEER_WAIT_HANDSHAKE_IN;
                } else {
                    close(p->socket_fd);
                    p->socket_fd = -1;
                    p->state = PEER_DISCONNECTED;
                }
            }
            
            // Handle handshake response
            else if (p->state == PEER_WAIT_HANDSHAKE_IN && FD_ISSET(p->socket_fd, &read_fds)) {
                unsigned char hs[68];
                pthread_mutex_unlock(&state_mutex);
                int got = recv(p->socket_fd, hs, 68, 0);
                pthread_mutex_lock(&state_mutex);
                
                if (got == 68 && hs[0] == 19 &&
                    memcmp(hs+1, "BitTorrent protocol", 19) == 0 &&
                    memcmp(hs+28, ts->meta->info_hash, 20) == 0) {
                    
                    p->state = PEER_ACTIVE;
                    p->am_choking = true;
                    
                    pthread_mutex_unlock(&state_mutex);
                    
                    if (ts->my_bitfield_len > 0) {
                        send_bitfield(p, ts);
                    }
                    send_interested(p);
                    p->am_interested = true;
                    
                    pthread_mutex_lock(&state_mutex);
                } else {
                    close(p->socket_fd);
                    p->socket_fd = -1;
                    p->state = PEER_DISCONNECTED;
                }
            }
            
            // Handle normal messages
            else if (p->state == PEER_ACTIVE && FD_ISSET(p->socket_fd, &read_fds)) {
                pthread_mutex_unlock(&state_mutex);
                handle_peer_message(ts, p, thread_id);
                pthread_mutex_lock(&state_mutex);
            }
        }
        
        pthread_mutex_unlock(&state_mutex);
        
        // Request more blocks
        pthread_mutex_lock(&state_mutex);
        for (int i = thread_id; i < ts->peer_count; i += NUM_WORKER_THREADS) {
            Peer *p = ts->peers[i];
            if (p->state == PEER_ACTIVE && p->socket_fd >= 0 && !p->is_choked) {
                pthread_mutex_unlock(&state_mutex);
                maybe_request_more(p, ts, thread_id);
                pthread_mutex_lock(&state_mutex);
            }
        }
        pthread_mutex_unlock(&state_mutex);
    }
    
    return NULL;
}

// ============================================================================
// Main download function with multithreading
// ============================================================================
int download_torrent_multithreaded(TorrentState *ts) {
    time_t last_tracker_contact = 0;
    
    ts->listen_fd = setup_listen_socket(ts->listen_port);
    if (ts->listen_fd >= 0) {
        printf("[LISTEN] Accepting peers on port %d (fd=%d)\n",
               ts->listen_port, ts->listen_fd);
    }

   

    // Create worker threads
    WorkerThread workers[NUM_WORKER_THREADS];
    for (int i = 0; i < NUM_WORKER_THREADS; i++) {
        workers[i].thread_id = i;
        workers[i].ts = ts;
        pthread_create(&workers[i].pthread, NULL, worker_thread_func, &workers[i]);
    }

    // Main thread handles tracker and progress
    while (1) {
        time_t now = time(NULL);

        // Check completion
        if (all_pieces_downloaded1(ts)) {
            double elapsed = get_time_seconds() - ts->download_start_time;
            printf("\n╔════════════════════════════════════════╗\n");
            printf("║     DOWNLOAD COMPLETE!                 ║\n");
            printf("║     Time: %.2f seconds                 ║\n", elapsed);
            printf("╚════════════════════════════════════════╝\n\n");
            
            shutdown_flag = true;
            break;
        }

        // Contact tracker
        if (now - last_tracker_contact > TRACKER_RECONTACT_INTERVAL ||
            last_tracker_contact == 0) {
            
            if (!ts->skip_tracker) {
                printf("\n[TRACKER] Contacting tracker...\n");
                TrackerResponse tr;
                if (contact_tracker(ts->meta, &tr) == 0) {
                    printf("[TRACKER] Received %d peers\n", tr.num_peers);
                    
                    pthread_mutex_lock(&state_mutex);
                    for (int i = 0; i < tr.num_peers && ts->peer_count < MAX_PEER_CONNECTIONS; i++) {
                        try_connect_peer(ts, tr.peers[i].ip, tr.peers[i].port);
                    }
                    pthread_mutex_unlock(&state_mutex);
                    
                    tracker_response_free(&tr);
                }
            }
            last_tracker_contact = now;
        }

        sleep(1);
    }

    // Wait for threads to finish
    for (int i = 0; i < NUM_WORKER_THREADS; i++) {
        pthread_join(workers[i].pthread, NULL);
    }

    return 0;
}