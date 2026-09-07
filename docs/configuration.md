# Configuration

Load `ngx_http_anthropic_openai_module.so` in the main nginx context, before
`events` and `http`. The [example](../conf/example.conf) is a complete local test
configuration. Adjust its upstream address and model before sending requests.

All module directives are valid in `http`, `server`, and `location` and inherit
from their enclosing context.

| Directive | Default | Meaning |
| --- | --- | --- |
| `anthropic_openai on\|off` | `off` | Enable Messages conversion. |
| `anthropic_openai_model NAME` | empty | Override the model sent upstream; responses echo the client model. |
| `anthropic_openai_api_key KEY` | empty | Override upstream credentials with `Authorization: Bearer KEY`. |
| `anthropic_openai_count_tokens heuristic\|off` | `heuristic` | Estimate tokens locally, or return HTTP 501 for count requests. |
| `anthropic_openai_stream_usage on\|off` | `on` | Request OpenAI streaming usage; disable for backends that reject it. |
| `anthropic_openai_max_tools N` | `256` | Maximum number of tools accepted from a single request; larger lists are rejected with a self-describing 400. Raise to support clients (e.g. Claude Code) that send many tools. |

Without an override, the incoming `x-api-key` becomes an upstream bearer token.
An existing Authorization header can pass through when no replacement key is
provided. The module removes `x-api-key`, `anthropic-*`, `x-stainless-*` and
`Accept-Encoding` from forwarded input headers. Do not add proxy header directives
that inadvertently replace the generated Authorization header.

The API-key directive takes a literal value, not an environment-variable lookup.
If a fixed upstream key is needed, put the directive in a restricted, untracked
include file inside the location. Never commit that file or print `nginx -T`
output containing credentials. This key configures upstream authentication; it
does not authenticate callers of the bridge.

Keep `proxy_request_buffering on`; request conversion needs a buffered body.
Set body size/buffer limits appropriate for the payload (the example uses 16 MiB).
Use HTTP/1.1 upstream, clear `Connection`, and disable upstream compression.
Use `proxy_buffering off` and `proxy_ignore_headers X-Accel-Buffering` for SSE;
the module also forces streaming response buffering off. A long read timeout is
an operational choice, not a guarantee that the model completes.

The prefix location `/v1/messages` also handles `/v1/messages/count_tokens` locally.
For custom routing, preserve the count endpoint and map message traffic to the
backend's `/v1/chat/completions` endpoint. Other API families, including OpenAI
Responses, are not implemented by this module.
