#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

#define SOCK_PATH "/tmp/threadbank.sock"
#define MAX_LINE 512

/*
 * Read a single line from fd into buf (like in server.c).
 * Returns:
 *   >0  number of bytes read
 *    0  EOF with no data
 *   -1  error
 */
static ssize_t read_line(int fd, char *buf, size_t maxlen)
{
    size_t pos = 0;
    while (pos + 1 < maxlen) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n == 0) {
            /* EOF */
            break;
        } else if (n < 0) {
            if (errno == EINTR) continue;
            return -1; /* real error */
        }
        buf[pos++] = c;
        if (c == '\n') {
            break;
        }
    }
    if (pos == 0) {
        return 0; /* EOF, nothing read */
    }
    buf[pos] = '\0';
    return (ssize_t)pos;
}

/* Print an error message and terminate the client. */
static void die(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}



/*
 * Client program:
 * Starts with:
 * - Connecting to the UNIX socket /tmp/threadbank.sock.
 * - Waiting for a line from the server ("READY\n").
 * - And prints "ready\n" to stdout
 *  After that:
 * - Reads commands from stdin (from user or testbench).
 * - Sends each command line to the server.
 * - Reads one response line from the server.
 * - Prints that line to stdout.
 * - If the response is "ok: bye", exits.
 */
int main(void)
{
    int sock_fd;
    struct sockaddr_un addr;
    char line[MAX_LINE];

    /* Create socket */
    if ((sock_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        die("socket");
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        die("connect");
    }

    /* Wait for server READY */
    if (read_line(sock_fd, line, sizeof(line)) <= 0) {
        die("read READY");
    }

    /* When we get READY from server, we print exactly 'ready' to stdout */
    printf("ready\n");
    fflush(stdout);

    /* Forward commands from stdin to server, and replies back to stdout */
    while (fgets(line, sizeof(line), stdin) != NULL) {
        size_t len = strlen(line);
        if (len == 0) {
            continue;
        }

        if (write(sock_fd, line, len) < 0) {
            die("write to server");
        }

        if (read_line(sock_fd, line, sizeof(line)) <= 0) {
            die("read from server");
        }

        fputs(line, stdout);
        fflush(stdout);

        if (line[0] == 'o' && line[1] == 'k' && line[2] == ':' &&
            strstr(line, "bye") != NULL) {
            break;
        }
    }

    close(sock_fd);
    return 0;
}
