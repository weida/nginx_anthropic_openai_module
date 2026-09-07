#include "ngx_http_anthropic_openai_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *
ngx_http_ao_fail_req(ngx_http_ao_req_opt_t *opt, const char *msg)
{
    if (opt != NULL) {
        opt->err = msg;
        opt->err_status = 400;
    }
    return NULL;
}


static cJSON *
ngx_http_ao_join_text(cJSON *content, int *has_image, int *bad_block)
{
    cJSON       *out;
    cJSON       *el;
    cJSON       *t;
    cJSON       *txt;
    size_t       cap, len, n, need, ntext;
    char        *buf;
    const char  *s;
    const char  *typ;

    *has_image = 0;
    *bad_block = 0;

    if (content == NULL) {
        return cJSON_CreateString("");
    }

    if (cJSON_IsString(content)) {
        s = content->valuestring != NULL ? content->valuestring : "";
        return cJSON_CreateString(s);
    }

    if (!cJSON_IsArray(content)) {
        *bad_block = 1;
        return NULL;
    }

    cap = 256;
    len = 0;
    ntext = 0;
    buf = (char *) malloc(cap);
    if (buf == NULL) {
        return NULL;
    }
    buf[0] = '\0';

    cJSON_ArrayForEach(el, content) {
        if (!cJSON_IsObject(el)) {
            continue;
        }
        t = cJSON_GetObjectItemCaseSensitive(el, "type");
        typ = (t != NULL && cJSON_IsString(t)) ? t->valuestring : "text";
        if (typ != NULL && strcmp(typ, "image") == 0) {
            *has_image = 1;
            continue;
        }
        if (typ != NULL && (strcmp(typ, "document") == 0
                            || strcmp(typ, "file") == 0
                            || strcmp(typ, "thinking") == 0))
        {
            if (strcmp(typ, "thinking") == 0) {
                continue;
            }
            *bad_block = 1;
            free(buf);
            return NULL;
        }
        if (typ != NULL && strcmp(typ, "text") != 0
            && strcmp(typ, "tool_use") != 0
            && strcmp(typ, "tool_result") != 0)
        {
            continue;
        }
        if (typ != NULL && strcmp(typ, "text") != 0) {
            continue;
        }
        txt = cJSON_GetObjectItemCaseSensitive(el, "text");
        if (txt == NULL || !cJSON_IsString(txt) || txt->valuestring == NULL) {
            continue;
        }
        n = strlen(txt->valuestring);
        need = len + n + 2;
        if (need > cap) {
            char  *nb;
            while (cap < need) {
                cap *= 2;
            }
            nb = (char *) realloc(buf, cap);
            if (nb == NULL) {
                free(buf);
                return NULL;
            }
            buf = nb;
        }
        if (ntext++ > 0) {
            buf[len++] = '\n';
        }
        memcpy(buf + len, txt->valuestring, n);
        len += n;
        buf[len] = '\0';
    }

    out = cJSON_CreateString(buf);
    free(buf);
    return out;
}


static cJSON *
ngx_http_ao_image_part(cJSON *el)
{
    cJSON       *src, *url, *b64, *mt, *part, *iu, *o;
    const char  *u;
    char        *dataurl;
    size_t       n;

    src = cJSON_GetObjectItemCaseSensitive(el, "source");
    if (src == NULL || !cJSON_IsObject(src)) {
        return NULL;
    }
    url = cJSON_GetObjectItemCaseSensitive(src, "url");
    if (url != NULL && cJSON_IsString(url) && url->valuestring != NULL) {
        part = cJSON_CreateObject();
        iu = cJSON_CreateObject();
        cJSON_AddStringToObject(iu, "url", url->valuestring);
        cJSON_AddItemToObject(part, "type", cJSON_CreateString("image_url"));
        cJSON_AddItemToObject(part, "image_url", iu);
        return part;
    }
    b64 = cJSON_GetObjectItemCaseSensitive(src, "data");
    mt = cJSON_GetObjectItemCaseSensitive(src, "media_type");
    if (b64 != NULL && cJSON_IsString(b64) && b64->valuestring != NULL) {
        u = (mt != NULL && cJSON_IsString(mt) && mt->valuestring != NULL)
            ? mt->valuestring : "image/png";
        n = strlen("data:") + strlen(u) + strlen(";base64,")
            + strlen(b64->valuestring) + 1;
        dataurl = (char *) malloc(n);
        if (dataurl == NULL) {
            return NULL;
        }
        sprintf(dataurl, "data:%s;base64,%s", u, b64->valuestring);
        part = cJSON_CreateObject();
        iu = cJSON_CreateObject();
        o = cJSON_CreateString(dataurl);
        free(dataurl);
        cJSON_AddItemToObject(iu, "url", o);
        cJSON_AddStringToObject(part, "type", "image_url");
        cJSON_AddItemToObject(part, "image_url", iu);
        return part;
    }
    return NULL;
}


