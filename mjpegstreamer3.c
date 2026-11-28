#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RING_CAP         (256u * 1024u * 1024u)
#define MAX_CLIENTS      16
#define MAX_TCP_PORTS    16
#define MAX_FILE_WORKERS 16
#define MAILBOX_CAP      512
#define STATS_INTERVAL_S 5

static volatile sig_atomic_t g_stop = 0;

static void handle_stop_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static void install_signal_handlers(void) {
    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_stop_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* Intentionally no SA_RESTART so blocking read() wakes on signal */

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

typedef struct {
    uint64_t       seq;
    uint64_t       ts_ns;
    const uint8_t *ring_base;
    const uint8_t *ring_end;
    const uint8_t *start;
    const uint8_t *end;
    size_t         len;
} FrameSpan;

typedef struct {
    uint64_t seq;
    uint64_t ts_ns;
    uint64_t offset;
    uint64_t len;
} IndexEntry;

typedef struct {
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
    FrameSpan       slots[MAILBOX_CAP];
    size_t          head;
    size_t          tail;
    size_t          count;
    bool            closed;
    uint64_t        dropped;
} Mailbox;

typedef struct {
    uint16_t port;
    uint32_t decimation;
} TcpEndpointConfig;

typedef struct {
    int        slot_id;
    int        fd;
    uint16_t   port;
    uint32_t   decimation;
    bool       active;
    uint64_t   sent;
    pthread_t  tid;
    Mailbox    mbox;
} ClientSlot;

typedef struct {
    struct App       *app;
    TcpEndpointConfig cfg;
    pthread_t         tid;
} ListenerCtx;

typedef struct {
    char       base_name[4096];
    uint32_t   decimation;
    bool       enabled;
    uint64_t   sent;
    pthread_t  tid;
    Mailbox    mbox;
    struct App *app;
} FileWorkerCtx;

typedef struct {
    bool       enabled;
    uint32_t   decimation;
    uint64_t   sent;
    pthread_t  tid;
    Mailbox    mbox;
    struct App *app;
} StdoutWorkerCtx;

typedef struct App {
    uint8_t           *ring;

    TcpEndpointConfig  tcp_cfgs[MAX_TCP_PORTS];
    int                num_tcp_cfgs;
    ListenerCtx        listeners[MAX_TCP_PORTS];

    FileWorkerCtx      file_workers[MAX_FILE_WORKERS];
    int                num_file_workers;

    StdoutWorkerCtx    stdout_worker;

    pthread_mutex_t    clients_mtx;
    ClientSlot         clients[MAX_CLIENTS];

    pthread_mutex_t    stats_mtx;
    pthread_cond_t     stats_cv;
    volatile bool      running;
} App;

static uint64_t now_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static bool is_running(const App *app) {
    return app->running && !g_stop;
}

static bool parse_uint32_decimation(const char *s, uint32_t *out_dec) {
    if (!s || *s == '\0') return false;
    char *endp = NULL;
    unsigned long val = strtoul(s, &endp, 10);
    if (*endp != '\0' || val == 0 || val > UINT32_MAX) return false;
    *out_dec = (uint32_t)val;
    return true;
}

static bool parse_tcp_spec(const char *arg, TcpEndpointConfig *out) {
    const char *colon = strchr(arg, ':');
    char port_buf[16];
    uint32_t dec = 1;

    if (colon) {
        size_t plen = (size_t)(colon - arg);
        if (plen == 0 || plen >= sizeof(port_buf)) return false;
        memcpy(port_buf, arg, plen);
        port_buf[plen] = '\0';
        if (!parse_uint32_decimation(colon + 1, &dec)) return false;
    } else {
        if (strlen(arg) >= sizeof(port_buf)) return false;
        strcpy(port_buf, arg);
    }

    char *endp = NULL;
    unsigned long p = strtoul(port_buf, &endp, 10);
    if (port_buf[0] == '\0' || *endp != '\0' || p == 0 || p > 65535UL) {
        return false;
    }

    out->port = (uint16_t)p;
    out->decimation = dec;
    return true;
}

static bool parse_stdout_spec(const char *arg, uint32_t *out_dec) {
    if (strcmp(arg, "--stdout") == 0) {
        *out_dec = 1;
        return true;
    }
    if (strncmp(arg, "--stdout:", 9) == 0) {
        return parse_uint32_decimation(arg + 9, out_dec);
    }
    return false;
}

static bool parse_file_spec(const char *arg, char *base_out, size_t base_cap, uint32_t *out_dec) {
    if (!arg || *arg == '\0') return false;
    uint32_t dec = 1;
    const char *colon = strrchr(arg, ':');

    if (colon && parse_uint32_decimation(colon + 1, &dec)) {
        size_t blen = (size_t)(colon - arg);
        if (blen == 0 || blen >= base_cap) return false;
        memcpy(base_out, arg, blen);
        base_out[blen] = '\0';
    } else {
        size_t blen = strlen(arg);
        if (blen == 0 || blen >= base_cap) return false;
        memcpy(base_out, arg, blen + 1);
    }

    *out_dec = dec;
    return true;
}

static void span_parts(const FrameSpan *s,
                       const uint8_t **p1, size_t *n1,
                       const uint8_t **p2, size_t *n2) {
    size_t to_end = (size_t)(s->ring_end - s->start);
    if (s->len <= to_end) {
        *p1 = s->start;
        *n1 = s->len;
        *p2 = s->ring_base;
        *n2 = 0;
    } else {
        *p1 = s->start;
        *n1 = to_end;
        *p2 = s->ring_base;
        *n2 = s->len - to_end;
    }
}

static bool write_all_fd(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static bool write_all_plain_fd(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, p + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static bool send_span_socket(int fd, const FrameSpan *s) {
    const uint8_t *p1, *p2;
    size_t n1, n2;
    span_parts(s, &p1, &n1, &p2, &n2);
    if (!write_all_fd(fd, p1, n1)) return false;
    if (n2 > 0 && !write_all_fd(fd, p2, n2)) return false;
    return true;
}

static bool write_span_plain_fd(int fd, const FrameSpan *s) {
    const uint8_t *p1, *p2;
    size_t n1, n2;
    span_parts(s, &p1, &n1, &p2, &n2);
    if (!write_all_plain_fd(fd, p1, n1)) return false;
    if (n2 > 0 && !write_all_plain_fd(fd, p2, n2)) return false;
    return true;
}

static bool write_span_file(FILE *fp, const FrameSpan *s) {
    const uint8_t *p1, *p2;
    size_t n1, n2;
    span_parts(s, &p1, &n1, &p2, &n2);
    if (fwrite(p1, 1, n1, fp) != n1) return false;
    if (n2 > 0 && fwrite(p2, 1, n2, fp) != n2) return false;
    return true;
}

static void mailbox_init(Mailbox *m) {
    pthread_mutex_init(&m->mtx, NULL);
    pthread_cond_init(&m->cv, NULL);
    m->head = m->tail = m->count = 0;
    m->closed = false;
    m->dropped = 0;
}

static void mailbox_destroy(Mailbox *m) {
    pthread_mutex_destroy(&m->mtx);
    pthread_cond_destroy(&m->cv);
}

static void mailbox_close(Mailbox *m) {
    pthread_mutex_lock(&m->mtx);
    m->closed = true;
    pthread_cond_broadcast(&m->cv);
    pthread_mutex_unlock(&m->mtx);
}

static void mailbox_push(Mailbox *m, const FrameSpan *span) {
    pthread_mutex_lock(&m->mtx);
    if (!m->closed) {
        if (m->count == MAILBOX_CAP) {
            m->head = (m->head + 1) % MAILBOX_CAP;
            m->count--;
            m->dropped++;
        }
        m->slots[m->tail] = *span;
        m->tail = (m->tail + 1) % MAILBOX_CAP;
        m->count++;
        pthread_cond_signal(&m->cv);
    }
    pthread_mutex_unlock(&m->mtx);
}

static bool mailbox_pop(Mailbox *m, FrameSpan *out) {
    pthread_mutex_lock(&m->mtx);
    while (m->count == 0 && !m->closed) {
        pthread_cond_wait(&m->cv, &m->mtx);
    }
    if (m->count == 0 && m->closed) {
        pthread_mutex_unlock(&m->mtx);
        return false;
    }
    *out = m->slots[m->head];
    m->head = (m->head + 1) % MAILBOX_CAP;
    m->count--;
    pthread_mutex_unlock(&m->mtx);
    return true;
}

static uint64_t mailbox_get_dropped(Mailbox *m) {
    pthread_mutex_lock(&m->mtx);
    uint64_t d = m->dropped;
    pthread_mutex_unlock(&m->mtx);
    return d;
}

static void publish_frame(App *app, const FrameSpan *span) {
    for (int i = 0; i < app->num_file_workers; i++) {
        if (app->file_workers[i].enabled) {
            mailbox_push(&app->file_workers[i].mbox, span);
        }
    }

    if (app->stdout_worker.enabled) {
        mailbox_push(&app->stdout_worker.mbox, span);
    }

    pthread_mutex_lock(&app->clients_mtx);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (app->clients[i].active) {
            mailbox_push(&app->clients[i].mbox, span);
        }
    }
    pthread_mutex_unlock(&app->clients_mtx);
}

static void print_stats_snapshot(App *app, const char *label) {
    fprintf(stderr, "=== [%s] worker stats ===\n", label);

    for (int i = 0; i < app->num_file_workers; i++) {
        FileWorkerCtx *fw = &app->file_workers[i];
        if (!fw->enabled) continue;
        uint64_t sent = __atomic_load_n(&fw->sent, __ATOMIC_RELAXED);
        uint64_t dropped = mailbox_get_dropped(&fw->mbox);
        fprintf(stderr, "  file[%d] (%s, 1:%u): sent=%llu dropped=%llu\n",
                i, fw->base_name, fw->decimation,
                (unsigned long long)sent, (unsigned long long)dropped);
    }

    if (app->stdout_worker.enabled) {
        uint64_t sent = __atomic_load_n(&app->stdout_worker.sent, __ATOMIC_RELAXED);
        uint64_t dropped = mailbox_get_dropped(&app->stdout_worker.mbox);
        fprintf(stderr, "  stdout (1:%u): sent=%llu dropped=%llu\n",
                app->stdout_worker.decimation,
                (unsigned long long)sent, (unsigned long long)dropped);
    }

    pthread_mutex_lock(&app->clients_mtx);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        ClientSlot *c = &app->clients[i];
        if (!c->active) continue;
        uint64_t sent = __atomic_load_n(&c->sent, __ATOMIC_RELAXED);
        uint64_t dropped = mailbox_get_dropped(&c->mbox);
        fprintf(stderr, "  tcp_client[%d] (port %u, 1:%u): sent=%llu dropped=%llu\n",
                c->slot_id, c->port, c->decimation,
                (unsigned long long)sent, (unsigned long long)dropped);
    }
    pthread_mutex_unlock(&app->clients_mtx);
}

static void *stats_reporter_thread(void *arg) {
    App *app = (App *)arg;

    pthread_mutex_lock(&app->stats_mtx);
    while (is_running(app)) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += STATS_INTERVAL_S;

        int rc = pthread_cond_timedwait(&app->stats_cv, &app->stats_mtx, &deadline);
        if (!is_running(app)) break;
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&app->stats_mtx);
            print_stats_snapshot(app, "periodic");
            pthread_mutex_lock(&app->stats_mtx);
        }
    }
    pthread_mutex_unlock(&app->stats_mtx);
    return NULL;
}

