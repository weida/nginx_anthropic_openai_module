#ifndef NGX_HTTP_AO_COMPAT_H
#define NGX_HTTP_AO_COMPAT_H

/*
 * Minimal compatibility shim so the translator files (req.c, etc.) can
 * emit a few informational log lines without pulling in the full nginx
 * core headers. Two build modes:
 *
 *   - NGX build (default): include real <ngx_core.h>; ngx_log_t,
 *     ngx_log_error, NGX_LOG_WARN all come from nginx.
 *
 *   - Standalone build (NGX_HTTP_AO_NO_NGX defined, e.g. conv-test):
 *     provide inline stubs so the translator compiles with only libc +
 *     cJSON on the include path.
 */

#ifdef NGX_HTTP_AO_NO_NGX

#include <stdarg.h>
#include <stdio.h>

struct ngx_log_s { int placeholder; };
typedef struct ngx_log_s ngx_log_t;

static void
ngx_http_ao_log_stub(int level, ngx_log_t *log, int err,
                     const char *fmt, ...)
{
    (void) log;
    (void) err;
    if (level > 4) {
        return;
    }
    {
        va_list  ap;
        va_start(ap, fmt);
        fprintf(stderr, "[ao:%d] ", level);
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
        va_end(ap);
    }
}

#define ngx_log_error  ngx_http_ao_log_stub

#define NGX_LOG_EMERG    0
#define NGX_LOG_ALERT    1
#define NGX_LOG_CRIT     2
#define NGX_LOG_ERR      3
#define NGX_LOG_WARN     4
#define NGX_LOG_NOTICE   5
#define NGX_LOG_INFO     6
#define NGX_LOG_DEBUG    7

#else

#include <ngx_core.h>

#endif

#endif /* NGX_HTTP_AO_COMPAT_H */
