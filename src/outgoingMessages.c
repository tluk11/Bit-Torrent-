#include <stdint.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include "contact_tracker.h"
#include "torrent_parser.h"
#include "store_pieces.h"
#include "outgoingMessages.h"

static long total_uploaded_bytes = 0;

// KEEP-ALIVE (length = 0)
int send_keep_alive(Peer *peer) {
    uint32_t zero = 0;
    return send(peer->socket_fd, &zero, 4, 0) == 4 ? 0 : -1;
}

// CHOKE (id = 0)
int send_choke(Peer *peer) {
    uint32_t len = htonl(1);
    unsigned char msg[5] = {0}; // ID = 0

    memcpy(msg, &len, 4);
    msg[4] = 0;

    return send(peer->socket_fd, msg, 5, 0) == 5 ? 0 : -1;
}

// UNCHOKE (id = 1)
int send_unchoke(Peer *peer) {
    uint32_t len = htonl(1);
    unsigned char msg[5] = {0};

    memcpy(msg, &len, 4);
    msg[4] = 1;

    return send(peer->socket_fd, msg, 5, 0) == 5 ? 0 : -1;
}

// INTERESTED (id = 2)
int send_interested(Peer *peer) {
    uint32_t len = htonl(1);
    unsigned char msg[5];

    memcpy(msg, &len, 4);
    msg[4] = 2;

    return send(peer->socket_fd, msg, 5, 0) == 5 ? 0 : -1;
}

// NOT INTERESTED (id = 3)
int send_not_interested(Peer *peer) {
    uint32_t len = htonl(1);
    unsigned char msg[5];

    memcpy(msg, &len, 4);
    msg[4] = 3;

    return send(peer->socket_fd, msg, 5, 0) == 5 ? 0 : -1;
}

// HAVE (id = 4)
int send_have(Peer *peer, uint32_t piece_index) {
    uint32_t len = htonl(5);
    uint32_t net_index = htonl(piece_index);

    unsigned char msg[9];
    memcpy(msg, &len, 4);
    msg[4] = 4; 
    memcpy(msg + 5, &net_index, 4);

    return send(peer->socket_fd, msg, 9, 0) == 9 ? 0 : -1;
}

// BITFIELD (id = 5)
int send_bitfield(Peer *peer, TorrentState *ts) {
    if (!peer || !ts || !ts->my_bitfield) {
        fprintf(stderr, " Invalid parameters\n");
        return -1;
    }
    
    printf(" send_bitfield called for %s:%d, bitfield_len=%d\n", 
           peer->ip, peer->port, ts->my_bitfield_len);
    int bf_len = ts->my_bitfield_len;
    uint32_t len = htonl(1 + bf_len);

    unsigned char *msg = malloc(4 + 1 + bf_len);
    if (!msg) {
        fprintf(stderr, " malloc failed\n");
        return -1;
    }
    
    memcpy(msg, &len, 4);
    msg[4] = 5; 
    memcpy(msg + 5, ts->my_bitfield, bf_len);

    int sent = send(peer->socket_fd, msg, 5 + bf_len, 0);
    free(msg);

    return (sent == 5 + bf_len) ? 0 : -1;
}

// tell peers what we have
int broadcast_have(TorrentState *ts, int piece_index) {
    if (!ts) return -1;
    
    for (int i = 0; i < ts->peer_count; i++) {
        Peer *p = ts->peers[i];
        if (!p || p->socket_fd < 0) continue;
        
        printf(" Broadcasting HAVE %d to %s:%d\n",
               piece_index, p->ip, p->port);
        
        send_have(p, (uint32_t)piece_index);
    }
    return 0;
}

// BITFIELD (id = 7)
int send_piece(Peer *peer, TorrentState *ts, int index, int begin, int length)
{
    if (!peer || peer->socket_fd < 0) {
        fprintf(stderr, "[PIECE] Invalid peer or socket\n");
        return -1;
    }
    
    if (!ts || !ts->pieces) {
        fprintf(stderr, "[PIECE] Invalid TorrentState or pieces not initialized\n");
        return -1;
    }
    
    if (index < 0 || index >= ts->total_pieces) {
        fprintf(stderr, "[PIECE] Invalid piece index: %d\n", index);
        return -1;
    }

    PieceBuffer *pb = &ts->pieces[index];
    
    if (!pb->data) {
        fprintf(stderr, "[PIECE] Piece %d data is NULL\n", index);
        return -1;
    }
    
    if (!pb->verified) {
        fprintf(stderr, "[PIECE] Piece %d not verified\n", index);
        return -1;
    }
    
    if (begin < 0 || length <= 0 || begin + length > pb->length) {
        fprintf(stderr, "[PIECE] Invalid block: begin=%d + length=%d > piece_length=%d\n",
                begin, length, pb->length);
        return -1;
    }

    
    uint32_t msg_len = htonl(9 + length);
    unsigned char *msg = malloc(4 + 9 + length);
    if (!msg) {
        fprintf(stderr, "[PIECE] malloc failed for %d bytes\n", 4 + 9 + length);
        return -1;
    }

    // Construct message
    memcpy(msg, &msg_len, 4);                   
    msg[4] = 7;                                  

    uint32_t index_be = htonl(index);
    uint32_t begin_be = htonl(begin);

    memcpy(msg+5, &index_be, 4);               
    memcpy(msg+9, &begin_be, 4);               
    memcpy(msg+13, pb->data + begin, length);  

    // Send message
    int sent = send(peer->socket_fd, msg, 4 + 9 + length, 0);
    free(msg);

    if (sent <= 0) {
        fprintf(stderr, " Failed to send to %s:%d\n", peer->ip, peer->port);
        return -1;
    }

    if (sent != 4 + 9 + length) {
        fprintf(stderr, " Partial send: %d/%d bytes\n", sent, 4 + 9 + length);
        return -1;
    }

    total_uploaded_bytes += length;
    printf("  Sent piece=%d begin=%d length=%d to %s:%d (total: %ld bytes)\n",
           index, begin, length, peer->ip, peer->port, total_uploaded_bytes);

    return 0;
}