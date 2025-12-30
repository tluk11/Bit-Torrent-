#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>

#include "torrent_parser.h"
#include "init_torrent_state.h"
#include "download_coordinator.h"
#include "contact_tracker.h"
#include "manage_peers.h"
#include "global_state.h"
#include "upload_manager.h"

TorrentState *g_torrent_state = NULL;

// exit loop
void signal_handler(int sig) {
    printf("\n Caught %d, shutting down...\n", sig);
    if (g_torrent_state) {
        cleanup_torrent_state(g_torrent_state);
    }
    exit(0);
}

// print info
void print_torrent_info(TorrentInfo *ti) {
    printf("\n=== Torrent Information ===\n");
    printf("Name: %s\n", ti->name);
    printf("File size: %ld bytes (%.2f MB)\n",
           ti->file_length,
           ti->file_length / (1024.0 * 1024.0));
    printf("Piece length: %d bytes (%.2f KB)\n",
           ti->piece_length,
           ti->piece_length / 1024.0);
    printf("Number of pieces: %d\n", ti->num_pieces);

    printf("\nInfo hash: ");
    for (int i = 0; i < 20; i++) printf("%02x", ti->info_hash[i]);
    printf("\n\n");
}

void run_torrent_session(const char *torrent_file, int port) {
    printf("\n=== BitTorrent Client ===\n");
    printf("Loading: %s\n", torrent_file);
    printf("Port: %d\n", port);

    // 1. Parse the torrent 
    TorrentInfo ti;
    if (torrentparser(torrent_file, &ti) != 0) {
        printf(" Failed to parse %s\n", torrent_file);
        return;
    }
    print_torrent_info(&ti);

    // 2. Init state 
    TorrentState ts;
    if (init_torrent_state(&ts, &ti,port) != 0) {
        printf(" Failed to init torrent state\n");
        torrent_info_free(&ti);
        return;
    }
    g_torrent_state = &ts;

    // Set port
    ts.listen_port = port;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    ts.download_start_time = tv.tv_sec + tv.tv_usec / 1e6;
    ts.last_speed_print = ts.download_start_time;
    ts.bytes_downloaded = 0;
    ts.bytes_uploaded = 0;

    printf("✓ Initialized %d pieces\n", ts.total_pieces);

    // 3. Tracker 
    printf("\n Contacting tracker...\n");
    TrackerResponse tr;
    if (contact_tracker(&ti, &tr) != 0) {
        printf(" tracker failed\n");
        cleanup_torrent_state(&ts);
        torrent_info_free(&ti);
        return;
    }
    printf(" Received %d peers\n", tr.num_peers);
    tracker_response_free(&tr);

    // 4. Download 
    printf("\n");
    printf("******************************************\n");
    printf("*     STARTING DOWNLOAD PHASE            *\n");
    printf("******************************************\n");
    printf("\n");
    
    int result = download_torrent(&ts);

    if (result == 0) {
        printf("\n");
        printf("******************************************\n");
        printf("*     Download completed successfully!   *\n");
        printf("*     Output: %-25s  ║\n", ti.name);
        printf("******************************************\n");
        printf("\n");
        
        // seeding state
        printf("Do you want to seed this file? (y/n): ");
        fflush(stdout);
        
        char answer[10];
        if (fgets(answer, sizeof(answer), stdin)) {
            if (answer[0] == 'y' || answer[0] == 'Y') {
                printf("\n Moving to seeding mode...\n");
                start_seeding(&ts);
            } else {
                printf("\n Skipping seeding phase.\n");
            }
        }
        
    } else {
        printf(" Download failed or stopped.\n");
    }

    cleanup_torrent_state(&ts);
    torrent_info_free(&ti);
    g_torrent_state = NULL;
}

