// Course demo CGI: emit its headers and body on stdout and report elapsed time.
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(void)
{
    const char *query = getenv("QUERY_STRING");
    double delay = 5.0;
    if (query && *query) {
        char *end;
        delay = strtod(query, &end);
        if (end == query || (*end && *end != '&') || !isfinite(delay) ||
            delay < 0 || delay > 60) return 1;
    }
    struct timespec start, finish;
    struct timespec remaining = {(time_t)delay, (long)((delay - (time_t)delay) * 1e9)};
    if (clock_gettime(CLOCK_MONOTONIC, &start)) return 1;
    while (nanosleep(&remaining, &remaining) < 0) if (errno != EINTR) return 1;
    if (clock_gettime(CLOCK_MONOTONIC, &finish)) return 1;
    double elapsed = finish.tv_sec - start.tv_sec + (finish.tv_nsec - start.tv_nsec) / 1e9;
    char content[256];
    int n = snprintf(content, sizeof content, "<p>Welcome to the CGI program</p>\r\n"
        "<p>My only purpose is to waste time on the server!</p>\r\n"
        "<p>I spun for %.2f seconds</p>\r\n", elapsed);
    if (n < 0 || (size_t)n >= sizeof content) return 1;
    return printf("Content-length: %d\r\nContent-type: text/html\r\n\r\n%s", n, content) < 0;
}
