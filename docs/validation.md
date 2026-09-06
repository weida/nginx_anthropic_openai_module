# Validate a backend and client

Start with [configuration](configuration.md), [protocol limits](protocol.md), and
the [local suite](build-and-test.md). Discover the actual backend endpoint, model
ID and authentication mode without requesting plaintext keys. Confirm the test
host, listener and existing authorization before operating a service.

Use a dedicated nginx prefix, pid/log paths and loopback listener like the example.
Keep credentials in a restricted file or existing credential mechanism. Use
synthetic prompts. Do not stop or reload another nginx instance.

1. **Direct backend:** send a minimal OpenAI Chat Completions request to the model
   server. Confirm endpoint, model and authentication work without the bridge.
2. **Messages JSON:** send a minimal Anthropic-style request through the bridge.
   Check HTTP status, message envelope, text blocks, model echo and stop reason.
3. **Text SSE:** repeat with `stream:true` and a client that does not buffer output.
   Confirm first-event visibility, content deltas and a complete message stop.
4. **Tools:** supply a harmless function schema. Validate returned name, ID and
   parsed input; send a matching `tool_result` and verify the model continuation.
   Repeat with streaming and verify fragmented arguments reconstruct correctly.
5. **Intended client:** configure that client's supported base URL/model settings
   for this test endpoint. Exercise a text exchange and a complete tool round trip.
   Record client version, streaming behavior and any unsupported features.

The bridge only translates tool messages; the client/application executes tools.
A client asking for unsupported server-side tools or document blocks may fail by
design. Real streaming-tool and full Claude Code tool/TUI acceptance remain
unverified; successful local SDK fixtures do not close those checks.

Report each stage as passed, failed, skipped or blocked, with source commit,
module SHA-256, nginx version/build flags, architecture/runtime, backend/model
identity and client version. Redact credentials and private prompts. A local test
pass, an artifact build, an actual backend check and production deployment are
separate outcomes.

For load errors, compare architecture and nginx version/signature first. For
upstream HTTP errors, validate the direct backend request and model/auth settings.
For delayed streams, inspect proxy buffering, upstream SSE support and client
buffering. For usage errors, try the stream-usage directive only if the backend
rejects that field. For protocol defects, capture a minimal synthetic fixture
and reproduce it in the relevant local test before changing converter behavior.
