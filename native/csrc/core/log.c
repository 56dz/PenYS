#include "core/log.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define YS_LOG_PATH "/tmp/penys.log"
#define YS_LOG_SWITCH "/tmp/penys_log"
#define YS_LINE_CAP 1024

static int g_fd = -1;
static int g_on = 0;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

static long long mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void ys_log_init(void) {
    if (access(YS_LOG_SWITCH, F_OK) == 0) {
        g_fd = open(YS_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (g_fd >= 0) g_on = 1;
    }
}

int ys_log_on(void) { return g_on; }
const char *ys_log_path(void) { return YS_LOG_PATH; }

static void log_line(const char *fmt, va_list ap, int force) {
    char buf[YS_LINE_CAP];
    int n;
    if (!force && !g_on) return;
    if (g_fd < 0) return;

    /* "[123456 ms] " prefix: 8-12 chars, cheap and sortable. */
    n = snprintf(buf, sizeof(buf), "[%lld ms] ", mono_ms());
    if (n < 0 || n >= (int)sizeof(buf)) return;
    {
        int m = vsnprintf(buf + n, sizeof(buf) - (size_t)n - 2, fmt, ap);
        if (m < 0) return;
        n += m;
        if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    }
    buf[n++] = '\n';

    pthread_mutex_lock(&g_mtx);
    /* One write per line: keeps lines intact when two threads log concurrently. */
    (void)!write(g_fd, buf, (size_t)n);
    pthread_mutex_unlock(&g_mtx);
}

void ys_logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_line(fmt, ap, 0);
    va_end(ap);
}

void ys_log_always(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_line(fmt, ap, 1);
    va_end(ap);
}
