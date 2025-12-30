
#include <stddef.h>
#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>


#include <stdlib.h>     
#include <string.h>     
#include <sys/socket.h> 
#include <arpa/inet.h>  
#include "receive_message.h"

void print_hex(const unsigned char *buf, size_t len) {
    for (size_t i = 0; i < len; i++)
        printf("%02X ", buf[i]);
    printf("\n");
}

// function that safely read specific number of bytes from socket 
// returns -1 on error, 0 on peer close, or the number of bytes read on success 
int safe_recv(int sock_fd, unsigned char *buffer, size_t n_bytes) {
    size_t received = 0;

    while (received < n_bytes) {
        ssize_t n = recv(sock_fd, buffer + received, n_bytes - received, 0);

        if (n == 0)
            return 0;      

        if (n < 0)
            return -1;     

        received += n;
    }
    return received;
}
// receives message and returns the buffer that the message is stored in
// still need to create a function that reads the message content 
unsigned char* receive_message(int sock_fd) {
    uint32_t len_net;
    int result = safe_recv(sock_fd, (unsigned char*)&len_net, 4);

    if (result <= 0) {
        if (result < 0) perror("recv length prefix");
        return NULL; // peer closed or error
    }

    uint32_t len_host = ntohl(len_net);

    // Safety guard
    if (len_host > (1 << 20)) { // 1 MB max frame
        fprintf(stderr, "Peer sent invalid length: %u\n", len_host);
        return NULL;
    }

    // KEEP-ALIVE message (no id byte)
    if (len_host == 0) {
        unsigned char *buf = malloc(4);
        if (!buf) return NULL;

        uint32_t zero = 0;
        memcpy(buf, &zero, 4);

        printf("Received KEEP-ALIVE\n");
        return buf;
    }

    // Allocate full message (prefix + id + payload)
    size_t full_len = 4 + len_host;
    unsigned char *buf = malloc(full_len);

    if (!buf) {
        perror("malloc");
        return NULL;
    }

    // Copy prefix
    memcpy(buf, &len_net, 4);

    // Read ID + payload
    result = safe_recv(sock_fd, buf + 4, len_host);

    if (result != (int)len_host) {
        fprintf(stderr, "recv payload failed (%d/%u)\n", result, len_host);
        printf("Partial data:\n");
        print_hex(buf, 4 + (result > 0 ? result : 0));  // print what you have
        free(buf);
        return NULL;
    }

    return buf;
}
