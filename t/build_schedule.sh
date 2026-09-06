#!/bin/sh
set -eu
NGX_SRC=${NGX_SRC:-.build-src/nginx-1.26.3}
${CC:-cc} ${SCHEDULE_CFLAGS:--std=c89 -pedantic -Wall -Wno-unused-parameter -g} -Dinline=__inline__ -Wno-variadic-macros -ffunction-sections -fdata-sections \
 -I "$NGX_SRC/src/core" -I "$NGX_SRC/src/event" -I "$NGX_SRC/src/event/modules" \
 -I "$NGX_SRC/src/event/quic" -I "$NGX_SRC/src/os/unix" -I "$NGX_SRC/src/http" \
 -I "$NGX_SRC/src/http/modules" -I build -I src -I deps/cJSON \
 t/filter_schedule.c src/ngx_http_anthropic_openai_json.c src/ngx_http_anthropic_openai_req.c \
 src/ngx_http_anthropic_openai_sse.c deps/cJSON/cJSON.c -Wl,--gc-sections -o "${SCHEDULE_OUTPUT:-t/filter_schedule}"
