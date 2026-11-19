// White-box test: establish an actual waiting writer before starting the late
// reader. No sleep or assumed scheduler order establishes the priority result.
#include "../log.h"
#include <stdatomic.h>

static pthread_cond_t *reader_condition;
static atomic_int reader_blocked;
static int observe_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    if (cond == reader_condition) atomic_store(&reader_blocked, 1);
    return pthread_cond_wait(cond, mutex);
}
#define pthread_cond_wait observe_wait
#include "../log.c"
#undef pthread_cond_wait
#include <assert.h>
#include <sched.h>
#include <stdatomic.h>

static atomic_int order;
static void *writer(void *p) {
    log_write_lock(p);
    assert(atomic_fetch_add(&order, 1) == 0);
    log_write_unlock(p);
    return NULL;
}
static void *late_reader(void *p) {
    log_read_lock(p);
    assert(atomic_fetch_add(&order, 1) == 1);
    log_read_unlock(p);
    return NULL;
}
static void *peer_reader(void *p) {
    log_read_lock(p);
    log_read_unlock(p);
    return NULL;
}
int main(void) {
    alarm(15);
    server_log log = create_log();
    pthread_t first, second;
    reader_condition = &log->can_read;
    log_read_lock(log);
    assert(!pthread_create(&first, NULL, peer_reader, log));
    assert(!pthread_join(first, NULL)); // Both readers entered at once.
    assert(!pthread_create(&first, NULL, writer, log));
    for (;;) {
        pthread_mutex_lock(&log->mtx);
        int waiting = log->waiting_writers;
        pthread_mutex_unlock(&log->mtx);
        if (waiting) break;
        sched_yield();
    }
    assert(!pthread_create(&second, NULL, late_reader, log));
    while (!atomic_load(&reader_blocked)) sched_yield();
    log_read_unlock(log);
    assert(!pthread_join(first, NULL));
    assert(!pthread_join(second, NULL));
    assert(order == 2);
    destroy_log(log);
    puts("reader overlap and writer precedence passed");
}
