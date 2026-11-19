#ifndef SERVER_LOG_H
#define SERVER_LOG_H

#include "segel.h"

// Opaque log handle
typedef struct Server_Log* server_log;

// Creates a new server log instance
server_log create_log();

// Destroys and frees the log
void destroy_log(server_log log);

// Returns the log contents as a string (null-terminated)
// NOTE: caller is responsible for freeing *dst
int get_log(server_log log, char** dst);

// Appends a new entry to the log
void add_to_log(server_log log, const char* data, int data_len);

/*
 * Debug sleep control:
 * If seconds > 0, every writer sleeps(seconds) INSIDE the writing critical section.
 * If seconds <= 0, no sleep.
 */
void set_log_debug_sleep(server_log log, int seconds);

/* ---- Extra helpers used by your request.c ---- */
/* Locks: writer-priority RW */
void log_read_lock(server_log log);
void log_read_unlock(server_log log);
void log_write_lock(server_log log);
void log_write_unlock(server_log log);

/* Sleep INSIDE writer CS if enabled (call only after log_write_lock) */
void log_writer_debug_sleep(server_log log);

/* "Locked" versions (assume the matching lock is already held) */
int  get_log_locked(server_log log, char** dst); // assumes read lock held
void add_to_log_locked(server_log log, const char* data, int data_len); // assumes write lock held

#endif // SERVER_LOG_H