/* Tool results permit only omitted content, strings, or text-block arrays. */
static int
ngx_http_ao_tool_result_content_valid(cJSON *content)
{
    cJSON  *item, *type, *text;

    if (content == NULL || cJSON_IsString(content)) {
        return 1;
    }
    if (!cJSON_IsArray(content)) {
        return 0;
    }
    cJSON_ArrayForEach(item, content) {
        type = cJSON_GetObjectItemCaseSensitive(item, "type");
        text = cJSON_GetObjectItemCaseSensitive(item, "text");
        if (!cJSON_IsObject(item) || !cJSON_IsString(type)
            || strcmp(type->valuestring, "text") != 0
            || !cJSON_IsString(text))
        {
            return 0;
        }
    }
    return 1;
}


static int
ngx_http_ao_append_user_content(cJSON *out_msgs, cJSON *content)
{
    cJSON       *el, *t, *joined, *img, *user, *textp, *tx;
    cJSON       *pending;
    int          has_image, bad, ntext;
    const char  *typ;

    pending = cJSON_CreateArray();
    if (pending == NULL) {
        return -1;
    }

    if (cJSON_IsString(content)) {
        user = cJSON_CreateObject();
        cJSON_AddStringToObject(user, "role", "user");
        cJSON_AddStringToObject(user, "content",
                                content->valuestring ? content->valuestring : "");
        cJSON_AddItemToArray(out_msgs, user);
        cJSON_Delete(pending);
        return 0;
    }

    if (!cJSON_IsArray(content)) {
        cJSON_Delete(pending);
        return -1;
    }

    cJSON_ArrayForEach(el, content) {
        if (!cJSON_IsObject(el)) {
            continue;
        }
        t = cJSON_GetObjectItemCaseSensitive(el, "type");
        typ = (t != NULL && cJSON_IsString(t)) ? t->valuestring : "text";

        if (strcmp(typ, "tool_result") == 0) {
            cJSON       *id, *c, *ie, *tool;
            char        *body;
            const char  *raw;

            /* flush pending user content first? spec: tool_result immediately,
               then leftover user at end. pending stays until end unless we
               emit tools as we go — emit tool now, keep pending. */
            id = cJSON_GetObjectItemCaseSensitive(el, "tool_use_id");
            c = cJSON_GetObjectItemCaseSensitive(el, "content");
            if (!ngx_http_ao_tool_result_content_valid(c)) {
                cJSON_Delete(pending);
                return -1;
            }
            ie = cJSON_GetObjectItemCaseSensitive(el, "is_error");
            raw = "";
            if (c != NULL && cJSON_IsString(c) && c->valuestring != NULL) {
                raw = c->valuestring;
            } else if (c != NULL && cJSON_IsArray(c)) {
                joined = ngx_http_ao_join_text(c, &has_image, &bad);
                if (bad || has_image || joined == NULL) {
                    cJSON_Delete(pending);
                    if (joined) {
                        cJSON_Delete(joined);
                    }
                    return -1;
                }
                raw = joined->valuestring ? joined->valuestring : "";
                body = (char *) malloc(strlen(raw) + 8);
                if (body == NULL) {
                    cJSON_Delete(joined);
                    cJSON_Delete(pending);
                    return -1;
                }
                if (ie != NULL && cJSON_IsTrue(ie)) {
                    sprintf(body, "ERROR: %s", raw);
                } else {
                    strcpy(body, raw);
                }
                tool = cJSON_CreateObject();
                cJSON_AddStringToObject(tool, "role", "tool");
                if (id != NULL && cJSON_IsString(id) && id->valuestring) {
                    cJSON_AddStringToObject(tool, "tool_call_id",
                                            id->valuestring);
                }
                cJSON_AddStringToObject(tool, "content", body);
                free(body);
                cJSON_AddItemToArray(out_msgs, tool);
                cJSON_Delete(joined);
                continue;
            }
            body = (char *) malloc(strlen(raw) + 8);
            if (body == NULL) {
                cJSON_Delete(pending);
                return -1;
            }
            if (ie != NULL && cJSON_IsTrue(ie)) {
                sprintf(body, "ERROR: %s", raw);
            } else {
                strcpy(body, raw);
            }
            tool = cJSON_CreateObject();
            cJSON_AddStringToObject(tool, "role", "tool");
            if (id != NULL && cJSON_IsString(id) && id->valuestring) {
                cJSON_AddStringToObject(tool, "tool_call_id", id->valuestring);
            }
            cJSON_AddStringToObject(tool, "content", body);
            free(body);
            cJSON_AddItemToArray(out_msgs, tool);
            continue;
        }

        if (strcmp(typ, "image") == 0) {
            img = ngx_http_ao_image_part(el);
            if (img != NULL) {
                cJSON_AddItemToArray(pending, img);
            }
            continue;
        }

        if (strcmp(typ, "document") == 0 || strcmp(typ, "file") == 0) {
            cJSON_Delete(pending);
            return -1;
        }

        if (strcmp(typ, "text") == 0) {
            cJSON  *tx;
            tx = cJSON_GetObjectItemCaseSensitive(el, "text");
            if (tx != NULL && cJSON_IsString(tx)) {
                textp = cJSON_CreateObject();
                cJSON_AddStringToObject(textp, "type", "text");
                cJSON_AddStringToObject(textp, "text",
                    tx->valuestring ? tx->valuestring : "");
                cJSON_AddItemToArray(pending, textp);
            }
        }
    }

    ntext = cJSON_GetArraySize(pending);
    if (ntext > 0) {
        user = cJSON_CreateObject();
        cJSON_AddStringToObject(user, "role", "user");
        if (ntext == 1) {
            el = cJSON_GetArrayItem(pending, 0);
            t = cJSON_GetObjectItemCaseSensitive(el, "type");
            if (t != NULL && cJSON_IsString(t)
                && strcmp(t->valuestring, "text") == 0)
            {
                tx = cJSON_GetObjectItemCaseSensitive(el, "text");
                cJSON_AddStringToObject(user, "content",
                    (tx && cJSON_IsString(tx) && tx->valuestring)
                    ? tx->valuestring : "");
                cJSON_AddItemToArray(out_msgs, user);
                cJSON_Delete(pending);
                return 0;
            }
        }
        /* detach pending as content array */
        cJSON_AddItemToObject(user, "content", pending);
        cJSON_AddItemToArray(out_msgs, user);
        return 0;
    }

    cJSON_Delete(pending);
    return 0;
}