static void *file_worker_thread(void *arg) {
    FileWorkerCtx *fw = (FileWorkerCtx *)arg;
    char mjpeg_path[4120];
    char idx_path[4120];

    snprintf(mjpeg_path, sizeof(mjpeg_path), "%s.MJPEG", fw->base_name);
    snprintf(idx_path, sizeof(idx_path), "%s.INDEX", fw->base_name);

    FILE *fp_data = fopen(mjpeg_path, "wb");
    FILE *fp_idx  = fopen(idx_path, "wb");
    if (!fp_data || !fp_idx) {
        perror("fopen file worker output/index");
        if (fp_data) fclose(fp_data);
        if (fp_idx) fclose(fp_idx);
        return NULL;
    }

    uint32_t dec = (fw->decimation == 0) ? 1u : fw->decimation;
    uint64_t seen = 0;
    uint64_t current_offset = 0;
    FrameSpan span;

    while (mailbox_pop(&fw->mbox, &span)) {
        if ((seen++ % dec) != 0) {
            continue;
        }

        if (!write_span_file(fp_data, &span)) {
            perror("write_span_file");
            break;
        }

	fprintf(fp_idx, "seq=% 20llu, ts_ns=% 20llu, offset=% 20llu, len=% 20llu" "\n", span.seq, span.ts_ns, current_offset, (uint64_t)span.len);

        current_offset += (uint64_t)span.len;
        __atomic_fetch_add(&fw->sent, 1u, __ATOMIC_RELAXED);
    }

    fflush(fp_idx);
    fflush(fp_data);
    fclose(fp_idx);
    fclose(fp_data);
    return NULL;
}

