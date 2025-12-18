#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>

#define BUFFER_SIZE 512
#define LENGTH_ID 64
#define MAX_ATTEMPTS 16
#define PUNCH_ROUNDS 40

int set_nonblocking(int fd) {
    // Do a non-blocking read, avoid the program to be stucked in reading.
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <server_ip> <server_port> <my_id> [target_id]\n", argv[0]);
        return 1;
    }
    char *server_ip = argv[1];
    int server_port = atoi(argv[2]);
    char my_id[LENGTH_ID]; strncpy(my_id, argv[3], LENGTH_ID-1);
    char target_id[LENGTH_ID] = {0};
    int want_connect = 0;
    if (argc >= 5) { strncpy(target_id, argv[4], LENGTH_ID-1); want_connect = 1; }

    // Try to connect with socket or exit the program
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket"); 
        exit(1);
    }

    struct sockaddr_in local;
    memset(&local,0,sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(0);
    if (bind(sock, (struct sockaddr*)&local, sizeof(local)) < 0) {
        perror("bind"); 
        exit(1);
    }

    // Building server address
    struct sockaddr_in server;
    memset(&server,0,sizeof(server));
    server.sin_family = AF_INET;
    inet_pton(AF_INET, server_ip, &server.sin_addr);
    server.sin_port = htons(server_port);

    char buffer[BUFFER_SIZE];
    // Initial register
    snprintf(buffer, sizeof(buffer), "REGISTER %s", my_id);
    sendto(sock, buffer, strlen(buffer), 0, (struct sockaddr*)&server, sizeof(server));
    printf("Sent: %s to %s:%d\n", buffer, server_ip, server_port);

    fd_set rfds;
    struct timeval tv;
    int registered = 0;
    struct sockaddr_in observed_self;
    socklen_t observed_len = sizeof(observed_self);

    time_t start = time(NULL);
    while (!registered && (time(NULL) - start) < 5) { // Wait until registered
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int rv = select(sock+1, &rfds, NULL, NULL, &tv); // Sent the register again if timeout occurs
        if (rv > 0 && FD_ISSET(sock, &rfds)) {
            struct sockaddr_in from;
            socklen_t flen = sizeof(from);
            ssize_t n = recvfrom(sock, buffer, sizeof(buffer)-1, 0, (struct sockaddr*)&from, &flen);
            if (n <= 0) continue;
            buffer[n] = '\0';
            if (strncmp(buffer, "REGISTERED ", 11) == 0) {
                char rid[LENGTH_ID], rip[INET_ADDRSTRLEN];
                int rport;
                if (sscanf(buffer+11, "%63s %15s %d", rid, rip, &rport) >= 3) {
                    printf("Server observed us as: %s %d\n", rip, rport);
                    memset(&observed_self,0,sizeof(observed_self));
                    observed_self.sin_family = AF_INET;
                    inet_pton(AF_INET, rip, &observed_self.sin_addr);
                    observed_self.sin_port = htons(rport);
                    registered = 1;
                    break;
                }
            } else {
                printf("Server reply: %s\n", buffer);
            }
        } else {
            // Retry registration
            sendto(sock, buffer, strlen(buffer), 0, (struct sockaddr*)&server, sizeof(server));
        }
    }

    if (!registered) {
        fprintf(stderr, "Failed to register with server\n");
    }

    // If a target was given, request connection
    if (want_connect) {
        char req[BUFFER_SIZE];
        snprintf(req, sizeof(req), "REQUEST %s %s", my_id, target_id);
        sendto(sock, req, strlen(req), 0, (struct sockaddr*)&server, sizeof(server));
        printf("Sent REQUEST for %s\n", target_id);
    }

    // Wait for PEER messages and for punching
    struct sockaddr_in remote;
    int have_peer = 0;
    int connected = 0;
    char peer_id[LENGTH_ID] = {0};
    int base_peer_port = 0;
    char peer_ipstr[INET_ADDRSTRLEN] = {0};

    set_nonblocking(sock);
    set_nonblocking(STDIN_FILENO);

    int rounds = 0;
    while (1) {
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        FD_SET(STDIN_FILENO, &rfds);
        int maxfd = (sock > STDIN_FILENO) ? sock : STDIN_FILENO;
        tv.tv_sec = 0;
        tv.tv_usec = 200 * 1000;

        int rv = select(maxfd+1, &rfds, NULL, NULL, &tv);
        if (rv > 0) {
            if (FD_ISSET(sock, &rfds)) {
                struct sockaddr_in from;
                socklen_t flen = sizeof(from);
                ssize_t n = recvfrom(sock, buffer, sizeof(buffer)-1, 0, (struct sockaddr*)&from, &flen);
                if (n <= 0) continue;
                buffer[n] = '\0';

                // Handle messages
                if (strncmp(buffer, "PEER ", 5) == 0) {
                    char pid[LENGTH_ID], ip[INET_ADDRSTRLEN];
                    int port;
                    if (sscanf(buffer+5, "%63s %15s %d", pid, ip, &port) >= 3) {
                        strncpy(peer_id, pid, LENGTH_ID-1);
                        strcpy(peer_ipstr, ip);
                        base_peer_port = port;
                        memset(&remote,0,sizeof(remote));
                        remote.sin_family = AF_INET;
                        inet_pton(AF_INET, peer_ipstr, &remote.sin_addr);
                        remote.sin_port = htons(base_peer_port);
                        have_peer = 1;
                        printf("Received PEER info: %s %s:%d\n", peer_id, peer_ipstr, base_peer_port);
                    }
                } else if (strncmp(buffer, "PUNCH ", 6) == 0 || strstr(buffer, "PUNCH") != NULL) {
                    // Received a punch from peer, connection established
                    char addrstr[64];
                    inet_ntop(AF_INET, &from.sin_addr, addrstr, sizeof(addrstr));
                    printf("Received PUNCH from %s:%d -> %s\n", addrstr, ntohs(from.sin_port), buffer);
                    connected = 1;
                    char ack[BUFFER_SIZE];
                    snprintf(ack, sizeof(ack), "Message %s: ACK from %s", peer_id, my_id);
                    sendto(sock, ack, strlen(ack), 0, (struct sockaddr*)&from, flen);
                } else if (strncmp(buffer, "Message ", 4) == 0) {
                    char addrstr[64];
                    inet_ntop(AF_INET, &from.sin_addr, addrstr, sizeof(addrstr));
                    printf("[Message from %s:%d] %s\n", addrstr, ntohs(from.sin_port), buffer+4);
                } else {
                    char addrstr[64];
                    inet_ntop(AF_INET, &from.sin_addr, addrstr, sizeof(addrstr));
                    printf("[UDP %s:%d] %s\n", addrstr, ntohs(from.sin_port), buffer);
                }
            }

            if (FD_ISSET(STDIN_FILENO, &rfds)) {
                // Allow user to type application messages after connection
                char line[BUFFER_SIZE];
                if (fgets(line, sizeof(line), stdin) != NULL) {
                    size_t L = strlen(line);
                    if (L > 0 && line[L-1] == '\n') line[L-1] = '\0';
                    if (have_peer && connected) {
                        char message[BUFFER_SIZE];
                        snprintf(message, sizeof(message), "Message %s: %s", my_id, line);
                        sendto(sock, message, strlen(message), 0, (struct sockaddr*)&remote, sizeof(remote));
                    } else {
                        printf("Not yet connected to peer. You can still send a REQUEST to server or wait.\n");
                    }
                }
            }
        } 

        // If we have peer info but not connected, send repeated PUNCH attempts
        if (have_peer && !connected) {
            // Rounds control to avoid infinite loop
            if (rounds++ > PUNCH_ROUNDS) {
                printf("Punching attempts exhausted (%d rounds). Still not connected.\n", PUNCH_ROUNDS);
                rounds = 0;
                sleep(1);
                continue;
            }
            for (int i = 0; i < MAX_ATTEMPTS; ++i) {
                struct sockaddr_in dst = remote;
                dst.sin_port = htons(base_peer_port + i);
                char punch[BUFFER_SIZE];
                snprintf(punch, sizeof(punch), "PUNCH %s seq=%d", my_id, rounds*MAX_ATTEMPTS + i);
                sendto(sock, punch, strlen(punch), 0, (struct sockaddr*)&dst, sizeof(dst));
                usleep(10000);
            }
            usleep(200000);
        }

        static time_t last_keepalive = 0;
        if (time(NULL) - last_keepalive > 5) {
            char keepAlive[BUFFER_SIZE];
            snprintf(keepAlive, sizeof(keepAlive), "KEEPALIVE %s", my_id);
            sendto(sock, keepAlive, strlen(keepAlive), 0, (struct sockaddr*)&server, sizeof(server));
            last_keepalive = time(NULL);
        }
    }

    close(sock);
    return 0;
}
