#include "ngx_http_anthropic_openai.h"
#include "ngx_http_anthropic_openai_json.h"

#include <stdlib.h>

static ngx_http_output_header_filter_pt  ngx_http_ao_next_header_filter;
static ngx_http_output_body_filter_pt    ngx_http_ao_next_body_filter;
static ngx_http_request_body_filter_pt   ngx_http_ao_next_request_body_filter;

static ngx_int_t ngx_http_anthropic_openai_rewrite_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_anthropic_openai_request_body_filter(
    ngx_http_request_t *r, ngx_chain_t *in);
static ngx_int_t ngx_http_anthropic_openai_header_filter(ngx_http_request_t *r);
static ngx_int_t ngx_http_anthropic_openai_body_filter(ngx_http_request_t *r,
    ngx_chain_t *in);
static ngx_int_t ngx_http_ao_stream_body(ngx_http_request_t *r,
    ngx_http_anthropic_openai_ctx_t *ctx, ngx_chain_t *in);
static ngx_int_t ngx_http_ao_held_stream_finish(ngx_http_request_t *r,
    ngx_http_anthropic_openai_ctx_t *ctx);
static void ngx_http_anthropic_openai_count_tokens_post(ngx_http_request_t *r);
static ngx_int_t ngx_http_anthropic_openai_count_tokens_handler(
    ngx_http_request_t *r);
static ngx_int_t ngx_http_anthropic_openai_postconfiguration(ngx_conf_t *cf);
static void *ngx_http_anthropic_openai_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_anthropic_openai_merge_loc_conf(ngx_conf_t *cf,
    void *parent, void *child);

static ngx_conf_enum_t  ngx_http_ao_count_tokens_enum[] = {
    { ngx_string("heuristic"), NGX_HTTP_AO_COUNT_TOKENS_HEURISTIC },
    { ngx_string("off"),       NGX_HTTP_AO_COUNT_TOKENS_OFF },
    { ngx_null_string, 0 }
};

static ngx_command_t  ngx_http_anthropic_openai_commands[] = {

    { ngx_string("anthropic_openai"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_anthropic_openai_loc_conf_t, enable),
      NULL },

    { ngx_string("anthropic_openai_model"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_anthropic_openai_loc_conf_t, model),
      NULL },

    { ngx_string("anthropic_openai_api_key"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_anthropic_openai_loc_conf_t, api_key),
      NULL },

    { ngx_string("anthropic_openai_count_tokens"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_enum_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_anthropic_openai_loc_conf_t, count_tokens),
      &ngx_http_ao_count_tokens_enum },

    { ngx_string("anthropic_openai_stream_usage"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_anthropic_openai_loc_conf_t, stream_usage),
      NULL },

      ngx_null_command
};

static ngx_http_module_t  ngx_http_anthropic_openai_module_ctx = {
    NULL,
    ngx_http_anthropic_openai_postconfiguration,
    NULL,
    NULL,
    NULL,
    NULL,
    ngx_http_anthropic_openai_create_loc_conf,
    ngx_http_anthropic_openai_merge_loc_conf
};

ngx_module_t  ngx_http_anthropic_openai_module = {
    NGX_MODULE_V1,
    &ngx_http_anthropic_openai_module_ctx,
    ngx_http_anthropic_openai_commands,
    NGX_HTTP_MODULE,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NGX_MODULE_V1_PADDING
};


static void
ngx_http_ao_hide_input_header(ngx_table_elt_t *h)
{
    h->hash = 0;
    h->key.len = sizeof("Connection") - 1;
    h->key.data = (u_char *) "Connection";
    h->lowcase_key = (u_char *) "connection";
}


static ngx_int_t
ngx_http_ao_header_match_prefix(ngx_str_t *key, const char *p, size_t plen)
{
    if (key->len < plen) {
        return 0;
    }
    return ngx_strncasecmp(key->data, (u_char *) p, plen) == 0;
}


static ngx_int_t
ngx_http_ao_set_authorization(ngx_http_request_t *r, ngx_str_t *bearer)
{
    ngx_table_elt_t  *h;

    h = r->headers_in.authorization;
    if (h == NULL) {
        h = ngx_list_push(&r->headers_in.headers);
        if (h == NULL) {
            return NGX_ERROR;
        }
        ngx_memzero(h, sizeof(ngx_table_elt_t));
        r->headers_in.authorization = h;
    }

    h->key.len = sizeof("Authorization") - 1;
    h->key.data = (u_char *) "Authorization";
    h->lowcase_key = (u_char *) "authorization";
    h->hash = ngx_hash_key_lc(h->lowcase_key, h->key.len);
    h->value = *bearer;
    return NGX_OK;
}


