#include "ngx_http_anthropic_openai_json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define AO_SSE_LINE_MAX  (1024 * 1024)
#define AO_SSE_ARGS_MAX  (256 * 1024)
#define AO_SSE_TEXT_MAX  (32 * 1024)
#define AO_SSE_MAX_TOOLS  32

typedef struct {
    unsigned               oi;
    char                   id[256];
    char                   name[256];
    char                  *args;
    size_t                 args_len;
} ao_tool_t;

/* The parser holds one event tree. No encoded event is retained internally.
 * All continuation state advances only when pull writes a complete event. */
struct ngx_http_ao_sse_s {
    ngx_http_ao_alloc_pt   alloc;
    void                  *alloc_ctx;
    char                  *model;
    char                   msg_id[256];
    char                   stop_reason[32];
    unsigned               id_set, started, terminal, done, errored, error_sent;
    unsigned               last_was_cr, has_data, usage_seen, seen_finish,
                           committed;
    unsigned               msg_delta_sent, text_open, text_index, next_index;
    unsigned               commit_pending, tool_i, tool_phase, pending_phase;
    int                    input_tokens, output_tokens;
    char                  *parser;
    size_t                 data_len, line_len;
    char                   event[256];
    cJSON                 *chunk;
    const char            *text;
    size_t                 text_len, slice_off;
    unsigned               close_text;
    char                  *pending_text;
    size_t                 pt_len;
    ao_tool_t              tools[AO_SSE_MAX_TOOLS];
    unsigned               ntools;
    /* Only standalone convenience feed/copy_out use this collection. */
    char                  *collection;
    size_t                 collection_len;
    unsigned char         *out;
    size_t                 out_len, out_cap;
};

static void *
ao_alloc(ngx_http_ao_sse_t *s, size_t n)
{
    return s->alloc != NULL ? s->alloc(s->alloc_ctx, n) : malloc(n);
}

static int
ao_emit_obj(ngx_http_ao_sse_t *s, const char *event, cJSON *obj)
{
    size_t  prefix, n;
    int     ok;

    if (obj == NULL) { return -1; }
    prefix = 14 + strlen(event);
    if (prefix + 8 >= s->out_cap) { cJSON_Delete(obj); return -1; }
    memcpy(s->out, "event: ", 7);
    memcpy(s->out + 7, event, strlen(event));
    memcpy(s->out + 7 + strlen(event), "\ndata: ", 7);
    ok = cJSON_PrintPreallocated(obj, (char *) s->out + prefix,
                                (int) (s->out_cap - prefix - 2), 0);
    cJSON_Delete(obj);
    if (!ok) { return -1; }
    n = strlen((char *) s->out + prefix);
    s->out[prefix + n] = '\n';
    s->out[prefix + n + 1] = '\n';
    s->out_len = prefix + n + 2;
    return 0;
}

static int
ao_emit_message_start(ngx_http_ao_sse_t *s)
{
    cJSON  *root, *msg, *usage, *content;

    root = cJSON_CreateObject();
    if (root == NULL) {
        return -1;
    }
    cJSON_AddStringToObject(root, "type", "message_start");
    msg = cJSON_CreateObject();
    if (msg == NULL) {
        cJSON_Delete(root);
        return -1;
    }
    cJSON_AddItemToObject(root, "message", msg);
    cJSON_AddStringToObject(msg, "id",
                            s->id_set ? s->msg_id : "msg_ao");
    cJSON_AddStringToObject(msg, "type", "message");
    cJSON_AddStringToObject(msg, "role", "assistant");
    content = cJSON_CreateArray();
    if (content == NULL) {
        cJSON_Delete(root);
        return -1;
    }
    cJSON_AddItemToObject(msg, "content", content);
    cJSON_AddStringToObject(msg, "model", s->model);
    cJSON_AddNullToObject(msg, "stop_reason");
    cJSON_AddNullToObject(msg, "stop_sequence");
    usage = cJSON_CreateObject();
    if (usage == NULL) {
        cJSON_Delete(root);
        return -1;
    }
    cJSON_AddNumberToObject(usage, "input_tokens",
                            s->usage_seen ? s->input_tokens : 0);
    cJSON_AddNumberToObject(usage, "output_tokens", 0);
    cJSON_AddItemToObject(msg, "usage", usage);
    return ao_emit_obj(s, "message_start", root);
}


