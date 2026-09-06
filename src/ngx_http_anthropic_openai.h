#ifndef NGX_HTTP_ANTHROPIC_OPENAI_H
#define NGX_HTTP_ANTHROPIC_OPENAI_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#define NGX_HTTP_AO_COUNT_TOKENS_HEURISTIC  0
#define NGX_HTTP_AO_COUNT_TOKENS_OFF        1
#define NGX_HTTP_AO_RESP_BUFFER_MAX     (16 * 1024 * 1024)
#define NGX_HTTP_AO_BUFFERED                0x08

#define NGX_HTTP_AO_NSLOTS  2
#define NGX_HTTP_AO_SLOT_SIZE  (256 * 1024)
#define NGX_HTTP_AO_IN_Q_MAX  32

typedef struct {
    u_char                  *data;
    ngx_buf_t                buf;
    ngx_chain_t              chain;
    unsigned                 held:1;
} ngx_http_ao_out_slot_t;

typedef struct ngx_http_ao_in_node_s  ngx_http_ao_in_node_t;
struct ngx_http_ao_in_node_s {
    ngx_buf_t               *buf;
    u_char                  *cur;
    u_char                  *end;
    ngx_http_ao_in_node_t   *next;
    unsigned                 last:1;
};

typedef struct {
    ngx_http_ao_out_slot_t   slots[NGX_HTTP_AO_NSLOTS];
    ngx_http_ao_in_node_t    nodes[NGX_HTTP_AO_IN_Q_MAX];
    ngx_http_ao_in_node_t   *head, *tail, *free;
    unsigned                 input_last:1;
    unsigned                 input_error:1;
    unsigned                 next_again:1;
} ngx_http_ao_stream_t;

typedef struct {
    ngx_flag_t               enable;
    ngx_str_t                model;
    ngx_str_t                api_key;
    ngx_uint_t               count_tokens;
    ngx_flag_t               stream_usage;
} ngx_http_anthropic_openai_loc_conf_t;

typedef struct {
    unsigned                 enabled:1;
    unsigned                 skip_filters:1;
    unsigned                 count_tokens:1;
    unsigned                 client_stream:1;
    unsigned                 headers_held:1;
    unsigned                 header_sent:1;
    unsigned                 response_done:1;

    ngx_str_t                orig_model;
    u_char                  *req_buf;
    size_t                   req_len;
    size_t                   req_cap;

    u_char                  *resp_buf;
    size_t                   resp_len;
    size_t                   resp_cap;

    ngx_http_ao_stream_t    *stream;
    void                    *sse;          /* ngx_http_ao_sse_t */
} ngx_http_anthropic_openai_ctx_t;

extern ngx_module_t  ngx_http_anthropic_openai_module;

#endif
