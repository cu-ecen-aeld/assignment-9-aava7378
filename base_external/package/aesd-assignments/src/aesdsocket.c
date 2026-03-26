#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#include "aesd_ioctl.h"

#define PORT 9000
#define BACKLOG 10
#define DEVICE_FILE "/dev/aesdchar"

static volatile sig_atomic_t g_exit_requested = 0;
static int g_listen_fd = -1;

static void dbg(const char *msg)
{
    (void)write(STDERR_FILENO, msg, strlen(msg));
}


static void handle_signal(int signo)
{
    (void)signo;
    g_exit_requested = 1;
}

static int setup_listen_socket(void)
{
    int fd;
    int optval;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        syslog(LOG_ERR, "socket failed: %s", strerror(errno));
        return -1;
    }

    optval = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) != 0)
    {
        syslog(LOG_ERR, "setsockopt failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        syslog(LOG_ERR, "bind failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, BACKLOG) != 0)
    {
        syslog(LOG_ERR, "listen failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static int daemonize_after_bind(void)
{
    pid_t pid;
    int devnull;

    pid = fork();
    if (pid < 0)
    {
        return -1;
    }
    if (pid > 0)
    {
        exit(EXIT_SUCCESS);
    }

    if (setsid() < 0)
    {
        return -1;
    }

    pid = fork();
    if (pid < 0)
    {
        return -1;
    }
    if (pid > 0)
    {
        exit(EXIT_SUCCESS);
    }

    if (chdir("/") != 0)
    {
        return -1;
    }

    devnull = open("/dev/null", O_RDWR);
    if (devnull < 0)
    {
        return -1;
    }

    if (dup2(devnull, STDIN_FILENO) < 0 ||
        dup2(devnull, STDOUT_FILENO) < 0 ||
        dup2(devnull, STDERR_FILENO) < 0)
    {
        close(devnull);
        return -1;
    }

    if (devnull > STDERR_FILENO)
    {
        close(devnull);
    }

    return 0;
}

static int send_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;

    while (off < len)
    {
        ssize_t s = send(fd, buf + off, len - off, 0);
        if (s < 0)
        {
            syslog(LOG_ERR, "send failed: %s", strerror(errno));
            return -1;
        }
        off += (size_t)s;
    }

    return 0;
}

static int handle_complete_packet(int client_fd, const char *buf, size_t len)
{
    int dev_fd;
    char *line;
    struct aesd_seekto seekto;
    unsigned int x;
    unsigned int y;

    line = malloc(len + 1);
    if (!line)
    {
        syslog(LOG_ERR, "malloc failed");
        return -1;
    }

    memcpy(line, buf, len);
    line[len] = '\0';
    dbg("NEWLINE\n");
    if ((strncmp(line, "AESDCHAR_IOCSEEKTO:", 19) == 0) &&
        (sscanf(line, "AESDCHAR_IOCSEEKTO:%u,%u", &x, &y) == 2))
    {
        dev_fd = open(DEVICE_FILE, O_RDWR);
        if (dev_fd < 0)
        {
            syslog(LOG_ERR, "open(%s) failed: %s", DEVICE_FILE, strerror(errno));
            free(line);
            return -1;
        }

        seekto.write_cmd = x;
        seekto.write_cmd_offset = y;

        if (ioctl(dev_fd, AESDCHAR_IOCSEEKTO, &seekto) != 0)
        {
            syslog(LOG_ERR, "ioctl failed: %s", strerror(errno));
            close(dev_fd);
            free(line);
            return -1;
        }
    }
    else
    {
        size_t off = 0;

        dev_fd = open(DEVICE_FILE, O_WRONLY);
        if (dev_fd < 0)
        {
            syslog(LOG_ERR, "open(%s) for write failed: %s", DEVICE_FILE, strerror(errno));
            free(line);
            return -1;
        }

	dbg("WRITE_PATH\n");
        while (off < len)
        {
            ssize_t w = write(dev_fd, line + off, len - off);
            if (w < 0)
            {
                syslog(LOG_ERR, "write failed: %s", strerror(errno));
                close(dev_fd);
                free(line);
                return -1;
            }
            off += (size_t)w;
        }

        close(dev_fd);

        dev_fd = open(DEVICE_FILE, O_RDONLY);
        if (dev_fd < 0)
        {
            syslog(LOG_ERR, "open(%s) for read failed: %s", DEVICE_FILE, strerror(errno));
            free(line);
            return -1;
        }
    }

    while (1)
    {
        char readbuf[4096];
        ssize_t r = read(dev_fd, readbuf, sizeof(readbuf));
        if (r < 0)
        {
            syslog(LOG_ERR, "read failed: %s", strerror(errno));
            close(dev_fd);
            free(line);
            return -1;
        }
        if (r == 0)
        {
            break;
        }

        if (send_all(client_fd, readbuf, (size_t)r) != 0)
        {
            close(dev_fd);
            free(line);
            return -1;
        }
    }

    close(dev_fd);
    free(line);
    return 0;
}

int main(int argc, char *argv[])
{
    bool daemon_mode;
    struct sigaction sa;

    daemon_mode = false;
    if (argc == 2 && strcmp(argv[1], "-d") == 0)
    {
        daemon_mode = true;
    }
    else if (argc != 1)
    {
        fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
        return -1;
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0)
    {
        syslog(LOG_ERR, "sigaction failed: %s", strerror(errno));
        closelog();
        return -1;
    }

    g_listen_fd = setup_listen_socket();
    if (g_listen_fd < 0)
    {
        closelog();
        return -1;
    }

    if (daemon_mode)
    {
        if (daemonize_after_bind() < 0)
        {
            close(g_listen_fd);
            closelog();
            return -1;
        }
    }

    while (!g_exit_requested)
    {
        struct sockaddr_in client_addr;
        socklen_t client_len;
        int cfd;
        char ipstr[INET_ADDRSTRLEN];
        char *packet;
        size_t packet_sz;

        client_len = sizeof(client_addr);
        cfd = accept(g_listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (cfd < 0)
        {
            if (errno == EINTR && g_exit_requested)
            {
                break;
            }
            syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            continue;
        }

        dbg("ACCEPTED\n");

        memset(ipstr, 0, sizeof(ipstr));
        inet_ntop(AF_INET, &client_addr.sin_addr, ipstr, sizeof(ipstr));
        syslog(LOG_INFO, "Accepted connection from %s", ipstr);

        packet = NULL;
        packet_sz = 0;

        while (!g_exit_requested)
        {
            char ch;
            char *newbuf;
            ssize_t r;

            r = recv(cfd, &ch, 1, 0);
            dbg("RECV\n");

            if (r < 0)
            {
                if (errno == EINTR && g_exit_requested)
                {
                    break;
                }
                syslog(LOG_ERR, "recv failed: %s", strerror(errno));
                break;
            }

            if (r == 0)
            {
                break;
            }

            newbuf = realloc(packet, packet_sz + 1);
            if (!newbuf)
            {
                syslog(LOG_ERR, "realloc failed");
                free(packet);
                packet = NULL;
                packet_sz = 0;
                break;
            }

            packet = newbuf;
            packet[packet_sz] = ch;
            packet_sz++;

            if (ch == '\n')
            {
                dbg("NEWLINE\n");

                if (handle_complete_packet(cfd, packet, packet_sz) != 0)
                {
                    syslog(LOG_ERR, "handle_complete_packet failed");
                }

                free(packet);
                packet = NULL;
                packet_sz = 0;
            }
        }

        free(packet);
        shutdown(cfd, SHUT_RDWR);
        close(cfd);

        syslog(LOG_INFO, "Closed connection from %s", ipstr);
    }

    syslog(LOG_INFO, "Caught signal, exiting");
    if (g_listen_fd >= 0)
    {
        close(g_listen_fd);
        g_listen_fd = -1;
    }

    closelog();
    return 0;
}
