#ifndef LOCAL_PROXY_H
#define LOCAL_PROXY_H

#include <signal.h>

int local_proxy_run(volatile sig_atomic_t *stop_requested);

#endif