static int
ao_emit_ping(ngx_http_ao_sse_t *s)
{
    cJSON  *root;

    root = cJSON_CreateObject();
    if (root == NULL) {
        return -1;
    }
    cJSON_AddStringToObject(root, "type", "ping");
    return ao_emit_obj(s, "ping", root);
}


static int
ao_emit_block_stop(ngx_http_ao_sse_t *s, unsigned idx)
{
    cJSON  *root;

    root = cJSON_CreateObject();
    if (root == NULL) {
        return -1;
    }
    cJSON_AddStringToObject(root, "type", "content_block_stop");
    cJSON_AddNumberToObject(root, "index", (double) idx);
    return ao_emit_obj(s, "content_block_stop", root);
}


static int
ao_emit_text_start(ngx_http_ao_sse_t *s, unsigned idx)
{
    cJSON  *root, *blk;

    root = cJSON_CreateObject();
    if (root == NULL) {
        return -1;
    }
    cJSON_AddStringToObject(root, "type", "content_block_start");
    cJSON_AddNumberToObject(root, "index", (double) idx);
    blk = cJSON_CreateObject();
    if (blk == NULL) {
        cJSON_Delete(root);
        return -1;
    }
    cJSON_AddStringToObject(blk, "type", "text");
    cJSON_AddStringToObject(blk, "text", "");
    cJSON_AddItemToObject(root, "content_block", blk);
    return ao_emit_obj(s, "content_block_start", root);
}


static int
ao_emit_tool_start(ngx_http_ao_sse_t *s, unsigned idx, ao_tool_t *t)
{
    cJSON  *root, *blk, *input;

    root = cJSON_CreateObject();
    if (root == NULL) {
        return -1;
    }
    cJSON_AddStringToObject(root, "type", "content_block_start");
    cJSON_AddNumberToObject(root, "index", (double) idx);
    blk = cJSON_CreateObject();
    if (blk == NULL) {
        cJSON_Delete(root);
        return -1;
    }
    cJSON_AddStringToObject(blk, "type", "tool_use");
    cJSON_AddStringToObject(blk, "id", t->id);
    cJSON_AddStringToObject(blk, "name", t->name);
    input = cJSON_CreateObject();
    if (input == NULL) {
        cJSON_Delete(root);
        return -1;
    }
    cJSON_AddItemToObject(blk, "input", input);
    cJSON_AddItemToObject(root, "content_block", blk);
    return ao_emit_obj(s, "content_block_start", root);
}


static int
ao_emit_message_delta(ngx_http_ao_sse_t *s)
{
    cJSON  *root, *delta, *usage;

    root = cJSON_CreateObject();
    if (root == NULL) {
        return -1;
    }
    cJSON_AddStringToObject(root, "type", "message_delta");
    delta = cJSON_CreateObject();
    if (delta == NULL) {
        cJSON_Delete(root);
        return -1;
    }
    cJSON_AddStringToObject(delta, "stop_reason", s->stop_reason);
    cJSON_AddNullToObject(delta, "stop_sequence");
    cJSON_AddItemToObject(root, "delta", delta);
    usage = cJSON_CreateObject();
    if (usage == NULL) {
        cJSON_Delete(root);
        return -1;
    }
    if (s->usage_seen) {
        cJSON_AddNumberToObject(usage, "input_tokens", s->input_tokens);
    }
    cJSON_AddNumberToObject(usage, "output_tokens", s->output_tokens);
    cJSON_AddItemToObject(root, "usage", usage);
    s->msg_delta_sent = 1;
    return ao_emit_obj(s, "message_delta", root);
}


