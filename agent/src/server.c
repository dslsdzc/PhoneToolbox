#include "server.h"
#include "audit.h"
#include "frame.h"
#include "protocol.h"
#include "secure/identity.h"

#include <sodium.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_ACTIVE_CONNECTIONS 32

static volatile int active_connections;

#if defined(__ANDROID__) && __ANDROID_API__ < 21
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
static int legacy_accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags)
{
    const int out = accept(fd, addr, len);
    if (out >= 0 && (flags & SOCK_CLOEXEC))
        fcntl(out, F_SETFD, FD_CLOEXEC);
    return out;
}
#define accept4 legacy_accept4
#endif

typedef struct {
    int client_fd;
    agent_identity identity;
    audit_log *audit;
    char *state_dir;
    char *pair_code;
    char *token;
    int allow_plaintext;
    size_t max_frame;
    size_t max_output;
} client_job;

static void *serve_client_job(void *argument)
{
    client_job *job = (client_job *)argument;
    if (!job)
        return NULL;

    protocol_context context;
    memset(&context, 0, sizeof(context));
    context.client_fd = job->client_fd;
    context.token = job->token;
    context.audit = job->audit;
    context.max_frame = job->max_frame;
    context.max_output = job->max_output;
    context.identity = &job->identity;
    context.state_dir = job->state_dir;
    context.pair_code = job->pair_code;
    context.allow_plaintext = job->allow_plaintext;

    protocol_serve_client(&context);
    if (context.noise) {
        noise_channel_free(context.noise);
        free(context.noise);
    }

    close(job->client_fd);
    free(job->state_dir);
    free(job->pair_code);
    free(job->token);
    free(job);
    __sync_sub_and_fetch(&active_connections, 1);
    return NULL;
}

int server_run(const server_config *config)
{
    if (!config || !config->state_dir || config->state_dir[0] == '\0')
        return -1;
    if (config->allow_plaintext
        && (!config->token || config->token[0] == '\0'))
        return -1;

    agent_identity identity;
    char identity_error[256] = {0};
    if (identity_load_or_create(&identity, config->state_dir,
                                identity_error, sizeof(identity_error)) != 0) {
        fprintf(stderr, "identity: %s\n", identity_error);
        return -1;
    }

    signal(SIGPIPE, SIG_IGN);

    const int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd < 0)
        return -1;

    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)config->port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 只监听 127.0.0.1

    if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(listen_fd);
        return -1;
    }
    if (listen(listen_fd, 16) != 0) {
        close(listen_fd);
        return -1;
    }

    socklen_t address_length = sizeof(address);
    if (getsockname(listen_fd, (struct sockaddr *)&address, &address_length) != 0) {
        close(listen_fd);
        return -1;
    }
    char public_hex[65];
    sodium_bin2hex(public_hex, sizeof(public_hex), identity.public_key,
                   sizeof(identity.public_key));
    printf("PORT=%u\n", (unsigned)ntohs(address.sin_port));
    printf("PID=%d\n", (int)getpid());
    printf("PUBKEY=%s\n", public_hex);
    uint8_t paired_peer[AGENT_IDENTITY_KEY_LEN];
    printf("PAIRED=%d\n", peers_first(config->state_dir, paired_peer) == 0 ? 1 : 0);
    fflush(stdout);

    audit_log audit;
    audit_log *audit_ptr = NULL;
    if (config->audit_path && config->audit_path[0] != '\0') {
        if (audit_open(&audit, config->audit_path) != 0) {
            close(listen_fd);
            return -1;
        }
        audit_ptr = &audit;
    }

    for (;;) {
        const int client_fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        // Bound unauthenticated/trickling connections: each accepted socket
        // gets an I/O deadline, and only a fixed number of workers may run.
        if (__sync_add_and_fetch(&active_connections, 1) > MAX_ACTIVE_CONNECTIONS) {
            __sync_sub_and_fetch(&active_connections, 1);
            close(client_fd);
            continue;
        }

        const int io_timeout_ms = config->io_timeout_ms > 0
            ? config->io_timeout_ms
            : 10000;
        struct timeval io_timeout;
        io_timeout.tv_sec = io_timeout_ms / 1000;
        io_timeout.tv_usec = (io_timeout_ms % 1000) * 1000;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &io_timeout,
                   sizeof(io_timeout));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &io_timeout,
                   sizeof(io_timeout));

        // 帧是「4 字节头 + payload」两次写。不开 TCP_NODELAY 时 Nagle 会把
        // 小包攒到对端延迟 ACK，本地回环也会出现约 40ms 的固定往返。
        int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        // 每个连接独立线程：一个僵死的旧会话不能阻塞后面的握手。
        client_job *job = calloc(1, sizeof(*job));
        if (!job) {
            close(client_fd);
            __sync_sub_and_fetch(&active_connections, 1);
            continue;
        }
        job->client_fd = client_fd;
        memcpy(&job->identity, &identity, sizeof(job->identity));
        job->audit = audit_ptr;
        job->state_dir = strdup(config->state_dir);
        job->pair_code = config->pair_code ? strdup(config->pair_code) : NULL;
        job->token = config->token ? strdup(config->token) : NULL;
        job->allow_plaintext = config->allow_plaintext;
        job->max_frame = config->max_frame ? config->max_frame : FRAME_DEFAULT_MAX;
        job->max_output = config->max_output ? config->max_output
                                             : 8u * 1024u * 1024u;

        if (!job->state_dir
            || (config->pair_code && !job->pair_code)
            || (config->token && !job->token)) {
            free(job->state_dir);
            free(job->pair_code);
            free(job->token);
            free(job);
            close(client_fd);
            __sync_sub_and_fetch(&active_connections, 1);
            continue;
        }

        pthread_t thread;
        if (pthread_create(&thread, NULL, serve_client_job, job) != 0) {
            free(job->state_dir);
            free(job->pair_code);
            free(job->token);
            free(job);
            close(client_fd);
            __sync_sub_and_fetch(&active_connections, 1);
            continue;
        }
        pthread_detach(thread);
    }

    while (__sync_fetch_and_add(&active_connections, 0) > 0)
        usleep(10000);

    if (audit_ptr)
        audit_close(audit_ptr);
    close(listen_fd);
    return 0;
}
