#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
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

int create_signalfd()
{
    sigset_t mask;
    int fd;
    int rc;

    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGCHLD);

    rc = sigprocmask(SIG_BLOCK, &mask, NULL);
    if (rc < 0) {
        perror("sigprocmask");
        return -1;
    }

    fd = signalfd(-1, &mask, 0);
    if (fd < 0) {
        perror("signalfd");
        return -1;
    }

    return fd;
}

int main(int argc, char *argv[])
{
    int listen_sock = -1;
    int sock = -1;
    int pty_master = -1;
    int signal_fd = -1;
    int port;
    int rc;
    struct sockaddr_in sin;
    pid_t pid;
    int child_exited = 0;
    struct pollfd pfds[3];

    if (argc != 2) {
        printf("usage: tcp-pty-server <port>\n");
        return 1;
    }

    port = atoi(argv[1]);

    if (port == 0 || port > 0xffff) {
        printf("Invalid port\n");
        return 1;
    }

    listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) {
        perror("socket");
        return 1;
    }

    int flag = 1;
    rc = setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    if (rc < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        goto err;
    }

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = inet_addr("127.0.0.1");
    sin.sin_port = htons(port);

    rc = bind(listen_sock, (struct sockaddr *)&sin, sizeof(sin));
    if (rc < 0) {
        perror("bind");
        goto err;
    }

    rc = listen(listen_sock, 1);
    if (rc < 0) {
        perror("listen");
        goto err;
    }

    printf("Listening on 127.0.0.1:%d\n", port);

    sock = accept(listen_sock, NULL, 0);
    if (sock < 0) {
        perror("accept");
        goto err;
    }

    close(listen_sock);
    listen_sock = -1;

    signal_fd = create_signalfd();
    if (signal_fd < 0) {
        goto err;
    }

    pty_master = posix_openpt(O_RDWR);
    if (pty_master < 0) {
        perror("posix_openpt");
        goto err;
    }

    rc = grantpt(pty_master);
    if (rc < 0) {
        perror("grantpt");
        goto err;
    }

    rc = unlockpt(pty_master);
    if (rc < 0) {
        perror("unlockpt");
        goto err;
    }

    pid = fork();

    if (pid == 0) { // child
        int pty_slave;
        char slave_path[256];

        rc = ptsname_r(pty_master, slave_path, sizeof(slave_path));
        if (rc < 0) {
            perror("ptsname_r");
            exit(1);
        }

        pty_slave = open(slave_path, O_RDWR);
        if (pty_slave < 0) {
            perror("open");
            exit(1);
        }

        dup2(pty_slave, STDIN_FILENO);
        dup2(pty_slave, STDOUT_FILENO);
        dup2(pty_slave, STDERR_FILENO);

        setsid();

        execlp("bash", "bash", NULL);
        perror("exec");
        exit(1);
    } else if (pid < 0) {
        perror("fork");
        goto err;
    }

    // parent
    memset(pfds, 0, sizeof(pfds));
    pfds[0].fd = pty_master;
    pfds[0].events = POLLIN;
    pfds[1].fd = sock;
    pfds[1].events = POLLIN;
    pfds[2].fd = signal_fd;
    pfds[2].events = POLLIN;

    while (1) {
        char buf[4096];
        ssize_t rsz;
        size_t i;
        int break_outer = 0;

        rc = poll(pfds, elemof(pfds), 200);
        if (rc < 0)
            break;

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

                    write_all(pty_master, buf, rsz);
                } else if (pfds[i].fd == pty_master) {
                    rsz = read(pty_master, buf, sizeof(buf));

                    if (rsz <= 0) {
                        break_outer = 1;
                        break;
                    }

                    write_all(sock, buf, rsz);
                } else if (pfds[i].fd == signal_fd) {
                    struct signalfd_siginfo fdsi;

                    rsz = read(signal_fd, &fdsi, sizeof(fdsi));
                    if (rsz != sizeof(fdsi)) {
                        break_outer = 1;
                        break;
                    }

                    if (fdsi.ssi_signo == SIGINT || fdsi.ssi_signo == SIGTERM) {
                        break_outer = 1;
                        break;
                    } else if (fdsi.ssi_signo == SIGCHLD) {
                        rc = waitpid(pid, NULL, WNOHANG);
                        if (rc > 0) {
                            break_outer = 1;
                            child_exited = 1;
                            break;
                        }
                    }
                }
            }
        }

        if (break_outer)
            break;
    }

    if (!child_exited) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }

    close(signal_fd);
    close(sock);
    close(pty_master);

    return 0;

err:
    if (listen_sock != -1)
        close(listen_sock);
    if (signal_fd != -1)
        close(signal_fd);
    if (sock != -1)
        close(sock);
    if (pty_master != -1)
        close(pty_master);
    return 1;
}
