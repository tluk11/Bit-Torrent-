#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/sha.h>
#include <errno.h>
#include "torrent_parser.h"
#include "bencode.h"   
#include "contact_tracker.h" 
#include "store_pieces.h"    

// helper functions
static char *safe_strndup(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (p) {
        memcpy(p, s, n);
        p[n] = '\0';
    }
    return p;
}

// frees all heap-allocated memory within the TorrentInfo structure.
void torrent_info_free(TorrentInfo *ti) {
    if (!ti) return;

    // 1. Free tracker strings and the array of pointers
    if (ti->trackers) {
        for (int i = 0; i < ti->num_trackers; i++) {
            free(ti->trackers[i]); 
        }
        free(ti->trackers); 
    }

    // 2. Free the pieces hash block
    if (ti->pieces) {
        free(ti->pieces);
    }

    // 3. Free the file name string
    if (ti->name) {
        free(ti->name); 
    }
    
    // Set members to NULL/0 after freeing
    memset(ti, 0, sizeof(*ti));
}


// Returns 0 on success, 1 on failure 
int torrentparser(const char *path, TorrentInfo *ti) {
   
    unsigned char *data = NULL; // Pointer to file data
    bencode_t root;


    // 1. Clear the structure and initialize values
    memset(ti, 0, sizeof(*ti));


    // 2. Read the file into memory
    FILE *f = fopen(path, "rb");      
    if (!f) {
        perror("fopen");
        return 1;
    }


    // get file size so we know how much data to read
    fseek(f, 0, SEEK_END);
    size_t len = ftell(f);
    rewind(f);


    data = malloc(len);
    if (!data) {
        perror("malloc");
        fclose(f);
        return 1;
    }


    size_t n = fread(data, 1, len, f);
    fclose(f);


    if (n != len) {
        fprintf(stderr, "Error reading file: read %zu bytes, expected %zu\n", n, len);
        goto error_cleanup;
    }


    // 3. Decode the bencoded data
    bencode_init(&root, (char*)data, len);


    if (!bencode_is_dict(&root)) {
        fprintf(stderr, "Torrent file root is not a dictionary.\n");
        goto error_cleanup;
    }


    // 4. Iterate through the dictionary
    bencode_t val;
    const char *key;
    int klen;

    while (bencode_dict_has_next(&root)) {
        if (!bencode_dict_get_next(&root, &val, &key, &klen)){
            printf("No more items in dictionary.\n");
            break;
        }
        // printf("Key: %.*s\n", klen, key);

        if (klen == 8 && memcmp(key, "announce", 8) == 0) {
            bencode_string_value(&val, &key, &klen);
            // printf("Announce found: %.*s\n", klen, key);

            // handle announce URL
            ti->num_trackers = 1;
            ti->trackers = realloc(ti->trackers, sizeof(char*));
            ti->trackers[0] = safe_strndup(key, klen);
        }
        else if (klen == 13 && memcmp(key, "announce-list", 13) == 0) {
            // get rid of previous announce if exists
            if (ti->trackers) {
                for (int i = 0; i < ti->num_trackers; i++) {
                    free(ti->trackers[i]);
                }
                free(ti->trackers);
                ti->trackers = NULL;
                ti->num_trackers = 0;
            }
            // val is a list
            bencode_t tierlist;
            bencode_clone(&val, &tierlist);
            bencode_t tier;
            // printf("Announce-list found:\n");
            while (bencode_list_has_next(&tierlist)) {
                bencode_list_get_next(&tierlist, &tier);
                if (!bencode_is_list(&tier)) {
                    if (!bencode_is_string(&tier)) {
                        printf("Invalid announce-list format.\n");
                        continue;
                    } else {
                        const char *u; int ulen;
                        bencode_string_value(&tier, &u, &ulen);
                        // printf("NON-TIER URL: %.*s\n", ulen, u);
                        // handle tracker URL
                        ti->trackers = realloc(ti->trackers, sizeof(char *) * (ti->num_trackers+1));
                        ti->trackers[ti->num_trackers++] = safe_strndup(u, ulen);
                        continue;
                    }
                }
                
                // printf("Tier:\n");
                bencode_t tracker_item;
                while (bencode_list_has_next(&tier)) {
                    bencode_list_get_next(&tier, &tracker_item);
                    const char *u; int ulen;
                    bencode_string_value(&tracker_item, &u, &ulen);
                    // printf("%.*s ", ulen, u);
                    
                    ti->trackers = realloc(ti->trackers, sizeof(char *) * (ti->num_trackers+1));
                    ti->trackers[ti->num_trackers++] = safe_strndup(u, ulen);
                }
                // printf("\n");
            }
        } else if (klen == 4 && memcmp(key, "info", 4) == 0) {
            // handle info dictionary
            // printf("Info dictionary start: %s\n", val.str);
            const char *info_start = val.str;
            while (bencode_dict_has_next(&val)) {
                bencode_t info_val;
                const char *info_key;
                int info_klen;

                if (!bencode_dict_get_next(&val, &info_val, &info_key, &info_klen)){
                    break;
                }
                // printf(" Info Key: %.*s\n", info_klen, info_key);
                
                if (memcmp(info_key, "piece length", info_klen) == 0) {
                    long int piece_length;
                    if (bencode_int_value(&info_val, &piece_length)) {
                        ti->piece_length = (int)piece_length;
                        // printf("  Piece Length: %d\n", ti->piece_length);
                    }
                } else if (memcmp(info_key, "pieces", info_klen) == 0) {
                    const char *pieces_str;
                    int pieces_len;
                    if (bencode_string_value(&info_val, &pieces_str, &pieces_len)) {
                        ti->num_pieces = pieces_len / 20;
                        ti->pieces = malloc(pieces_len);
                        memcpy(ti->pieces, pieces_str, pieces_len);
                        // printf("  Number of Pieces: %d\n", ti->num_pieces);
                    }
                } else if (memcmp(info_key, "name", info_klen) == 0) {
                    const char *name_str;
                    int name_len;
                    if (bencode_string_value(&info_val, &name_str, &name_len)) {
                        ti->name = safe_strndup(name_str, name_len);
                        // printf("  Name: %s\n", ti->name);
                    }
                } else if (memcmp(info_key, "length", info_klen) == 0) {
                    long int file_length;
                    if (bencode_int_value(&info_val, &file_length)) {
                        ti->file_length = file_length;
                        // printf("  File Length: %ld\n", ti->file_length);
                    }
                }
            }
            
            // hash everything from info start to val.str (inclusive)
            SHA1((unsigned char*)info_start, val.str - info_start+1, ti->info_hash);
        }
    }
    free(data);
    return 0; // Success


// --- Centralized Error Handling ---
error_cleanup:
    if (data) free(data); // Free the file data buffer
    torrent_info_free(ti); // Free all heap memory allocated within the TorrentInfo structure
    return 1; // Failure
}
