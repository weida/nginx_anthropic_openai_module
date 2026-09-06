# Build, tests and releases

Use Linux with a C compiler, make, Python 3, PCRE2 development headers.
Integration tests additionally require Perl with Test::Nginx::Socket 0.30 and Node
22. Install dependencies using your environment's approved package workflow.
Use a checkout path without spaces and a dedicated build directory.

```sh
python3 scripts/build.py --nginx-version 1.26.3
npm ci --prefix t/sdk --ignore-scripts --no-audit --no-fund
make test NGX_SRC=.build-src/nginx-1.26.3 TEST_NGINX_BINARY="$PWD/build/nginx"
make ubsan-test NGX_SRC=.build-src/nginx-1.26.3
```

The helper builds nginx and the dynamic module together. Outputs are
`build/nginx` and `build/ngx_http_anthropic_openai_module.so`. nginx configure
rewrites its source tree's Makefile, so do not use a source tree that another
build owns. Build and test one nginx version at a time in each checkout.

## Compatibility and CI targets

[ci/versions.json](../ci/versions.json) is the machine-readable matrix. The release
configuration targets nginx 1.26.3, 1.30.4 and 1.31.5, each for Linux amd64 and
arm64, in two runtime profiles: standard Ubuntu 24.04 and a glibc 2.17
compatibility baseline. These are build targets, not a claim that every job has
passed or an artifact is already downloadable. The initial release is planned as
`v0.1.0-rc.1`, a prerelease.

Version, architecture, libc baseline, nginx configure features and module
signature all matter. `--with-compat` permits some feature differences but does
not make one `.so` work with every nginx binary. A glibc baseline does not remove
other shared-library dependencies or nginx ABI requirements. Distribution patches
can also affect compatibility. Inspect the selected archive's metadata, verify
checksums, compare the target's `nginx -V`, inspect runtime dependencies, and run
`nginx -t` with the actual target binary before use. Build against matching sources
and options when a supplied artifact cannot be loaded.

A CI build/load test validates the paired nginx executable. A release artifact is
usable on a specific host only after that host's compatibility check. Source,
package and runtime acceptance are separate results.

## Test targets

| Target | Scope |
| --- | --- |
| `make test` | All local converter, scheduling, Perl, SDK, slow-reader and golden tests. |
| `make conv-test` | Standalone C request/response conversion regressions. |
| `make stream-test` | SSE conversion boundaries and validation. |
| `make schedule-test` | Production filter with a deterministic downstream sink and backpressure. |
| `make integration-test` | Build module and run Perl integration tests. |
| `make slow-reader-test` | Real loopback TCP, paused/throttled reader, full payload reconstruction. |
| `make sdk-test` | Pinned Anthropic Node SDK JSON/text/tool-stream oracle. |
| `make golden-test` | Recorded mapping samples; see [oracle notes](../t/oracle/README.md). |
| `make ubsan-test` | Undefined-behavior checks on converter and filter harnesses. |
| `make asan-test` | Address/leak checks on a supported host. |

Pass the same `NGX_SRC` and absolute `TEST_NGINX_BINARY` settings when selecting
individual targets. The sanitizer targets instrument harnesses, not a running
nginx server. An unavailable sanitizer run is not a pass. Tests use local mocks
and synthetic credentials, start temporary listeners, and do not call a model
API. Avoid concurrent suites on the same host because test ports may overlap.

Record the source commit, compiler, architecture, nginx version/configure flags,
module SHA-256, commands and results for reproducibility. Keep secrets and raw
credential-bearing traffic out of reports. Continue with
[real-backend validation](validation.md) separately.
