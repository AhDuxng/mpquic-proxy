#include <signal.h>
#include <stdio.h>

#include "local_proxy.h"
#include "mpquic_client.h"

static volatile sig_atomic_t stop_requested = 0;

static void on_signal(int signo)
{
    (void)signo;
    stop_requested = 1;
}

int main(void)
{
    int result;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (mpquic_client_init() != 0) {
        fprintf(stderr, "Cannot initialize MPQUIC client\n");
        return 1;
    }

    result = local_proxy_run(&stop_requested);
    mpquic_client_shutdown();

    return result == 0 ? 0 : 1;
}
