# nginx_anthropic_openai_module

An nginx dynamic module that translates Anthropic Messages requests into OpenAI
Chat Completions requests and translates JSON or SSE responses back. nginx
`proxy_pass` handles the upstream connection. Use it to connect an Anthropic-style
client to an OpenAI-compatible model server.

The module implements a defined protocol subset, including text, supported images,
and function tools. It does not provide an Anthropic service or a model tokenizer.
See [protocol behavior](docs/protocol.md) before selecting a backend.

## Downloads

See [GitHub Releases](https://github.com/weida/nginx_anthropic_openai_module/releases)
for published builds. Each target provides a `.so`, a bundle with licenses, and
JSON build metadata; `SHA256SUMS` covers all release assets. Choose the exact
nginx version, Linux architecture, and standard or compat variant. See the
[compatibility guide](docs/build-and-test.md) before loading a download.

## Build

On Linux, provide a C compiler, make, Python 3, PCRE2 development libraries.
Use a checkout path without spaces. The build helper downloads the selected nginx
source and builds a matching test nginx and module in `build/`:

```sh
python3 scripts/build.py --nginx-version 1.26.3
```

The module is `build/ngx_http_anthropic_openai_module.so`. See
[build, tests and release compatibility](docs/build-and-test.md) for the target
matrix and tests. CI is configured to produce versioned `.so` release archives;
an asset should only be treated as available after its release job succeeds.

**A dynamic module must match the target nginx version and build signature, CPU
architecture and runtime libraries. `--with-compat` is not universal ABI
compatibility.** Check `nginx -V` and test loading with the actual target binary.
For distro nginx, building against matching distro sources/options may be needed.

## Try locally

The complete [example configuration](conf/example.conf) binds only to
`127.0.0.1:8080` and expects an OpenAI-compatible server at `127.0.0.1:8000`.
Set `anthropic_openai_model` to a model served by that backend. From the checkout:

```sh
mkdir -p logs
./build/nginx -p "$PWD/" -c conf/example.conf -t
./build/nginx -p "$PWD/" -c conf/example.conf
curl -sS http://127.0.0.1:8080/v1/messages \
  -H 'Content-Type: application/json' \
  -d '{"model":"client-model","max_tokens":64,"messages":[{"role":"user","content":"Say hello."}]}'
```

Add `"stream":true` and use `curl -N` to inspect Anthropic SSE. To stop this test
instance, use `./build/nginx -p "$PWD/" -c conf/example.conf -s quit`.
The example has no inbound authentication; keep it on loopback unless you add
appropriate access controls. Authentication and TLS termination are nginx/operator
responsibilities. See [configuration](docs/configuration.md) for upstream keys.

## Verify and integrate

[AGENTS.md](AGENTS.md) is the entry point for agents configuring, testing, or
changing this module. [The validation guide](docs/validation.md) describes backend,
JSON, SSE, tools and client checks in order. Local mock/SDK tests do not establish
real-model or production acceptance. Real streaming-tool and full Claude Code
tool/TUI acceptance remain unverified.

The project uses the [MIT license](LICENSE). Vendored cJSON retains its own
[MIT notice](deps/cJSON/LICENSE).
