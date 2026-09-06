"""Replay recorded LiteLLM samples against the C converter, with named spec overrides."""
import copy
import ctypes as c
import json
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
class Req(c.Structure):
    _fields_ = [('model_override',c.c_char_p),('stream_usage',c.c_int),('client_stream',c.c_int),('orig_model',c.c_char*256),('err',c.c_char_p),('err_status',c.c_int)]
class Resp(c.Structure):
    _fields_ = [('echo_model',c.c_char_p),('err',c.c_char_p),('err_status',c.c_int)]
def normalize(value):
    value=copy.deepcopy(value)
    if value.get('stream') is False:
        value.pop('stream')
    for msg in value.get('messages',[]):
        if msg.get('tool_calls'):
            if msg.get('content') in (None,''):
                msg['content']=None
            for tool in msg['tool_calls']:
                tool['function']['arguments']=json.loads(tool['function']['arguments'])
    return value
with tempfile.TemporaryDirectory(prefix='ao-oracle-') as tmp:
    binary=Path(tmp)/'converter.so'
    subprocess.run(['cc','-shared','-fPIC','-I',str(root/'src'),'-I',str(root/'deps/cJSON'),str(root/'src/ngx_http_anthropic_openai_json.c'),str(root/'src/ngx_http_anthropic_openai_req.c'),str(root/'deps/cJSON/cJSON.c'),'-o',str(binary)],check=True)
    lib=c.CDLL(str(binary))
    lib.cJSON_free.argtypes=[c.c_void_p]
    for fn,typ in [('ngx_http_ao_convert_request',Req),('ngx_http_ao_convert_response',Resp)]:
        f=getattr(lib,fn);f.argtypes=[c.c_char_p,c.c_size_t,c.POINTER(typ),c.POINTER(c.c_size_t)];f.restype=c.c_void_p
    samples=json.loads(Path(__file__).with_name('litellm-1.77.3.json').read_text())
    failed=[]
    for sample in samples['cases']:
        data=json.dumps(sample['input'],ensure_ascii=False).encode()
        opt=Req() if sample['kind']=='request' else Resp(echo_model=b'client-model')
        fn=lib.ngx_http_ao_convert_request if sample['kind']=='request' else lib.ngx_http_ao_convert_response
        n=c.c_size_t();ptr=fn(data,len(data),c.byref(opt),c.byref(n))
        if not ptr:
            failed.append(sample['name']); print('FAIL',sample['name'],'conversion rejected'); continue
        got=json.loads(c.string_at(ptr,n.value));lib.cJSON_free(ptr)
        expected=copy.deepcopy(sample['output'])
        # Binding project contract takes precedence over known adapter differences.
        if sample['name']=='custom_tool':
            expected['tools'][0]['function']['parameters']['type']='object'
        elif sample['name']=='image':
            expected['messages'][0]['content'][0]['image_url']['url']='data:image/png;base64,aGVsbG8='
        elif sample['name']=='cache_control':
            expected['messages'][0]['content']='Brief'
        if normalize(got)!=normalize(expected):
            failed.append(sample['name']); print('FAIL',sample['name'],json.dumps({'actual':got,'expected':expected},ensure_ascii=False))
        else:
            print('PASS',sample['name'])
    print(f'{len(samples["cases"])-len(failed)}/{len(samples["cases"])} recorded adapter samples match approved semantics')
    raise SystemExit(bool(failed))
