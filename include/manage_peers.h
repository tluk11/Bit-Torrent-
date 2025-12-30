#ifndef MANAGE_PEERS_H
#define MANAGE_PEERS_H

#include "torrent_parser.h"



Peer *add_peer(TorrentState *ts, const char *ip, int port);
void remove_peer(TorrentState *ts, int index);
void cleanup_dead_peers(TorrentState *ts);
void start_peer_listener();

Peer *find_peer_by_fd(TorrentState *ts, int fd);

#endif
