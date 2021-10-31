// compiles with: gcc  -Wall -Werror -pedantic ./selfpipetrick.c -o selfpipetrick
// You can quit by suspending with Ctrl-Z and then sending a `kill -9`

#include <sys/signalfd.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>

void handle_err(char *desc)
{
    perror(desc);
    exit(EXIT_FAILURE);
}

static int pipefds[2] = {0};

void signal_action(int signum)
{
    uint8_t empty[1] = {0};
    int write_fd = pipefds[1];
    
    write(write_fd, empty, 1); // There's not much we can do with an error here - maybe call _exit?
}

void handle_signal(int signal_fd)
{
    uint8_t buff[1] = {0};
    if (read(signal_fd, buff, 1) != 1)
    {
        handle_err("read");
    }
    printf("Received signal\n");
}

int main()
{
    if (pipe(pipefds) == -1)
    {
        handle_err("pipe");
    }
    if (fcntl(pipefds[0], F_SETFD, O_NONBLOCK) == -1)
    {
        handle_err("fcntl");
    }
    if (fcntl(pipefds[1], F_SETFD, O_NONBLOCK) == -1)
    {
        handle_err("fcntl");
    }

    int read_fd = pipefds[0];

    signal(SIGINT, signal_action);

    struct pollfd pollfd = {
        .fd = read_fd,
        .events = POLLIN,
    };

    for (;;)
    {
        int poll_result = poll(&pollfd, 1, 5000);
        switch (poll_result)
        {
        case 0:
            printf("Poll timed out without any signals\n");
            exit(EXIT_SUCCESS);
        case -1:
            if (errno == EINTR)
            {
                // System call interrupted, but could have been from another signal; poll our fd again.
                continue; 
            }
            else
            {
                handle_err("poll");
            }
        default:
            handle_signal(read_fd);
            break;
        }
    }
}