static int
ngx_http_ao_append_assistant(cJSON *out_msgs, cJSON *content)
{
    cJSON       *el, *t, *asst, *calls, *call, *fn, *id, *name, *input;
    cJSON       *textj;
    int          has_image, bad;
    const char  *typ;
    char        *args;

    asst = cJSON_CreateObject();
    cJSON_AddStringToObject(asst, "role", "assistant");
    calls = NULL;
    textj = ngx_http_ao_join_text(content, &has_image, &bad);
    if (bad) {
        cJSON_Delete(asst);
        if (textj) {
            cJSON_Delete(textj);
        }
        return -1;
    }
    if (textj != NULL && textj->valuestring != NULL
        && textj->valuestring[0] != '\0')
    {
        cJSON_AddItemToObject(asst, "content", textj);
        textj = NULL;
    } else if (textj != NULL) {
        cJSON_Delete(textj);
        cJSON_AddNullToObject(asst, "content");
    }

    if (cJSON_IsArray(content)) {
        cJSON_ArrayForEach(el, content) {
            if (!cJSON_IsObject(el)) {
                continue;
            }
            t = cJSON_GetObjectItemCaseSensitive(el, "type");
            typ = (t && cJSON_IsString(t)) ? t->valuestring : "";
            if (strcmp(typ, "tool_use") != 0) {
                continue;
            }
            if (calls == NULL) {
                calls = cJSON_CreateArray();
            }
            id = cJSON_GetObjectItemCaseSensitive(el, "id");
            name = cJSON_GetObjectItemCaseSensitive(el, "name");
            input = cJSON_GetObjectItemCaseSensitive(el, "input");
            call = cJSON_CreateObject();
            if (id && cJSON_IsString(id) && id->valuestring) {
                cJSON_AddStringToObject(call, "id", id->valuestring);
            }
            cJSON_AddStringToObject(call, "type", "function");
            fn = cJSON_CreateObject();
            if (name && cJSON_IsString(name) && name->valuestring) {
                cJSON_AddStringToObject(fn, "name", name->valuestring);
            }
            if (input != NULL) {
                args = cJSON_PrintUnformatted(input);
                if (args != NULL) {
                    cJSON_AddStringToObject(fn, "arguments", args);
                    cJSON_free(args);
                } else {
                    cJSON_AddStringToObject(fn, "arguments", "{}");
                }
            } else {
                cJSON_AddStringToObject(fn, "arguments", "{}");
            }
            cJSON_AddItemToObject(call, "function", fn);
            cJSON_AddItemToArray(calls, call);
        }
    }

    if (calls != NULL) {
        cJSON_AddItemToObject(asst, "tool_calls", calls);
    }
    cJSON_AddItemToArray(out_msgs, asst);
    return 0;
}


