#include "log.h"

#if PSPCAP_LOG

#include <pspiofilemgr.h>
#include <pspsysclib.h>
#include <stdarg.h>

#define LOG_PATH "ms0:/seplugins/pspcap.log"

typedef struct {
    char *buf;
    int len;
    int cap;
} line_buf;

// prnt() feeds characters one by one; it also sends 0x200 at the start
// and 0x201 at the end of formatting, which are not characters.
static void line_putc(void *ctx, int ch)
{
    line_buf *lb = ctx;
    if (ch >= 0x100)
        return;
    if (lb->len < lb->cap)
        lb->buf[lb->len++] = (char)ch;
}

void log_reset(void)
{
    SceUID fd = sceIoOpen(LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0)
        sceIoClose(fd);
}

void log_printf(const char *fmt, ...)
{
    static char line[256];
    line_buf lb = { line, 0, sizeof(line) - 1 };
    va_list args;

    va_start(args, fmt);
    prnt(line_putc, &lb, fmt, args);
    va_end(args);

    line[lb.len++] = '\n';

    SceUID fd = sceIoOpen(LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd < 0)
        return;
    sceIoWrite(fd, line, lb.len);
    sceIoClose(fd);
}

#endif
