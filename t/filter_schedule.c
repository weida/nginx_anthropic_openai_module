/* Actual production filter in one translation unit; nginx types, deterministic
 * downstream sink. Linker discards unrelated request/configuration handlers. */
#include "../src/ngx_http_anthropic_openai_module.c"
#include <assert.h>
#include <stdio.h>

static unsigned allocations;
static void *pool_allocations[128];
static unsigned pool_count;
static int hold_ok, block_terminal, block_each;
static unsigned last_count;
static ngx_buf_t *held[2];
static unsigned nheld, submitted, null_calls;
static char *captured;
static size_t captured_len;
static int blocked = 1;

void *ngx_palloc(ngx_pool_t *pool, size_t n)
{
    void *p;
    (void) pool; allocations++; p = calloc(1, n);
    assert(pool_count < 128); pool_allocations[pool_count++] = p; return p;
}
void *ngx_pnalloc(ngx_pool_t *pool, size_t n) { return ngx_palloc(pool, n); }
void *ngx_pcalloc(ngx_pool_t *pool, size_t n) { return ngx_palloc(pool, n); }
ngx_http_cleanup_t *ngx_http_cleanup_add(ngx_http_request_t *r, size_t n)
{
    (void) n; return ngx_palloc(r->pool, sizeof(ngx_http_cleanup_t));
}
static void consume(ngx_buf_t *b)
{
    size_t n;
    n = b->last - b->pos;
    captured = realloc(captured, captured_len + n + 1);
    memcpy(captured + captured_len, b->pos, n); captured_len += n;
    captured[captured_len] = 0; b->pos = b->last;
    last_count += b->last_buf;
}
static ngx_int_t sink(ngx_http_request_t *r, ngx_chain_t *in)
{
    unsigned i;
    (void) r;
    if (in == NULL) {
        null_calls++;
        if (blocked) { return NGX_AGAIN; }
        for (i = 0; i < nheld; i++) { consume(held[i]); }
        nheld = 0; return NGX_OK;
    }
    for (; in; in = in->next) {
        submitted++;
        if (block_each) { blocked = 1; }
        if (block_terminal && in->buf->last_buf) { blocked = 1; }
        if (blocked) { assert(nheld < 2); held[nheld++] = in->buf; }
        else { consume(in->buf); }
    }
    return blocked && !hold_ok ? NGX_AGAIN : NGX_OK;
}
static void setup(ngx_http_request_t *r, ngx_http_anthropic_openai_ctx_t *ctx)
{
    blocked = 1; hold_ok = 0; block_terminal = 0; block_each = 0; last_count = 0;
    nheld = 0; submitted = 0; null_calls = 0; captured_len = 0;
    memset(r, 0, sizeof(*r)); memset(ctx, 0, sizeof(*ctx));
    r->main = r;
    ctx->enabled = 1; ctx->client_stream = 1; ctx->header_sent = 1;
    ngx_str_set(&ctx->orig_model, "schedule-model");
    ngx_http_ao_next_body_filter = sink;
}

