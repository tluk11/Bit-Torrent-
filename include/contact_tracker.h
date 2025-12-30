#ifndef CONTACT_TRACKER_H
#define CONTACT_TRACKER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    PEER_DISCONNECTED = 0,      // Peer not connected
    PEER_CONNECTING,            // socket() + connect() started (non-blocking)
    PEER_WAIT_HANDSHAKE_OUT,    // TCP connected -> waiting to send handshake
    PEER_WAIT_HANDSHAKE_IN,     // handshake sent -> waiting for peer handshake
    PEER_ACTIVE,                 // handshake validated -> normal BT messaging
    PEER_ESTABLISHED
} PeerState;

// Peer entry
typedef struct Peer {
    char ip[16];          
    int port;             
    PeerState state;
    int socket_fd;        // TCP connection socket

    // Bitfield received from peer
    uint8_t *bitfield;   
    int bitfield_len;

    // Choking/interested state
    bool am_choking;      // we choke peer
    bool am_interested;   // we are interested in peer
    bool is_choked;       // peer is choking us
    bool is_interested;   // peer is interested in us

    // Download state
    int current_piece;     // -1 = none selected
    int current_offset;    // next block offset to request

    // Peer ID from handshake
    unsigned char peer_id[20];
    int outstanding_requests;
    int max_pipeline;
} Peer;


// TrackerInfo — used when sending GET request
typedef struct {
    char announce[512];       // announce URL
    char host[256];
    int port;
    char port_str[16];
    char path[512];

    unsigned char info_hash[20];
    unsigned char peer_id[20];

    long uploaded;
    long downloaded;
    long left;

    const char *event;        // "started", "stopped", etc.
} TrackerInfo;

//
// TrackerResponse — parsed response from tracker
//
typedef struct {
    int interval;
    int complete;
    int incomplete;

    int num_peers;
    Peer *peers;

    char *tracker_id;
    char *warning_message;
    char *failure_reason;
} TrackerResponse;

struct TorrentInfo;
struct TorrentState;

int contact_tracker(const struct TorrentInfo *ti, TrackerResponse *tr);
void tracker_response_free(TrackerResponse *tr);

int sendGETRequest(TrackerInfo *ti, unsigned char **response, size_t *response_len);
int tracker_parse_response(unsigned char *resp, size_t resp_len, TrackerResponse *tr);

#endif
