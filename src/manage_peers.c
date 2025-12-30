#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "handshake_with_peer.h"
#include "manage_peers.h"
#include "contact_tracker.h"

#include "manage_peers.h"
#include "outgoingMessages.h"
#include "global_state.h"




// Expand peer list if needed
static void ensure_capacity(TorrentState *ts) {
    if (ts->peer_count < ts->peer_capacity)
        return;

    ts->peer_capacity *= 2;
    ts->peers = realloc(ts->peers, ts->peer_capacity * sizeof(Peer *));
}

// Add a peer to the state
Peer *add_peer(TorrentState *ts, const char *ip, int port) {
    ensure_capacity(ts);

    Peer *p = malloc(sizeof(Peer));
    memset(p, 0, sizeof(Peer));

    strcpy(p->ip, ip);
    p->port = port;
    p->am_choking = true;      // We start by choking
    p->socket_fd = -1;
    p->is_choked = true;   
    p->am_interested    = false;
    p->outstanding_requests = 0;
    p->max_pipeline = 50;     
    p->state = PEER_DISCONNECTED;

    ts->peers[ts->peer_count++] = p;

    printf(" Added peer %s:%d (total=%d)\n",
           ip, port, ts->peer_count);

    return p;
}

// Remove peer by index
void remove_peer(TorrentState *ts, int index) {
    if (index < 0 || index >= ts->peer_count)
        return;

    Peer *p = ts->peers[index];

    printf(" Removing peer %s:%d\n", p->ip, p->port);

    if (p->socket_fd >= 0)
        close(p->socket_fd);

    free(p->bitfield);
    free(p);

    // shift peers left
    for (int i = index; i < ts->peer_count - 1; i++) {
        ts->peers[i] = ts->peers[i + 1];
    }

    ts->peer_count--;
}

// Find a peer by its socket FD
Peer *find_peer_by_fd(TorrentState *ts, int fd) {
    for (int i = 0; i < ts->peer_count; i++) {
        if (ts->peers[i]->socket_fd == fd)
            return ts->peers[i];
    }
    return NULL;
}

// Remove any peers that disconnected (socket_fd < 0)
void cleanup_dead_peers(TorrentState *ts) {
    for (int i = ts->peer_count - 1; i >= 0; i--) {
        if (ts->peers[i]->socket_fd < 0) {
            remove_peer(ts, i);
        }
    }
}