static ngx_int_t
ngx_http_ao_rewrite_auth_and_hide_headers(ngx_http_request_t *r,
    ngx_http_anthropic_openai_loc_conf_t *alcf)
{
    ngx_list_part_t  *part;
    ngx_table_elt_t  *h;
    ngx_uint_t        i;
    ngx_str_t         bearer, key;
    u_char           *p;

    key.len = 0;
    key.data = NULL;

    part = &r->headers_in.headers.part;
    h = part->elts;
    for (i = 0; /* void */ ; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            part = part->next;
            h = part->elts;
            i = 0;
        }

        if (h[i].key.len == sizeof("x-api-key") - 1
            && ngx_strncasecmp(h[i].key.data, (u_char *) "x-api-key",
                               sizeof("x-api-key") - 1) == 0)
        {
            if (key.data == NULL) {
                key = h[i].value;
            }
            ngx_http_ao_hide_input_header(&h[i]);
            continue;
        }

        if (ngx_http_ao_header_match_prefix(&h[i].key, "anthropic-", 10)
            || ngx_http_ao_header_match_prefix(&h[i].key, "x-stainless-", 12)
            || (h[i].key.len == sizeof("Accept-Encoding") - 1
                && ngx_strncasecmp(h[i].key.data, (u_char *) "Accept-Encoding",
                                   sizeof("Accept-Encoding") - 1) == 0))
        {
            ngx_http_ao_hide_input_header(&h[i]);
        }
    }

    if (alcf->api_key.len) {
        key = alcf->api_key;
    }

    if (key.len == 0) {
        return NGX_OK;
    }

    bearer.len = sizeof("Bearer ") - 1 + key.len;
    bearer.data = ngx_pnalloc(r->pool, bearer.len);
    if (bearer.data == NULL) {
        return NGX_ERROR;
    }
    p = ngx_copy(bearer.data, "Bearer ", sizeof("Bearer ") - 1);
    ngx_memcpy(p, key.data, key.len);

    return ngx_http_ao_set_authorization(r, &bearer);
}


static ngx_int_t
ngx_http_ao_uri_is_count_tokens(ngx_str_t *uri)
{
    static u_char  suffix[] = "/count_tokens";
    size_t         slen = sizeof(suffix) - 1;

    if (uri->len < slen) {
        return 0;
    }
    return ngx_strncmp(uri->data + uri->len - slen, suffix, slen) == 0;
}


static ngx_int_t
ngx_http_ao_buf_append(ngx_http_request_t *r, u_char **dst, size_t *len,
    size_t *cap, u_char *p, size_t n)
{
    u_char  *nb;
    size_t   ncap;

    if (n == 0) {
        return NGX_OK;
    }
    if (*len + n > *cap) {
        ncap = (*cap != 0) ? *cap : 4096;
        while (ncap < *len + n) {
            ncap *= 2;
        }
        nb = ngx_pnalloc(r->pool, ncap);
        if (nb == NULL) {
            return NGX_ERROR;
        }
        if (*len && *dst) {
            ngx_memcpy(nb, *dst, *len);
        }
        *dst = nb;
        *cap = ncap;
    }
    ngx_memcpy(*dst + *len, p, n);
    *len += n;
    return NGX_OK;
}


static ngx_int_t
ngx_http_anthropic_openai_rewrite_handler(ngx_http_request_t *r)
{
    ngx_http_anthropic_openai_loc_conf_t  *alcf;
    ngx_http_anthropic_openai_ctx_t       *ctx;

    if (r != r->main) {
        return NGX_DECLINED;
    }

    alcf = ngx_http_get_module_loc_conf(r, ngx_http_anthropic_openai_module);
    if (alcf == NULL || alcf->enable != 1) {
        return NGX_DECLINED;
    }

    if (r->method != NGX_HTTP_POST) {
        return NGX_DECLINED;
    }

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_anthropic_openai_ctx_t));
    if (ctx == NULL) {
        return NGX_ERROR;
    }
    ctx->enabled = 1;
    ngx_http_set_ctx(r, ctx, ngx_http_anthropic_openai_module);

    if (ngx_http_ao_rewrite_auth_and_hide_headers(r, alcf) != NGX_OK) {
        return NGX_ERROR;
    }

    if (ngx_http_ao_uri_is_count_tokens(&r->uri)) {
        ctx->count_tokens = 1;
        ctx->skip_filters = 1;
        r->content_handler = ngx_http_anthropic_openai_count_tokens_handler;
    }

    return NGX_DECLINED;
}


static ngx_int_t
ngx_http_ao_send_memory(ngx_http_request_t *r, u_char *p, size_t n,
    ngx_uint_t status, ngx_str_t *content_type)
{
    ngx_buf_t    *b;
    ngx_chain_t   out;

    r->headers_out.status = status;
    r->headers_out.content_length_n = (off_t) n;
    r->headers_out.content_type = *content_type;
    r->headers_out.content_type_len = content_type->len;
    r->headers_out.content_type_lowcase = NULL;

    if (ngx_http_send_header(r) == NGX_ERROR) {
        return NGX_ERROR;
    }

    b = ngx_calloc_buf(r->pool);
    if (b == NULL) {
        return NGX_ERROR;
    }
    b->pos = p;
    b->last = p + n;
    b->memory = 1;
    b->last_buf = 1;
    b->last_in_chain = 1;
    out.buf = b;
    out.next = NULL;
    return ngx_http_output_filter(r, &out);
}


