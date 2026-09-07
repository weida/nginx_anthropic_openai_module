# Protocol contract

The bridge accepts Anthropic-style Messages JSON and emits OpenAI Chat Completions
JSON upstream. JSON and streaming responses are converted back. Compatibility is
limited to the fields and block types described here; a backend must support the
requested capabilities independently.

| Input | Upstream behavior |
| --- | --- |
| `system`, user/assistant text | OpenAI messages, preserving supported text. Mid-conversation `system` text entries remain at their original position as separate upstream system messages. |
| `model` | Passed through unless the model directive overrides it. |
| `max_tokens`, `temperature`, `top_p` | Corresponding Chat Completions fields. |
| `stop_sequences` | OpenAI `stop`. |
| URL/base64 image blocks | OpenAI `image_url` content. |
| `tools` with name, description, `input_schema` | Function definitions with `parameters`. The tool-definition cap defaults to 256 and is configurable via `anthropic_openai_max_tools`; the separate streaming limit remains 32 tool calls per response. |
| Assistant `tool_use` | Function `tool_calls` with serialized arguments. |
| User `tool_result` | Tool messages associated with the tool-use ID. |
| `tool_choice`: auto, any, none, named tool | auto, required, none, named function selection. |
| `disable_parallel_tool_use` | When true, sets OpenAI `parallel_tool_calls` to false. |
| `metadata.user_id` | OpenAI `user`. |
| `cache_control` | Dropped. |
| Thinking/reasoning content | Dropped. |
| Document/file blocks and unsupported tool types | Rejected. |

The response uses the client's model name even when the upstream model is
replaced. The first OpenAI choice supplies response content. Function calls become
`tool_use` blocks. Finish reasons map into Anthropic stop reasons, and upstream
prompt/completion usage supplies input/output token counts when available.

Streaming emits Anthropic SSE events including message start, block start/delta/
stop, message delta and message stop. Tool argument fragments form the JSON input
for a tool-use block. The converter handles fragmented upstream SSE and downstream
backpressure; clients must consume the complete event sequence and distinguish an
error from a successfully terminated message.

`POST /v1/messages/count_tokens` returns a local estimate based on UTF-8 byte
length divided by four (minimum one for nonempty counted content), including
system/message text, tool inputs and tools JSON. It is not a model tokenizer and
must not be used as an exact context-window or billing calculation. Setting
`anthropic_openai_count_tokens off` returns HTTP 501.

JSON strings or keys decoding to U+0000 are rejected because cJSON cannot preserve
them in its C-string representation. Literal backslash-u0000 and ordinary escaped
newline, tab and carriage return remain supported.

Streaming output has two 256 KiB slots and up to 32 retained-input nodes per
request. These are output-buffer limits, not a total memory budget; parsing,
accumulated tool arguments and buffered JSON use additional bounded storage.

The pinned Node SDK 0.39.0 oracle does not merge late input-token usage into
`finalMessage().usage`, even when the wire event contains it. Keep wire-event
validation separate from SDK aggregation. Mock coverage does not establish that a
particular real backend implements tools or that a full client workflow works.

Mid-conversation system content accepts strings or arrays of text blocks only.
Unsupported roles/content and system `clear_at`/`output_config` fields return
400 before contacting the backend; these rejected inputs are not dropped to
make the request succeed. This subset does not imply support for all
Anthropic beta features or validation against a particular real model.
