#include "segel.h"
#include "request.h"
#include "log.h"
#include <limits.h>
#include <poll.h>

/*
 * HW3 server:
 * ./server <portnum> <threads> <queue_size> <debug_sleep_time>
 */

// =========================
// Bounded FIFO queue
// (enforces waiting + active <= queue_size)
// =========================

typedef struct {
    int connfd;
    struct timeval arrival;   // time at accept()
} request_item;

typedef struct {
    request_item *buf;
    int cap;

    int head;
    int tail;

    int count;   // waiting in queue
    int stopping;
    int active;  // being processed by workers

    pthread_mutex_t mtx;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} request_queue;

static void queue_init(request_queue *q, int cap) {
    q->buf = (request_item*)malloc(sizeof(request_item) * cap);
    if (!q->buf) {
        perror("malloc");
        exit(1);
    }
    q->cap = cap;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->active = 0;
    q->stopping = 0;

    int err = pthread_mutex_init(&q->mtx, NULL);
    if (err) posix_error(err, "queue mutex");
    err = pthread_cond_init(&q->not_empty, NULL);
    if (err) posix_error(err, "not-empty condition");
    err = pthread_cond_init(&q->not_full, NULL);
    if (err) posix_error(err, "not-full condition");
}

// Master blocks when (waiting + active) == cap
static void queue_wait_space(request_queue *q) {
    pthread_mutex_lock(&q->mtx);
    while (q->count + q->active == q->cap) {
        pthread_cond_wait(&q->not_full, &q->mtx);
    }
    pthread_mutex_unlock(&q->mtx);
}

// Only the master admits new work, so the observed space stays available.
static void queue_push(request_queue *q, request_item it) {
    pthread_mutex_lock(&q->mtx);
    q->buf[q->tail] = it;
    q->tail = (q->tail + 1) % q->cap;
    q->count++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mtx);
}

// Worker blocks when empty; on pop it becomes "active"
static request_item queue_pop_make_active(request_queue *q) {
    pthread_mutex_lock(&q->mtx);
    while (!q->stopping && q->count == 0) {
        pthread_cond_wait(&q->not_empty, &q->mtx);
    }

    if (!q->count) {
        pthread_mutex_unlock(&q->mtx);
        return (request_item){.connfd = -1};
    }
    request_item it = q->buf[q->head];
    q->head = (q->head + 1) % q->cap;
    q->count--;
    q->active++;

    pthread_mutex_unlock(&q->mtx);
    return it;
}

// Worker calls when done so master can accept more
static void queue_finish_active(request_queue *q) {
    pthread_mutex_lock(&q->mtx);
    q->active--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mtx);
}

// =========================
// Thread pool
// =========================

typedef struct {
    request_queue *q;
    server_log log;
} worker_args;

static pthread_mutex_t g_id_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_next_id = 1; // IDs must be 1..N

static void* worker_main(void *arg) {
    worker_args *wa = (worker_args*)arg;

    // Per-thread persistent stats
    struct Threads_stats stats = {0};
    pthread_mutex_lock(&g_id_mtx);
    stats.id = g_next_id++;
    pthread_mutex_unlock(&g_id_mtx);

    while (1) {
        request_item it = queue_pop_make_active(wa->q);

        if (it.connfd < 0) break;
        time_stats tm;
        memset(&tm, 0, sizeof(tm));
        tm.task_arrival = it.arrival;
        gettimeofday(&tm.task_dispatch, NULL);

        requestHandle(it.connfd, tm, &stats, wa->log);

        close(it.connfd);
        queue_finish_active(wa->q);
    }

    return NULL;
}

// =========================
// Args parsing
// =========================

static int number(const char *text, int minimum, int maximum) {
    char *end;
    errno = 0;
    long n = strtol(text, &end, 10);
    if (end == text || *end || errno || n < minimum || n > maximum) {
        fprintf(stderr, "Invalid integer argument: %s\n", text);
        exit(EXIT_FAILURE);
    }
    return (int)n;
}

