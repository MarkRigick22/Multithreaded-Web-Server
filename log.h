#ifndef SERVER_LOG_H
#define SERVER_LOG_H

#include "segel.h"

// Opaque log handle
typedef struct Server_Log* server_log;

// Creates a new server log instance
server_log create_log(void);

// Destroys and frees the log
void destroy_log(server_log log);

void set_log_debug_sleep(server_log log, int seconds);

/* ---- Extra helpers used by your request.c ---- */
/* Locks: writer-priority RW */
void log_read_lock(server_log log);
void log_read_unlock(server_log log);
void log_write_lock(server_log log);
void log_write_unlock(server_log log);

/* Caller holds logical read or write ownership during the delay and operation. */
void log_debug_sleep(server_log log);
char *get_log_locked(server_log log, size_t *length);
int add_to_log_locked(server_log log, const char *data, size_t length);
#endif
