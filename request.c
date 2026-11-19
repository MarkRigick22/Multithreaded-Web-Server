#include "request.h"
#include <limits.h>
#include <spawn.h>

// The assignment uses a literal second colon at the start of each Stat value.
static int format_stats(char *buf, size_t size, threads_stats t, time_stats tm)
{
    int n = snprintf(buf, size,
        "Stat-Req-Arrival:: %ld.%06ld\r\nStat-Req-Dispatch:: %ld.%06ld\r\n"
        "Stat-Log-Arrival:: %ld.%06ld\r\nStat-Log-Dispatch:: %ld.%06ld\r\n"
        "Stat-Thread-Id:: %d\r\nStat-Thread-Count:: %" PRIu64 "\r\n"
        "Stat-Thread-Static:: %" PRIu64 "\r\nStat-Thread-Dynamic:: %" PRIu64 "\r\n"
        "Stat-Thread-Post:: %" PRIu64 "\r\n\r\n",
        (long)tm.task_arrival.tv_sec, (long)tm.task_arrival.tv_usec,
        (long)tm.task_dispatch.tv_sec, (long)tm.task_dispatch.tv_usec,
        (long)tm.log_enter.tv_sec, (long)tm.log_enter.tv_usec,
        (long)tm.log_exit.tv_sec, (long)tm.log_exit.tv_usec,
        t->id, t->total_req, t->stat_req, t->dynm_req, t->post_req);
    return n >= 0 && (size_t)n < size ? n : -1;
}

static int send_headers(int fd, const char *status, const char *type, const char *stats,
                    size_t length)
{
    char header[2048];
    int n = snprintf(header, sizeof header,
        "HTTP/1.0 %s\r\nServer: OS-HW3 Web Server\r\nContent-Length: %zu\r\n"
        "Content-Type: %s\r\nConnection: close\r\n%s", status, length, type, stats);
    return n > 0 && (size_t)n < sizeof header && rio_writen(fd, header, n) == n;
}

// Consume bounded CRLF-terminated headers. EOF is terminal even mid-request.
static int read_headers(rio_t *rio)
{
    char line[MAXLINE];
    for (unsigned count = 0; count < 128; ++count) {
        ssize_t n = rio_readlineb(rio, line, sizeof line);
        if (n < 2 || memchr(line, 0, (size_t)n) || strcmp(line + n - 2, "\r\n"))
            return -1;
        if (n == 2) return 0;
        char *colon = strchr(line, ':');
        if (!colon || colon == line) return -1;
        for (char *p = line; p < colon; ++p)
            if (!isalnum((unsigned char)*p) && !strchr("!#$%&'*+-.^_`|~", *p)) return -1;
    }
    return -1;
}

static const char *filetype(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext) return "text/plain";
    if (!strcmp(ext, ".html")) return "text/html";
    if (!strcmp(ext, ".gif")) return "image/gif";
    if (!strcmp(ext, ".jpg")) return "image/jpeg";
    return "text/plain";
}

// Use a temporary output descriptor so CGI output cannot fill a pipe while
// its parent waits. Only programs installed by the operator belong in public/.
static FILE *run_cgi(const char *path, const char *query)
{
    char env_query[MAXLINE + sizeof "QUERY_STRING="];
    snprintf(env_query, sizeof env_query, "QUERY_STRING=%s", query);
    char *env[] = {env_query, "PATH=/usr/bin:/bin", NULL};
    char *args[] = {(char *)path, NULL};
    FILE *out = tmpfile();
    if (!out) return NULL;
    posix_spawn_file_actions_t actions;
    int err = posix_spawn_file_actions_init(&actions);
    if (err) goto fail;
    err = posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    if (!err) err = posix_spawn_file_actions_adddup2(&actions, fileno(out), 1);
    if (!err) err = posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    pid_t child;
#ifdef __APPLE__
    posix_spawnattr_t attr;
    if (!err) err = posix_spawnattr_init(&attr);
    if (!err) {
        err = posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
        if (!err) err = posix_spawn(&child, path, &actions, &attr, args, env);
        posix_spawnattr_destroy(&attr);
    }
#else
    if (!err) err = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
    if (!err) err = posix_spawn(&child, path, &actions, NULL, args, env);
#endif
    posix_spawn_file_actions_destroy(&actions);
    if (err) goto fail;
    int status;
    while (waitpid(child, &status, 0) < 0) if (errno != EINTR) goto fail;
    if (!WIFEXITED(status) || WEXITSTATUS(status) || fseek(out, 0, SEEK_SET)) goto fail;
    return out;
fail:
    fclose(out);
    return NULL;
}

