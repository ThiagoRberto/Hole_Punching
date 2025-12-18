#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define MAX_PEERS 128
#define LENGTH_ID 64
#define BUFFER_SIZE 512
#define TTL 120

// Structure of a peer in server
typedef struct {
    int used;
    char id[LENGTH_ID];
    struct sockaddr_in addr;
    time_t last_seen;
} peer_t;

peer_t peers[MAX_PEERS];

int find_peer_index_by_id(const char *id) {
    // Search for a index in peers array
    for (int i = 0; i < MAX_PEERS; ++i) {
        if (peers[i].used && strcmp(peers[i].id, id) == 0) return i;
    }
    return -1;
}

int add_or_update_peer(const char *id, struct sockaddr_in *addr) {
    // Add a new peer or update a existing one
    int idx = find_peer_index_by_id(id);
    if (idx >= 0) {
        peers[idx].addr = *addr;
        peers[idx].last_seen = time(NULL);
        return idx;
    }
    for (int i = 0; i < MAX_PEERS; ++i) {
        if (!peers[i].used) {
            peers[i].used = 1;
            strncpy(peers[i].id, id, LENGTH_ID-1);
            peers[i].addr = *addr;
            peers[i].last_seen = time(NULL);
            return i;
        }
    }
    return -1;
}

void cleanup_peers() {
    // Clean a peer if the time to live has expired
    time_t now = time(NULL);
    for (int i = 0; i < MAX_PEERS; ++i) {
        if (peers[i].used && (now - peers[i].last_seen) > TTL) {
            printf("Cleaning up peer %s (timeout)\n", peers[i].id);
            peers[i].used = 0;
        }
    }
}

int sockaddr_to_str(struct sockaddr_in *a, char *buffer, size_t bufferSize) {
    // Convert a sockaddr_in attribute in a string
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
    int port = ntohs(a->sin_port);
    return snprintf(buffer, bufferSize, "%s %d", ip, port);
}

int main(int argc, char **argv) {
    int port = 5000;
    if (argc >= 2) port = atoi(argv[1]);

    // Try to connect with socket or exit the program
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); exit(1); }

    struct sockaddr_in srvaddr;
    memset(&srvaddr,0,sizeof(srvaddr));
    srvaddr.sin_family = AF_INET;
    srvaddr.sin_addr.s_addr = INADDR_ANY;
    srvaddr.sin_port = htons(port);

    if (bind(sock, (struct sockaddr*)&srvaddr, sizeof(srvaddr)) < 0) {
        perror("bind");
        exit(1);
    }

    printf("Registration server listening on port %d\n", port);

    char buffer[BUFFER_SIZE];
    while (1) { // Receive UDP messages in buffer and detect commands
        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);
        ssize_t n = recvfrom(sock, buffer, sizeof(buffer)-1, 0, (struct sockaddr*)&cli, &cli_len);
        if (n <= 0) continue;
        buffer[n] = '\0';

        if (strncmp(buffer, "REGISTER ", 9) == 0) {
            // Register a new xxx
            char id[LENGTH_ID];
            if (sscanf(buffer+9, "%63s", id) >= 1) {
                add_or_update_peer(id, &cli);
                char addrstr[64];
                sockaddr_to_str(&cli, addrstr, sizeof(addrstr));
                printf("REGISTER %s from %s\n", id, addrstr);
                char reply[BUFFER_SIZE];
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));
                int port_observed = ntohs(cli.sin_port);
                snprintf(reply, sizeof(reply), "REGISTERED %s %s %d", id, ip, port_observed);
                sendto(sock, reply, strlen(reply), 0, (struct sockaddr*)&cli, cli_len);
            }
        } else if (strncmp(buffer, "REQUEST ", 8) == 0) {
            char requester_id[LENGTH_ID];
            char target_id[LENGTH_ID];
            
            int parsed = sscanf(buffer+8, "%63s %63s", requester_id, target_id);
            char actual_requester_id[LENGTH_ID] = {0};
            char actual_target_id[LENGTH_ID] = {0};
            if (parsed == 2) {
                strncpy(actual_requester_id, requester_id, LENGTH_ID-1);
                strncpy(actual_target_id, target_id, LENGTH_ID-1);
            } else if (parsed == 1) {
                // The requester did not sent an id, trying to find peer by address
                strncpy(actual_target_id, requester_id, LENGTH_ID-1);
                int idx = -1;
                for (int i = 0; i < MAX_PEERS; ++i) {
                    if (peers[i].used &&
                        peers[i].addr.sin_addr.s_addr == cli.sin_addr.s_addr &&
                        peers[i].addr.sin_port == cli.sin_port) {
                        idx = i; break;
                    }
                }
                if (idx >= 0) strncpy(actual_requester_id, peers[idx].id, LENGTH_ID-1);
                else strncpy(actual_requester_id, "unknown", LENGTH_ID-1);
            } else {
                continue;
            }

            int tidx = find_peer_index_by_id(actual_target_id);
            if (tidx < 0) {
                char reply[BUFFER_SIZE];
                snprintf(reply, sizeof(reply), "ERROR target_not_found %s", actual_target_id);
                sendto(sock, reply, strlen(reply), 0, (struct sockaddr*)&cli, cli_len);
                printf("REQUEST: target %s not found\n", actual_target_id);
                continue;
            }

            // Send PEER info to requester
            char reply_req[BUFFER_SIZE];
            char target_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &peers[tidx].addr.sin_addr, target_ip, sizeof(target_ip));
            int target_port = ntohs(peers[tidx].addr.sin_port);
            snprintf(reply_req, sizeof(reply_req), "PEER %s %s %d", peers[tidx].id, target_ip, target_port);
            sendto(sock, reply_req, strlen(reply_req), 0, (struct sockaddr*)&cli, cli_len);
            printf("Forwarded %s info to requester %s\n", peers[tidx].id, actual_requester_id);

            // Send requester mapping to target
            char reply_tgt[BUFFER_SIZE];
            char requester_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &cli.sin_addr, requester_ip, sizeof(requester_ip));
            int requester_port = ntohs(cli.sin_port);
            snprintf(reply_tgt, sizeof(reply_tgt), "PEER %s %s %d", actual_requester_id, requester_ip, requester_port);
            sendto(sock, reply_tgt, strlen(reply_tgt), 0, (struct sockaddr*)&peers[tidx].addr, sizeof(peers[tidx].addr));
            printf("Notified target %s about requester %s\n", peers[tidx].id, actual_requester_id);

        } else if (strncmp(buffer, "KEEPALIVE ", 10) == 0) {
            // Keep an id regitered
            char id[LENGTH_ID];
            if (sscanf(buffer+10, "%63s", id) >= 1) {
                int idx = find_peer_index_by_id(id);
                if (idx >= 0) {
                    peers[idx].last_seen = time(NULL);
                }
            }
        } else if (strncmp(buffer, "UNREGISTER ", 11) == 0) {
            // Remove unused ids
            char id[LENGTH_ID];
            if (sscanf(buffer+11, "%63s", id) >= 1) {
                int idx = find_peer_index_by_id(id);
                if (idx >= 0) {
                    peers[idx].used = 0;
                    printf("UNREGISTER %s\n", id);
                }
            }
        } else {
            // Unknow command
            char addrstr[64];
            sockaddr_to_str(&cli, addrstr, sizeof(addrstr));
            printf("Unknown message from %s: %s\n", addrstr, buffer);
        }

        cleanup_peers();
    }

    close(sock);
    return 0;
}
