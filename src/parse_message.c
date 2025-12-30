#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include "parse_message.h"
#include "requestPayload.h"
#include "receive_message.h"
#include "sendRequest.h"
#include "store_pieces.h"
#include "torrent_parser.h"
#include "outgoingMessages.h"

/* parse a full BitTorrent message frame into ParsedMessage  
   returns 0 on success, -1 on error */
int parse_message(unsigned char *buffer, ParsedMessage *out_msg) {
    if (!buffer || !out_msg)
        return -1;

    /* read message length */
    uint32_t net_len;
    memcpy(&net_len, buffer, 4);
    uint32_t msg_len = ntohl(net_len);

    /* keep-alive (no ID, no payload) */
    if (msg_len == 0) {
        out_msg->id = MSG_KEEP_ALIVE;
        out_msg->payload_len = 0;
        out_msg->payload = NULL;
        return 0;
    }

    /* basic sanity check */
    if (msg_len < 1) {
        fprintf(stderr, "Invalid message length: %u\n", msg_len);
        return -1;
    }

    /* message ID byte */
    unsigned char id = buffer[4];
    if (id > 9) {
        fprintf(stderr, "Unknown message ID: %d\n", id);
        return -1;
    }

    out_msg->id = (MessageId)id;

    /* payload starts after the ID */
    out_msg->payload_len = msg_len - 1;
    out_msg->payload = (out_msg->payload_len > 0) ? buffer + 5 : NULL;

    return 0;
}

/* extract fields from a PIECE message  
   returns 0 on success */
int parse_piece_payload(ParsedMessage *msg, PiecePayload *out_piece) {
    if (msg->id != MSG_PIECE || msg->payload_len < 8) {
        return -1;
    }

    /* piece index */
    uint32_t net_index;
    memcpy(&net_index, msg->payload, 4);
    out_piece->index = ntohl(net_index);

    /* block offset */
    uint32_t net_begin;
    memcpy(&net_begin, msg->payload + 4, 4);
    out_piece->begin = ntohl(net_begin);

    /* pointer to block data */
    out_piece->data = msg->payload + 8;

    /* size of data portion */
    out_piece->data_len = msg->payload_len - 8;

    return 0;
}

/* main loop for reading and handling peer messages */
void handle_peer_communication(Peer *peer, TorrentState *ts)
{
    int sock_fd = peer->socket_fd;

    while (1) {
        /* get full message from socket */
        unsigned char *raw_buf = receive_message(sock_fd);
        if (!raw_buf) {
            printf("[PEER] Connection closed or receive error.\n");
            break;
        }

        /* parse message into header + payload */
        ParsedMessage msg;
        if (parse_message(raw_buf, &msg) < 0) {
            printf("[ERROR] Failed to parse peer message.\n");
            free(raw_buf);
            break;
        }

        /* handle message types */
        switch (msg.id)
        {
            case MSG_CHOKE:
                peer->is_choked = true;
                printf("[PEER] Peer is CHOKING us.\n");
                break;

            case MSG_UNCHOKE:
                peer->is_choked = false;
                printf("[PEER] Peer UNCHOKED us.\n");
                request_next_block(peer, ts);
                break;

            case MSG_INTERESTED:
                peer->is_interested = true;
                printf("[PEER] Peer is INTERESTED.\n");
                break;

            case MSG_NOT_INTERESTED:
                peer->is_interested = false;
                printf("[PEER] Peer is NOT interested.\n");
                break;

            case MSG_HAVE: {
                if (msg.payload_len != 4) {
                    printf("[ERROR] HAVE payload wrong size.\n");
                    break;
                }

                uint32_t idx = ntohl(*(uint32_t*)msg.payload);
                printf("[PEER] Peer HAS piece %u\n", idx);

                /* update peer bitfield */
                if (peer->bitfield && idx < peer->bitfield_len * 8) {
                    peer->bitfield[idx/8] |= (1 << (7 - (idx % 8)));
                }
                break;
            }

            case MSG_BITFIELD:
                printf("[PEER] Received BITFIELD (%u bytes)\n", msg.payload_len);

                if (msg.payload_len == 0) break;

                if (peer->bitfield) {
                    free(peer->bitfield);
                    peer->bitfield = NULL;
                }

                peer->bitfield_len = msg.payload_len;
                peer->bitfield = malloc(peer->bitfield_len);
                if (!peer->bitfield) {
                    fprintf(stderr, "malloc failed for bitfield\n");
                    exit(1);
                }

                memcpy(peer->bitfield, msg.payload, peer->bitfield_len);
                break;

            case MSG_PIECE: {
                PiecePayload piece;
                if (parse_piece_payload(&msg, &piece) == 0) {

                    printf("[DOWNLOAD] Received PIECE idx=%u begin=%u size=%u\n",
                           piece.index, piece.begin, piece.data_len);

                    store_received_block(ts, piece.index, piece.begin,
                                         piece.data, piece.data_len);
                }
                break;
            }

            case MSG_REQUEST: {
                RequestPayload req;

                if (parse_request_payload(&msg, &req) == 0) {
                    printf("[UPLOAD] Peer REQUESTS idx=%u begin=%u len=%u\n",
                           req.index, req.begin, req.length);

                    /* only serve pieces we have */
                    if (ts->piece_complete[req.index]) {
                        send_piece(peer, ts, req.index, req.begin, req.length);
                    } else {
                        printf("[UPLOAD] Cannot send; we do NOT have piece %u.\n",
                               req.index);
                    }
                }
                break;
            }

            case MSG_CANCEL:
                printf("[PEER] Received CANCEL\n");
                break;

            case MSG_KEEP_ALIVE:
                printf("[PEER] Keep-alive\n");
                break;

            default:
                printf("[WARNING] Unknown message ID=%d\n", msg.id);
                break;
        }

        free(raw_buf);
    }
}
