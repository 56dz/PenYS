#ifndef YS_LOG_H
#define YS_LOG_H
/*
 * Minimal logging for an embedded quick-JS runtime.
 *
 * Design notes that are not obvious:
 *  - Logging is OFF unless /tmp/penys_log exists. A cloud-gaming session writes
 *    a line per state transition at most; turning it on permanently would add
 *    measurable jitter to a 60fps pipeline on a 4x A35.
 *  - Every line is prefixed with the monotonic millisecond clock, because when
 *    we debug A/V sync later, wall-clock timestamps are useless.
 *  - Writes go through write(2) on a fd opened once, never through stdio: the
 *    Falcon runtime also uses stdio and interleaving corrupts its output.
 */
#include <stddef.h>

void ys_log_init(void);
int ys_log_on(void);
void ys_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Unconditional: for the handful of lines that must always appear (startup banner). */
void ys_log_always(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *ys_log_path(void);

#endif
