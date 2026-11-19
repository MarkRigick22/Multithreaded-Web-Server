#include "segel.h"
#include "log.h"
#include <stdint.h>

struct Server_Log {
    // log storage
    char*  buf;
    size_t len;
    size_t cap;

    // readers-writer with writer priority
    pthread_mutex_t mtx;
    pthread_cond_t  can_read;
    pthread_cond_t  can_write;

    int active_readers;   // readers currently inside
    int active_writer;    // 0/1
    int waiting_writers;  // writers queued up (priority trigger)

    int debug_sleep_seconds;
};

static int ensure_capacity(server_log log, size_t need)
{
    if (need <= log->cap) return 0;

    size_t newcap = (log->cap == 0) ? 1024 : log->cap;
    while (newcap < need) {
        if (newcap > SIZE_MAX / 2) return -1;
        newcap *= 2;
    }

    char* nb = (char*)realloc(log->buf, newcap);
    if (!nb) {
        return -1;
    }
    log->buf = nb;
    log->cap = newcap;
    return 0;
}

/* ---------- internal RW primitives (writer priority) ---------- */

static void reader_lock(server_log log)
{
    pthread_mutex_lock(&log->mtx);
    while (log->active_writer || log->waiting_writers > 0) {
        pthread_cond_wait(&log->can_read, &log->mtx);
    }
    log->active_readers++;
    pthread_mutex_unlock(&log->mtx);
}

static void reader_unlock(server_log log)
{
    pthread_mutex_lock(&log->mtx);
    log->active_readers--;
    if (log->active_readers == 0 && log->waiting_writers > 0) {
        pthread_cond_signal(&log->can_write);
    }
    pthread_mutex_unlock(&log->mtx);
}

static void writer_lock(server_log log)
{
    pthread_mutex_lock(&log->mtx);
    log->waiting_writers++;
    while (log->active_writer || log->active_readers > 0) {
        pthread_cond_wait(&log->can_write, &log->mtx);
    }
    log->waiting_writers--;
    log->active_writer = 1;
    pthread_mutex_unlock(&log->mtx);
}

static void writer_unlock(server_log log)
{
    pthread_mutex_lock(&log->mtx);
    log->active_writer = 0;

    if (log->waiting_writers > 0) {
        pthread_cond_signal(&log->can_write);     // writers first
    } else {
        pthread_cond_broadcast(&log->can_read);   // then readers
    }
    pthread_mutex_unlock(&log->mtx);
}

/* ---------- public helpers used by request.c ---------- */

void log_read_lock(server_log log)   { reader_lock(log); }
void log_read_unlock(server_log log) { reader_unlock(log); }
void log_write_lock(server_log log)  { writer_lock(log); }
void log_write_unlock(server_log log){ writer_unlock(log); }

void log_debug_sleep(server_log log)
{
    if (!log) return;

    int s;
    pthread_mutex_lock(&log->mtx);
    s = log->debug_sleep_seconds;
    pthread_mutex_unlock(&log->mtx);

    if (s > 0) {
        unsigned remaining = (unsigned)s;
        while (remaining) remaining = sleep(remaining);
    }
}

char *get_log_locked(server_log log, size_t *length)
{
    char *copy = malloc(log->len + 1);
    if (copy) {
        memcpy(copy, log->buf, log->len + 1);
        *length = log->len;
    }
    return copy;
}

int add_to_log_locked(server_log log, const char *data, size_t length)
{
    if (length >= SIZE_MAX - log->len ||
        ensure_capacity(log, log->len + length + 1) < 0) return -1;
    memcpy(log->buf + log->len, data, length);
    log->len += length;
    log->buf[log->len] = 0;
    return 0;
}

/* ---------- required API ---------- */

server_log create_log(void)
{
    server_log log = (server_log)malloc(sizeof(struct Server_Log));
    if (!log) {
        unix_error("malloc failed in create_log");
    }

    log->buf = NULL;
    log->len = 0;
    log->cap = 0;

    int err = pthread_mutex_init(&log->mtx, NULL);
    if (err) posix_error(err, "log mutex");
    err = pthread_cond_init(&log->can_read, NULL);
    if (err) posix_error(err, "reader condition");
    err = pthread_cond_init(&log->can_write, NULL);
    if (err) posix_error(err, "writer condition");

    log->active_readers = 0;
    log->active_writer = 0;
    log->waiting_writers = 0;

    log->debug_sleep_seconds = 0;

    if (ensure_capacity(log, 1) < 0) unix_error("create_log");
    log->buf[0] = '\0';

    return log;
}

void destroy_log(server_log log)
{
    if (!log) return;

    writer_lock(log);
    free(log->buf);
    log->buf = NULL;
    log->len = 0;
    log->cap = 0;
    writer_unlock(log);

    pthread_mutex_destroy(&log->mtx);
    pthread_cond_destroy(&log->can_read);
    pthread_cond_destroy(&log->can_write);

    free(log);
}

void set_log_debug_sleep(server_log log, int seconds)
{
    if (!log) return;

    pthread_mutex_lock(&log->mtx);
    log->debug_sleep_seconds = (seconds > 0) ? seconds : 0;
    pthread_mutex_unlock(&log->mtx);
}