static cJSON *
ngx_http_ao_convert_tools(cJSON *tools, ngx_http_ao_req_opt_t *opt)
{
    cJSON  *out, *el, *one, *fn, *name, *desc, *schema, *typ;
    int     n;

    if (tools == NULL || !cJSON_IsArray(tools)) {
        return NULL;
    }
    n = cJSON_GetArraySize(tools);
    if (n > (opt && opt->max_tools > 0 ? opt->max_tools : 256)) {
        ngx_http_ao_fail_req(opt, "too many tools");
        return NULL;
    }
    out = cJSON_CreateArray();
    cJSON_ArrayForEach(el, tools) {
        if (!cJSON_IsObject(el)) {
            continue;
        }
        typ = cJSON_GetObjectItemCaseSensitive(el, "type");
        if (typ != NULL && !cJSON_IsNull(typ) && cJSON_IsString(typ)
            && typ->valuestring != NULL
            && strcmp(typ->valuestring, "custom") != 0)
        {
            cJSON_Delete(out);
            ngx_http_ao_fail_req(opt, "unsupported tool type");
            return NULL;
        }
        name = cJSON_GetObjectItemCaseSensitive(el, "name");
        if (name == NULL || !cJSON_IsString(name) || name->valuestring == NULL) {
            cJSON_Delete(out);
            ngx_http_ao_fail_req(opt, "tool missing name");
            return NULL;
        }
        one = cJSON_CreateObject();
        cJSON_AddStringToObject(one, "type", "function");
        fn = cJSON_CreateObject();
        cJSON_AddStringToObject(fn, "name", name->valuestring);
        desc = cJSON_GetObjectItemCaseSensitive(el, "description");
        if (desc != NULL && cJSON_IsString(desc) && desc->valuestring) {
            cJSON_AddStringToObject(fn, "description", desc->valuestring);
        }
        schema = cJSON_GetObjectItemCaseSensitive(el, "input_schema");
        if (schema != NULL) {
            cJSON_AddItemToObject(fn, "parameters", cJSON_Duplicate(schema, 1));
        }
        cJSON_AddItemToObject(one, "function", fn);
        cJSON_AddItemToArray(out, one);
    }
    return out;
}


static cJSON *
ngx_http_ao_convert_tool_choice(cJSON *tc)
{
    cJSON       *typ, *name, *dpar, *out;
    const char  *ts;

    if (tc == NULL) {
        return NULL;
    }
    if (cJSON_IsString(tc) && tc->valuestring != NULL) {
        if (strcmp(tc->valuestring, "any") == 0) {
            return cJSON_CreateString("required");
        }
        return cJSON_CreateString(tc->valuestring);
    }
    if (!cJSON_IsObject(tc)) {
        return NULL;
    }
    typ = cJSON_GetObjectItemCaseSensitive(tc, "type");
    ts = (typ && cJSON_IsString(typ)) ? typ->valuestring : "";
    dpar = cJSON_GetObjectItemCaseSensitive(tc, "disable_parallel_tool_use");
    if (strcmp(ts, "auto") == 0) {
        out = cJSON_CreateString("auto");
    } else if (strcmp(ts, "any") == 0) {
        out = cJSON_CreateString("required");
    } else if (strcmp(ts, "none") == 0) {
        out = cJSON_CreateString("none");
    } else if (strcmp(ts, "tool") == 0) {
        name = cJSON_GetObjectItemCaseSensitive(tc, "name");
        out = cJSON_CreateObject();
        cJSON_AddStringToObject(out, "type", "function");
        {
            cJSON  *fn;
            fn = cJSON_CreateObject();
            if (name && cJSON_IsString(name) && name->valuestring) {
                cJSON_AddStringToObject(fn, "name", name->valuestring);
            }
            cJSON_AddItemToObject(out, "function", fn);
        }
    } else {
        out = cJSON_CreateString("auto");
    }
    (void) dpar;
    return out;
}


