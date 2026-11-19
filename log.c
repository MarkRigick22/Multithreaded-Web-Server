#include "segel.h"
#include "log.h"

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

static void ensure_capacity(server_log log, size_t need)
{
    if (need <= log->cap) return;

    size_t newcap = (log->cap == 0) ? 1024 : log->cap;
    while (newcap < need) newcap *= 2;

    char* nb = (char*)realloc(log->buf, newcap);
    if (!nb) {
        unix_error("realloc failed in ensure_capacity");
    }
    log->buf = nb;
    log->cap = newcap;
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

void log_writer_debug_sleep(server_log log)
{
    if (!log) return;

    int s;
    pthread_mutex_lock(&log->mtx);
    s = log->debug_sleep_seconds;
    pthread_mutex_unlock(&log->mtx);

    if (s > 0) {
        sleep((unsigned int)s);
    }
}

int get_log_locked(server_log log, char** dst)
{
    if (!log || !dst) return 0;

    int out_len = (int)log->len;
    char* copy = (char*)malloc((size_t)out_len + 1);
    if (!copy) {
        unix_error("malloc failed in get_log_locked");
    }

    if (out_len > 0) {
        memcpy(copy, log->buf, (size_t)out_len);
    }
    copy[out_len] = '\0';

    *dst = copy;
    return out_len;
}

void add_to_log_locked(server_log log, const char* data, int data_len)
{
    if (!log || !data || data_len <= 0) return;

    size_t new_len = log->len + (size_t)data_len;
    ensure_capacity(log, new_len + 1);

    memcpy(log->buf + log->len, data, (size_t)data_len);
    log->len = new_len;
    log->buf[log->len] = '\0';
}

/* ---------- required API ---------- */

server_log create_log()
{
    server_log log = (server_log)malloc(sizeof(struct Server_Log));
    if (!log) {
        unix_error("malloc failed in create_log");
    }

    log->buf = NULL;
    log->len = 0;
    log->cap = 0;

    pthread_mutex_init(&log->mtx, NULL);
    pthread_cond_init(&log->can_read, NULL);
    pthread_cond_init(&log->can_write, NULL);

    log->active_readers = 0;
    log->active_writer = 0;
    log->waiting_writers = 0;

    log->debug_sleep_seconds = 0;

    ensure_capacity(log, 1);
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

int get_log(server_log log, char** dst)
{
    if (!log || !dst) return 0;

    reader_lock(log);
    int n = get_log_locked(log, dst);
    reader_unlock(log);

    return n;
}

void add_to_log(server_log log, const char* data, int data_len)
{
    if (!log || !data || data_len <= 0) return;

    writer_lock(log);
    log_writer_debug_sleep(log);          // MUST be inside writer CS
    add_to_log_locked(log, data, data_len);
    writer_unlock(log);
}
