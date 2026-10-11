// Plain-text log at ms0:/seplugins/pspcap.log (compiled out with LOG=0).
#ifndef PSPCAP_LOG_H
#define PSPCAP_LOG_H

#if PSPCAP_LOG

// Truncates the log file. Call once at startup.
void log_reset(void);

// printf-style; a newline is appended. Not for hot paths: every call
// opens, appends and closes the file so nothing is lost on a crash.
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#else

static inline void log_reset(void) {}
static inline void log_printf(const char *fmt, ...) { (void)fmt; }

#endif

#endif