static void
ngx_http_anthropic_openai_count_tokens_post(ngx_http_request_t *r)
{
    ngx_http_anthropic_openai_loc_conf_t  *alcf;
    ngx_http_anthropic_openai_ctx_t       *ctx;
    ngx_str_t                              type;
    u_char                                *body, *json;
    size_t                                 n;
    unsigned int                           tok;
    ssize_t                                nr;
    off_t                                  size;
    ngx_chain_t                           *cl;

    ctx = ngx_http_get_module_ctx(r, ngx_http_anthropic_openai_module);
    alcf = ngx_http_get_module_loc_conf(r, ngx_http_anthropic_openai_module);

    ngx_str_set(&type, "application/json");

    if (alcf->count_tokens == NGX_HTTP_AO_COUNT_TOKENS_OFF) {
        json = (u_char *) "{\"type\":\"error\",\"error\":"
               "{\"type\":\"not_found_error\",\"message\":\"count_tokens off\"}}";
        ngx_http_finalize_request(r,
            ngx_http_ao_send_memory(r, json, ngx_strlen(json), 501, &type));
        return;
    }

    n = 0;
    if (r->request_body && r->request_body->bufs) {
        for (cl = r->request_body->bufs; cl; cl = cl->next) {
            if (ngx_buf_size(cl->buf) > 0) {
                n += ngx_buf_size(cl->buf);
            }
        }
        body = ngx_pnalloc(r->pool, n + 1);
        if (body == NULL) {
            ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
            return;
        }
        n = 0;
        for (cl = r->request_body->bufs; cl; cl = cl->next) {
            size = ngx_buf_size(cl->buf);
            if (size <= 0) {
                continue;
            }
            if (cl->buf->in_file) {
                nr = ngx_read_file(cl->buf->file, body + n, (size_t) size,
                                   cl->buf->file_pos);
                if (nr == NGX_ERROR || nr != size) {
                    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                                  "anthropic_openai: request body file read failed");
                    ngx_http_finalize_request(r,
                                              NGX_HTTP_INTERNAL_SERVER_ERROR);
                    return;
                }
            } else {
                ngx_memcpy(body + n, cl->buf->pos, (size_t) size);
            }
            n += (size_t) size;
        }
        if (ngx_http_ao_count_tokens_json(body, n, &tok) != 0) {
            json = (u_char *) "{\"type\":\"error\",\"error\":"
                "{\"type\":\"invalid_request_error\","
                "\"message\":\"invalid count_tokens request\"}}";
            ngx_http_finalize_request(r,
                ngx_http_ao_send_memory(r, json, ngx_strlen(json), 400, &type));
            return;
        }
    } else {
        tok = 0;
    }

    json = ngx_pnalloc(r->pool, 64);
    if (json == NULL) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }
    n = ngx_sprintf(json, "{\"input_tokens\":%ui}", (ngx_uint_t) tok) - json;
    (void) ctx;
    ngx_http_finalize_request(r,
        ngx_http_ao_send_memory(r, json, n, 200, &type));
}


static ngx_int_t
ngx_http_anthropic_openai_count_tokens_handler(ngx_http_request_t *r)
{
    ngx_int_t  rc;

    rc = ngx_http_read_client_request_body(r,
             ngx_http_anthropic_openai_count_tokens_post);
    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        return rc;
    }
    return NGX_DONE;
}


