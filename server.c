#include "segel.h"
#include "request.h"
#include "log.h"

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

    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

static void queue_destroy(request_queue *q) {
    free(q->buf);
    pthread_mutex_destroy(&q->mtx);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

// Master blocks when (waiting + active) == cap
static void queue_push(request_queue *q, request_item it) {
    pthread_mutex_lock(&q->mtx);
    while (q->count + q->active == q->cap) {
        pthread_cond_wait(&q->not_full, &q->mtx);
    }

    q->buf[q->tail] = it;
    q->tail = (q->tail + 1) % q->cap;
    q->count++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mtx);
}

// Worker blocks when empty; on pop it becomes "active"
static request_item queue_pop_make_active(request_queue *q) {
    pthread_mutex_lock(&q->mtx);
    while (q->count == 0) {
        pthread_cond_wait(&q->not_empty, &q->mtx);
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
    threads_stats t = (threads_stats)malloc(sizeof(struct Threads_stats));
    if (!t) {
        perror("malloc");
        return NULL;
    }

    pthread_mutex_lock(&g_id_mtx);
    t->id = g_next_id++;
    pthread_mutex_unlock(&g_id_mtx);

    t->stat_req  = 0;
    t->dynm_req  = 0;
    t->post_req  = 0;
    t->total_req = 0;

    while (1) {
        request_item it = queue_pop_make_active(wa->q);

        time_stats tm;
        memset(&tm, 0, sizeof(tm));
        tm.task_arrival = it.arrival;
        gettimeofday(&tm.task_dispatch, NULL);

        requestHandle(it.connfd, tm, t, wa->log);

        Close(it.connfd);
        queue_finish_active(wa->q);
    }

    // unreachable
    // free(t);
    // return NULL;
}

// =========================
// Args parsing
// =========================

static void getargs(int *port, int *nthreads, int *qsize, int *debug_sleep_time,
                    int argc, char *argv[])
{
    if (argc != 5) {
        fprintf(stderr,
                "Usage: %s <portnum> <threads> <queue_size> <debug_sleep_time>\n",
                argv[0]);
        exit(1);
    }

    *port = atoi(argv[1]);
    *nthreads = atoi(argv[2]);
    *qsize = atoi(argv[3]);
    *debug_sleep_time = atoi(argv[4]);

    if (*port <= 0 || *nthreads <= 0 || *qsize <= 0) {
        fprintf(stderr, "Invalid arguments: port/threads/queue_size must be > 0\n");
        exit(1);
    }
    // debug_sleep_time: <=0 => no sleep, >0 => sleep seconds (handled by log)
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

    // Start worker pool
    pthread_t *tids = (pthread_t*)malloc(sizeof(pthread_t) * nthreads);
    if (!tids) {
        perror("malloc");
        exit(1);
    }

    worker_args wa;
    wa.q = &q;
    wa.log = log;

    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&tids[i], NULL, worker_main, &wa) != 0) {
            perror("pthread_create");
            exit(1);
        }
    }

    int listenfd = Open_listenfd(port);

    while (1) {
        struct sockaddr_in clientaddr;
        socklen_t clientlen = sizeof(clientaddr);

        int connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);

        request_item it;
        it.connfd = connfd;
        gettimeofday(&it.arrival, NULL);  // Stat-Req-Arrival

        queue_push(&q, it);
    }

    // unreachable (server runs forever)
    // destroy_log(log);
    // queue_destroy(&q);
    // free(tids);
    // Close(listenfd);
    // return 0;
}