static int
ao_emit_message_stop(ngx_http_ao_sse_t *s)
{
    cJSON  *root;

    root = cJSON_CreateObject();
    if (root == NULL) {
        return -1;
    }
    cJSON_AddStringToObject(root, "type", "message_stop");
    return ao_emit_obj(s, "message_stop", root);
}


/* Copy complete UTF-8 codepoints while budgeting JSON escaping and framing. */
static int
ao_emit_slice(ngx_http_ao_sse_t *s, unsigned idx, const char *text,
    size_t len, size_t *off, int tool)
{
    char               prefix[192];
    static const char  hex[] = "0123456789abcdef";
    size_t             n, i, width, cost, j;
    unsigned char      c;

    sprintf(prefix, "event: content_block_delta\ndata: {\"type\":"
            "\"content_block_delta\",\"index\":%u,\"delta\":{\"type\":\"%s\",\"%s\":\"",
            idx, tool ? "input_json_delta" : "text_delta",
            tool ? "partial_json" : "text");
    n = strlen(prefix);
    if (s->out_cap < n + 12) { return -1; }
    memcpy(s->out, prefix, n);
    i = *off;
    while (i < len) {
        c = (unsigned char) text[i];
        width = c < 0x80 ? 1 : c >= 0xc2 && c <= 0xdf ? 2
                : c >= 0xe0 && c <= 0xef ? 3
                : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        if (width == 0 || width > len - i) { return -1; }
        for (j = 1; j < width; j++) {
            if (((unsigned char) text[i + j] & 0xc0) != 0x80) { return -1; }
        }
        if ((c == 0xe0 && (unsigned char) text[i + 1] < 0xa0)
            || (c == 0xed && (unsigned char) text[i + 1] >= 0xa0)
            || (c == 0xf0 && (unsigned char) text[i + 1] < 0x90)
            || (c == 0xf4 && (unsigned char) text[i + 1] >= 0x90))
        { return -1; }
        cost = c < 0x20 ? 6 : c == '"' || c == '\\' ? 2 : width;
        if (n + cost + 5 > s->out_cap) { break; }
        if (c < 0x20) {
            memcpy(s->out + n, "\\u00", 4);
            s->out[n + 4] = hex[c >> 4]; s->out[n + 5] = hex[c & 15];
        } else if (c == '"' || c == '\\') {
            s->out[n] = '\\'; s->out[n + 1] = c;
        } else { memcpy(s->out + n, text + i, width); }
        n += cost; i += width;
    }
    if (i == *off && i < len) { return -1; }
    memcpy(s->out + n, "\"}}\n\n", 5);
    s->out_len = n + 5; *off = i;
    return 0;
}

static void
ao_error(ngx_http_ao_sse_t *s)
{
    if (!s->done && !s->terminal) {
        s->errored = 1; s->terminal = 1;
    }
}

static const char *
ao_string(cJSON *obj, const char *key)
{
    cJSON  *v;
    v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) && v->valuestring != NULL ? v->valuestring : NULL;
}

static int
ao_copy(char *dst, size_t size, const char *v)
{
    if (v == NULL || strlen(v) >= size) { return -1; }
    memcpy(dst, v, strlen(v) + 1); return 0;
}

static void
ao_usage(ngx_http_ao_sse_t *s, cJSON *usage)
{
    cJSON  *v;
    if (!cJSON_IsObject(usage)) { return; }
    v = cJSON_GetObjectItemCaseSensitive(usage, "prompt_tokens");
    if (v == NULL) { v = cJSON_GetObjectItemCaseSensitive(usage, "input_tokens"); }
    if (cJSON_IsNumber(v)) { s->input_tokens = v->valueint; }
    v = cJSON_GetObjectItemCaseSensitive(usage, "completion_tokens");
    if (v == NULL) { v = cJSON_GetObjectItemCaseSensitive(usage, "output_tokens"); }
    if (cJSON_IsNumber(v)) { s->output_tokens = v->valueint; }
    s->usage_seen = 1;
}

