# Agent entry point

This repository builds an nginx dynamic module bridging Anthropic Messages to
OpenAI Chat Completions. Begin with README.md, docs/configuration.md,
docs/protocol.md and docs/validation.md. These documents describe configuration
and client use as well as implementation work.

## Configure or use the bridge

- Establish the backend endpoint, model ID, authentication mode and target nginx
  version/build flags. Inspect existing authorized configuration before asking
  for missing facts. Never request or print plaintext credentials.
- Check the protocol subset against the client's required features. Token counting
  is heuristic; thinking/cache metadata are dropped; documents are unsupported.
- Build or select a module matching nginx version/signature, architecture and
  runtime. `--with-compat` is not universal compatibility. Use docs/build-and-test.md
  and ci/versions.json; do not infer release availability from a planned matrix.
- Use conf/example.conf with a dedicated test prefix and loopback listener. Change
  the backend model/address as needed, validate with the actual nginx binary's
  `-t`, then perform direct-backend, JSON, SSE, tools and client checks in order.
- Keep local mocks, CI artifact results, real-model acceptance and deployment
  distinct. Streaming tools and full Claude Code tool/TUI remain unverified until
  the actual environment supplies evidence.

## Work on the implementation

- Respect existing changes and nested instructions. Keep edits within the task's
  scope; do not change global settings or other services incidentally.
- Source lives in src/, vendored cJSON in deps/cJSON/, tests in t/, build tooling
  in scripts/, and release targets in ci/versions.json.
- Follow C89 and nginx conventions, including declaration alignment and request
  pool ownership. Preserve resumable streaming cursors and NGX_AGAIN handling.
- For behavior changes, first add a failing reproducer, implement the correction,
  then run the relevant regressions. Run the full local suite for protocol/filter
  changes using the matching test nginx binary. See docs/build-and-test.md.
- Never remove third-party license notices. Do not commit credentials, generated
  binaries, local configs, node_modules, raw private traffic or unrelated files.
- Report what changed, the exact checks and results, and unverified limitations.
  A compiled module or local SDK test is not proof of real-client acceptance.

Installing packages, changing system services/firewalls, exposing listeners, or
publishing credentials is not authorized by these instructions. Follow the user's
existing task authorization and ask only for genuinely missing required scope.