static ngx_int_t
ngx_http_anthropic_openai_request_body_filter(ngx_http_request_t *r,
    ngx_chain_t *in)
{
    ngx_http_anthropic_openai_ctx_t       *ctx;
    ngx_http_anthropic_openai_loc_conf_t  *alcf;
    ngx_chain_t                           *cl, *out;
    ngx_buf_t                             *b;
    ngx_http_ao_req_opt_t                  opt;
    char                                  *converted;
    cJSON                                 *root, *model;
    size_t                                 out_n;
    ngx_int_t                              last;
    ngx_table_elt_t                       *length;
    u_char                                *value;

    ctx = ngx_http_get_module_ctx(r, ngx_http_anthropic_openai_module);
    if (ctx == NULL || !ctx->enabled || ctx->skip_filters) {
        return ngx_http_ao_next_request_body_filter(r, in);
    }

    if (r->request_body_no_buffering) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "anthropic_openai requires proxy_request_buffering on");
        return NGX_HTTP_BAD_REQUEST;
    }

    last = 0;
    for (cl = in; cl; cl = cl->next) {
        if (cl->buf->last > cl->buf->pos) {
            if (ngx_http_ao_buf_append(r, &ctx->req_buf, &ctx->req_len,
                                       &ctx->req_cap, cl->buf->pos,
                                       (size_t) (cl->buf->last - cl->buf->pos))
                != NGX_OK)
            {
                return NGX_ERROR;
            }
        }
        if (cl->buf->last_buf) {
            last = 1;
        }
        cl->buf->pos = cl->buf->last;
    }

    if (!last) {
        return NGX_OK;
    }

    alcf = ngx_http_get_module_loc_conf(r, ngx_http_anthropic_openai_module);
    ngx_memzero(&opt, sizeof(opt));
    opt.stream_usage = (alcf->stream_usage != 0);
    if (alcf->model.len) {
        u_char  *m;
        m = ngx_pnalloc(r->pool, alcf->model.len + 1);
        if (m == NULL) {
            return NGX_ERROR;
        }
        ngx_memcpy(m, alcf->model.data, alcf->model.len);
        m[alcf->model.len] = 0;
        opt.model_override = (char *) m;
    }

    converted = ngx_http_ao_convert_request(ctx->req_buf, ctx->req_len,
                                            &opt, &out_n);
    if (converted == NULL) {
        return NGX_HTTP_BAD_REQUEST;
    }

    ctx->client_stream = opt.client_stream ? 1 : 0;
    root = ngx_http_ao_json_parse(ctx->req_buf, ctx->req_len);
    if (root == NULL) {
        cJSON_free(converted);
        return NGX_ERROR;
    }
    model = cJSON_GetObjectItemCaseSensitive(root, "model");
    if (cJSON_IsString(model)) {
        ctx->orig_model.len = ngx_strlen(model->valuestring);
        ctx->orig_model.data = ngx_pnalloc(r->pool, ctx->orig_model.len + 1);
        if (ctx->orig_model.data == NULL) {
            cJSON_Delete(root);
            cJSON_free(converted);
            return NGX_ERROR;
        }
        ngx_memcpy(ctx->orig_model.data, model->valuestring,
                   ctx->orig_model.len + 1);
    }
    cJSON_Delete(root);

    b = ngx_create_temp_buf(r->pool, out_n);
    if (b == NULL) {
        cJSON_free(converted);
        return NGX_ERROR;
    }
    ngx_memcpy(b->pos, converted, out_n);
    b->last = b->pos + out_n;
    b->last_buf = 1;
    cJSON_free(converted);

    out = ngx_alloc_chain_link(r->pool);
    if (out == NULL) {
        return NGX_ERROR;
    }
    out->buf = b;
    out->next = NULL;

    value = ngx_pnalloc(r->pool, NGX_OFF_T_LEN);
    if (value == NULL) {
        return NGX_ERROR;
    }
    length = r->headers_in.content_length;
    if (length == NULL) {
        length = ngx_list_push(&r->headers_in.headers);
        if (length == NULL) {
            return NGX_ERROR;
        }
        ngx_memzero(length, sizeof(ngx_table_elt_t));
        r->headers_in.content_length = length;
    }
    ngx_str_set(&length->key, "Content-Length");
    length->lowcase_key = (u_char *) "content-length";
    length->hash = ngx_hash_key_lc(length->lowcase_key, length->key.len);
    length->value.data = value;
    length->value.len = ngx_sprintf(value, "%uz", out_n) - value;
    r->headers_in.content_length_n = (off_t) out_n;
    r->headers_in.chunked = 0;
    if (r->upstream) {
        r->upstream->buffering = ctx->client_stream ? 0 : 1;
    }

    return ngx_http_ao_next_request_body_filter(r, out);
}


static ngx_int_t
ngx_http_anthropic_openai_header_filter(ngx_http_request_t *r)
{
    ngx_http_anthropic_openai_ctx_t  *ctx;
    ngx_list_part_t                  *part;
    ngx_table_elt_t                  *h;
    ngx_uint_t                        i;

    ctx = ngx_http_get_module_ctx(r, ngx_http_anthropic_openai_module);
    if (ctx == NULL || !ctx->enabled || ctx->skip_filters) {
        return ngx_http_ao_next_header_filter(r);
    }

    part = &r->headers_out.headers.part;
    h = part->elts;
    for (i = 0; /* void */; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            part = part->next;
            h = part->elts;
            i = 0;
        }
        if (ngx_http_ao_header_match_prefix(&h[i].key, "openai-", 7)
            || ngx_http_ao_header_match_prefix(&h[i].key, "x-ratelimit-", 12))
        {
            h[i].hash = 0;
        } else if (h[i].key.len == sizeof("x-request-id") - 1
            && ngx_http_ao_header_match_prefix(&h[i].key, "x-request-id", 12))
        {
            ngx_str_set(&h[i].key, "request-id");
            h[i].lowcase_key = (u_char *) "request-id";
        }
    }

    ngx_http_clear_content_length(r);
    r->main_filter_need_in_memory = 1;

    if (ctx->client_stream && r->upstream) {
        r->upstream->buffering = 0;
    }

    if (!ctx->client_stream || r->headers_out.status >= 400) {
        ctx->headers_held = 1;
        return NGX_OK;
    }

    /* stream + 200: if JSON we still hold; event-stream send now */
    if (r->headers_out.content_type.len >= sizeof("text/event-stream") - 1
        && ngx_strncasecmp(r->headers_out.content_type.data,
                           (u_char *) "text/event-stream",
                           sizeof("text/event-stream") - 1) == 0)
    {
        ngx_str_set(&r->headers_out.content_type, "text/event-stream");
        r->headers_out.content_type_len = sizeof("text/event-stream") - 1;
        ctx->header_sent = 1;
        return ngx_http_ao_next_header_filter(r);
    }

    ctx->headers_held = 1;
    return NGX_OK;
}