static int
ao_tools(ngx_http_ao_sse_t *s, cJSON *tc)
{
    cJSON       *el, *v, *fn;
    const char  *str;
    unsigned     i, oi, k;
    size_t       n;
    ao_tool_t   *t;

    if (s->committed) { return -1; }
    for (i = 0; i < (unsigned) cJSON_GetArraySize(tc); i++) {
        el = cJSON_GetArrayItem(tc, (int) i);
        if (!cJSON_IsObject(el)) { return -1; }
        v = cJSON_GetObjectItemCaseSensitive(el, "index");
        oi = i;
        if (v != NULL) {
            if (!cJSON_IsNumber(v) || v->valuedouble < 0
                || v->valuedouble > 2147483647.0
                || v->valuedouble != (double) v->valueint) { return -1; }
            oi = (unsigned) v->valueint;
        }
        for (k = 0; k < s->ntools && s->tools[k].oi != oi; k++) { /* void */ }
        if (k == s->ntools) {
            if (k == AO_SSE_MAX_TOOLS) { return -1; }
            s->ntools++; s->tools[k].oi = oi;
        }
        t = &s->tools[k]; str = ao_string(el, "id");
        if (str != NULL && *str && !t->id[0]
            && ao_copy(t->id, sizeof(t->id), str) != 0) { return -1; }
        fn = cJSON_GetObjectItemCaseSensitive(el, "function");
        str = ao_string(fn, "name");
        if (str == NULL) { str = ao_string(el, "name"); }
        if (str != NULL && *str && !t->name[0]
            && ao_copy(t->name, sizeof(t->name), str) != 0) { return -1; }
        v = cJSON_GetObjectItemCaseSensitive(fn, "arguments");
        if (v != NULL && !cJSON_IsString(v)) { return -1; }
        str = ao_string(fn, "arguments");
        if (str == NULL) { continue; }
        n = strlen(str);
        if (n > AO_SSE_ARGS_MAX - t->args_len) { return -1; }
        if (t->args == NULL) {
            t->args = ao_alloc(s, AO_SSE_ARGS_MAX + 1);
            if (t->args == NULL) { return -1; }
        }
        memcpy(t->args + t->args_len, str, n + 1); t->args_len += n;
    }
    return 0;
}

/* Validate the entire commit before emitting any tool_use block. */
static int
ao_commit(ngx_http_ao_sse_t *s)
{
    unsigned    i, j;
    ao_tool_t   tmp;
    cJSON      *v;

    if (s->committed) { return 0; }
    for (i = 0; i < s->ntools; i++) {
        if (!s->tools[i].id[0] || !s->tools[i].name[0]) { return -1; }
        v = ngx_http_ao_json_parse((unsigned char *)
                  (s->tools[i].args_len ? s->tools[i].args : "{}"),
                  s->tools[i].args_len ? s->tools[i].args_len : 2);
        if (!cJSON_IsObject(v)) { cJSON_Delete(v); return -1; }
        cJSON_Delete(v);
    }
    for (i = 0; i < s->ntools; i++) {
        for (j = i + 1; j < s->ntools; j++) {
            if (s->tools[j].oi < s->tools[i].oi) {
                tmp = s->tools[i]; s->tools[i] = s->tools[j]; s->tools[j] = tmp;
            }
        }
    }
    s->committed = 1; s->commit_pending = 1;
    return 0;
}

