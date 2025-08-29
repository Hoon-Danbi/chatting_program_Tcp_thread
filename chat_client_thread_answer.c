// chat_client_thread.c
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define BUF_SIZE 4096

#pragma pack(push, 1)
typedef struct {
    uint16_t ver;
    uint16_t type;
    uint32_t length;
    uint32_t seq;
    uint64_t ts_ms;
    uint32_t room_id;
    char     sender[32]; // not guaranteed NUL-terminated
} pkt_hdr_t;
#pragma pack(pop)

enum pkt_type { PKT_JOIN=1, PKT_MSG=2, PKT_LEAVE=3, PKT_NICK=4, PKT_SYS=5, PKT_PING=6, PKT_PONG=7 };

static volatile sig_atomic_t g_running = 1;
static uint32_t g_seq = 1;

static inline uint64_t htonll(uint64_t x){
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint64_t)htonl((uint32_t)(x & 0xffffffffULL)) << 32) | htonl((uint32_t)(x >> 32));
#else
    return x;
#endif
}
static inline uint64_t ntohll(uint64_t x){
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint64_t)ntohl((uint32_t)(x & 0xffffffffULL)) << 32) | ntohl((uint32_t)(x >> 32));
#else
    return x;
#endif
}

static uint64_t now_ms(void){
    struct timeval tv; gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

static ssize_t recv_exact(int s, void *buf, size_t len){
    size_t got = 0;
    while (got < len){
        ssize_t r = recv(s, (char*)buf + got, len - got, 0);
        if (r == 0) return 0;
        if (r < 0){
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t)r;
    }
    return (ssize_t)got;
}

static ssize_t send_exact(int s, const void *buf, size_t len){
    size_t sent = 0;
    while (sent < len){
        ssize_t r = send(s, (const char*)buf + sent, len - sent, 0);
        if (r < 0){
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)r;
    }
    return (ssize_t)sent;
}

static int send_packet(int sock, uint16_t type, const char sender[32], uint32_t room_id,
                       const void *payload, uint32_t length)
{
    pkt_hdr_t h = {0};
    h.ver     = htons(1);
    h.type    = htons(type);
    h.length  = htonl(length);
    h.seq     = htonl(g_seq++);
    h.ts_ms   = htonll(now_ms());
    h.room_id = htonl(room_id);
    if (sender) memcpy(h.sender, sender, 32);

    if (send_exact(sock, &h, sizeof(h)) < 0) return -1;
    if (length > 0 && payload){
        if (send_exact(sock, payload, length) < 0) return -1;
    }
    return 0;
}

static int recv_packet(int sock, pkt_hdr_t *out_hdr, char **out_payload){
    pkt_hdr_t net;
    ssize_t r = recv_exact(sock, &net, sizeof(net));
    if (r <= 0) return (int)r;

    pkt_hdr_t h;
    h.ver     = ntohs(net.ver);
    h.type    = ntohs(net.type);
    h.length  = ntohl(net.length);
    h.seq     = ntohl(net.seq);
    h.ts_ms   = ntohll(net.ts_ms);
    h.room_id = ntohl(net.room_id);
    memcpy(h.sender, net.sender, 32);
    *out_hdr = h;

    char *payload = NULL;
    if (h.length > 0){
        payload = (char*)malloc(h.length);
        if (!payload) return -1;
        r = recv_exact(sock, payload, h.length);
        if (r <= 0){ free(payload); return (int)r; }
    }
    *out_payload = payload;
    return 1;
}

static void on_sigint(int sig){
    (void)sig;
    g_running = 0;
}

static void print_sender(const char name[32]){
    char tmp[33]; memcpy(tmp, name, 32); tmp[32] = '\0';
    // trim trailing NULs are already, just print
    fprintf(stdout, "%s", tmp);
}

int main(int argc, char **argv){
    if (argc < 3){
        fprintf(stderr, "Usage: %s <server_ip> <nickname> [room_id]\n", argv[0]);
        return 1;
    }
    const char *server_ip = argv[1];
    char myname[32] = {0};
    strncpy(myname, argv[2], 31);
    uint32_t room_id = (argc >= 4) ? (uint32_t)atoi(argv[3]) : 1;

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0){ perror("socket"); return 1; }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8081);
    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) != 1){
        fprintf(stderr, "invalid server ip: %s\n", server_ip);
        close(s); return 1;
    }

    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0){
        perror("connect");
        close(s); return 1;
    }

    // 첫 JOIN
    if (send_packet(s, PKT_JOIN, myname, room_id, NULL, 0) < 0){
        perror("send JOIN");
        close(s); return 1;
    }
    fprintf(stderr, "[INFO] connected. type messages or commands: /nick NAME, /join ROOM, /ping, /quit\n");

    char inbuf[BUF_SIZE];

    while (g_running){
        fd_set rfds; FD_ZERO(&rfds);
        FD_SET(s, &rfds);
        FD_SET(STDIN_FILENO, &rfds);
        int maxfd = (s > STDIN_FILENO ? s : STDIN_FILENO);

        int r = select(maxfd+1, &rfds, NULL, NULL, NULL);
        if (r < 0){
            if (errno == EINTR) continue;
            perror("select"); break;
        }

        // 소켓 수신
        if (FD_ISSET(s, &rfds)){
            pkt_hdr_t hdr = {0}; 
            char *payload = NULL;

            int rr = recv_packet(s, &hdr, &payload);
            if (rr <= 0){
                fprintf(stderr, "[INFO] server closed\n");
                break;
            }

            if (hdr.type == PKT_MSG){
                // 안전 출력(수신 길이 기반)
                fprintf(stdout, "[%u][", hdr.room_id);
                print_sender(hdr.sender);
                fprintf(stdout, "] ");
                if (payload && hdr.length){
                    fwrite(payload, 1, hdr.length, stdout);
                }
                fprintf(stdout, "\n");
                fflush(stdout);
            } else if (hdr.type == PKT_SYS){
                fprintf(stdout, "*** ");
                if (payload && hdr.length) fwrite(payload, 1, hdr.length, stdout);
                fprintf(stdout, " ***\n");
                fflush(stdout);
            } else if (hdr.type == PKT_PING){
                // 서버에서 보낼 일은 드물지만 PONG
                send_packet(s, PKT_PONG, myname, room_id, NULL, 0);
            } else if (hdr.type == PKT_PONG){
                fprintf(stdout, "(pong)\n");
                fflush(stdout);
            }
            if (payload) free(payload);
        }

        // 사용자 입력
        if (FD_ISSET(STDIN_FILENO, &rfds)){
            if (!fgets(inbuf, sizeof(inbuf), stdin)){
                // EOF
                break;
            }
            size_t L = strlen(inbuf);
            if (L > 0 && inbuf[L-1] == '\n') inbuf[--L] = '\0';

            if (inbuf[0] == '/'){
                // 명령
                if (!strncmp(inbuf, "/quit", 5)){
                    send_packet(s, PKT_LEAVE, myname, room_id, NULL, 0);
                    break;
                } else if (!strncmp(inbuf, "/nick ", 6)){
                    const char *nn = inbuf + 6;
                    char newname[32] = {0};
                    strncpy(newname, nn, 31);
                    if (send_packet(s, PKT_NICK, myname, room_id, newname, (uint32_t)strnlen(newname, 32)) == 0){
                        memcpy(myname, newname, 32);
                    }
                } else if (!strncmp(inbuf, "/join ", 6)){
                    uint32_t nr = (uint32_t)atoi(inbuf + 6);
                    room_id = nr ? nr : 1;
                    // JOIN 패킷: sender=현재 닉, room_id=변경 후
                    send_packet(s, PKT_JOIN, myname, room_id, NULL, 0);
                } else if (!strncmp(inbuf, "/ping", 5)){
                    send_packet(s, PKT_PING, myname, room_id, NULL, 0);
                } else {
                    fprintf(stderr, "unknown cmd. use: /nick NAME, /join ROOM, /ping, /quit\n");
                }
            } else {
                // 일반 메시지
                if (L > 0){
                    send_packet(s, PKT_MSG, myname, room_id, inbuf, (uint32_t)L);
                }
            }
        }
    }

    close(s);
    return 0;
}