static const char *
ngx_http_ao_ctx_model(ngx_http_request_t *r,
    ngx_http_anthropic_openai_ctx_t *ctx)
{
    u_char  *copy;

    if (ctx->orig_model.len == 0 || ctx->orig_model.data == NULL) {
        return "";
    }
    copy = ngx_pnalloc(r->pool, ctx->orig_model.len + 1);
    if (copy == NULL) {
        return "";
    }
    ngx_memcpy(copy, ctx->orig_model.data, ctx->orig_model.len);
    copy[ctx->orig_model.len] = 0;
    return (char *) copy;
}


static void
ngx_http_ao_cleanup_sse(void *data)
{
    ngx_http_anthropic_openai_ctx_t  *ctx;

    ctx = data;
    if (ctx != NULL && ctx->sse != NULL) {
        ngx_http_ao_sse_destroy(ctx->sse);
        ctx->sse = NULL;
    }
}


static void *
ngx_http_ao_stream_alloc(void *pool, size_t n)
{
    return ngx_palloc(pool, n);
}

static ngx_int_t
ngx_http_ao_sse_ensure(ngx_http_request_t *r,
    ngx_http_anthropic_openai_ctx_t *ctx)
{
    ngx_http_cleanup_t    *cln;
    ngx_http_ao_stream_t  *st;
    ngx_uint_t             i;

    if (ctx->sse != NULL) { return NGX_OK; }
    st = ngx_pcalloc(r->pool, sizeof(*st));
    if (st == NULL) { return NGX_ERROR; }
    for (i = 0; i < NGX_HTTP_AO_NSLOTS; i++) {
        st->slots[i].data = ngx_palloc(r->pool, NGX_HTTP_AO_SLOT_SIZE);
        if (st->slots[i].data == NULL) { return NGX_ERROR; }
    }
    for (i = 0; i < NGX_HTTP_AO_IN_Q_MAX; i++) {
        st->nodes[i].next = st->free; st->free = &st->nodes[i];
    }
    ctx->sse = ngx_http_ao_sse_create_alloc(ngx_http_ao_ctx_model(r, ctx),
                                           ngx_http_ao_stream_alloc, r->pool);
    if (ctx->sse == NULL) { return NGX_ERROR; }
    cln = ngx_http_cleanup_add(r, 0);
    if (cln == NULL) {
        ngx_http_ao_sse_destroy(ctx->sse); ctx->sse = NULL;
        return NGX_ERROR;
    }
    ctx->stream = st;
    cln->handler = ngx_http_ao_cleanup_sse; cln->data = ctx;
    return NGX_OK;
}

static int
ngx_http_ao_looks_like_sse(u_char *p, size_t n)
{
    size_t  i;

    for (i = 0; i < n; i++) {
        if (p[i] == ' ' || p[i] == '\t' || p[i] == '\r' || p[i] == '\n') {
            continue;
        }
        if (p[i] == ':') {
            return 1;
        }
        if (i + 5 <= n && ngx_strncmp(p + i, (u_char *) "data:", 5) == 0) {
            return 1;
        }
        if (i + 6 <= n && ngx_strncmp(p + i, (u_char *) "event:", 6) == 0) {
            return 1;
        }
        return 0;
    }
    return 0;
}


/* Snapshot only this invocation's input chain. Upstream owns and mutates
 * cl->next after returning; our preallocated nodes never reference it. */
static ngx_int_t
ngx_http_ao_stream_enqueue(ngx_http_ao_stream_t *st, ngx_chain_t *in)
{
    ngx_chain_t            *cl;
    ngx_http_ao_in_node_t  *node;

    for (cl = in; cl; cl = cl->next) {
        for (node = st->head; node; node = node->next) {
            if (node->buf == cl->buf) { break; }
        }
        if (node != NULL) { continue; }
        if (cl->buf->pos == cl->buf->last && !cl->buf->last_buf) { continue; }
        node = st->free;
        if (node == NULL) { return NGX_ERROR; }
        st->free = node->next;
        node->buf = cl->buf; node->cur = cl->buf->pos; node->end = cl->buf->last;
        node->last = cl->buf->last_buf; node->next = NULL;
        if (st->tail) { st->tail->next = node; } else { st->head = node; }
        st->tail = node;
    }
    return NGX_OK;
}

static void
ngx_http_ao_stream_pop(ngx_http_ao_stream_t *st)
{
    ngx_http_ao_in_node_t  *node;
    node = st->head;
    if (node->last) { st->input_last = 1; }
    st->head = node->next;
    if (st->head == NULL) { st->tail = NULL; }
    node->buf = NULL; node->cur = NULL; node->end = NULL;
    node->next = st->free; st->free = node;
}

static ngx_uint_t
ngx_http_ao_stream_reap(ngx_http_ao_stream_t *st)
{
    ngx_uint_t  i, held;
    held = 0;
    for (i = 0; i < NGX_HTTP_AO_NSLOTS; i++) {
        /* Called only after next returned: no module out chain references
         * these slots. The held flag is our fixed-size busy reference set. */
        if (st->slots[i].held && st->slots[i].buf.pos == st->slots[i].buf.last) {
            st->slots[i].held = 0;
        }
        held += st->slots[i].held;
    }
    return held;
}