char *
ngx_http_ao_convert_request(unsigned char *p, size_t n,
    ngx_http_ao_req_opt_t *opt, size_t *out_len)
{
    cJSON   *root, *out, *msgs, *sys, *max, *stream, *md;
    cJSON   *item, *role, *content, *omsg;
    char    *printed;
    size_t   plen;

    if (opt != NULL) {
        opt->err = NULL;
        opt->err_status = 0;
        opt->client_stream = 0;
        opt->orig_model[0] = '\0';
    }

    root = ngx_http_ao_json_parse(p, n);
    if (root == NULL || !cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        return ngx_http_ao_fail_req(opt, "invalid json");
    }

    max = cJSON_GetObjectItemCaseSensitive(root, "max_tokens");
    if (max == NULL || !cJSON_IsNumber(max)) {
        cJSON_Delete(root);
        return ngx_http_ao_fail_req(opt, "max_tokens required");
    }

    out = cJSON_CreateObject();
    msgs = cJSON_CreateArray();
    cJSON_AddItemToObject(out, "messages", msgs);

    item = cJSON_GetObjectItemCaseSensitive(root, "model");
    if (item != NULL && cJSON_IsString(item) && item->valuestring != NULL
        && opt != NULL)
    {
        strncpy(opt->orig_model, item->valuestring,
                sizeof(opt->orig_model) - 1);
        opt->orig_model[sizeof(opt->orig_model) - 1] = '\0';
    }
    if (opt != NULL && opt->model_override != NULL
        && opt->model_override[0] != '\0')
    {
        cJSON_AddStringToObject(out, "model", opt->model_override);
    } else if (item != NULL && cJSON_IsString(item) && item->valuestring) {
        cJSON_AddStringToObject(out, "model", item->valuestring);
    }

    sys = cJSON_GetObjectItemCaseSensitive(root, "system");
    if (sys != NULL) {
        cJSON  *sysmsg, *joined;
        int     has_image, bad;

        if (cJSON_IsString(sys)) {
            sysmsg = cJSON_CreateObject();
            cJSON_AddStringToObject(sysmsg, "role", "system");
            cJSON_AddStringToObject(sysmsg, "content",
                sys->valuestring ? sys->valuestring : "");
            cJSON_AddItemToArray(msgs, sysmsg);
        } else if (cJSON_IsArray(sys)) {
            joined = ngx_http_ao_join_text(sys, &has_image, &bad);
            if (joined != NULL) {
                sysmsg = cJSON_CreateObject();
                cJSON_AddStringToObject(sysmsg, "role", "system");
                cJSON_AddItemToObject(sysmsg, "content", joined);
                cJSON_AddItemToArray(msgs, sysmsg);
            }
        }
    }

    omsg = cJSON_GetObjectItemCaseSensitive(root, "messages");
    if (omsg == NULL || !cJSON_IsArray(omsg)) {
        cJSON_Delete(out);
        cJSON_Delete(root);
        return ngx_http_ao_fail_req(opt, "messages required");
    }

    cJSON_ArrayForEach(item, omsg) {
        const char  *rs;

        if (!cJSON_IsObject(item)) {
            continue;
        }
        role = cJSON_GetObjectItemCaseSensitive(item, "role");
        content = cJSON_GetObjectItemCaseSensitive(item, "content");
        rs = (role && cJSON_IsString(role)) ? role->valuestring : "";
        if (strcmp(rs, "user") == 0) {
            if (ngx_http_ao_append_user_content(msgs, content) != 0) {
                cJSON_Delete(out);
                cJSON_Delete(root);
                return ngx_http_ao_fail_req(opt, "unsupported content block");
            }
        } else if (strcmp(rs, "assistant") == 0) {
            if (ngx_http_ao_append_assistant(msgs, content) != 0) {
                cJSON_Delete(out);
                cJSON_Delete(root);
                return ngx_http_ao_fail_req(opt, "unsupported content block");
            }
        } else if (strcmp(rs, "system") == 0) {
            cJSON  *sysmsg, *joined, *block, *type, *text;
            int     has_image, bad, valid;

            valid = cJSON_IsString(content) || cJSON_IsArray(content);
            if (cJSON_GetObjectItemCaseSensitive(item, "clear_at") != NULL
                || cJSON_GetObjectItemCaseSensitive(item, "output_config")
                   != NULL)
            {
                valid = 0;
            }
            if (cJSON_IsArray(content)) {
                cJSON_ArrayForEach(block, content) {
                    type = cJSON_GetObjectItemCaseSensitive(block, "type");
                    text = cJSON_GetObjectItemCaseSensitive(block, "text");
                    if (!cJSON_IsObject(block) || !cJSON_IsString(type)
                        || strcmp(type->valuestring, "text") != 0
                        || !cJSON_IsString(text))
                    {
                        valid = 0;
                        break;
                    }
                }
            }
            if (!valid) {
                cJSON_Delete(out);
                cJSON_Delete(root);
                return ngx_http_ao_fail_req(opt, "unsupported system content");
            }

            joined = ngx_http_ao_join_text(content, &has_image, &bad);
            sysmsg = cJSON_CreateObject();
            if (joined == NULL || sysmsg == NULL || has_image || bad) {
                cJSON_Delete(joined);
                cJSON_Delete(sysmsg);
                cJSON_Delete(out);
                cJSON_Delete(root);
                return ngx_http_ao_fail_req(opt, "system conversion failed");
            }
            cJSON_AddStringToObject(sysmsg, "role", "system");
            cJSON_AddItemToObject(sysmsg, "content", joined);
            cJSON_AddItemToArray(msgs, sysmsg);
        } else {
            cJSON_Delete(out);
            cJSON_Delete(root);
            return ngx_http_ao_fail_req(opt, "invalid role");
        }
    }

    cJSON_AddNumberToObject(out, "max_tokens", max->valuedouble);

    stream = cJSON_GetObjectItemCaseSensitive(root, "stream");
    if (stream != NULL && cJSON_IsTrue(stream)) {
        cJSON  *so;
        if (opt) {
            opt->client_stream = 1;
        }
        cJSON_AddBoolToObject(out, "stream", 1);
        if (opt != NULL && opt->stream_usage) {
            so = cJSON_CreateObject();
            cJSON_AddBoolToObject(so, "include_usage", 1);
            cJSON_AddItemToObject(out, "stream_options", so);
        }
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "temperature");
    if (item != NULL && cJSON_IsNumber(item)) {
        cJSON_AddNumberToObject(out, "temperature", item->valuedouble);
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "top_p");
    if (item != NULL && cJSON_IsNumber(item)) {
        cJSON_AddNumberToObject(out, "top_p", item->valuedouble);
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "stop_sequences");
    if (item != NULL) {
        cJSON_AddItemToObject(out, "stop", cJSON_Duplicate(item, 1));
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "tools");
    if (item != NULL) {
        cJSON  *ot;
        ot = ngx_http_ao_convert_tools(item, opt);
        if (ot == NULL && opt != NULL && opt->err != NULL) {
            cJSON_Delete(out);
            cJSON_Delete(root);
            return NULL;
        }
        if (ot != NULL) {
            cJSON_AddItemToObject(out, "tools", ot);
        }
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "tool_choice");
    if (item != NULL) {
        cJSON  *otc, *dpar;
        otc = ngx_http_ao_convert_tool_choice(item);
        if (otc != NULL) {
            cJSON_AddItemToObject(out, "tool_choice", otc);
        }
        dpar = cJSON_IsObject(item)
            ? cJSON_GetObjectItemCaseSensitive(item,
                                               "disable_parallel_tool_use")
            : NULL;
        if (dpar != NULL && cJSON_IsTrue(dpar)) {
            cJSON_AddBoolToObject(out, "parallel_tool_calls", 0);
        }
    }

    md = cJSON_GetObjectItemCaseSensitive(root, "metadata");
    if (md != NULL && cJSON_IsObject(md)) {
        item = cJSON_GetObjectItemCaseSensitive(md, "user_id");
        if (item != NULL && cJSON_IsString(item) && item->valuestring) {
            cJSON_AddStringToObject(out, "user", item->valuestring);
        }
    }

    /* cache_control / thinking dropped by not copying */

    printed = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    cJSON_Delete(root);
    if (printed == NULL) {
        return ngx_http_ao_fail_req(opt, "print failed");
    }
    plen = strlen(printed);
    if (out_len) {
        *out_len = plen;
    }
    return printed;
}


