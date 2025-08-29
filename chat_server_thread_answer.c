// chat_server_thread.c
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
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdarg.h>
#define PORT 8081
#define MAX_CLIENTS 1024
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

typedef struct {
    int active;
    int sock;
    int room_id;
    char name[32];
    pthread_t tid;
} client_t;

static client_t clients[MAX_CLIENTS];
static pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;
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
        if (r == 0) return 0;           // peer closed
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
    h.seq     = htonl(__sync_fetch_and_add(&g_seq, 1));
    h.ts_ms   = htonll(now_ms());
    h.room_id = htonl(room_id);
    if (sender) memcpy(h.sender, sender, 32);

    if (send_exact(sock, &h, sizeof(h)) < 0) return -1;
    if (length > 0 && payload){
        if (send_exact(sock, payload, length) < 0) return -1;
    }
    return 0;
}

static void broadcast(uint32_t room_id, uint16_t type, const char sender[32],
                      const void *payload, uint32_t len)
{
    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < MAX_CLIENTS; ++i){
        if (!clients[i].active) continue;
        if ((uint32_t)clients[i].room_id != room_id) continue;
        if (send_packet(clients[i].sock, type, sender, room_id, payload, len) < 0){
            // on error, mark for removal
            close(clients[i].sock);
            clients[i].active = 0;
        }
    }
    pthread_mutex_unlock(&clients_lock);
}

static void syscast(uint32_t room_id, const char *fmt, ...){
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    uint32_t len = (uint32_t)((n >= 0) ? (n < (int)sizeof(buf) ? n : (int)sizeof(buf)) : 0);
    broadcast(room_id, PKT_SYS, "server", buf, len);
}

static void remove_client(client_t *c, const char *who){
    if (!c->active) return;
    int room = c->room_id;
    char name32[32]; memcpy(name32, c->name, 32);
    close(c->sock);
    c->active = 0;
    fprintf(stderr, "[INFO] %.*s disconnected (%s)\n", 32, name32, who ? who : "EOF");
    syscast((uint32_t)room, "*** %.*s left room %d ***", 32, name32, room);
}

static int recv_packet(int sock, pkt_hdr_t *out_hdr, char **out_payload){
    pkt_hdr_t net;
    ssize_t r = recv_exact(sock, &net, sizeof(net));
    if (r <= 0) return (int)r; // 0: closed, -1: error

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

static void *client_thread(void *arg){
    client_t *c = (client_t*)arg;
    // 초기 상태
    c->room_id = 1;
    memset(c->name, 0, 32);

    // 첫 패킷은 JOIN 기대(닉/룸 세팅)
    while (g_running){
        pkt_hdr_t hdr; char *payload = NULL;
        int rr = recv_packet(c->sock, &hdr, &payload);
        if (rr <= 0){ remove_client(c, rr==0 ? "peer-closed" : "recv-error"); break; }

        switch (hdr.type){
        case PKT_JOIN: {
            c->room_id = (int)hdr.room_id;
            memcpy(c->name, hdr.sender, 32);
            fprintf(stderr, "[INFO] %.*s joined room %u\n", 32, c->name, hdr.room_id);
            syscast(hdr.room_id, "*** %.*s joined room %u ***", 32, c->name, hdr.room_id);
        } break;

        case PKT_NICK: {
            char oldname[32]; memcpy(oldname, c->name, 32);
            size_t newlen = (payload ? hdr.length : 0);
            memset(c->name, 0, 32);
            if (payload && newlen){
                size_t copy = newlen < 32 ? newlen : 32;
                memcpy(c->name, payload, copy);
            }
            fprintf(stderr, "[INFO] nick: %.*s -> %.*s\n", 32, oldname, 32, c->name);
            syscast((uint32_t)c->room_id, "*** %.*s is now %.*s ***", 32, oldname, 32, c->name);
        } break;

        case PKT_MSG: {
            // 같은 방으로 브로드캐스트
            broadcast((uint32_t)c->room_id, PKT_MSG, c->name, payload, hdr.length);
            fprintf(stderr, "[MSG] [room %d][%.*s] (%u bytes)\n", c->room_id, 32, c->name, hdr.length);
        } break;

        case PKT_LEAVE: {
            free(payload);
            remove_client(c, "leave");
            return NULL;
        }

        case PKT_PING: {
            // 즉시 PONG
            send_packet(c->sock, PKT_PONG, "server", (uint32_t)c->room_id, NULL, 0);
        } break;

        default:
            // 무시
            break;
        }

        if (payload) free(payload);
    }
    return NULL;
}

static void on_sigint(int sig){
    (void)sig;
    g_running = 0;
}

int main(void){
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0){ perror("socket"); return 1; }
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORT);

    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0){ perror("bind"); return 1; }
    if (listen(s, 128) < 0){ perror("listen"); return 1; }
    fprintf(stderr, "[INFO] server listening on 0.0.0.0:%d\n", PORT);

    for (int i = 0; i < MAX_CLIENTS; ++i) clients[i].active = 0;

    while (g_running){
        struct sockaddr_in cli; socklen_t cl = sizeof(cli);
        int cs = accept(s, (struct sockaddr*)&cli, &cl);
        if (cs < 0){
            if (errno == EINTR) continue;
            perror("accept"); break;
        }

        pthread_mutex_lock(&clients_lock);
        int idx = -1;
        for (int i = 0; i < MAX_CLIENTS; ++i){
            if (!clients[i].active){ idx = i; break; }
        }
        if (idx < 0){
            pthread_mutex_unlock(&clients_lock);
            fprintf(stderr, "[WARN] too many clients\n");
            close(cs);
            continue;
        }

        clients[idx].active = 1;
        clients[idx].sock = cs;
        clients[idx].room_id = 1;
        memset(clients[idx].name, 0, 32);

        if (pthread_create(&clients[idx].tid, NULL, client_thread, &clients[idx]) != 0){
            perror("pthread_create");
            close(cs);
            clients[idx].active = 0;
            pthread_mutex_unlock(&clients_lock);
            continue;
        }
        pthread_detach(clients[idx].tid);
        pthread_mutex_unlock(&clients_lock);
    }

    // 종료 처리
    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < MAX_CLIENTS; ++i){
        if (clients[i].active){
            close(clients[i].sock);
            clients[i].active = 0;
        }
    }
    pthread_mutex_unlock(&clients_lock);
    close(s);
    fprintf(stderr, "[INFO] server stopped\n");
    return 0;
}