static ngx_int_t
ngx_http_ao_stream_body(ngx_http_request_t *r,
    ngx_http_anthropic_openai_ctx_t *ctx, ngx_chain_t *in)
{
    ngx_http_ao_stream_t    *st;
    ngx_http_ao_in_node_t   *node;
    ngx_http_ao_out_slot_t  *slot;
    ngx_chain_t             *cl;
    ngx_int_t                rc;
    ngx_uint_t               i, held;
    size_t                   n, used;
    int                      last, pulled;
    static char              tag;

    if (ngx_http_ao_sse_ensure(r, ctx) != NGX_OK) { return NGX_ERROR; }
    st = ctx->stream;
    r->buffered |= NGX_HTTP_AO_BUFFERED;
    if (r->upstream) {
        r->upstream->buffering = 0;
        if (r->upstream->error
            || (r->upstream->pipe && r->upstream->pipe->upstream_error)
            || (r->upstream->cleanup == NULL && in != NULL)
            || ((in == NULL
                 || (in->next == NULL && in->buf->flush
                     && in->buf->pos == in->buf->last))
                && r->upstream->peer.connection == NULL))
        { st->input_error = 1; }
    }
    if (ngx_http_ao_stream_enqueue(st, in) != NGX_OK) {
        st->input_error = 1;
        /* Drop borrowed references before advancing buffers that upstream
         * can recycle while the bounded error output is blocked. */
        while (st->head) {
            st->head->buf->pos = st->head->end;
            ngx_http_ao_stream_pop(st);
        }
        for (cl = in; cl; cl = cl->next) { cl->buf->pos = cl->buf->last; }
    }
    held = 0;
    for (i = 0; i < NGX_HTTP_AO_NSLOTS; i++) { held += st->slots[i].held; }
    if (held || st->next_again) {
        rc = ngx_http_ao_next_body_filter(r, NULL);
        held = ngx_http_ao_stream_reap(st);
        st->next_again = rc == NGX_AGAIN;
        if (rc == NGX_ERROR || rc == NGX_AGAIN) { return rc; }
    }
    for (;;) {
        if (st->input_error) {
            (void) ngx_http_ao_sse_finish_input(ctx->sse, 1);
        }
        if (ngx_http_ao_sse_done(ctx->sse)) {
            while (st->head) {
                st->head->buf->pos = st->head->end;
                ngx_http_ao_stream_pop(st);
            }
            if (held) { return NGX_AGAIN; }
            ctx->response_done = 1;
            r->buffered &= ~NGX_HTTP_AO_BUFFERED;
            return NGX_OK;
        }
        slot = NULL;
        for (i = 0; i < NGX_HTTP_AO_NSLOTS; i++) {
            if (!st->slots[i].held) { slot = &st->slots[i]; break; }
        }
        if (slot == NULL) { return NGX_AGAIN; }
        pulled = ngx_http_ao_sse_pull(ctx->sse, slot->data,
                                      NGX_HTTP_AO_SLOT_SIZE, &n, &last);
        if (pulled < 0) { return NGX_ERROR; }
        if (pulled > 0) {
            ngx_memzero(&slot->buf, sizeof(slot->buf));
            slot->buf.start = slot->data;
            slot->buf.end = slot->data + NGX_HTTP_AO_SLOT_SIZE;
            slot->buf.pos = slot->data; slot->buf.last = slot->data + n;
            slot->buf.temporary = 1; slot->buf.flush = 1;
            slot->buf.tag = &tag; slot->buf.last_buf = last;
            slot->buf.last_in_chain = last;
            slot->chain.buf = &slot->buf; slot->chain.next = NULL;
            slot->held = 1;
            rc = ngx_http_ao_next_body_filter(r, &slot->chain);
            held = ngx_http_ao_stream_reap(st);
            st->next_again = rc == NGX_AGAIN;
            if (rc == NGX_ERROR || rc == NGX_AGAIN) { return rc; }
            continue;
        }
        node = st->head;
        if (node != NULL) {
            n = node->cur == node->end ? 0 : (size_t) (node->end - node->cur);
            if (n) {
                (void) ngx_http_ao_sse_consume(ctx->sse, node->cur, n, &used);
                node->cur += used; node->buf->pos = node->cur;
            }
            if (node->cur == node->end) { ngx_http_ao_stream_pop(st); }
            continue;
        }
        if (st->input_last || st->input_error) {
            (void) ngx_http_ao_sse_finish_input(ctx->sse, st->input_error);
            continue;
        }
        /* NULL is a scheduling wakeup, never an upstream EOF. */
        return held ? NGX_AGAIN : NGX_OK;
    }
}

static ngx_int_t
ngx_http_ao_stream_headers(ngx_http_request_t *r,
    ngx_http_anthropic_openai_ctx_t *ctx)
{
    ngx_int_t  rc;
    r->headers_out.status = 200; r->headers_out.status_line.len = 0;
    ngx_str_set(&r->headers_out.content_type, "text/event-stream");
    r->headers_out.content_type_len = sizeof("text/event-stream") - 1;
    r->headers_out.content_type_lowcase = NULL;
    ngx_http_clear_content_length(r);
    rc = ngx_http_ao_next_header_filter(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) { return rc; }
    ctx->header_sent = 1; ctx->headers_held = 0;
    return NGX_OK;
}

