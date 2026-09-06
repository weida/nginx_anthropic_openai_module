#!/usr/bin/env python3
"""Behavioral SSE regression probes, also runnable against the baseline library."""
import ctypes as C, json, os, unittest
lib=C.CDLL(os.environ.get('AO_CONV_LIB','./build/libao-conv.so'))
lib.ngx_http_ao_sse_create.argtypes=[C.c_char_p]; lib.ngx_http_ao_sse_create.restype=C.c_void_p
lib.ngx_http_ao_sse_feed.argtypes=[C.c_void_p,C.c_char_p,C.c_size_t]
lib.ngx_http_ao_sse_end.argtypes=[C.c_void_p,C.c_int]
lib.ngx_http_ao_sse_json.argtypes=[C.c_void_p,C.c_char_p,C.c_size_t]
lib.ngx_http_ao_sse_copy_out.argtypes=[C.c_void_p,C.POINTER(C.c_size_t)]; lib.ngx_http_ao_sse_copy_out.restype=C.c_void_p
lib.ngx_http_ao_sse_destroy.argtypes=[C.c_void_p]
free=C.CDLL(None).free; free.argtypes=[C.c_void_p]
def frame(delta=None, finish=None, **extra):
 return ('data: '+json.dumps(dict(choices=[dict(delta=delta or {},finish_reason=finish)],**extra),ensure_ascii=False)+'\n\n').encode()
def convert(wire,model=b'model',error=0,synthetic=False):
 s=lib.ngx_http_ao_sse_create(model)
 (lib.ngx_http_ao_sse_json if synthetic else lib.ngx_http_ao_sse_feed)(s,wire,len(wire))
 lib.ngx_http_ao_sse_end(s,error)
 n=C.c_size_t(); p=lib.ngx_http_ao_sse_copy_out(s,C.byref(n)); out=C.string_at(p,n.value) if p else b''
 free(p); lib.ngx_http_ao_sse_destroy(s)
 return [(x+b'\n\n',json.loads(x.split(b'\ndata: ',1)[1])) for x in out.split(b'\n\n') if x]
class Regression(unittest.TestCase):
 def test_event_limit_and_utf8_escape_roundtrip(self):
  text=('漢🙂"\\\n\t'*20000)
  ev=convert(frame({'content':text})+frame(finish='stop')+b'data: [DONE]\n\n')
  self.assertLessEqual(max(len(w) for w,e in ev),262144)
  self.assertEqual(''.join(e['delta'].get('text','') for w,e in ev if e['type']=='content_block_delta'),text)
 def test_validate_all_tools_before_any_tool_start(self):
  ev=convert(frame({'tool_calls':[{'index':0,'id':'a','function':{'name':'A','arguments':'{}'}},{'index':1,'id':'b','function':{'name':'B','arguments':'bad'}}]},'tool_calls'))
  self.assertFalse(any(e['type']=='content_block_start' and e['content_block']['type']=='tool_use' for w,e in ev))
  self.assertIn('error',[e['type'] for w,e in ev])
 def test_long_model_echo(self):
  model=b'model-'+b'x'*400
  ev=convert(frame({'content':'ok'},'stop')+b'data: [DONE]\n\n',model)
  self.assertEqual(ev[0][1]['message']['model'],model.decode())
 def test_done_requires_blank_line(self):
  ev=convert(frame({'content':'x'})+b'data: [DONE]\n')
  types=[e['type'] for w,e in ev]; self.assertIn('error',types); self.assertNotIn('message_delta',types)
 def test_transport_after_finish_is_error(self):
  ev=convert(frame({'content':'x'},'stop'),error=1)
  types=[e['type'] for w,e in ev]; self.assertIn('error',types); self.assertNotIn('message_delta',types)
 def test_wrong_type_arguments_fail_closed(self):
  for value in [None,17,[],True]:
   tool={'index':0,'id':'a','function':{'name':'A','arguments':value}}
   for synthetic in [False,True]:
    wire=json.dumps({'choices':[{'message':{'tool_calls':[tool]},'finish_reason':'tool_calls'}]}).encode() if synthetic else frame({'tool_calls':[tool]},'tool_calls')
    ev=convert(wire,synthetic=synthetic)
    types=[e['type'] for w,e in ev]
    self.assertIn('error',types,(value,synthetic))
    self.assertNotIn('message_delta',types)
 def test_synthetic_tools_without_finish_are_tool_use(self):
  for finish in [None,'']:
   wire=json.dumps({'choices':[{'message':{'tool_calls':[{'id':'a','function':{'name':'A','arguments':'{}'}}]},'finish_reason':finish}]}).encode()
   ev=convert(wire,synthetic=True)
   self.assertEqual(next(e['delta']['stop_reason'] for w,e in ev if e['type']=='message_delta'),'tool_use')
 def test_caps_fail_with_bounded_terminal_output(self):
  wires=[
   b'data: '+b'x'*(1024*1024+1),
   frame({'tool_calls':[{'index':i,'id':str(i),'function':{'name':'A','arguments':'{}'}} for i in range(33)]},'tool_calls'),
   frame({'tool_calls':[{'id':'a','function':{'name':'A','arguments':'{"x":"'+'x'*(256*1024)+'"}'}}]},'tool_calls'),
   frame({'tool_calls':[{'id':'a','function':{'name':'A','arguments':'{}'}}]})+frame({'content':'x'*(32*1024+1)},'tool_calls')]
  for wire in wires:
   ev=convert(wire); types=[e['type'] for w,e in ev]
   self.assertIn('error',types); self.assertNotIn('message_delta',types)
   self.assertLessEqual(max(len(w) for w,e in ev),262144)
 def test_consume_pauses_without_eating_next_event(self):
  lib.ngx_http_ao_sse_consume.argtypes=[C.c_void_p,C.c_char_p,C.c_size_t,C.POINTER(C.c_size_t)]
  lib.ngx_http_ao_sse_pull.argtypes=[C.c_void_p,C.c_void_p,C.c_size_t,C.POINTER(C.c_size_t),C.POINTER(C.c_int)]
  first=frame({'content':'a'}); wire=first+frame({'content':'b'})
  s=lib.ngx_http_ao_sse_create(b'model'); used=C.c_size_t(); n=C.c_size_t(); last=C.c_int(); buf=C.create_string_buffer(262144)
  try:
   self.assertEqual(lib.ngx_http_ao_sse_consume(s,wire,len(wire),C.byref(used)),1)
   self.assertEqual(used.value,len(first))
   self.assertEqual(lib.ngx_http_ao_sse_consume(s,wire,len(wire),C.byref(used)),1)
   self.assertEqual(used.value,0)
   emitted=[]
   while lib.ngx_http_ao_sse_pull(s,buf,len(buf),C.byref(n),C.byref(last))>0: emitted.append(bytes(buf[:n.value]))
   self.assertEqual(len(emitted),4)
   self.assertIn(b'"text":"a"',emitted[-1])
   self.assertNotIn(b'"text":"b"',b''.join(emitted))
  finally: lib.ngx_http_ao_sse_destroy(s)
if __name__=='__main__': unittest.main()