// Peer mode 
void run_peer_mode(const char *torrent_file, int local_port, const char *peer_ip, int peer_port) {
    printf("\n*****************************************\n");
    printf("*               PEER MODE                 *\n");
    printf("******************************************\n");
    printf("Torrent: %s\n", torrent_file);
    printf("Local port: %d\n", local_port);
    printf("Peer: %s:%d\n\n", peer_ip, peer_port);

    // 1. parse torrent 
    TorrentInfo ti;
    if (torrentparser(torrent_file, &ti) != 0) {
        printf(" Failed to parse %s\n", torrent_file);
        return;
    }
    print_torrent_info(&ti);

    // 2. initialize
    TorrentState ts;
    if (init_torrent_state(&ts, &ti, local_port) != 0) {
        printf(" Failed to init torrent state\n");
        torrent_info_free(&ti);
        return;
    }
    g_torrent_state = &ts;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    ts.download_start_time = tv.tv_sec + tv.tv_usec / 1e6;
    ts.last_speed_print = ts.download_start_time;
    ts.bytes_downloaded = 0;
    ts.bytes_uploaded = 0;

    printf("✓ Initialized %d pieces\n", ts.total_pieces);

    // 3. manually add peer
    printf("\n Skipping tracker, connecting directly to peer...\n");
    ts.skip_tracker = true;

    if (try_connect_peer(&ts, peer_ip, peer_port) != 0) {
        printf(" Failed to add peer\n");
        cleanup_torrent_state(&ts);
        torrent_info_free(&ti);
        return;
    }
    
    printf(" Added peer %s:%d\n", peer_ip, peer_port);

    // 4. download from peer
    printf("\n");
    printf("******************************************\n");
    printf("*     DOWNLOADING FROM PEER              *\n");
    printf("******************************************\n");
    printf("\n");
    
    int result = download_torrent(&ts);

    if (result == 0) {
        printf("\n");
        printf("******************************************\n");
        printf("*     Download completed successfully!   *\n");
        printf("*     Downloaded from: %s:%d            *\n", peer_ip, peer_port);
        printf("*****************************************\n");
        printf("\n");
    } else {
        printf(" Download failed.\n");
    }

    cleanup_torrent_state(&ts);
    torrent_info_free(&ti);
    g_torrent_state = NULL;
}
// 
int main(int argc, char **argv) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Parse command line arguments
    if (argc < 2) {
        printf("Usage:\n");
        printf("  Normal mode:  %s <port>\n", argv[0]);
        printf("  Peer mode:    %s <port> --peer <peer_ip> <peer_port>\n", argv[0]);
        printf("\nExamples:\n");
        printf("  %s 6881\n", argv[0]);
        printf("  %s 6882 --peer 127.0.0.1 6881\n", argv[0]);
        return 1;
    }

    int local_port = atoi(argv[1]);

    // Check for --peer mode
    if (argc >= 5 && strcmp(argv[2], "--peer") == 0) {
        const char *peer_ip = argv[3];
        int peer_port = atoi(argv[4]);

        printf("*************************************\n");
        printf("   BitTorrent Client (Peer Mode)\n");
        printf("*************************************\n\n");

        
        char torrent_file[512];
        printf("Enter .torrent file to download from peer:\n> ");
        fflush(stdout);

        if (!fgets(torrent_file, sizeof(torrent_file), stdin)) {
            return 1;
        }

        torrent_file[strcspn(torrent_file, "\n")] = 0;

        if (strlen(torrent_file) > 0) {
            run_peer_mode(torrent_file, local_port, peer_ip, peer_port);
        }

        return 0;
    }

    // normal mode 
    printf("*************************************\n");
    printf("   BitTorrent Client (Normal Mode)\n");
    printf("   Listening on port: %d\n", local_port);
    printf("*************************************\n\n");

    while (1) {
        char input[512];

        printf("Enter .torrent file to download (or 'quit'):\n> ");
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin))
            break;

        input[strcspn(input, "\n")] = 0;
        if (strcmp(input, "quit") == 0)
            break;

        if (strlen(input) == 0)
            continue;

        run_torrent_session(input, local_port);
    }

    printf("\n Shutting down client.\n");
    return 0;
}