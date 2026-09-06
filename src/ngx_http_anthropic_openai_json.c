#include "ngx_http_anthropic_openai_json.h"

#include <string.h>

static int
ngx_http_ao_json_bytes_ok(unsigned char *p, size_t n)
{
    size_t         i;
    unsigned       in_string, escaped;
    unsigned char  c;

    in_string = 0;
    escaped = 0;

    for (i = 0; i < n; i++) {
        c = p[i];

        if (!in_string) {
            if (c == '"') {
                in_string = 1;
            } else if (c == '-' || (c >= '0' && c <= '9')) {
                /* cJSON accepts 01 and 1.; enforce JSON number grammar. */
                if (c == '-') {
                    i++;
                    if (i == n) {
                        return 0;
                    }
                }
                if (p[i] == '0') {
                    i++;
                } else {
                    if (p[i] < '1' || p[i] > '9') {
                        return 0;
                    }
                    while (i < n && p[i] >= '0' && p[i] <= '9') {
                        i++;
                    }
                }
                if (i < n && p[i] == '.') {
                    i++;
                    if (i == n || p[i] < '0' || p[i] > '9') {
                        return 0;
                    }
                    while (i < n && p[i] >= '0' && p[i] <= '9') {
                        i++;
                    }
                }
                if (i < n && (p[i] == 'e' || p[i] == 'E')) {
                    i++;
                    if (i < n && (p[i] == '+' || p[i] == '-')) {
                        i++;
                    }
                    if (i == n || p[i] < '0' || p[i] > '9') {
                        return 0;
                    }
                    while (i < n && p[i] >= '0' && p[i] <= '9') {
                        i++;
                    }
                }
                if (i < n && p[i] != ',' && p[i] != ']' && p[i] != '}'
                    && p[i] != ' ' && p[i] != '\t'
                    && p[i] != '\r' && p[i] != '\n')
                {
                    return 0;
                }
                i--;
            } else if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') {
                return 0;
            }
            continue;
        }

        if (c < 0x20) {
            return 0;
        }

        if (escaped) {
            /* cJSON stores C strings without their decoded lengths.  Reject
             * a decoded NUL before it can hide any following bytes.  A
             * literal escaped backslash followed by u0000 is unaffected. */
            if (c == 'u' && i + 4 < n
                && p[i + 1] == '0' && p[i + 2] == '0'
                && p[i + 3] == '0' && p[i + 4] == '0')
            {
                return 0;
            }
            escaped = 0;
            continue;
        }

        if (c == '\\') {
            escaped = 1;
            continue;
        }

        if (c == '"') {
            in_string = 0;
            continue;
        }
    }

    if (in_string || escaped) {
        return 0;
    }

    return 1;
}


cJSON *
ngx_http_ao_json_parse(unsigned char *p, size_t n)
{
    cJSON       *root;
    const char  *end;
    const char  *limit;
    char         c;

    if (p == NULL || !ngx_http_ao_json_bytes_ok(p, n)) {
        return NULL;
    }

    end = NULL;
    root = cJSON_ParseWithLengthOpts((char *) p, n, &end, 0);
    if (root == NULL || end == NULL) {
        if (root != NULL) {
            cJSON_Delete(root);
        }
        return NULL;
    }

    limit = (char *) p + n;
    while (end < limit) {
        c = *end;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            cJSON_Delete(root);
            return NULL;
        }
        end++;
    }

    return root;
}
