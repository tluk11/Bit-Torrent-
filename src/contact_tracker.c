#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>      
#include <curl/curl.h>
#include <errno.h>
#include "bencode.h"
#include <stdbool.h>
#include "torrent_parser.h"
#include "contact_tracker.h"
#include "store_pieces.h"

// --- Helper Functions ---

// POSIX strndup implementation for portability
char *safe_strndup(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (p) {
        memcpy(p, s, n);
        p[n] = '\0';
    }
    return p;
}

void tracker_response_free(TrackerResponse *tr) {
    if (tr->peers) free(tr->peers);
    if (tr->warning_message) free(tr->warning_message);
    if (tr->failure_reason) free(tr->failure_reason);
    memset(tr, 0, sizeof(*tr));
}

static int parse_url(const char *url, char *host, char *port_str, char *path) {
    if (strncmp(url, "http://", 7) != 0) return 1;

    const char *p = url + 7;
    const char *colon = strchr(p, ':');
    const char *slash = strchr(p, '/');

    if (!slash) return 1;

    // Extract Host
    if (colon && colon < slash) {
        strncpy(host, p, colon - p);
        host[colon - p] = '\0';
        
        // Extract Port
        strncpy(port_str, colon + 1, slash - (colon + 1));
        port_str[slash - (colon + 1)] = '\0';
    } else {
        strncpy(host, p, slash - p);
        host[slash - p] = '\0';
        strcpy(port_str, "80"); // Default HTTP port
    }

    strcpy(path, slash);
    return 0;
}

// --- Main Functions ---
int sendGETRequest(TrackerInfo *ti, unsigned char **response, size_t *response_len) {
    char host[256], path[512], port_str[16];

    // 1. Parse URL
    if (parse_url(ti->announce, host, port_str, path) != 0) {
        fprintf(stderr, "Invalid announce URL: %s\n", ti->announce);
        return 1;
    }

    // 2. URL Encode 
    CURL *curl = curl_easy_init();
    if (!curl) return 1;

    char *ih_encoded = curl_easy_escape(curl, (char *)ti->info_hash, 20);
    char *pid_encoded = curl_easy_escape(curl, (char *)ti->peer_id, 20);

    if (!ih_encoded || !pid_encoded) {
        curl_easy_cleanup(curl);
        return 1;
    }

    // 3. Build Request
    char req[2048];
    int client_port = ti->port;  // Port our client listens on for peers

    snprintf(req, sizeof(req),
        "GET %s?info_hash=%s&peer_id=%s&port=%d&uploaded=%llu&downloaded=%llu&left=%llu&compact=1&event=%s HTTP/1.1\r\n"
        "Host: %s:%s\r\n"
        "User-Agent: C-BitTorrent/1.0\r\n"
        "Connection: close\r\n\r\n",
        path, ih_encoded, pid_encoded, client_port,
        (unsigned long long)ti->uploaded, (unsigned long long)ti->downloaded, (unsigned long long)ti->left,
        ti->event ? ti->event : "",
        host, port_str
    );


    curl_free(ih_encoded);
    curl_free(pid_encoded);
    curl_easy_cleanup(curl);

    // 4. Resolve Hostname 
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;       
    hints.ai_socktype = SOCK_STREAM; 

    int status = getaddrinfo(host, port_str, &hints, &res);
    if (status != 0) {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status));
        return 1;
    }

    // 5. Connect
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        perror("socket");
        freeaddrinfo(res);
        return 1;
    }

    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        perror("connect");
        close(sock);
        freeaddrinfo(res);
        return 1;
    }

    freeaddrinfo(res); 

    // 6. Send
    if (send(sock, req, strlen(req), 0) < 0) {
        perror("send");
        close(sock);
        return 1;
    }

    // 7. Receive
    size_t cap = 4096, len = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) { close(sock); return 1; }

    while (1) {
        ssize_t r = recv(sock, buf + len, cap - len, 0);
        if (r < 0) { perror("recv"); break; }
        if (r == 0) break; 

        len += r;
        if (len == cap) {
            cap *= 2;
            unsigned char *tmp = realloc(buf, cap);
            if (!tmp) { free(buf); close(sock); return 1; }
            buf = tmp;
        }
    }
    close(sock);

    // 8. Return data
    if (response)*response = buf;
    else free(buf);
    if (response_len) *response_len = len;

    return 0;
}