char *
ngx_http_ao_convert_response(unsigned char *p, size_t n,
    ngx_http_ao_resp_opt_t *opt, size_t *out_len)
{
    cJSON       *root, *out, *choices, *ch0, *msg, *content, *usage, *fr;
    cJSON       *blocks, *blk, *tc, *el, *fn, *args, *parsed;
    char        *printed;
    const char  *model, *stop;
    int          input_t, output_t;

    if (opt) {
        opt->err = NULL;
        opt->err_status = 0;
    }

    root = ngx_http_ao_json_parse(p, n);
    if (root == NULL || !cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        if (opt) {
            opt->err = "invalid json";
            opt->err_status = 502;
        }
        return NULL;
    }

    choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    if (choices == NULL || !cJSON_IsArray(choices)
        || cJSON_GetArraySize(choices) < 1)
    {
        cJSON_Delete(root);
        if (opt) {
            opt->err = "upstream returned no choices";
            opt->err_status = 502;
        }
        return NULL;
    }

    ch0 = cJSON_GetArrayItem(choices, 0);
    msg = cJSON_GetObjectItemCaseSensitive(ch0, "message");
    if (msg == NULL) {
        msg = cJSON_GetObjectItemCaseSensitive(ch0, "delta");
    }

    if (!cJSON_IsObject(msg)) {
        cJSON_Delete(root);
        if (opt) {
            opt->err = "upstream returned no message";
            opt->err_status = 502;
        }
        return NULL;
    }

    out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "type", "message");
    cJSON_AddStringToObject(out, "role", "assistant");
    {
        cJSON  *idp;
        idp = cJSON_GetObjectItemCaseSensitive(root, "id");
        if (idp && cJSON_IsString(idp) && idp->valuestring) {
            cJSON_AddStringToObject(out, "id", idp->valuestring);
        } else {
            cJSON_AddStringToObject(out, "id", "msg_ao");
        }
    }
    model = (opt && opt->echo_model && opt->echo_model[0])
        ? opt->echo_model : NULL;
    if (model == NULL) {
        cJSON  *m;
        m = cJSON_GetObjectItemCaseSensitive(root, "model");
        model = (m && cJSON_IsString(m) && m->valuestring)
            ? m->valuestring : "";
    }
    cJSON_AddStringToObject(out, "model", model);

    blocks = cJSON_CreateArray();
    content = (msg != NULL)
        ? cJSON_GetObjectItemCaseSensitive(msg, "content") : NULL;
    /* ignore reasoning_content */
    if (content != NULL && cJSON_IsString(content)
        && content->valuestring != NULL && content->valuestring[0] != '\0')
    {
        blk = cJSON_CreateObject();
        cJSON_AddStringToObject(blk, "type", "text");
        cJSON_AddStringToObject(blk, "text", content->valuestring);
        cJSON_AddItemToArray(blocks, blk);
    }

    tc = (msg != NULL)
        ? cJSON_GetObjectItemCaseSensitive(msg, "tool_calls") : NULL;
    if (tc != NULL && cJSON_IsArray(tc)) {
        cJSON_ArrayForEach(el, tc) {
            cJSON  *tid, *name;

            fn = cJSON_GetObjectItemCaseSensitive(el, "function");
            tid = cJSON_GetObjectItemCaseSensitive(el, "id");
            name = cJSON_GetObjectItemCaseSensitive(fn, "name");
            if (!cJSON_IsString(tid) || !cJSON_IsString(name)
                || tid->valuestring[0] == '\0' || name->valuestring[0] == '\0')
            {
                cJSON_Delete(blocks);
                cJSON_Delete(out);
                cJSON_Delete(root);
                if (opt) {
                    opt->err = "tool call missing id or name";
                    opt->err_status = 502;
                }
                return NULL;
            }
            blk = cJSON_CreateObject();
            cJSON_AddStringToObject(blk, "type", "tool_use");
            {
                cJSON  *tid;
                tid = cJSON_GetObjectItemCaseSensitive(el, "id");
                if (tid && cJSON_IsString(tid) && tid->valuestring) {
                    cJSON_AddStringToObject(blk, "id", tid->valuestring);
                }
            }
            if (fn != NULL) {
                cJSON  *nm;
                nm = cJSON_GetObjectItemCaseSensitive(fn, "name");
                if (nm && cJSON_IsString(nm) && nm->valuestring) {
                    cJSON_AddStringToObject(blk, "name", nm->valuestring);
                }
                args = cJSON_GetObjectItemCaseSensitive(fn, "arguments");
                if (args == NULL) {
                    cJSON_AddItemToObject(blk, "input", cJSON_CreateObject());
                } else if (cJSON_IsString(args) && args->valuestring) {
                    if (args->valuestring[0] == '\0') {
                        cJSON_AddItemToObject(blk, "input",
                                              cJSON_CreateObject());
                    } else {
                        parsed = ngx_http_ao_json_parse(
                            (unsigned char *) args->valuestring,
                            strlen(args->valuestring));
                        if (parsed == NULL || !cJSON_IsObject(parsed)) {
                            if (parsed) {
                                cJSON_Delete(parsed);
                            }
                            cJSON_Delete(blk);
                            cJSON_Delete(blocks);
                            cJSON_Delete(out);
                            cJSON_Delete(root);
                            if (opt) {
                                opt->err =
                                    "tool call arguments are not a JSON object";
                                opt->err_status = 502;
                            }
                            return NULL;
                        }
                        cJSON_AddItemToObject(blk, "input", parsed);
                    }
                } else {
                    cJSON_Delete(blk);
                    cJSON_Delete(blocks);
                    cJSON_Delete(out);
                    cJSON_Delete(root);
                    if (opt) {
                        opt->err =
                            "tool call arguments are not a JSON object";
                        opt->err_status = 502;
                    }
                    return NULL;
                }
            }
            cJSON_AddItemToArray(blocks, blk);
        }
    }

    cJSON_AddItemToObject(out, "content", blocks);

    fr = cJSON_GetObjectItemCaseSensitive(ch0, "finish_reason");
    stop = "end_turn";
    if (fr != NULL && cJSON_IsString(fr) && fr->valuestring) {
        if (strcmp(fr->valuestring, "length") == 0) {
            stop = "max_tokens";
        } else if (strcmp(fr->valuestring, "tool_calls") == 0) {
            stop = "tool_use";
        } else if (strcmp(fr->valuestring, "stop") == 0) {
            stop = "end_turn";
        }
    } else if (tc != NULL && cJSON_IsArray(tc)
               && cJSON_GetArraySize(tc) > 0)
    {
        stop = "tool_use";
    }
    cJSON_AddStringToObject(out, "stop_reason", stop);
    cJSON_AddNullToObject(out, "stop_sequence");

    usage = cJSON_GetObjectItemCaseSensitive(root, "usage");
    input_t = 0;
    output_t = 0;
    if (usage != NULL && cJSON_IsObject(usage)) {
        cJSON  *pt, *ct;
        pt = cJSON_GetObjectItemCaseSensitive(usage, "prompt_tokens");
        ct = cJSON_GetObjectItemCaseSensitive(usage, "completion_tokens");
        if (pt && cJSON_IsNumber(pt)) {
            input_t = (int) pt->valuedouble;
        }
        if (ct && cJSON_IsNumber(ct)) {
            output_t = (int) ct->valuedouble;
        }
    }
    {
        cJSON  *u;
        u = cJSON_CreateObject();
        cJSON_AddNumberToObject(u, "input_tokens", input_t);
        cJSON_AddNumberToObject(u, "output_tokens", output_t);
        cJSON_AddItemToObject(out, "usage", u);
    }

    printed = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    cJSON_Delete(root);
    if (printed == NULL) {
        if (opt) {
            opt->err = "print failed";
            opt->err_status = 502;
        }
        return NULL;
    }
    if (out_len) {
        *out_len = strlen(printed);
    }
    return printed;
}