static int
ao_chunk(ngx_http_ao_sse_t *s, cJSON *root, int synthetic)
{
    cJSON       *choices, *choice, *delta, *tc, *err;
    const char  *str, *finish;
    size_t       n;
    unsigned     had_tools;

    s->chunk = root;
    err = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (!cJSON_IsObject(root) || (err != NULL && !cJSON_IsNull(err))) { return -1; }
    str = ao_string(root, "id");
    if (!s->id_set && str != NULL && *str) {
        if (ao_copy(s->msg_id, sizeof(s->msg_id), str) != 0) { return -1; }
        s->id_set = 1;
    }
    ao_usage(s, cJSON_GetObjectItemCaseSensitive(root, "usage"));
    choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    if (!cJSON_IsArray(choices)) { return -1; }
    if (cJSON_GetArraySize(choices) == 0) { return synthetic ? -1 : 0; }
    choice = cJSON_GetArrayItem(choices, 0);
    if (!cJSON_IsObject(choice)) { return -1; }
    delta = cJSON_GetObjectItemCaseSensitive(choice, "delta");
    if (!cJSON_IsObject(delta)) {
        delta = cJSON_GetObjectItemCaseSensitive(choice, "message");
    }
    if (synthetic && !cJSON_IsObject(delta)) { return -1; }
    had_tools = s->ntools;
    str = ao_string(delta, "content");
    tc = cJSON_GetObjectItemCaseSensitive(delta, "tool_calls");
    if (s->seen_finish && ((str != NULL && *str) || cJSON_GetArraySize(tc))) { return -1; }
    if (str != NULL && *str) {
        n = strlen(str);
        if (had_tools) {
            if (n > AO_SSE_TEXT_MAX - s->pt_len) { return -1; }
            if (s->pending_text == NULL) {
                s->pending_text = ao_alloc(s, AO_SSE_TEXT_MAX + 1);
                if (s->pending_text == NULL) { return -1; }
            }
            memcpy(s->pending_text + s->pt_len, str, n + 1); s->pt_len += n;
        } else { s->text = str; s->text_len = n; s->slice_off = 0; }
    }
    if (cJSON_IsArray(tc) && cJSON_GetArraySize(tc)) {
        if (ao_tools(s, tc) != 0) { return -1; }
        s->close_text = 1;
    }
    finish = ao_string(choice, "finish_reason");
    if (finish != NULL && *finish) {
        if (s->seen_finish) { return -1; }
        s->seen_finish = 1;
        strcpy(s->stop_reason, strcmp(finish, "length") == 0 ? "max_tokens"
               : strcmp(finish, "tool_calls") == 0 ? "tool_use" : "end_turn");
        if (ao_commit(s) != 0) { return -1; }
    }
    if (synthetic) {
        if (!s->seen_finish && s->ntools) { strcpy(s->stop_reason, "tool_use"); }
        if (ao_commit(s) != 0) { return -1; }
        s->terminal = 1;
    }
    return 0;
}

int
ngx_http_ao_sse_pending(ngx_http_ao_sse_t *s)
{
    return !s->done && (s->chunk != NULL || s->terminal || s->commit_pending);
}

int
ngx_http_ao_sse_done(ngx_http_ao_sse_t *s)
{
    return s->done;
}