static void teardown(ngx_http_anthropic_openai_ctx_t *ctx)
{
    ngx_http_ao_cleanup_sse(ctx);
    while (pool_count) { free(pool_allocations[--pool_count]); }
    free(captured); captured = NULL;
}
static unsigned occurrences(const char *haystack, const char *needle)
{
    unsigned n;
    n = 0;
    while ((haystack = strstr(haystack, needle)) != NULL) { n++; haystack++; }
    return n;
}
static void set_input_buf(ngx_buf_t *b, const char *wire)
{
    memset(b, 0, sizeof(*b)); b->memory = 1;
    b->pos = (u_char *) wire; b->last = b->pos + strlen(wire);
}
int main(void)
{
    ngx_http_request_t r;
    ngx_http_anthropic_openai_ctx_t ctx;
    ngx_buf_t b, c, overflow[33];
    ngx_chain_t overflow_chain[33];
    u_char recycled[4];
    ngx_chain_t in, cin;
    ngx_int_t rc;
    unsigned i, before;
    ngx_http_upstream_t upstream;
    ngx_http_cleanup_pt cleanup;
    char *args, *toolwire, *json, *reconstructed;
    size_t arglen, wirelen, reconstructed_len;
    cJSON *root, *choices, *choice, *message, *tools, *tool, *fn, *obj, *d;
    char *cursor, *boundary;
    unsigned slices;
    u_char *saved;
    char wire[] = "data: {\"choices\":[{\"delta\":{\"content\":\"B\"}}]}\n\n"
                  "data: {\"choices\":[{\"delta\":{\"content\":\"B2\"}}]}\n\n";
    char end[] = "data: {\"choices\":[{\"delta\":{\"content\":\"C\"},"
                 "\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
    char delta[] = "data: {\"choices\":[{\"delta\":{\"content\":\"z\"}}]}\n\n";
    setup(&r, &ctx); set_input_buf(&b, wire); set_input_buf(&c, end);
    in.buf = &b; in.next = NULL; cin.buf = &c; cin.next = NULL;
    rc = ngx_http_ao_stream_body(&r, &ctx, &in);
    assert(rc == NGX_AGAIN);
    assert(b.pos < b.last); saved = b.pos;
    assert(nheld == 1); assert(!ctx.response_done);
    /* Upstream links C to B's original chain after return, then passes C.
     * We must never traverse that now-mutated B chain again. */
    in.next = &cin;
    rc = ngx_http_ao_stream_body(&r, &ctx, &cin);
    assert(rc == NGX_AGAIN); assert(b.pos == saved); assert(c.pos < c.last);
    assert(ctx.stream->head->next->buf == &c);
    rc = ngx_http_ao_stream_body(&r, &ctx, &cin); /* dedup queued ngx_buf_t */
    assert(rc == NGX_AGAIN); assert(ctx.stream->head->next->next == NULL);
    blocked = 0;
    do { rc = ngx_http_ao_stream_body(&r, &ctx, NULL); } while (rc == NGX_AGAIN);
    assert(rc == NGX_OK); assert(b.pos == b.last); assert(c.pos == c.last);
    assert(occurrences(captured, "\"text\":\"B\"") == 1);
    assert(occurrences(captured, "\"text\":\"B2\"") == 1);
    assert(occurrences(captured, "\"text\":\"C\"") == 1);
    assert(last_count == 1); assert(ctx.response_done); assert(null_calls >= 3);
    printf("ok retained B + new C + mutated upstream chain + dedup resume only NULL\n");
    teardown(&ctx);

    setup(&r, &ctx); set_input_buf(&b, end); in.buf = &b; in.next = NULL;
    hold_ok = 1;
    rc = ngx_http_ao_stream_body(&r, &ctx, &in);
    assert(rc == NGX_AGAIN); assert(nheld == 2);
    assert(held[0] != held[1]);
    assert(held[0]->start == ctx.stream->slots[0].data);
    assert(held[1]->start == ctx.stream->slots[1].data);
    assert(memcmp(held[0]->pos, "event: message_start", 20) == 0);
    assert(memcmp(held[1]->pos, "event: ping", 11) == 0);
    blocked = 0; hold_ok = 0;
    rc = ngx_http_ao_stream_body(&r, &ctx, NULL);
    assert(rc == NGX_OK); assert(last_count == 1);
    printf("ok two held fixed slots stop production and preserve referenced bytes\n");
    teardown(&ctx);

    setup(&r, &ctx); set_input_buf(&b, end); in.buf = &b; in.next = NULL;
    blocked = 0; block_terminal = 1;
    rc = ngx_http_ao_stream_body(&r, &ctx, &in);
    assert(rc == NGX_AGAIN); assert(!ctx.response_done); assert(last_count == 0);
    assert(nheld == 1); assert(held[0]->last_buf);
    block_terminal = 0; blocked = 0;
    rc = ngx_http_ao_stream_body(&r, &ctx, NULL);
    assert(rc == NGX_OK); assert(ctx.response_done); assert(last_count == 1);
    assert(occurrences(captured, "event: message_stop") == 1);
    printf("ok terminal message_stop slot remains held until downstream consumption\n");
    teardown(&ctx);

    setup(&r, &ctx); blocked = 0;
    for (i = 0; i < 2000; i++) {
        set_input_buf(&b, delta); in.buf = &b; in.next = NULL;
        rc = ngx_http_ao_stream_body(&r, &ctx, &in);
        assert(rc == NGX_OK); assert(b.pos == b.last);
        if (i == 0) { before = allocations; }
        else { assert(allocations == before); }
        assert(ctx.stream->head == NULL); /* popped before upstream reuse */
        assert(!ctx.response_done);
        rc = ngx_http_ao_stream_body(&r, &ctx, NULL);
        assert(rc == NGX_OK); assert(!ctx.response_done);
    }
    set_input_buf(&b, end); rc = ngx_http_ao_stream_body(&r, &ctx, &in);
    assert(rc == NGX_OK); assert(last_count == 1);
    assert(occurrences(captured, "\"text\":\"z\"") == 2000);
    assert(allocations == before);
    printf("ok 2000 frames reuse input nodes and output data with zero pool allocation growth\n");
    teardown(&ctx);

    /* A last slot held despite next==OK must also delay request completion. */
    setup(&r, &ctx); set_input_buf(&b, end); in.buf = &b; in.next = NULL;
    blocked = 0; block_terminal = 1; hold_ok = 1;
    rc = ngx_http_ao_stream_body(&r, &ctx, &in);
    assert(rc == NGX_AGAIN); assert(!ctx.response_done); assert(last_count == 0);
    block_terminal = 0; blocked = 0;
    rc = ngx_http_ao_stream_body(&r, &ctx, NULL);
    assert(rc == NGX_OK); assert(ctx.response_done); assert(last_count == 1);
    printf("ok terminal slot held with downstream OK still delays completion\n");
    teardown(&ctx);

    /* Force an AGAIN at every emitted event, including escaped argument slices. */
    arglen = 150000 + 10;
    args = malloc(arglen + 1); memcpy(args, "{\"v\":\"", 6);
    for (i = 6; i < 150006; i += 2) { args[i] = '\\'; args[i + 1] = '\\'; }
    memcpy(args + 150006, "\"}", 3); arglen = 150008;
    root = cJSON_CreateObject(); choices = cJSON_AddArrayToObject(root, "choices");
    choice = cJSON_CreateObject(); cJSON_AddItemToArray(choices, choice);
    message = cJSON_AddObjectToObject(choice, "delta");
    tools = cJSON_AddArrayToObject(message, "tool_calls"); tool = cJSON_CreateObject();
    cJSON_AddItemToArray(tools, tool); cJSON_AddStringToObject(tool, "id", "forced");
    fn = cJSON_AddObjectToObject(tool, "function"); cJSON_AddStringToObject(fn, "name", "Forced");
    cJSON_AddStringToObject(fn, "arguments", args);
    cJSON_AddStringToObject(choice, "finish_reason", "tool_calls");
    json = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    wirelen = strlen(json) + 24; toolwire = malloc(wirelen);
    sprintf(toolwire, "data: %s\n\ndata: [DONE]\n\n", json); cJSON_free(json);
    setup(&r, &ctx); set_input_buf(&b, toolwire); in.buf = &b; in.next = NULL;
    block_each = 1;
    rc = ngx_http_ao_stream_body(&r, &ctx, &in);
    while (rc == NGX_AGAIN) { blocked = 0; rc = ngx_http_ao_stream_body(&r, &ctx, NULL); }
    assert(rc == NGX_OK); assert(ctx.response_done); assert(last_count == 1);
    reconstructed = malloc(arglen + 1); reconstructed_len = 0; slices = 0;
    cursor = captured;
    while ((boundary = strstr(cursor, "\n\n")) != NULL) {
        assert((size_t) (boundary + 2 - cursor) <= NGX_HTTP_AO_SLOT_SIZE);
        cursor = strstr(cursor, "\ndata: ") + 7;
        obj = ngx_http_ao_json_parse((u_char *) cursor, boundary - cursor); assert(obj);
        d = cJSON_GetObjectItemCaseSensitive(obj, "delta");
        fn = cJSON_GetObjectItemCaseSensitive(d, "partial_json");
        if (cJSON_IsString(fn)) {
            wirelen = strlen(fn->valuestring); assert(reconstructed_len + wirelen <= arglen);
            memcpy(reconstructed + reconstructed_len, fn->valuestring, wirelen);
            reconstructed_len += wirelen; slices++;
        }
        cJSON_Delete(obj); cursor = boundary + 2;
    }
    assert(slices >= 2); assert(reconstructed_len == arglen);
    assert(memcmp(args, reconstructed, arglen) == 0);
    free(reconstructed); free(args); free(toolwire);
    printf("ok forced AGAIN at every tool slice preserves exact escaped argument bytes\n");
    teardown(&ctx);


    setup(&r, &ctx); block_each = 1;
    for (i = 0; i < 33; i++) {
        set_input_buf(&overflow[i], delta); overflow_chain[i].buf = &overflow[i];
        overflow_chain[i].next = i == 32 ? NULL : &overflow_chain[i + 1];
    }
    rc = ngx_http_ao_stream_body(&r, &ctx, overflow_chain);
    assert(rc == NGX_AGAIN);
    if (ctx.stream->head != NULL) {
        fprintf(stderr, "FAIL overflow advanced buffers but retained recyclable input pointers\n");
        return 1;
    }
    for (i = 0; i < 33; i++) { overflow[i].pos = recycled; overflow[i].last = recycled + 4; }
    do {
        assert(!ctx.response_done); blocked = 0;
        rc = ngx_http_ao_stream_body(&r, &ctx, NULL);
    } while (rc == NGX_AGAIN);
    assert(rc == NGX_OK); assert(ctx.response_done);
    for (i = 0; i < 33; i++) { assert(overflow[i].pos == recycled); }
    assert(occurrences(captured, "event: error") == 1);
    printf("ok queue overflow releases all borrowed references before upstream recycling\n");
    teardown(&ctx);

    /* Ordinary flush is not EOF; disconnected upstream finalizer flush is. */
    setup(&r, &ctx); blocked = 0; memset(&upstream, 0, sizeof(upstream));
    cleanup = ngx_http_ao_cleanup_sse; upstream.cleanup = &cleanup;
    r.upstream = &upstream;
    set_input_buf(&b, delta); in.buf = &b; in.next = NULL;
    upstream.peer.connection = (ngx_connection_t *) &r;
    rc = ngx_http_ao_stream_body(&r, &ctx, &in); assert(rc == NGX_OK);
    memset(&b, 0, sizeof(b)); b.flush = 1;
    rc = ngx_http_ao_stream_body(&r, &ctx, &in); assert(rc == NGX_OK);
    assert(!ctx.response_done);
    upstream.peer.connection = NULL;
    rc = ngx_http_ao_stream_body(&r, &ctx, &in);
    if (!ctx.response_done) {
        fprintf(stderr, "FAIL disconnected upstream finalizer FLUSH did not terminate stream\n");
        return 1;
    }
    assert(rc == NGX_OK); assert(strstr(captured, "event: error"));
    assert(!strstr(captured, "event: message_delta"));
    printf("ok ordinary FLUSH ignored, disconnected finalizer FLUSH emits error+stop\n");
    teardown(&ctx);
    return 0;
}