unsigned int
ngx_http_ao_count_tokens_heuristic(unsigned char *p, size_t n)
{
    unsigned int  t;

    t = (unsigned int) (n / 4);
    if (t == 0 && n > 0) {
        t = 1;
    }
    return t;
}


/* Count only visible text, tool input JSON, and tool definitions. */
static int
ngx_http_ao_count_content(cJSON *content, size_t *bytes)
{
    cJSON       *item, *type, *value;
    char        *printed;
    const char  *name;

    if (cJSON_IsString(content)) {
        *bytes += strlen(content->valuestring);
        return 0;
    }
    if (!cJSON_IsArray(content)) {
        return 0;
    }
    cJSON_ArrayForEach(item, content) {
        type = cJSON_GetObjectItemCaseSensitive(item, "type");
        name = cJSON_IsString(type) ? type->valuestring : "";
        if (strcmp(name, "text") == 0) {
            value = cJSON_GetObjectItemCaseSensitive(item, "text");
            if (cJSON_IsString(value)) {
                *bytes += strlen(value->valuestring);
            }
        } else if (strcmp(name, "tool_use") == 0) {
            value = cJSON_GetObjectItemCaseSensitive(item, "input");
            if (value != NULL) {
                printed = cJSON_PrintUnformatted(value);
                if (printed == NULL) {
                    return -1;
                }
                *bytes += strlen(printed);
                cJSON_free(printed);
            }
        } else if (strcmp(name, "tool_result") == 0) {
            value = cJSON_GetObjectItemCaseSensitive(item, "content");
            if (ngx_http_ao_count_content(value, bytes) != 0) {
                return -1;
            }
        }
    }
    return 0;
}


