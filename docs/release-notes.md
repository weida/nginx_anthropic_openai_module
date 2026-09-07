This is a preview release of the MIT-licensed Anthropic Messages ↔ OpenAI Chat Completions nginx dynamic module.

Assets cover nginx 1.26.3, 1.30.4 and 1.31.5, Linux amd64 and arm64, standard and compat variants. Each target includes the module, JSON build metadata and a tar.gz bundle with license notices. Verify SHA256SUMS before installation.

The standard build uses Ubuntu 24.04; compat uses a glibc 2.17 baseline. JSON metadata records the measured module GLIBC requirement. Exact nginx version, build signature, architecture and runtime compatibility are still required: --with-compat is not universal compatibility. Run nginx -t with your actual target binary.

Publication requires all twelve build/load, local regression and artifact-check jobs to pass. These are local fixtures and SDK tests, not acceptance of your backend or production environment. Real streaming-tool sessions and full Claude Code tool/TUI acceptance remain unverified. Token counting is heuristic. See docs/protocol.md and docs/validation.md for the supported subset and integration checks.

## Changes in this build

- **`role: system` mid-conversation support.** Text-only `system` entries are preserved as separate OpenAI system messages at the original conversation position. Unsupported content remains a 400 error.
- **Configurable tool cap.** The hard 32-tool ceiling is replaced by `anthropic_openai_max_tools` (default 256). Raise it to support clients that ship large tool sets; oversized requests get a self-describing 400.
- **Self-describing request errors.** When request conversion fails, the failure reason is now logged at `error` level and surfaced in the response body rather than dropped on the floor.
