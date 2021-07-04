#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#include <termios.h>

#define elemof(x) (sizeof(x) / sizeof(x[0]))

ssize_t write_all(int fd, const void *buf, size_t count)
{
    const uint8_t *ubuf = buf;
    size_t wsz = 0;

    while (wsz < count) {
        ssize_t rc;

        rc = write(fd, &ubuf[wsz], count - wsz);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return rc;
        }

        wsz += (size_t)rc;
    }

    return wsz;
}

int main(int argc, char *argv[])
{
    int sock = -1;
    int pty = -1;
    int rc;
    char *ip;
    int port;
    struct sockaddr_in sin;
    struct pollfd pfds[2];
    char pty_name[256];
    struct termios prev_term_attrs, term_attrs;
    int raw_mode = 0;

    if (argc != 3) {
        printf("usage: tcp-pty-client <ip> <port>\n");
        return 1;
    }

    ip = argv[1];
    port = atoi(argv[2]);

    if (port == 0 || port > 0xffff) {
        fprintf(stderr, "Invalid port\n");
        return 1;
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    rc = inet_aton(ip, &sin.sin_addr);
    sin.sin_port = htons(port);

    if (rc == 0) {
        printf("Invalid ip\n");
        return 1;
    }

    rc = connect(sock, (struct sockaddr *)&sin, sizeof(sin));
    if (rc < 0) {
        perror("connect");
        goto err;
    }

    printf("Connected to %s:%d\n", ip, port);

    rc = tcgetattr(STDIN_FILENO, &prev_term_attrs);
    if (rc < 0) {
        perror("tcgetattr");
        goto err;
    }

    memcpy(&term_attrs, &prev_term_attrs, sizeof(term_attrs));
    cfmakeraw(&term_attrs);
    rc = tcsetattr(STDIN_FILENO, TCSANOW, &term_attrs);
    if (rc < 0) {
        perror("tcsetattr");
        goto err;
    }

    raw_mode = 1;

    rc = ttyname_r(STDIN_FILENO, pty_name, sizeof(pty_name));
    if (rc < 0) {
        perror("ttyname_r");
        goto err;
    }

    pty = open(pty_name, O_RDWR);
    if (pty < 0) {
        perror("open");
        goto err;
    }

    memset(pfds, 0, sizeof(pfds));
    pfds[0].fd = sock;
    pfds[0].events = POLLIN;
    pfds[1].fd = pty;
    pfds[1].events = POLLIN;

    while (1) {
        char buf[4096];
        ssize_t rsz;
        size_t i;
        int break_outer = 0;

        rc = poll(pfds, elemof(pfds), 200);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            break;
        } else if (rc == 0)
            continue;

        for (i = 0; i < elemof(pfds); i++) {
            if (pfds[i].revents & POLLNVAL)
                continue;

            if (pfds[i].revents & (POLLHUP | POLLERR)) {
                break_outer = 1;
                break;
            }

            if (pfds[i].revents & POLLIN) {
                if (pfds[i].fd == sock) {
                    rsz = read(sock, buf, sizeof(buf));

                    if (rsz <= 0) {
                        break_outer = 1;
                        break;
                    }

                    write_all(pty, buf, rsz);
                } else if (pfds[i].fd == pty) {
                    rsz = read(pty, buf, sizeof(buf));

                    if (rsz <= 0) {
                        break_outer = 1;
                        break;
                    }

                    write_all(sock, buf, rsz);
                }
            }
        }

        if (break_outer)
            break;
    }

    close(sock);
    close(pty);
    tcsetattr(STDIN_FILENO, TCSANOW, &prev_term_attrs);

    return 0;

err:
    if (sock != -1)
        close(sock);
    if (pty != -1)
        close(pty);
    if (raw_mode)
        tcsetattr(STDIN_FILENO, TCSANOW, &prev_term_attrs);
    return 1;
}