int tracker_parse_response(unsigned char *resp, size_t resp_len, TrackerResponse *tr) {
    memset(tr, 0, sizeof(*tr));

    // 1. Check HTTP Status
    if (resp_len < 15 || strncmp((char*)resp, "HTTP/1.", 7) != 0) {
        fprintf(stderr, "Invalid HTTP response\n");
        return 1;
    }
    if (strstr((char*)resp, "200 OK") == NULL) {
        fprintf(stderr, "Tracker returned non-200 status\n");
        return 1;
    }

    // 2. Find Body
    unsigned char *body = NULL;
    size_t body_len = 0;
    
    // Scan for \r\n\r\n
    for (size_t i = 0; i + 3 < resp_len; i++) {
        if (resp[i] == '\r' && resp[i+1] == '\n' && resp[i+2] == '\r' && resp[i+3] == '\n') {
            body = resp + i + 4;
            body_len = resp_len - (i + 4);
            break;
        }
    }

    if (!body) {
        fprintf(stderr, "No response body found\n");
        return 1;
    }

    // 3. Decode Bencode
    bencode_t root;
    bencode_init(&root, (char*)body, body_len);

    if (!bencode_is_dict(&root)) {
        fprintf(stderr, "Tracker body is not a dictionary\n");
        return 1;
    }


    bencode_t val;
    const char *key;
    int klen;

    while (bencode_dict_has_next(&root)) {

        if (!bencode_dict_get_next(&root, &val, &key, &klen))
            break;

        /* interval */
        if (strncmp(key, "interval", klen) == 0) {
            long l;
            if (bencode_int_value(&val, &l))
                tr->interval = (int)l;
        }

        /* failure reason */
        else if (strncmp(key, "failure reason", klen) == 0) {
            const char *s; int sl;
            if (bencode_string_value(&val, &s, &sl)) {
                tr->failure_reason = safe_strndup(s, sl);
                return 1;
            }
        }

        /* peers */
        else if (strncmp(key, "peers", klen) == 0) {

            /* ---- COMPACT MODEL: peers as a binary string ---- */
            if (bencode_is_string(&val)) {
                const char *p; int sl;

                if (bencode_string_value(&val, &p, &sl) && (sl % 6 == 0)) {

                    tr->num_peers = sl / 6;
                    tr->peers = malloc(sizeof(Peer) * tr->num_peers);

                    for (int i = 0; i < tr->num_peers; i++) {
                        const char *entry = p + (i * 6);

                        inet_ntop(AF_INET, entry, tr->peers[i].ip, sizeof(tr->peers[i].ip));

                        uint16_t raw_port;
                        memcpy(&raw_port, entry + 4, 2);

                        tr->peers[i].port = ntohs(raw_port);
                    }
                }
            }

            // non compact 
            else if (bencode_is_list(&val)) {

                bencode_t list = val;
                bencode_t entry;

                while (bencode_list_has_next(&list)) {

                    if (bencode_list_get_next(&list, &entry) <= 0)
                        break;

                    if (!bencode_is_dict(&entry))
                        continue;

                    Peer temp = {0};
                    const char *k2;
                    int k2len;
                    bencode_t v2;

                    while (bencode_dict_has_next(&entry)) {
                        if (!bencode_dict_get_next(&entry, &v2, &k2, &k2len))
                            break;

                        if (strncmp(k2, "ip", k2len) == 0) {
                            const char *s; int sl;
                            if (bencode_string_value(&v2, &s, &sl))
                                snprintf(temp.ip, sizeof(temp.ip), "%.*s", sl, s);
                        }

                        else if (strncmp(k2, "port", k2len) == 0) {
                            long portn;
                            if (bencode_int_value(&v2, &portn))
                                temp.port = (int)portn;
                        }
                    }

                    tr->peers = realloc(tr->peers, sizeof(Peer) * (tr->num_peers + 1));
                    tr->peers[tr->num_peers++] = temp;
                }
            }
        }
    }

    return 0;
}
int contact_tracker(const TorrentInfo *ti, TrackerResponse *tr) {
    TrackerInfo req = {0};

    // Copy announce URL
    strncpy(req.announce, ti->trackers[0], sizeof(req.announce)-1);

    // Fill tracker request parameters
    memcpy(req.info_hash, ti->info_hash, 20);
    memcpy(req.peer_id, "-TC0001-123456789012", 20);  
    req.uploaded = 0;
    req.downloaded = 0;
    req.left = ti->file_length;
    req.event = "started";

    unsigned char *resp = NULL;
    size_t resp_len = 0;

    // Send HTTP GET request
    if (sendGETRequest(&req, &resp, &resp_len) != 0) {
        fprintf(stderr, "Tracker request failed.\n");
        return 1;
    }

    // Parse bencoded response
    int r = tracker_parse_response(resp, resp_len, tr);
    free(resp);
    return r;
}
