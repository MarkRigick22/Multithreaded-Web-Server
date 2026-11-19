/*
 * client.c: A very, very primitive HTTP client.
 *
 * Usage:
 *   ./client <host> <port> <filename> <method>
 *
 * NOTE: Not required to change for HW3 grading; useful for your own testing.
 */

#include "segel.h"

// Sends an HTTP request to the server using the given socket
static void clientSend(int fd, char *filename, char* method, char *host)
{
    char buf[MAXLINE];


    // Use CRLF as required by HTTP
    // Keep filename as given (you can pass "/home.html" or "home.html")
    int length = snprintf(buf, sizeof(buf),
             "%s %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Connection: close\r\n"
             "\r\n",
             method, filename, host);
    if (length < 0 || (size_t)length >= sizeof buf) app_error("Request too long");

    printf("Request:\n%s\n", buf);

    Rio_writen(fd, buf, strlen(buf));
}

// Reads and prints the server's HTTP response
void clientPrint(int fd)
{
    rio_t rio;
    char buf[MAXBUF];
    int n;

    Rio_readinitb(&rio, fd);

    // Print headers until blank line
    n = Rio_readlineb(&rio, buf, MAXBUF);
    while (n > 0 && strcmp(buf, "\r\n")) {
        printf("Header: %s", buf);
        n = Rio_readlineb(&rio, buf, MAXBUF);
    }

    while ((n = Rio_readnb(&rio, buf, sizeof buf)) > 0) {
        if (fwrite(buf, 1, (size_t)n, stdout) != (size_t)n) unix_error("stdout");
    }
}

int main(int argc, char *argv[])
{
    char *host, *filename, *method;
    int port, clientfd;

    if (argc != 5) {
        fprintf(stderr, "Usage: %s <host> <port> <filename> <method>\n", argv[0]);
        exit(1);
    }

    host = argv[1];
    char *end;
    long value = strtol(argv[2], &end, 10);
    if (*end || value < 1 || value > 65535) app_error("Invalid port");
    port = (int)value;
    filename = argv[3];
    method = argv[4];

    clientfd = Open_clientfd(host, port);
    if (clientfd < 0) {
        fprintf(stderr, "Error connecting to %s:%d\n", host, port);
        exit(1);
    }

    clientSend(clientfd, filename, method, host);
    clientPrint(clientfd);

    Close(clientfd);
    return 0;
}