int
ngx_http_ao_count_tokens_json(unsigned char *p, size_t n,
    unsigned int *tokens)
{
    cJSON   *root, *messages, *item, *tools;
    char    *printed;
    size_t   bytes;
    int      rc;

    root = ngx_http_ao_json_parse(p, n);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return -1;
    }
    messages = cJSON_GetObjectItemCaseSensitive(root, "messages");
    if (!cJSON_IsArray(messages)) {
        cJSON_Delete(root);
        return -1;
    }
    bytes = 0;
    rc = ngx_http_ao_count_content(
        cJSON_GetObjectItemCaseSensitive(root, "system"), &bytes);
    cJSON_ArrayForEach(item, messages) {
        if (ngx_http_ao_count_content(
                cJSON_GetObjectItemCaseSensitive(item, "content"), &bytes) != 0)
        {
            rc = -1;
        }
    }
    tools = cJSON_GetObjectItemCaseSensitive(root, "tools");
    if (tools != NULL) {
        printed = cJSON_PrintUnformatted(tools);
        if (printed == NULL) {
            rc = -1;
        } else {
            bytes += strlen(printed);
            cJSON_free(printed);
        }
    }
    cJSON_Delete(root);
    *tokens = ngx_http_ao_count_tokens_heuristic(NULL, bytes);
    return rc;
}