static ngx_int_t
ngx_http_ao_held_stream_error(ngx_http_request_t *r,
    ngx_http_anthropic_openai_ctx_t *ctx)
{
    ngx_int_t  rc;
    if (ngx_http_ao_sse_ensure(r, ctx) != NGX_OK) { return NGX_ERROR; }
    (void) ngx_http_ao_sse_finish_input(ctx->sse, 1);
    rc = ngx_http_ao_stream_headers(r, ctx);
    if (rc != NGX_OK || r->header_only) { return rc; }
    return ngx_http_ao_stream_body(r, ctx, NULL);
}

static ngx_int_t
ngx_http_ao_held_stream_finish(ngx_http_request_t *r,
    ngx_http_anthropic_openai_ctx_t *ctx)
{
    ngx_int_t     rc;
    ngx_buf_t    *b;
    ngx_chain_t   in;
    int           is_sse;

    if (ngx_http_ao_sse_ensure(r, ctx) != NGX_OK) { return NGX_ERROR; }
    is_sse = ctx->resp_len > 0
             && ngx_http_ao_looks_like_sse(ctx->resp_buf, ctx->resp_len);
    if (!is_sse) {
        (void) ngx_http_ao_sse_json(ctx->sse, ctx->resp_buf, ctx->resp_len);
    }
    rc = ngx_http_ao_stream_headers(r, ctx);
    if (rc != NGX_OK || r->header_only) { return rc; }
    if (!is_sse) { return ngx_http_ao_stream_body(r, ctx, NULL); }
    b = ngx_calloc_buf(r->pool);
    if (b == NULL) { return NGX_ERROR; }
    b->pos = ctx->resp_buf; b->last = ctx->resp_buf + ctx->resp_len;
    b->memory = 1; b->last_buf = 1;
    in.buf = b; in.next = NULL;
    return ngx_http_ao_stream_body(r, ctx, &in);
}

static ngx_int_t
ngx_http_ao_send_held_json(ngx_http_request_t *r,
    ngx_http_anthropic_openai_ctx_t *ctx, u_char *p, size_t n,
    ngx_uint_t status)
{
    ngx_buf_t    *b;
    ngx_chain_t   out;
    ngx_int_t     rc;

    r->headers_out.status = status;
    r->headers_out.status_line.len = 0;
    r->headers_out.content_type_lowcase = NULL;
    r->headers_out.content_type.data = NULL;
    ngx_str_set(&r->headers_out.content_type, "application/json");
    r->headers_out.content_type_len = sizeof("application/json") - 1;
    r->headers_out.content_length_n = (off_t) n;

    rc = ngx_http_ao_next_header_filter(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }
    ctx->header_sent = 1;
    ctx->headers_held = 0;
    ctx->response_done = 1;
    r->buffered &= ~NGX_HTTP_AO_BUFFERED;

    b = ngx_calloc_buf(r->pool);
    if (b == NULL) {
        return NGX_ERROR;
    }
    b->pos = p;
    b->last = p + n;
    b->memory = 1;
    b->last_buf = 1;
    out.buf = b;
    out.next = NULL;
    return ngx_http_ao_next_body_filter(r, &out);
}


static ngx_int_t
ngx_http_ao_send_error(ngx_http_request_t *r,
    ngx_http_anthropic_openai_ctx_t *ctx, ngx_uint_t status)
{
    const char  *type;
    u_char      *body, *end;

    switch (status) {
    case 400: type = "invalid_request_error"; break;
    case 401: type = "authentication_error"; break;
    case 403: type = "permission_error"; break;
    case 404: type = "not_found_error"; break;
    case 429: type = "rate_limit_error"; break;
    case 529: type = "overloaded_error"; break;
    default: type = "api_error"; break;
    }
    body = ngx_pnalloc(r->pool, 192);
    if (body == NULL) {
        return NGX_ERROR;
    }
    end = ngx_sprintf(body,
        "{\"type\":\"error\",\"error\":{\"type\":\"%s\","
        "\"message\":\"request could not be completed\"}}", type);
    return ngx_http_ao_send_held_json(r, ctx, body, end - body, status);
}