// Keep course routing and the CGI stdout-as-body wire format.
static const char *prepare_get(char *uri, FILE **src, size_t *length,
                               const char **type, int *dynamic)
{
    if (strstr(uri, "..")) uri = "/home.html";
    *dynamic = strstr(uri, "cgi") != NULL;
    char *query = NULL;
    if (*dynamic && (query = strchr(uri, '?'))) *query++ = 0;
    char path[PATH_MAX], resolved[PATH_MAX], root[PATH_MAX];
    int n = snprintf(path, sizeof path, "public/%s%s", uri,
                     uri[0] && uri[strlen(uri)-1] == '/' ? "home.html" : "");
    if (n < 0 || (size_t)n >= sizeof path) return "414 URI Too Long";
    if (!realpath("public", root) || !realpath(path, resolved))
        return errno == EACCES ? "403 Forbidden" : "404 Not Found";
    size_t prefix = strlen(root);
    if (strncmp(resolved, root, prefix) || resolved[prefix] != '/') return "403 Forbidden";
    struct stat st;
    if (stat(resolved, &st)) return "404 Not Found";
    if (!S_ISREG(st.st_mode) || !(st.st_mode & (*dynamic ? S_IXUSR : S_IRUSR)))
        return "403 Forbidden";
    *src = *dynamic ? run_cgi(resolved, query ? query : "") : fopen(resolved, "rb");
    if (!*src || fstat(fileno(*src), &st) || st.st_size < 0 ||
        (uintmax_t)st.st_size > SIZE_MAX) return "500 Internal Server Error";
    *length = (size_t)st.st_size;
    *type = *dynamic ? "text/html" : filetype(resolved);
    return NULL;
}

void requestHandle(int fd, time_stats tm, threads_stats t, server_log log)
{
    rio_t rio;
    rio_readinitb(&rio, fd);
    char line[MAXLINE], method[32], uri[MAXLINE], version[16], extra;
    ssize_t n = rio_readlineb(&rio, line, sizeof line);
    if (n <= 0) return;
    ++t->total_req;
    const char *error = NULL, *type = "text/plain";
    char stats[1024], *body = NULL;
    FILE *src = NULL;
    size_t length = 0;
    int dynamic = 0;
    if (n < 2 || memchr(line, 0, (size_t)n) || strcmp(line+n-2, "\r\n") ||
        sscanf(line, "%31s %8191s %15s %c", method, uri, version, &extra) != 3 ||
        (strcmp(version, "HTTP/1.0") && strcmp(version, "HTTP/1.1")) || read_headers(&rio))
        error = "400 Bad Request";
    else if (!strcasecmp(method, "GET")) {
        error = prepare_get(uri, &src, &length, &type, &dynamic);
        if (!error) {
            uint64_t *counter = dynamic ? &t->dynm_req : &t->stat_req;
            ++*counter;
            gettimeofday(&tm.log_enter, NULL);
            log_write_lock(log);
            log_debug_sleep(log);
            // Stamp the commit under exclusive ownership. Log and wire share these bytes.
            gettimeofday(&tm.log_exit, NULL);
            int bytes = format_stats(stats, sizeof stats, t, tm);
            if (bytes < 0 || add_to_log_locked(log, stats, (size_t)bytes)) {
                --*counter;
                error = "500 Internal Server Error";
            }
            log_write_unlock(log);
        }
    } else if (!strcasecmp(method, "POST")) {
        gettimeofday(&tm.log_enter, NULL);
        log_read_lock(log);
        log_debug_sleep(log);
        body = get_log_locked(log, &length);
        gettimeofday(&tm.log_exit, NULL);
        log_read_unlock(log);
        if (body) ++t->post_req;
        else error = "500 Internal Server Error";
    } else error = "501 Not Implemented";

    if (error) {
        length = strlen(error);
        type = "text/plain";
    }
    if (format_stats(stats, sizeof stats, t, tm) >= 0 &&
        send_headers(fd, error ? error : "200 OK", type, stats, length)) {
        if (error || body) {
            (void)rio_writen(fd, error ? (void*)error : body, length);
        } else {
            char block[16384];
            while (length) {
                size_t count = fread(block, 1, length < sizeof block ? length : sizeof block, src);
                if (!count || rio_writen(fd, block, count) != (ssize_t)count) break;
                length -= count;
            }
        }
    }
    if (src) fclose(src);
    free(body);
}