static void *stdout_worker_thread(void *arg) {
    StdoutWorkerCtx *sw = (StdoutWorkerCtx *)arg;
    uint32_t dec = (sw->decimation == 0) ? 1u : sw->decimation;
    uint64_t seen = 0;
    FrameSpan span;

    while (mailbox_pop(&sw->mbox, &span)) {
        if ((seen++ % dec) != 0) {
            continue;
        }

        if (!write_span_plain_fd(STDOUT_FILENO, &span)) {
            break;
        }
        __atomic_fetch_add(&sw->sent, 1u, __ATOMIC_RELAXED);
    }

    return NULL;
}

static void *tcp_client_thread(void *arg) {
    ClientSlot *slot = (ClientSlot *)arg;
    uint32_t dec = (slot->decimation == 0) ? 1u : slot->decimation;
    uint64_t seen = 0;
    FrameSpan span;

    while (mailbox_pop(&slot->mbox, &span)) {
        if ((seen++ % dec) != 0) {
            continue;
        }

        if (!send_span_socket(slot->fd, &span)) break;

        __atomic_fetch_add(&slot->sent, 1u, __ATOMIC_RELAXED);
    }

    close(slot->fd);
    slot->fd = -1;
    pthread_mutex_lock(&slot->mbox.mtx);
    slot->active = false;
    pthread_mutex_unlock(&slot->mbox.mtx);
    return NULL;
}

