#ifndef RECEIVE_MESSAGE_H
#define RECEIVE_MESSAGE_H

#include <stdint.h>
#include <stddef.h>

// saferecv from your implementation
int safe_recv(int sock_fd, unsigned char *buffer, size_t n_bytes);

// receive a full BitTorrent message frame
unsigned char* receive_message(int sock_fd);

#endif
