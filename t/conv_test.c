/*
 * Standalone conversion tests. No nginx. Compile:
 *   gcc -std=c89 -Wall -I src -I deps/cJSON \
 *       t/conv_test.c src/ngx_http_anthropic_openai_json.c deps/cJSON/cJSON.c -o t/conv_test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ngx_http_anthropic_openai_json.h"

static int g_fail;

static void
hex_decode(const char *hex, unsigned char *out, size_t *n)
{
    size_t  i, len;

    len = strlen(hex);
    *n = len / 2;
    for (i = 0; i < *n; i++) {
        unsigned int  b;
        sscanf(hex + i * 2, "%2x", &b);
        out[i] = (unsigned char) b;
    }
}

static void
expect_parse(const char *name, const char *hex, int want_ok, int want_object)
{
    unsigned char  buf[256];
    size_t         n;
    cJSON         *root;
    int            ok, isobj;

    hex_decode(hex, buf, &n);
    root = ngx_http_ao_json_parse(buf, n);
    ok = (root != NULL);
    isobj = (root != NULL && cJSON_IsObject(root));
    if (ok != want_ok || (want_ok && isobj != want_object)) {
        fprintf(stderr, "FAIL %s hex=%s ok=%d want_ok=%d isobj=%d want_obj=%d\n",
                name, hex, ok, want_ok, isobj, want_object);
        g_fail++;
    } else {
        printf("ok %s\n", name);
    }
    if (root) {
        cJSON_Delete(root);
    }
}

static void
expect_parse_str(const char *name, const char *s, int want_ok)
{
    cJSON  *root;
    int     ok;

    root = ngx_http_ao_json_parse((unsigned char *) s, strlen(s));
    ok = (root != NULL);
    if (ok != want_ok) {
        fprintf(stderr, "FAIL %s str ok=%d want=%d\n", name, ok, want_ok);
        g_fail++;
    } else {
        printf("ok %s\n", name);
    }
    if (root) {
        cJSON_Delete(root);
    }
}

int
main(void)
{
    /* r3: trailing junk / NUL after object */
    expect_parse("empty_obj", "7b7d", 1, 1);
    expect_parse("obj_nl", "7b7d0a", 1, 1);
    expect_parse("obj_junk", "7b7d6a756e6b", 0, 0);
    expect_parse("obj_obj", "7b7d7b7d", 0, 0);
    expect_parse("obj_nul_x", "7b7d0078", 0, 0);
    expect_parse("nul_obj", "007b7d", 0, 0);
    expect_parse("brace_nul_brace", "7b007d", 0, 0);
    expect_parse("brace_vt_brace", "7b0b7d", 0, 0);

    /* r4: raw C0 inside string */
    expect_parse("str_raw_lf", "7b2278223a22610a62227d", 0, 0);
    expect_parse("str_raw_tab", "7b2278223a22610962227d", 0, 0);
    expect_parse("str_raw_cr", "7b2278223a22610d62227d", 0, 0);
    expect_parse("str_esc_n", "7b2278223a22615c6e62227d", 1, 1);
    expect_parse("ws_outside", "207b2278223a20317d0a", 1, 1);

    expect_parse_str("esc_quote", "{\"a\":\"\\\"\"}", 1);
    expect_parse_str("esc_backslash", "{\"a\":\"\\\\\"}", 1);
    expect_parse_str("unclosed", "{\"a\":\"", 0);
    expect_parse_str("dangling_esc", "{\"a\":\"\\", 0);

    /* request conversion — must fail to link until req.c exists */
    {
        ngx_http_ao_req_opt_t  opt;
        char                  *out;
        size_t                 out_n;
        const char            *in =
            "{\"model\":\"claude-x\",\"max_tokens\":16,"
            "\"system\":\"be brief\","
            "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";

        memset(&opt, 0, sizeof(opt));
        opt.stream_usage = 1;
        opt.model_override = "qwen-coder";
        out = ngx_http_ao_convert_request((unsigned char *) in, strlen(in),
                                          &opt, &out_n);
        if (out == NULL) {
            fprintf(stderr, "FAIL req_basic: %s\n",
                    opt.err ? opt.err : "null");
            g_fail++;
        } else {
            if (strstr(out, "\"role\":\"system\"") == NULL
                || strstr(out, "be brief") == NULL
                || strstr(out, "qwen-coder") == NULL
                || strstr(out, "claude-x") != NULL)
            {
                fprintf(stderr, "FAIL req_basic body: %s\n", out);
                g_fail++;
            } else if (strcmp(opt.orig_model, "claude-x") != 0) {
                fprintf(stderr, "FAIL orig_model %s\n", opt.orig_model);
                g_fail++;
            } else {
                printf("ok req_basic\n");
            }
            free(out);
        }
    }

    {
        /* Preserve a text system-role message at its conversation position. */
        ngx_http_ao_req_opt_t  opt;
        char                  *out;
        size_t                 out_n;
        const char            *in =
            "{\"model\":\"claude-x\",\"max_tokens\":16,"
            "\"messages\":["
            "{\"role\":\"user\",\"content\":\"hi\"},"
            "{\"role\":\"system\",\"content\":\"session summary\"}"
            "]}";

        memset(&opt, 0, sizeof(opt));
        out = ngx_http_ao_convert_request((unsigned char *) in, strlen(in),
                                          &opt, &out_n);
        if (out == NULL) {
            fprintf(stderr, "FAIL req_system_role: %s\n",
                    opt.err ? opt.err : "null");
            g_fail++;
        } else {
            /* Preserve both the injected system message and original user. */
            if (strstr(out, "\"role\":\"system\"") == NULL
                || strstr(out, "session summary") == NULL
                || strstr(out, "\"role\":\"user\"") == NULL)
            {
                fprintf(stderr, "FAIL req_system_role body: %s\n", out);
                g_fail++;
            } else {
                printf("ok req_system_role\n");
            }
            free(out);
        }
    }

    {
        /* Unknown roles must fail without sending a partial conversation. */
        ngx_http_ao_req_opt_t  opt;
        char                  *out;
        size_t                 out_n;
        const char            *in =
            "{\"model\":\"claude-x\",\"max_tokens\":16,"
            "\"messages\":["
            "{\"role\":\"user\",\"content\":\"hi\"},"
            "{\"role\":\"robot\",\"content\":\"beep\"}"
            "]}";

        memset(&opt, 0, sizeof(opt));
        out = ngx_http_ao_convert_request((unsigned char *) in, strlen(in),
                                          &opt, &out_n);
        if (out != NULL || opt.err == NULL
            || strcmp(opt.err, "invalid role") != 0)
        {
            fprintf(stderr, "FAIL req_unknown_role must reject\n");
            g_fail++;
        } else {
            printf("ok req_unknown_role\n");
        }
        free(out);
    }

    {
        ngx_http_ao_req_opt_t  opt;
        char                  *out;
        const char            *in =
            "{\"model\":\"claude-x\","
            "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";

        memset(&opt, 0, sizeof(opt));
        out = ngx_http_ao_convert_request((unsigned char *) in, strlen(in),
                                          &opt, NULL);
        if (out != NULL) {
            fprintf(stderr, "FAIL req_no_max_tokens should error\n");
            free(out);
            g_fail++;
        } else {
            printf("ok req_no_max_tokens\n");
        }
    }

    {
        ngx_http_ao_req_opt_t  opt;
        char                  *out;
        const char            *in =
            "{\"model\":\"claude-x\",\"max_tokens\":8,\"stream\":true,"
            "\"thinking\":{\"type\":\"enabled\",\"budget_tokens\":1024},"
            "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";

        memset(&opt, 0, sizeof(opt));
        opt.stream_usage = 1;
        out = ngx_http_ao_convert_request((unsigned char *) in, strlen(in),
                                          &opt, NULL);
        if (out == NULL || opt.client_stream != 1
            || strstr(out, "include_usage") == NULL
            || strstr(out, "thinking") != NULL)
        {
            fprintf(stderr, "FAIL req_stream_p3: %s\n",
                    out ? out : (opt.err ? opt.err : "null"));
            g_fail++;
        } else {
            printf("ok req_stream_p3\n");
        }
        if (out) {
            free(out);
        }
    }

    {
        ngx_http_ao_resp_opt_t  ropt;
        char                   *out;
        const char             *in =
            "{\"id\":\"chatcmpl-1\",\"model\":\"qwen\","
            "\"choices\":[{\"index\":0,\"message\":"
            "{\"role\":\"assistant\",\"content\":\"Hello\"},"
            "\"finish_reason\":\"stop\"}],"
            "\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":1}}";

        memset(&ropt, 0, sizeof(ropt));
        ropt.echo_model = "claude-x";
        out = ngx_http_ao_convert_response((unsigned char *) in, strlen(in),
                                           &ropt, NULL);
        if (out == NULL
            || strstr(out, "\"type\":\"message\"") == NULL
            || strstr(out, "Hello") == NULL
            || strstr(out, "claude-x") == NULL
            || strstr(out, "end_turn") == NULL
            || strstr(out, "reasoning_content") != NULL)
        {
            fprintf(stderr, "FAIL resp_text: %s\n",
                    out ? out : (ropt.err ? ropt.err : "null"));
            g_fail++;
        } else {
            printf("ok resp_text\n");
        }
        if (out) {
            free(out);
        }
    }

    {
        ngx_http_ao_resp_opt_t  ropt;
        char                   *out;
        const char             *in =
            "{\"id\":\"x\",\"choices\":[]}";

        memset(&ropt, 0, sizeof(ropt));
        out = ngx_http_ao_convert_response((unsigned char *) in, strlen(in),
                                           &ropt, NULL);
        if (out != NULL) {
            fprintf(stderr, "FAIL resp_no_choices\n");
            free(out);
            g_fail++;
        } else {
            printf("ok resp_no_choices\n");
        }
    }

    {
        ngx_http_ao_resp_opt_t  ropt;
        char                   *out;
        const char             *in =
            "{\"choices\":[{\"message\":{\"tool_calls\":[{"
            "\"id\":\"call_1\",\"type\":\"function\","
            "\"function\":{\"name\":\"Bash\",\"arguments\":\"{}junk\"}}]}}]}";

        memset(&ropt, 0, sizeof(ropt));
        out = ngx_http_ao_convert_response((unsigned char *) in, strlen(in),
                                           &ropt, NULL);
        if (out != NULL) {
            fprintf(stderr, "FAIL resp_bad_args should reject\n");
            free(out);
            g_fail++;
        } else {
            printf("ok resp_bad_args\n");
        }
    }

    {
        unsigned int  tok;
        tok = ngx_http_ao_count_tokens_heuristic((unsigned char *) "abcd", 4);
        if (tok != 1) {
            fprintf(stderr, "FAIL count_tokens got %u\n", tok);
            g_fail++;
        } else {
            printf("ok count_tokens\n");
        }
    }

    {
        ngx_http_ao_req_opt_t  opt;
        char                  *out;
        const char            *in =
            "{\"model\":\"claude-x\",\"max_tokens\":16,"
            "\"tools\":[{\"name\":\"Bash\",\"type\":\"custom\","
            "\"input_schema\":{\"type\":\"object\"}}],"
            "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";

        memset(&opt, 0, sizeof(opt));
        out = ngx_http_ao_convert_request((unsigned char *) in, strlen(in),
                                          &opt, NULL);
        if (out == NULL || strstr(out, "\"type\":\"function\"") == NULL) {
            fprintf(stderr, "FAIL req_custom_tool: %s\n",
                    out ? out : (opt.err ? opt.err : "null"));
            g_fail++;
        } else {
            printf("ok req_custom_tool\n");
        }
        if (out) {
            free(out);
        }
    }

    {
        const char *in =
            "data: {\"id\":\"chatcmpl-1\",\"choices\":[{\"index\":0,"
            "\"delta\":{\"role\":\"assistant\",\"content\":\"Hello\"},"
            "\"finish_reason\":null}]}\n\n"
            "data: {\"id\":\"chatcmpl-1\",\"choices\":[{\"index\":0,"
            "\"delta\":{\"content\":\" world\"},\"finish_reason\":null}]}\n\n"
            "data: {\"id\":\"chatcmpl-1\",\"choices\":[{\"index\":0,"
            "\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
            "data: {\"id\":\"chatcmpl-1\",\"choices\":[],"
            "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":2}}\n\n"
            "data: [DONE]\n\n";
        char   *out;
        size_t  n;

        out = ngx_http_ao_convert_sse((unsigned char *) in, strlen(in),
                                      "claude-x", &n);
        if (out == NULL
            || strstr(out, "event: message_start") == NULL
            || strstr(out, "claude-x") == NULL
            || strstr(out, "text_delta") == NULL
            || strstr(out, "Hello") == NULL
            || strstr(out, " world") == NULL
            || strstr(out, "end_turn") == NULL
            || strstr(out, "event: message_stop") == NULL
            || strstr(out, "[DONE]") != NULL)
        {
            fprintf(stderr, "FAIL sse_text: %s\n", out ? out : "null");
            g_fail++;
        } else {
            printf("ok sse_text\n");
        }
        if (out) {
            free(out);
        }
    }

    {
        ngx_http_ao_sse_t *s;
        char              *out;
        size_t             n;
        const char        *a =
            "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n";
        const char        *b =
            "\ndata: {\"choices\":[{\"delta\":{},"
            "\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";

        s = ngx_http_ao_sse_create("claude-x");
        if (s == NULL) {
            fprintf(stderr, "FAIL sse_split create\n");
            g_fail++;
        } else {
            ngx_http_ao_sse_feed(s, (unsigned char *) a, strlen(a));
            ngx_http_ao_sse_feed(s, (unsigned char *) b, strlen(b));
            ngx_http_ao_sse_end(s, 0);
            out = ngx_http_ao_sse_copy_out(s, &n);
            if (out == NULL || strstr(out, "Hello") == NULL
                || strstr(out, "message_stop") == NULL)
            {
                fprintf(stderr, "FAIL sse_split: %s\n", out ? out : "null");
                g_fail++;
            } else {
                printf("ok sse_split\n");
            }
            if (out) {
                free(out);
            }
            ngx_http_ao_sse_destroy(s);
        }
    }

    {
        ngx_http_ao_sse_t *s;
        char              *out;
        size_t             n;
        const char        *a =
            "data: {\"choices\":[{\"delta\":{\"content\":\"Hel";
        const char        *b =
            "lo\"},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";

        s = ngx_http_ao_sse_create("claude-x");
        if (s == NULL) {
            fprintf(stderr, "FAIL sse_midline create\n");
            g_fail++;
        } else {
            ngx_http_ao_sse_feed(s, (unsigned char *) a, strlen(a));
            ngx_http_ao_sse_feed(s, (unsigned char *) b, strlen(b));
            ngx_http_ao_sse_end(s, 0);
            out = ngx_http_ao_sse_copy_out(s, &n);
            if (out == NULL || strstr(out, "Hello") == NULL
                || strstr(out, "message_stop") == NULL)
            {
                fprintf(stderr, "FAIL sse_midline: %s\n", out ? out : "null");
                g_fail++;
            } else {
                printf("ok sse_midline\n");
            }
            if (out) {
                free(out);
            }
            ngx_http_ao_sse_destroy(s);
        }
    }

    {
        char   *out;
        size_t  n;
        const char *in =
            "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"},"
            "\"finish_reason\":\"stop\"}]}\r\n\r\n"
            "data: [DONE]\r\n\r\n";

        out = ngx_http_ao_convert_sse((unsigned char *) in, strlen(in),
                                      "claude-x", &n);
        if (out == NULL || strstr(out, "Hi") == NULL
            || strstr(out, "message_stop") == NULL)
        {
            fprintf(stderr, "FAIL sse_crlf: %s\n", out ? out : "null");
            g_fail++;
        } else {
            printf("ok sse_crlf\n");
        }
        if (out) {
            free(out);
        }
    }

    {
        ngx_http_ao_sse_t *s;
        char              *out;
        size_t             n;
        const char        *in =
            "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}\n";

        s = ngx_http_ao_sse_create("claude-x");
        ngx_http_ao_sse_feed(s, (unsigned char *) in, strlen(in));
        ngx_http_ao_sse_end(s, 0);
        out = ngx_http_ao_sse_copy_out(s, &n);
        if (out != NULL && strstr(out, "event: error") == NULL) {
            fprintf(stderr, "FAIL sse_incomplete should error: %s\n", out);
            g_fail++;
        } else {
            printf("ok sse_incomplete\n");
        }
        if (out) {
            free(out);
        }
        ngx_http_ao_sse_destroy(s);
    }

    {
        char   *out;
        size_t  n;
        const char *in =
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{"
            "\"index\":0,\"id\":\"call_1\",\"type\":\"function\","
            "\"function\":{\"name\":\"Bash\","
            "\"arguments\":\"{\\\"cmd\\\":\\\"ls\\\"}\"}}]},"
            "\"finish_reason\":null}]}\n\n"
            "data: {\"choices\":[{\"delta\":{},"
            "\"finish_reason\":\"tool_calls\"}]}\n\n"
            "data: {\"choices\":[],\"usage\":{\"completion_tokens\":12}}\n\n"
            "data: [DONE]\n\n";

        out = ngx_http_ao_convert_sse((unsigned char *) in, strlen(in),
                                      "claude-x", &n);
        if (out == NULL
            || strstr(out, "tool_use") == NULL
            || strstr(out, "call_1") == NULL
            || strstr(out, "Bash") == NULL
            || strstr(out, "{\\\"cmd\\\":\\\"ls\\\"}") == NULL
            || strstr(out, "tool_use") == NULL
            || strstr(out, "message_stop") == NULL)
        {
            fprintf(stderr, "FAIL sse_tools: %s\n", out ? out : "null");
            g_fail++;
        } else {
            printf("ok sse_tools\n");
        }
        if (out) {
            free(out);
        }
    }

    {
        char   *out;
        size_t  n;
        const char *in =
            "data: {\"id\":\"chatcmpl-j\",\"choices\":[{"
            "\"message\":{\"role\":\"assistant\",\"content\":\"Hi\"},"
            "\"finish_reason\":\"stop\"}]}\n\n"
            "data: [DONE]\n\n";

        out = ngx_http_ao_convert_sse((unsigned char *) in, strlen(in),
                                      "claude-x", &n);
        if (out == NULL || strstr(out, "Hi") == NULL
            || strstr(out, "message_stop") == NULL
            || strstr(out, "end_turn") == NULL)
        {
            fprintf(stderr, "FAIL sse_json_message: %s\n",
                    out ? out : "null");
            g_fail++;
        } else {
            printf("ok sse_json_message\n");
        }
        if (out) {
            free(out);
        }
    }

    if (g_fail) {
        fprintf(stderr, "%d failed\n", g_fail);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