/* 1 = one event, 0 = idle. No allocation of serialized output. */
int
ngx_http_ao_sse_pull(ngx_http_ao_sse_t *s, unsigned char *out, size_t cap,
    size_t *len, int *last)
{
    int         rc;
    unsigned    idx;
    ao_tool_t  *t;

    *len = 0; *last = 0;
    if (!ngx_http_ao_sse_pending(s)) { return 0; }
    s->out = out; s->out_cap = cap > NGX_HTTP_AO_EVENT_OUT_MAX
                                   ? NGX_HTTP_AO_EVENT_OUT_MAX : cap;
    s->out_len = 0;
    rc = 0;
    if (s->text != NULL && s->slice_off == s->text_len) {
        s->text = NULL; s->text_len = 0; s->slice_off = 0;
    }
    if (s->started == 0) { rc = ao_emit_message_start(s); s->started = 1; }
    else if (s->started == 1) { rc = ao_emit_ping(s); s->started = 2; }
    else if (s->errored) {
        if (!s->error_sent) {
            cJSON  *obj, *err;
            obj = cJSON_CreateObject(); err = cJSON_CreateObject();
            if (obj == NULL || err == NULL) {
                cJSON_Delete(obj); cJSON_Delete(err); return -1;
            }
            cJSON_AddStringToObject(obj, "type", "error");
            cJSON_AddStringToObject(err, "type", "api_error");
            cJSON_AddStringToObject(err, "message", "upstream protocol error");
            cJSON_AddItemToObject(obj, "error", err);
            rc = ao_emit_obj(s, "error", obj); s->error_sent = 1;
        } else { rc = ao_emit_message_stop(s); s->done = 1; }
    } else if (s->text != NULL && s->slice_off < s->text_len) {
        if (!s->text_open) {
            s->text_index = s->next_index++; s->text_open = 1;
            rc = ao_emit_text_start(s, s->text_index);
        } else {
            rc = ao_emit_slice(s, s->text_index, s->text, s->text_len,
                               &s->slice_off, 0);
        }
    } else if (s->text_open && (s->close_text || s->commit_pending || s->terminal)) {
        rc = ao_emit_block_stop(s, s->text_index); s->text_open = 0;
    } else if (s->commit_pending && s->tool_i < s->ntools) {
        t = &s->tools[s->tool_i]; idx = s->next_index;
        if (s->tool_phase == 0) {
            rc = ao_emit_tool_start(s, idx, t);
            s->tool_phase = 1; s->slice_off = 0;
        } else if (s->tool_phase == 1) {
            rc = ao_emit_slice(s, idx, t->args_len ? t->args : "{}",
                               t->args_len ? t->args_len : 2, &s->slice_off, 1);
            if (s->slice_off == (t->args_len ? t->args_len : 2)) { s->tool_phase = 2; }
        } else {
            rc = ao_emit_block_stop(s, idx); s->next_index++;
            s->tool_i++; s->tool_phase = 0; s->slice_off = 0;
        }
    } else if (s->commit_pending && s->pt_len && s->pending_phase < 3) {
        idx = s->next_index;
        if (s->pending_phase == 0) {
            rc = ao_emit_text_start(s, idx); s->pending_phase = 1; s->slice_off = 0;
        } else if (s->pending_phase == 1) {
            rc = ao_emit_slice(s, idx, s->pending_text, s->pt_len, &s->slice_off, 0);
            if (s->slice_off == s->pt_len) { s->pending_phase = 2; }
        } else {
            rc = ao_emit_block_stop(s, idx); s->next_index++; s->pending_phase = 3;
        }
    } else {
        s->commit_pending = 0;
        s->text = NULL; s->text_len = 0; s->slice_off = 0; s->close_text = 0;
        cJSON_Delete(s->chunk); s->chunk = NULL;
        if (!s->terminal) { return 0; }
        if (!s->msg_delta_sent) { rc = ao_emit_message_delta(s); }
        else { rc = ao_emit_message_stop(s); s->done = 1; }
    }
    if (rc != 0) {
        /* The caller has not sent this slot. Replace it with a terminal error. */
        if (s->errored) { return -1; }
        s->terminal = 0; ao_error(s);
        return ngx_http_ao_sse_pull(s, out, cap, len, last);
    }
    *len = s->out_len; *last = s->done;
    return 1;
}

static int
ao_dispatch(ngx_http_ao_sse_t *s)
{
    cJSON   *root;
    size_t   a, b;

    if (!s->has_data) { return 0; }
    if (strcmp(s->event, "error") == 0) { return -1; }
    a = 0; b = s->data_len;
    while (a < b && (s->parser[a] == ' ' || s->parser[a] == '\t')) { a++; }
    while (b > a && (s->parser[b - 1] == ' ' || s->parser[b - 1] == '\t')) { b--; }
    if (b - a == 6 && memcmp(s->parser + a, "[DONE]", 6) == 0) {
        if (ao_commit(s) != 0) { return -1; }
        if (!s->seen_finish && s->ntools) { strcpy(s->stop_reason, "tool_use"); }
        s->terminal = 1; return 0;
    }
    root = ngx_http_ao_json_parse((unsigned char *) s->parser, s->data_len);
    if (root == NULL) { return -1; }
    return ao_chunk(s, root, 0);
}