static void *tcp_listener_thread(void *arg) {
    ListenerCtx *lctx = (ListenerCtx *)arg;
    App *app = lctx->app;
    uint16_t port = lctx->cfg.port;
    uint32_t dec  = lctx->cfg.decimation;

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return NULL; }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(srv, 16) < 0) {
        perror("bind/listen");
        close(srv);
        return NULL;
    }

    while (is_running(app)) {
        int cfd = accept(srv, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (!is_running(app)) break;
            perror("accept");
            break;
        }

        int chosen = -1;
        pthread_mutex_lock(&app->clients_mtx);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!app->clients[i].active) {
                chosen = i;
                app->clients[i].active = true;
                app->clients[i].fd = cfd;
                app->clients[i].port = port;
                app->clients[i].decimation = dec;
                app->clients[i].sent = 0;
                app->clients[i].mbox.head = app->clients[i].mbox.tail = 0;
                app->clients[i].mbox.count = app->clients[i].mbox.dropped = 0;
                app->clients[i].mbox.closed = false;
                break;
            }
        }
        pthread_mutex_unlock(&app->clients_mtx);

        if (chosen < 0) {
            close(cfd);
            continue;
        }

        if (pthread_create(&app->clients[chosen].tid, NULL,
                           tcp_client_thread, &app->clients[chosen]) != 0) {
            perror("pthread_create client");
            close(cfd);
            pthread_mutex_lock(&app->clients_mtx);
            app->clients[chosen].active = false;
            app->clients[chosen].fd = -1;
            pthread_mutex_unlock(&app->clients_mtx);
        }
    }

    close(srv);
    return NULL;
}

