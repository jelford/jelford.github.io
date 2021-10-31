#include <sys/signalfd.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <poll.h>

void handle_signal(int);

int main()
{
    // setbuf(stdout, NULL);
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);

    sigprocmask(SIG_SETMASK, &mask, NULL);

    int signal_fd = signalfd(-1, &mask, SFD_NONBLOCK);

    struct pollfd pollfd = {
        .fd = signal_fd,
        .events = POLLIN,
    };

    while (poll(&pollfd, 1, 5000) > 0)
    {
        handle_signal(pollfd.fd);
    }
}


void handle_signal(int signal_fd)
{
    struct signalfd_siginfo siginfo;
    ssize_t s;
    s = read(signal_fd, &siginfo, sizeof(siginfo));
    if (s != sizeof(siginfo))
    {
        perror("read");
        exit(1);
    }

    uint32_t signo = siginfo.ssi_signo;
    char *signame = strsignal(signo);

    printf("Received signal %d (%s)\n", signo, signame);
}