static int
ao_line(ngx_http_ao_sse_t *s)
{
    char    *line, *value;
    size_t   i, n, namesize, vlen;
    int      rc;

    n = s->line_len; line = s->parser + s->data_len;
    if (n == 0) {
        rc = ao_dispatch(s);
        s->data_len = 0; s->has_data = 0; s->event[0] = 0;
        return rc;
    }
    if (line[0] == ':') { return 0; }
    namesize = n; value = line + n; vlen = 0;
    for (i = 0; i < n; i++) {
        if (line[i] == ':') {
            namesize = i; value = line + i + 1; vlen = n - i - 1;
            if (vlen && *value == ' ') { value++; vlen--; }
            break;
        }
    }
    if (namesize == 4 && memcmp(line, "data", 4) == 0) {
        if (s->has_data) {
            memmove(line + 1, value, vlen); *line = '\n'; s->data_len++;
        } else { memmove(line, value, vlen); }
        s->data_len += vlen; s->has_data = 1;
    } else if (namesize == 5 && memcmp(line, "event", 5) == 0) {
        if (vlen >= sizeof(s->event)) { return -1; }
        memcpy(s->event, value, vlen); s->event[vlen] = 0;
    }
    return 0;
}

/* Return consumed bytes, stopping at a dispatch that needs output. */
int
ngx_http_ao_sse_consume(ngx_http_ao_sse_t *s, const unsigned char *p,
    size_t n, size_t *used)
{
    unsigned char  c;
    int            rc;

    *used = 0;
    if (s->terminal || s->done) { *used = n; return 0; }
    if (ngx_http_ao_sse_pending(s)) { return 1; }
    while (*used < n) {
        c = p[(*used)++];
        if (s->last_was_cr) {
            s->last_was_cr = 0;
            if (c == '\n') { continue; }
        }
        if (c == '\n' || c == '\r') {
            s->last_was_cr = c == '\r';
            rc = ao_line(s); s->line_len = 0;
            if (rc != 0) { ao_error(s); return -1; }
            if (ngx_http_ao_sse_pending(s)) { return 1; }
        } else {
            if (s->data_len + s->line_len >= AO_SSE_LINE_MAX) {
                ao_error(s); return -1;
            }
            s->parser[s->data_len + s->line_len++] = (char) c;
        }
    }
    return 0;
}

int
ngx_http_ao_sse_finish_input(ngx_http_ao_sse_t *s, int transport_error)
{
    if (s->terminal || s->done) { return s->errored ? -1 : 0; }
    if (transport_error || s->has_data || s->line_len || !s->seen_finish) {
        ao_error(s); return -1;
    }
    s->terminal = 1; return 0;
}

int
ngx_http_ao_sse_json(ngx_http_ao_sse_t *s, unsigned char *p, size_t n)
{
    cJSON  *root;
    if (n > 16 * 1024 * 1024 || ngx_http_ao_sse_pending(s)) { ao_error(s); return -1; }
    root = ngx_http_ao_json_parse(p, n);
    if (root == NULL || ao_chunk(s, root, 1) != 0) {
        s->terminal = 0; ao_error(s); return -1;
    }
    return 0;
}