static ngx_int_t
ngx_http_anthropic_openai_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_http_anthropic_openai_ctx_t  *ctx;
    ngx_http_ao_resp_opt_t            ropt;
    ngx_chain_t                      *cl;
    char                             *converted;
    size_t                            out_n;
    ngx_int_t                         last;
    u_char                           *copy;

    ctx = ngx_http_get_module_ctx(r, ngx_http_anthropic_openai_module);
    if (ctx == NULL || !ctx->enabled || ctx->skip_filters) {
        return ngx_http_ao_next_body_filter(r, in);
    }

    if (ctx->response_done) {
        for (cl = in; cl; cl = cl->next) {
            cl->buf->pos = cl->buf->last;
            if (cl->buf->in_file) {
                cl->buf->file_pos = cl->buf->file_last;
            }
        }
        return ngx_http_ao_next_body_filter(r, NULL);
    }

    if (ctx->client_stream && ctx->header_sent) {
        return ngx_http_ao_stream_body(r, ctx, in);
    }

    last = 0;
    for (cl = in; cl; cl = cl->next) {
        if (cl->buf->last > cl->buf->pos) {
            if ((size_t) (cl->buf->last - cl->buf->pos)
                > NGX_HTTP_AO_RESP_BUFFER_MAX - ctx->resp_len)
            {
                cl->buf->pos = cl->buf->last;
                if (ctx->client_stream && r->headers_out.status < 400) {
                    return ngx_http_ao_held_stream_error(r, ctx);
                }
                return ngx_http_ao_send_error(r, ctx,
                    r->headers_out.status >= 400 ? r->headers_out.status : 502);
            }
            if (ngx_http_ao_buf_append(r, &ctx->resp_buf, &ctx->resp_len,
                                       &ctx->resp_cap, cl->buf->pos,
                                       (size_t) (cl->buf->last - cl->buf->pos))
                != NGX_OK)
            {
                return NGX_ERROR;
            }
        }
        if (cl->buf->last_buf) {
            last = 1;
        }
        cl->buf->pos = cl->buf->last;
    }

    if (!last && r->upstream
        && (r->upstream->error
            || (r->upstream->pipe && r->upstream->pipe->upstream_error)
            || (in && r->upstream->cleanup == NULL)))
    {
        if (ctx->client_stream && r->headers_out.status < 400) {
            return ngx_http_ao_held_stream_error(r, ctx);
        }
        return ngx_http_ao_send_error(r, ctx,
                    r->headers_out.status >= 400 ? r->headers_out.status : 502);
    }

    if (!last) {
        r->buffered |= NGX_HTTP_AO_BUFFERED;
        return NGX_OK;
    }

    r->buffered &= ~NGX_HTTP_AO_BUFFERED;

    if (r->headers_out.status >= 400) {
        return ngx_http_ao_send_error(r, ctx, r->headers_out.status);
    }

    if (ctx->client_stream) {
        return ngx_http_ao_held_stream_finish(r, ctx);
    }

    ngx_memzero(&ropt, sizeof(ropt));
    if (ctx->orig_model.len) {
        /* orig_model is not NUL-terminated; convert_response uses C string */
        copy = ngx_pnalloc(r->pool, ctx->orig_model.len + 1);
        if (copy == NULL) {
            return NGX_ERROR;
        }
        ngx_memcpy(copy, ctx->orig_model.data, ctx->orig_model.len);
        copy[ctx->orig_model.len] = 0;
        ropt.echo_model = (char *) copy;
    }

    converted = ngx_http_ao_convert_response(ctx->resp_buf, ctx->resp_len,
                                             &ropt, &out_n);
    if (converted == NULL) {
        static u_char  err[] =
            "{\"type\":\"error\",\"error\":{\"type\":\"api_error\","
            "\"message\":\"upstream protocol error\"}}";
        return ngx_http_ao_send_held_json(r, ctx, err, sizeof(err) - 1, 502);
    }

    copy = ngx_pnalloc(r->pool, out_n);
    if (copy == NULL) {
        cJSON_free(converted);
        return NGX_ERROR;
    }
    ngx_memcpy(copy, converted, out_n);
    cJSON_free(converted);

    return ngx_http_ao_send_held_json(r, ctx, copy, out_n, 200);
}


static ngx_int_t
ngx_http_anthropic_openai_postconfiguration(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
    h = ngx_array_push(&cmcf->phases[NGX_HTTP_REWRITE_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }
    *h = ngx_http_anthropic_openai_rewrite_handler;

    ngx_http_ao_next_request_body_filter = ngx_http_top_request_body_filter;
    ngx_http_top_request_body_filter = ngx_http_anthropic_openai_request_body_filter;

    ngx_http_ao_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_anthropic_openai_header_filter;

    ngx_http_ao_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_anthropic_openai_body_filter;

    return NGX_OK;
}


static void *
ngx_http_anthropic_openai_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_anthropic_openai_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_anthropic_openai_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }
    conf->enable = NGX_CONF_UNSET;
    conf->count_tokens = NGX_CONF_UNSET_UINT;
    conf->stream_usage = NGX_CONF_UNSET;
    return conf;
}


static char *
ngx_http_anthropic_openai_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child)
{
    ngx_http_anthropic_openai_loc_conf_t  *prev = parent;
    ngx_http_anthropic_openai_loc_conf_t  *conf = child;

    ngx_conf_merge_value(conf->enable, prev->enable, 0);
    ngx_conf_merge_str_value(conf->model, prev->model, "");
    ngx_conf_merge_str_value(conf->api_key, prev->api_key, "");
    ngx_conf_merge_uint_value(conf->count_tokens, prev->count_tokens,
                              NGX_HTTP_AO_COUNT_TOKENS_HEURISTIC);
    ngx_conf_merge_value(conf->stream_usage, prev->stream_usage, 1);

    return NGX_CONF_OK;
}
