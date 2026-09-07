#ifndef NGX_HTTP_ANTHROPIC_OPENAI_JSON_H
#define NGX_HTTP_ANTHROPIC_OPENAI_JSON_H

#include <stddef.h>
#include "cJSON.h"

cJSON *ngx_http_ao_json_parse(unsigned char *p, size_t n);

typedef struct {
    const char  *model_override;
    int          stream_usage;
    int          client_stream;
    char         orig_model[256];
    const char  *err;
    int          err_status;
    int          max_tools;     /* 0 means use built-in default (256) */
} ngx_http_ao_req_opt_t;

char *ngx_http_ao_convert_request(unsigned char *p, size_t n,
    ngx_http_ao_req_opt_t *opt, size_t *out_len);

typedef struct {
    const char  *echo_model;
    const char  *err;
    int          err_status;
} ngx_http_ao_resp_opt_t;

char *ngx_http_ao_convert_response(unsigned char *p, size_t n,
    ngx_http_ao_resp_opt_t *opt, size_t *out_len);

int ngx_http_ao_count_tokens_json(unsigned char *p, size_t n,
    unsigned int *tokens);

unsigned int ngx_http_ao_count_tokens_heuristic(unsigned char *p, size_t n);

typedef struct ngx_http_ao_sse_s  ngx_http_ao_sse_t;

#define NGX_HTTP_AO_EVENT_OUT_MAX  (256 * 1024)
typedef void *(*ngx_http_ao_alloc_pt)(void *ctx, size_t n);
ngx_http_ao_sse_t *ngx_http_ao_sse_create_alloc(const char *model,
    ngx_http_ao_alloc_pt alloc, void *ctx);
int ngx_http_ao_sse_consume(ngx_http_ao_sse_t *s, const unsigned char *p,
    size_t n, size_t *used);
int ngx_http_ao_sse_pull(ngx_http_ao_sse_t *s, unsigned char *out, size_t cap,
    size_t *len, int *last);
int ngx_http_ao_sse_pending(ngx_http_ao_sse_t *s);
int ngx_http_ao_sse_done(ngx_http_ao_sse_t *s);
int ngx_http_ao_sse_finish_input(ngx_http_ao_sse_t *s, int transport_error);
int ngx_http_ao_sse_json(ngx_http_ao_sse_t *s, unsigned char *p, size_t n);

/* Standalone/offline collection helpers; not a production streaming API. */
ngx_http_ao_sse_t *ngx_http_ao_sse_create(const char *echo_model);
int ngx_http_ao_sse_feed(ngx_http_ao_sse_t *s, unsigned char *p, size_t n);
int ngx_http_ao_sse_end(ngx_http_ao_sse_t *s, int transport_error);
char *ngx_http_ao_sse_copy_out(ngx_http_ao_sse_t *s, size_t *n);
void ngx_http_ao_sse_clear_out(ngx_http_ao_sse_t *s);
void ngx_http_ao_sse_destroy(ngx_http_ao_sse_t *s);

char *ngx_http_ao_convert_sse(unsigned char *p, size_t n,
    const char *echo_model, size_t *out_len);

#endif
