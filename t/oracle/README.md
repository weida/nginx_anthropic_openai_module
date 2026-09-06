# Recorded mapping oracle

`litellm-1.77.3.json` contains ten outputs from a real local run of `LiteLLMAnthropicMessagesAdapter` 1.77.3. The fixture records adapter source SHA-256 and UTC recording time. No model API calls are made. LiteLLM is a test dependency only.

Replay against the current C converter (Python standard library and C compiler only):

```sh
python3 t/oracle/replay.py
```

The replay compares recorded semantics, with explicit module contract differences: `custom` preserves the input schema's `type: object`; base64 images use the media type and data from the source object; system `cache_control` is dropped and text flattened. These are differences from the recorded adapter, not a claim of exact LiteLLM parity. JSON whitespace inside tool arguments, default `stream: false`, and empty assistant tool-call content are normalized.

To re-record in an isolated environment:

```sh
python3 -m venv /tmp/ao-oracle-venv
/tmp/ao-oracle-venv/bin/pip install -r t/oracle/requirements.lock
/tmp/ao-oracle-venv/bin/python t/oracle/record_litellm.py
```

The lock reflects the local Python 3.13 environment. The frozen fixture is replayable without installing these packages. SDK event accumulation is tested separately under `t/sdk`; neither oracle proves compatibility with a real Qwen server or Claude Code TUI.