static void run_stdin_parser(App *app) {
    uint64_t total_read  = 0;
    uint64_t total_scan  = 0;
    uint64_t frame_start = 0;
    bool     in_frame    = false;
    uint64_t seq         = 0;

    while (is_running(app)) {
        size_t w_idx = (size_t)(total_read % RING_CAP);
        size_t contiguous_space = RING_CAP - w_idx;

        ssize_t n = read(STDIN_FILENO, app->ring + w_idx, contiguous_space);
        if (n < 0) {
            if (errno == EINTR) {
                if (!is_running(app)) break;
                continue;
            }
            perror("read stdin");
            break;
        }
        if (n == 0) break;

        uint64_t chunk_ts = now_monotonic_ns();
        total_read += (uint64_t)n;

        if (total_scan < total_read && total_scan > 0) {
            total_scan--;
        }

        while (total_scan + 1 < total_read) {
            uint8_t b0 = app->ring[(size_t)(total_scan % RING_CAP)];
            uint8_t b1 = app->ring[(size_t)((total_scan + 1) % RING_CAP)];

            if (!in_frame && b0 == 0xFF && b1 == 0xD8) {
                frame_start = total_scan;
                in_frame = true;
                total_scan += 2;
                continue;
            }
            if (in_frame && b0 == 0xFF && b1 == 0xD9) {
                uint64_t frame_end = total_scan + 2;
                size_t len = (size_t)(frame_end - frame_start);

                FrameSpan span = {
                    .seq       = seq++,
                    .ts_ns     = chunk_ts,
                    .ring_base = app->ring,
                    .ring_end  = app->ring + RING_CAP,
                    .start     = app->ring + (size_t)(frame_start % RING_CAP),
                    .end       = app->ring + (size_t)(frame_end % RING_CAP),
                    .len       = len
                };
                publish_frame(app, &span);

                in_frame = false;
                total_scan = frame_end;
                frame_start = frame_end;
                continue;
            }
            total_scan++;
        }
    }
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [--tcp <port>[:<dec>]]... [--file <base>[:<dec>]]... [--stdout[:<dec>]]\n",
            prog);
}