static void getargs(int *port, int *nthreads, int *qsize, int *debug_sleep_time,
                    int argc, char *argv[])
{
    if (argc != 5) {
        fprintf(stderr,
                "Usage: %s <portnum> <threads> <queue_size> <debug_sleep_time>\n",
                argv[0]);
        exit(1);
    }

    *port = number(argv[1], 1, 65535);
    *nthreads = number(argv[2], 1, INT_MAX - 1);
    *qsize = number(argv[3], 1, INT_MAX);
    *debug_sleep_time = number(argv[4], INT_MIN, INT_MAX);
}

// Signals wake the master without invoking pthread routines in a handler.
static volatile sig_atomic_t wake_fd;
static void wake_master(int signo) {
    (void)signo;
    int saved = errno;
    char byte = 0;
    ssize_t sent;
    do { sent = write(wake_fd, &byte, 1); } while (sent < 0 && errno == EINTR);
    // EAGAIN means the pipe already holds a wakeup.
    errno = saved;
}

int main(int argc, char *argv[])
{
    // avoid termination on SIGPIPE if client closes early
    signal(SIGPIPE, SIG_IGN);

    int port, nthreads, qsize, debug_sleep_time;
    getargs(&port, &nthreads, &qsize, &debug_sleep_time, argc, argv);

    // Create global log and set debug sleep
    server_log log = create_log();
    set_log_debug_sleep(log, debug_sleep_time);

    // Init queue
    request_queue q;
    queue_init(&q, qsize);

    int wake[2];
    if (pipe(wake) || fcntl(wake[1], F_SETFL, O_NONBLOCK) < 0) unix_error("wake pipe");
    wake_fd = wake[1];
    struct sigaction action = {0};
    action.sa_handler = wake_master;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) || sigaction(SIGINT, &action, NULL))
        unix_error("sigaction");

    // Start worker pool
    int listenfd = Open_listenfd(port);
    if (fcntl(listenfd, F_SETFL, O_NONBLOCK) < 0) unix_error("nonblocking listener");
    pthread_t *tids = calloc((size_t)nthreads, sizeof(*tids));
    if (!tids) {
        perror("malloc");
        exit(1);
    }

    worker_args wa;
    wa.q = &q;
    wa.log = log;

    for (int i = 0; i < nthreads; i++) {
        int err = pthread_create(&tids[i], NULL, worker_main, &wa);
        if (err) posix_error(err, "pthread_create");
    }

    struct pollfd ready[] = {{listenfd, POLLIN, 0}, {wake[0], POLLIN, 0}};
    int result = EXIT_SUCCESS;

    while (1) {
        struct sockaddr_in clientaddr;
        socklen_t clientlen = sizeof(clientaddr);

        queue_wait_space(&q);
        if (poll(ready, 2, -1) < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            result = EXIT_FAILURE;
            break;
        }
        if (ready[1].revents) break;
        int connfd = accept(listenfd, (SA *)&clientaddr, &clientlen);
        if (connfd < 0) {
            if (errno == EINTR || errno == ECONNABORTED || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            perror("accept");
            result = EXIT_FAILURE;
            break;
        }

        request_item it;
        it.connfd = connfd;
        gettimeofday(&it.arrival, NULL);  // Stat-Req-Arrival

        struct timeval timeout = {10, 0};
        if (fcntl(connfd, F_SETFL, 0) < 0 ||
            setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) ||
            setsockopt(connfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout)) {
            close(connfd);
            continue;
        }
        queue_push(&q, it);
    }

    close(listenfd);
    pthread_mutex_lock(&q.mtx);
    q.stopping = 1;
    pthread_cond_broadcast(&q.not_empty);
    pthread_mutex_unlock(&q.mtx);
    for (int i = 0; i < nthreads; ++i) pthread_join(tids[i], NULL);
    free(tids);
    free(q.buf);
    pthread_mutex_destroy(&q.mtx);
    pthread_cond_destroy(&q.not_empty);
    pthread_cond_destroy(&q.not_full);
    destroy_log(log);
    signal(SIGTERM, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    close(wake[1]);
    close(wake[0]);
    return result;
}
