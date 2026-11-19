#ifndef __REQUEST_H__
#define __REQUEST_H__

#include "log.h"
#include <inttypes.h>

typedef struct Threads_stats {
    int id;           // Thread ID
    uint64_t stat_req;     // Number of static requests handled
    uint64_t dynm_req;     // Number of dynamic requests handled
    uint64_t post_req;     // Number of POST requests handled
    uint64_t total_req;    // Total number of requests handled
} * threads_stats;

typedef struct Time_stats {
    struct timeval task_arrival;
    struct timeval task_dispatch;
    struct timeval log_enter;
    struct timeval log_exit;
} time_stats;
// Handles a client request.
// - fd: the connection socket
// - arrival: time the request arrived
// - dispatch: time the thread began processing the request
// - t_stats: pointer to the current thread's statistics (must be updated by student)
// - log: server-wide shared log (thread-safe access required)
void requestHandle(int fd, time_stats tm_stats,  threads_stats t_stats, server_log log);
#endif
