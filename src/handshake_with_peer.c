#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include "contact_tracker.h"
#include <sys/time.h>
#include <fcntl.h>
#include "torrent_parser.h"


#define PSTR_LEN 19 
#define HANDSHAKE_LEN 68
#define PROTOCOL_STRING "BitTorrent protocol"
#define TIMEOUT_SECONDS 5

typedef struct{
    unsigned char pstrlen;
    char pstr[PSTR_LEN];
    unsigned char reserved[8];
    unsigned char info_hash[20];
    unsigned char peer_id[20];
} handshake_t;

int connect_and_handshake(Peer *peer,
                          const unsigned char *info_hash,
                          const unsigned char *client_id)
{
    int sock_fd;
    struct sockaddr_in serv_addr;
    unsigned char hs[HANDSHAKE_LEN]; // 68 bytes
    unsigned char recv_hs[HANDSHAKE_LEN]; // 68 bytes

    //  1. Create Socket 
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return -1;
    }

    // Remember original flags to restore later
    int flags = fcntl(sock_fd, F_GETFL, 0);
    if (flags < 0) {
        perror("fcntl(F_GETFL)");
        close(sock_fd);
        return -1;
    }

    // Set non-blocking for async connect
    if (fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl(F_SETFL O_NONBLOCK)");
        close(sock_fd);
        return -1;
    }

    //  2. Prepare Address 
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(peer->port);

    if (inet_pton(AF_INET, peer->ip, &serv_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sock_fd);
        return -1;
    }

    //  3. Start non-blocking connect 
    int r = connect(sock_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
    if (r < 0) {
        if (errno != EINPROGRESS) {
            // Immediate failure
            fprintf(stderr, "Connect failed %s:%d (%s)\n",
                    peer->ip, peer->port, strerror(errno));
            close(sock_fd);
            return -1;
        }

        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock_fd, &wfds);

        struct timeval tv;
        tv.tv_sec  = TIMEOUT_SECONDS;
        tv.tv_usec = 0;

        r = select(sock_fd + 1, NULL, &wfds, NULL, &tv);
        if (r <= 0) {
            if (r == 0) {
                fprintf(stderr, "Connect timeout to %s:%d\n",
                        peer->ip, peer->port);
            } else {
                perror("select on connect");
            }
            close(sock_fd);
            return -1;
        }

        // Check for actual connect error 
        int err = 0;
        socklen_t errlen = sizeof(err);
        if (getsockopt(sock_fd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0) {
            perror("getsockopt(SO_ERROR)");
            close(sock_fd);
            return -1;
        }
        if (err != 0) {
            fprintf(stderr, "Connect failed %s:%d (%s)\n",
                    peer->ip, peer->port, strerror(err));
            close(sock_fd);
            return -1;
        }
    }


    // Restore original flags back to blocking mode
    if (fcntl(sock_fd, F_SETFL, flags) < 0) {
        perror("fcntl(restore flags)");
        close(sock_fd);
        return -1;
    }

    struct timeval tv2 = {TIMEOUT_SECONDS, 0};
    setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv2, sizeof tv2);
    setsockopt(sock_fd, SOL_SOCKET, SO_SNDTIMEO, &tv2, sizeof tv2);

    //  4. Build  Handshake 
    memset(hs, 0, HANDSHAKE_LEN);

    hs[0] = 19;  // pstrlen
    memcpy(hs + 1, "BitTorrent protocol", 19);
    memset(hs + 20, 0, 8);          // reserved
    memcpy(hs + 28, info_hash, 20);
    memcpy(hs + 48, client_id, 20);

    //  5. Send Handshake 
    ssize_t to_send = HANDSHAKE_LEN;
    ssize_t sent_total = 0;
    while (sent_total < to_send) {
        ssize_t n = send(sock_fd, hs + sent_total, to_send - sent_total, 0);
        if (n < 0) {
            perror("send handshake");
            close(sock_fd);
            return -1;
        }
        if (n == 0) {
            fprintf(stderr, "send handshake: peer closed connection\n");
            close(sock_fd);
            return -1;
        }
        sent_total += n;
    }

    //  6. Receive Handshake 
    int received = 0;
    while (received < HANDSHAKE_LEN) {
        int n = recv(sock_fd, recv_hs + received, HANDSHAKE_LEN - received, 0);

        if (n <= 0) {
            fprintf(stderr, "recv handshake failed (%s)\n",
                    n == 0 ? "peer closed" : strerror(errno));
            close(sock_fd);
            return -1;
        }
        received += n;
    }

    //  7. Validate Handshake 
    if (recv_hs[0] != 19) {
        fprintf(stderr, "Invalid pstrlen\n");
        close(sock_fd);
        return -1;
    }

    if (memcmp(recv_hs + 1, "BitTorrent protocol", 19) != 0) {
        fprintf(stderr, "Invalid protocol string\n");
        close(sock_fd);
        return -1;
    }

    if (memcmp(recv_hs + 28, info_hash, 20) != 0) {
        fprintf(stderr, "Info-hash mismatch\n");
        close(sock_fd);
        return -1;
    }

    printf("Handshake OK with %s:%d\n", peer->ip, peer->port);
    return sock_fd;
}

int recv_handshake(int fd, TorrentState *ts) {
    unsigned char buf[68];
    int received = 0;

    while (received < 68) {
        int n = recv(fd, buf + received, 68 - received, 0);
        if (n <= 0) {
            return -1; // failed or closed
        }
        received += n;
    }

    // Validate protocol name
    if (buf[0] != 19 || memcmp(buf + 1, "BitTorrent protocol", 19) != 0)
        return -1;

    // Validate info hash
    if (memcmp(buf + 28, ts->meta->info_hash, 20) != 0)
        return -1;


    //  send handshake back to the peer
    unsigned char reply[68] = {0};
    reply[0] = 19;
    memcpy(reply + 1, "BitTorrent protocol", 19);
    memcpy(reply + 28, ts->meta->info_hash, 20);
    memcpy(reply + 48, ts->client_id, 20);

    if (send(fd, reply, 68, 0) != 68)
        return -1;

    return 0; // success
}

