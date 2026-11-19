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
void clientSend(int fd, char *filename, char* method)
{
    char buf[MAXLINE];
    char hostname[MAXLINE];

    Gethostname(hostname, MAXLINE);

    // Use CRLF as required by HTTP
    // Keep filename as given (you can pass "/home.html" or "home.html")
    snprintf(buf, sizeof(buf),
             "%s %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Connection: close\r\n"
             "\r\n",
             method, filename, hostname);

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

    // Print body (line-based; good enough for text/html and text/plain)
    n = Rio_readlineb(&rio, buf, MAXBUF);
    while (n > 0) {
        printf("%s", buf);
        n = Rio_readlineb(&rio, buf, MAXBUF);
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
    port = atoi(argv[2]);
    filename = argv[3];
    method = argv[4];

    clientfd = Open_clientfd(host, port);
    if (clientfd < 0) {
        fprintf(stderr, "Error connecting to %s:%d\n", host, port);
        exit(1);
    }

    clientSend(clientfd, filename, method);
    clientPrint(clientfd);

    Close(clientfd);
    return 0;
}