ngx_http_ao_sse_t *
ngx_http_ao_sse_create_alloc(const char *model, ngx_http_ao_alloc_pt alloc,
    void *ctx)
{
    ngx_http_ao_sse_t  *s;
    size_t              n;

    s = alloc != NULL ? alloc(ctx, sizeof(*s)) : malloc(sizeof(*s));
    if (s == NULL) { return NULL; }
    memset(s, 0, sizeof(*s)); s->alloc = alloc; s->alloc_ctx = ctx;
    n = model != NULL ? strlen(model) : 0;
    s->model = ao_alloc(s, n + 1);
    s->parser = ao_alloc(s, AO_SSE_LINE_MAX + 2);
    if (s->model == NULL || s->parser == NULL) {
        ngx_http_ao_sse_destroy(s); return NULL;
    }
    if (n) { memcpy(s->model, model, n); } s->model[n] = 0;
    strcpy(s->stop_reason, "end_turn"); return s;
}

ngx_http_ao_sse_t *
ngx_http_ao_sse_create(const char *model)
{
    return ngx_http_ao_sse_create_alloc(model, NULL, NULL);
}

void
ngx_http_ao_sse_destroy(ngx_http_ao_sse_t *s)
{
    unsigned  i;
    if (s == NULL) { return; }
    cJSON_Delete(s->chunk);
    free(s->collection);
    if (s->alloc != NULL) { return; }
    free(s->parser); free(s->model); free(s->pending_text);
    for (i = 0; i < s->ntools; i++) { free(s->tools[i].args); }
    free(s);
}

/* Test/offline collection wrappers. Never called by the nginx filter. */
static int
ao_collect(ngx_http_ao_sse_t *s)
{
    unsigned char  *buf;
    char           *p;
    size_t          n;
    int             rc, last;
    buf = malloc(NGX_HTTP_AO_EVENT_OUT_MAX);
    if (buf == NULL) { return -1; }
    while ((rc = ngx_http_ao_sse_pull(s, buf, NGX_HTTP_AO_EVENT_OUT_MAX, &n, &last)) > 0) {
        p = realloc(s->collection, s->collection_len + n + 1);
        if (p == NULL) { free(buf); return -1; }
        s->collection = p;
        memcpy(p + s->collection_len, buf, n); s->collection_len += n;
        p[s->collection_len] = 0;
    }
    free(buf); return rc;
}

int
ngx_http_ao_sse_feed(ngx_http_ao_sse_t *s, unsigned char *p, size_t n)
{
    size_t  used;
    if (s == NULL) { return -1; }
    while (n) {
        (void) ngx_http_ao_sse_consume(s, p, n, &used);
        p += used; n -= used;
        if (ao_collect(s) != 0) { return -1; }
    }
    return s->errored ? -1 : 0;
}

int
ngx_http_ao_sse_end(ngx_http_ao_sse_t *s, int transport_error)
{
    int  rc;
    if (s == NULL) { return -1; }
    rc = ngx_http_ao_sse_finish_input(s, transport_error);
    if (ao_collect(s) != 0) { return -1; }
    return rc;
}

char *
ngx_http_ao_sse_copy_out(ngx_http_ao_sse_t *s, size_t *n)
{
    char  *p;
    *n = s->collection_len;
    p = malloc(*n + 1);
    if (p != NULL) {
        if (*n) { memcpy(p, s->collection, *n); }
        p[*n] = 0;
    }
    return p;
}

void
ngx_http_ao_sse_clear_out(ngx_http_ao_sse_t *s)
{
    free(s->collection); s->collection = NULL; s->collection_len = 0;
}

char *
ngx_http_ao_convert_sse(unsigned char *p, size_t n, const char *model,
    size_t *out_len)
{
    ngx_http_ao_sse_t  *s;
    char               *out;
    *out_len = 0; s = ngx_http_ao_sse_create(model);
    if (s == NULL) { return NULL; }
    if (ngx_http_ao_sse_feed(s, p, n) != 0 || ngx_http_ao_sse_end(s, 0) != 0) {
        ngx_http_ao_sse_destroy(s); return NULL;
    }
    out = ngx_http_ao_sse_copy_out(s, out_len);
    ngx_http_ao_sse_destroy(s); return out;
}