int main(int argc, char **argv) {
    install_signal_handlers();

    App app;
    memset(&app, 0, sizeof(app));
    app.running = true;
    app.stdout_worker.app = &app;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tcp") == 0) {
            if (i + 1 >= argc || app.num_tcp_cfgs >= MAX_TCP_PORTS) {
                print_usage(argv[0]);
                return 1;
            }
            if (!parse_tcp_spec(argv[++i], &app.tcp_cfgs[app.num_tcp_cfgs])) {
                fprintf(stderr, "Invalid --tcp spec: %s\n", argv[i]);
                return 1;
            }
            app.num_tcp_cfgs++;
        } else if (strcmp(argv[i], "--file") == 0) {
            if (i + 1 >= argc || app.num_file_workers >= MAX_FILE_WORKERS) {
                print_usage(argv[0]);
                return 1;
            }
            FileWorkerCtx *fw = &app.file_workers[app.num_file_workers];
            fw->app = &app;
            if (!parse_file_spec(argv[++i], fw->base_name, sizeof(fw->base_name), &fw->decimation)) {
                fprintf(stderr, "Invalid --file spec: %s\n", argv[i]);
                return 1;
            }
            fw->enabled = true;
            app.num_file_workers++;
        } else if (strcmp(argv[i], "--stdout") == 0 || strncmp(argv[i], "--stdout:", 9) == 0) {
            uint32_t sdec = 1;
            if (!parse_stdout_spec(argv[i], &sdec)) {
                fprintf(stderr, "Invalid --stdout spec: %s\n", argv[i]);
                return 1;
            }
            app.stdout_worker.enabled = true;
            app.stdout_worker.decimation = sdec;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    app.ring = malloc(RING_CAP);
    if (!app.ring) { perror("malloc ring"); return 1; }

    pthread_mutex_init(&app.stats_mtx, NULL);
    pthread_cond_init(&app.stats_cv, NULL);
    pthread_mutex_init(&app.clients_mtx, NULL);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        app.clients[i].slot_id = i;
        app.clients[i].fd = -1;
        app.clients[i].active = false;
        mailbox_init(&app.clients[i].mbox);
    }

    pthread_t stats_tid;
    if (pthread_create(&stats_tid, NULL, stats_reporter_thread, &app) != 0) {
        perror("pthread_create stats");
        return 1;
    }

    for (int i = 0; i < app.num_file_workers; i++) {
        mailbox_init(&app.file_workers[i].mbox);
        if (pthread_create(&app.file_workers[i].tid, NULL,
                           file_worker_thread, &app.file_workers[i]) != 0) {
            perror("pthread_create file worker");
            return 1;
        }
    }

    if (app.stdout_worker.enabled) {
        mailbox_init(&app.stdout_worker.mbox);
        if (pthread_create(&app.stdout_worker.tid, NULL,
                           stdout_worker_thread, &app.stdout_worker) != 0) {
            perror("pthread_create stdout worker");
            return 1;
        }
    }

    for (int i = 0; i < app.num_tcp_cfgs; i++) {
        app.listeners[i].app = &app;
        app.listeners[i].cfg = app.tcp_cfgs[i];
        if (pthread_create(&app.listeners[i].tid, NULL,
                           tcp_listener_thread, &app.listeners[i]) != 0) {
            perror("pthread_create listener");
            return 1;
        }
    }

    run_stdin_parser(&app);

    app.running = false;
    pthread_mutex_lock(&app.stats_mtx);
    pthread_cond_broadcast(&app.stats_cv);
    pthread_mutex_unlock(&app.stats_mtx);
    pthread_join(stats_tid, NULL);

    for (int i = 0; i < app.num_file_workers; i++) {
        if (app.file_workers[i].enabled) {
            mailbox_close(&app.file_workers[i].mbox);
            pthread_join(app.file_workers[i].tid, NULL);
        }
    }

    if (app.stdout_worker.enabled) {
        mailbox_close(&app.stdout_worker.mbox);
        pthread_join(app.stdout_worker.tid, NULL);
    }

    pthread_mutex_lock(&app.clients_mtx);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (app.clients[i].active) mailbox_close(&app.clients[i].mbox);
    }
    pthread_mutex_unlock(&app.clients_mtx);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (app.clients[i].active) pthread_join(app.clients[i].tid, NULL);
    }

    for (int i = 0; i < app.num_tcp_cfgs; i++) {
        pthread_cancel(app.listeners[i].tid);
        pthread_join(app.listeners[i].tid, NULL);
    }

    print_stats_snapshot(&app, "final");

    for (int i = 0; i < MAX_CLIENTS; i++) mailbox_destroy(&app.clients[i].mbox);
    for (int i = 0; i < app.num_file_workers; i++) {
        if (app.file_workers[i].enabled) mailbox_destroy(&app.file_workers[i].mbox);
    }
    if (app.stdout_worker.enabled) {
        mailbox_destroy(&app.stdout_worker.mbox);
    }
    pthread_mutex_destroy(&app.clients_mtx);
    pthread_cond_destroy(&app.stats_cv);
    pthread_mutex_destroy(&app.stats_mtx);
    free(app.ring);
    return 0;
}

