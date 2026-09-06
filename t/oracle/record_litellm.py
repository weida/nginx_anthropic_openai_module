"""Record a real, pinned LiteLLM adapter run; no completion/API calls."""
import os
os.environ['LITELLM_LOCAL_MODEL_COST_MAP'] = 'True'
import copy
from datetime import datetime, timezone
import hashlib
import importlib.metadata
import json
from pathlib import Path
import litellm
litellm.telemetry = False
from litellm.llms.anthropic.experimental_pass_through.adapters import transformation
from litellm.types.utils import ModelResponse
assert importlib.metadata.version('litellm') == '1.77.3', 'Install the pinned oracle requirements first'
adapter = transformation.LiteLLMAnthropicMessagesAdapter()
base = {'model': 'client-model', 'max_tokens': 64, 'messages': [{'role': 'user', 'content': 'hello'}]}
tool = {'name': 'lookup', 'description': 'Look up a value', 'input_schema': {'type': 'object', 'properties': {'x': {'type': 'integer'}}}}
cases = []
for name, patch in [
    ('text', {}),
    ('system', {'system': 'Answer briefly.'}),
    ('tools', {'tools': [tool], 'tool_choice': {'type': 'auto'}}),
    ('custom_tool', {'tools': [dict(tool, type='custom')]}),
    ('tool_roundtrip', {'messages': [{'role': 'assistant', 'content': [{'type': 'tool_use', 'id': 'call_1', 'name': 'lookup', 'input': {'x': 1}}]}, {'role': 'user', 'content': [{'type': 'tool_result', 'tool_use_id': 'call_1', 'content': 'one'}]}]}),
    ('image', {'messages': [{'role': 'user', 'content': [{'type': 'image', 'source': {'type': 'base64', 'media_type': 'image/png', 'data': 'aGVsbG8='}}]}]}),
    ('cache_control', {'system': [{'type': 'text', 'text': 'Brief', 'cache_control': {'type': 'ephemeral'}}]}),
    ('thinking', {'messages': [{'role': 'assistant', 'content': [{'type': 'thinking', 'thinking': 'hidden', 'signature': 'test'}, {'type': 'text', 'text': 'visible'}]}]}),
]:
    request = dict(copy.deepcopy(base), **copy.deepcopy(patch))
    try:
        result = adapter.translate_anthropic_to_openai(copy.deepcopy(request))
        cases.append({'name': name, 'kind': 'request', 'input': request, 'output': result})
    except Exception as exc:
        cases.append({'name': name, 'kind': 'request', 'input': request, 'error': type(exc).__name__ + ': ' + str(exc)})
for name, content, tool_calls, finish in [('text_response', 'hello', None, 'stop'), ('tool_response', None, [{'id': 'call_1', 'type': 'function', 'function': {'name': 'lookup', 'arguments': '{"x":1}'}}], 'tool_calls')]:
    msg = {'role': 'assistant', 'content': content}
    if tool_calls:
        msg['tool_calls'] = tool_calls
    response = {'id': 'chatcmpl-test', 'object': 'chat.completion', 'created': 1, 'model': 'client-model', 'choices': [{'index': 0, 'message': msg, 'finish_reason': finish}], 'usage': {'prompt_tokens': 7, 'completion_tokens': 2, 'total_tokens': 9}}
    result = adapter.translate_openai_response_to_anthropic(ModelResponse(**response))
    if hasattr(result, 'model_dump'):
        result = result.model_dump()
    cases.append({'name': name, 'kind': 'response', 'input': response, 'output': result})
source = Path(transformation.__file__)
record = {'recorded_at': datetime.now(timezone.utc).isoformat(), 'oracle': 'LiteLLMAnthropicMessagesAdapter', 'version': importlib.metadata.version('litellm'), 'source_path': 'litellm/llms/anthropic/experimental_pass_through/adapters/transformation.py', 'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(), 'network': 'No model API calls; local adapter functions only', 'cases': cases}
Path(__file__).with_name('litellm-1.77.3.json').write_text(json.dumps(record, ensure_ascii=False, indent=2, default=lambda obj: obj.model_dump()) + '\n')
print(f"Recorded {len(cases)} real adapter samples; version {record['version']}; source SHA {record['source_sha256']}")
