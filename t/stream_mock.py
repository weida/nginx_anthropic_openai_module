#!/usr/bin/env python3
"""Loopback-only OpenAI fixture shared by Test::Nginx and SDK tests."""
import json, sys, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

def frame(delta=None,finish=None,**extra):
    return ('data: '+json.dumps(dict(choices=[dict(delta=delta or {},finish_reason=finish)],**extra),ensure_ascii=False)+'\n\n').encode()
DONE=b'data: [DONE]\n\n'
def completion(text='synthetic',tools=None):
    message=dict(role='assistant',content=text)
    if tools: message['tool_calls']=tools
    return json.dumps(dict(id='chat-sdk',choices=[dict(message=message,finish_reason='tool_calls' if tools else 'stop')],usage=dict(prompt_tokens=7,completion_tokens=3)),ensure_ascii=False).encode()
def tool(i,args,id=None,name=None):
    d=dict(index=i,function=dict(arguments=args))
    if id: d['id']=id
    if name: d['function']['name']=name
    return d
LONG='漢🙂"\\\n\t'*20000
ARGS='{ "value": '+json.dumps('漢🙂"\\\n'*15000,ensure_ascii=False)+' }'
def payload(name):
    if name in ('json-incomplete','json-timeout'): return b'{"choices":','application/json'
    if name=='json-oversize': return b' '*(16*1024*1024+1),'application/json'
    if name=='json': return completion(),'application/json'
    if name=='json-large': return completion(LONG),'application/json'
    if name=='json-empty': return completion(''),'application/json'
    if name=='json-invalid': return b'{}','application/json'
    if name=='text': return frame({'content':'hello 世界'},'stop')+DONE,'text/event-stream'
    if name=='empty': return frame(finish='stop')+DONE,'text/event-stream'
    if name=='long': return frame({'content':LONG},'stop')+DONE,'text/event-stream'
    if name=='thousand': return b''.join(frame({'content':str(i)+','}) for i in range(1200))+frame(finish='stop')+DONE,'text/event-stream'
    if name=='alternating': return b''.join(frame({'content':'x'*15000 if i%2==0 else '小'}) for i in range(40))+frame(finish='stop')+DONE,'text/event-stream'
    if name=='usage': return frame({'content':'usage'},'stop')+b'data: {"choices":[],"usage":{"prompt_tokens":17,"completion_tokens":23}}\n\n'+DONE,'text/event-stream'
    if name=='finish-eof': return frame({'content':'finished'},'stop'),'text/event-stream'
    if name=='timeout-finish': return frame({'content':'partial'},'stop'),'text/event-stream'
    if name in ('premature','timeout'): return frame({'content':'partial'}),'text/event-stream'
    if name=='partial': return frame({'content':'partial'})+b'data: [DONE]\n','text/event-stream'
    if name=='error': return frame({'content':'partial'})+b'event: error\ndata: {"error":{"message":"private"}}\n\n','text/event-stream'
    if name=='tools': return (frame({'content':'before'})+frame({'tool_calls':[tool(1,'{"b":','call-b','B'),tool(0,'{"a":','call-a','A')]})+frame({'content':'after'})+frame({'tool_calls':[tool(0,'1}'),tool(1,'2}')]},'tool_calls')+DONE),'text/event-stream'
    if name=='tool-long': return frame({'tool_calls':[tool(0,ARGS,'call-long','Long')]},'tool_calls')+DONE,'text/event-stream'
    if name=='tools-invalid': return frame({'tool_calls':[tool(0,'{}','a','A'),tool(1,'no','b','B')]},'tool_calls')+DONE,'text/event-stream'
    if name=='tools-incomplete': return frame({'tool_calls':[tool(0,'{"x":','a','A')]}),'text/event-stream'
    if name=='crlf': return (frame({'content':'跨包🙂'},'stop')+DONE).replace(b'\n',b'\r\n'),'text/event-stream'
    if name=='early': return frame({'content':'first'})+frame(finish='stop')+DONE,'text/event-stream'
    raise ValueError(name)
class Handler(BaseHTTPRequestHandler):
    def log_message(self,*args): pass
    def do_POST(self):
        self.rfile.read(int(self.headers.get('Content-Length','0')))
        name=self.path.strip('/').split('?')[0]
        body,mime=payload(name)
        self.send_response(200); self.send_header('Content-Type',mime)
        self.send_header('X-Accel-Buffering','yes')
        self.send_header('Content-Length',str(len(body)+(100 if name in ('timeout','timeout-finish','json-incomplete','json-timeout') else 0)))
        self.end_headers()
        try:
            if name=='crlf':
                # Split between CR/LF and inside UTF-8 codepoints, not only SSE frames.
                for b in body: self.wfile.write(bytes([b])); self.wfile.flush(); time.sleep(.0002)
            else: self.wfile.write(body); self.wfile.flush()
            if name in ('timeout','timeout-finish','json-timeout'): time.sleep(1)
        except (BrokenPipeError,ConnectionResetError): pass
if __name__=='__main__':
    server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
    print(server.server_port,flush=True)
    server.serve_forever()
