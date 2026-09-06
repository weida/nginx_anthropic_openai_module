import assert from 'node:assert/strict';
import http from 'node:http';
import net from 'node:net';
import {spawn} from 'node:child_process';
import {mkdtemp,mkdir,writeFile,readFile,rm} from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import Anthropic from '@anthropic-ai/sdk';

const deadline=(promise,label,ms=1000)=>Promise.race([promise,new Promise((_,reject)=>{const t=setTimeout(()=>reject(new Error(label+' timed out')),ms);t.unref();})]);
const root=path.resolve(import.meta.dirname,'../..');
const frame=(delta={},finish_reason=null,extra={})=>'data: '+JSON.stringify({choices:[{delta,finish_reason}],...extra})+'\n\n';
const done='data: [DONE]\n\n';
const expectedText='SDK 世界🙂';
let releaseNext, sentNext=false, requests=0;
const fixture=http.createServer(async(req,res)=>{
  let input=''; for await (const chunk of req) input+=chunk;
  const body=JSON.parse(input); assert.equal(body.stream,true); requests++;
  const scenario=req.headers['x-test-case'];
  if(scenario==='json') {
    res.writeHead(200,{'Content-Type':'application/json'});
    res.end(JSON.stringify({id:'sdk-json',choices:[{message:{content:expectedText},finish_reason:'stop'}],usage:{prompt_tokens:7,completion_tokens:3}})); return;
  }
  res.writeHead(200,{'Content-Type':'text/event-stream','X-Accel-Buffering':req.headers['x-test-buffering']||'yes'});
  if(scenario==='early') {
    res.write(frame({content:'first'}));
    await new Promise(resolve=>{releaseNext=resolve;});
    sentNext=true; res.end(frame({},'stop')+done); return;
  }
  if(scenario==='done-open') {
    res.write(frame({content:'complete'},'stop')+done);
    req.on('close',()=>{});
    const timer=setTimeout(()=>res.end(),1500); timer.unref(); return;
  }
  if(scenario==='tools') {
    res.write(frame({tool_calls:[{index:1,id:'b',function:{name:'B',arguments:'{"b":'}},{index:0,id:'a',function:{name:'A',arguments:'{"a":'}}]}));
    res.end(frame({tool_calls:[{index:0,function:{arguments:'1}'}},{index:1,function:{arguments:'2}'}}]},'tool_calls')+done); return;
  }
  res.write(frame({content:'SDK '}));
  res.end(frame({content:'世界🙂'},'stop')+frame({},null,{choices:[],usage:{prompt_tokens:17,completion_tokens:23}})+done);
});
await deadline(new Promise((resolve,reject)=>{fixture.once('error',reject);fixture.listen(0,'127.0.0.1',resolve);}), 'mock startup');
const reserve=net.createServer(); await new Promise(resolve=>reserve.listen(0,'127.0.0.1',resolve));
const port=reserve.address().port; await new Promise(resolve=>reserve.close(resolve));
const dir=await mkdtemp(path.join(os.tmpdir(),'ao-sdk-'));
await mkdir(path.join(dir,'logs'));
await writeFile(path.join(dir,'nginx.conf'),`load_module ${root}/build/ngx_http_anthropic_openai_module.so;
daemon off; master_process off; pid ${dir}/nginx.pid; error_log ${dir}/error.log info;
events {worker_connections 128;} http {access_log off; client_body_temp_path ${dir}/body;
server {listen 127.0.0.1:${port}; location / {anthropic_openai on; proxy_buffering on;
proxy_read_timeout 2s; proxy_pass http://127.0.0.1:${fixture.address().port};}}}`);
const nginx=spawn(process.env.TEST_NGINX_BINARY||'/usr/sbin/nginx',['-p',dir+'/', '-c',path.join(dir,'nginx.conf')],{stdio:['ignore','ignore','pipe']});
let spawnError;
const nginxClosed=new Promise(resolve=>{nginx.once('close',resolve);nginx.once('error',error=>{spawnError=error;resolve();});});
let stderr=''; nginx.stderr.on('data',b=>stderr+=b);
const wait=ms=>new Promise(resolve=>setTimeout(resolve,ms));
try {
  for(let i=0;i<100;i++) {
    if(spawnError) throw spawnError;
    if(nginx.exitCode!==null || nginx.signalCode!==null) throw new Error('nginx exited during startup');
    try {await new Promise((resolve,reject)=>{const s=net.connect(port,'127.0.0.1',()=>{s.end();resolve();});s.on('error',reject);});break;}catch {await wait(20);}}
  for(const scenario of ['json','text','tools']) {
    const client=new Anthropic({apiKey:'test-only-local-key',baseURL:`http://127.0.0.1:${port}`,maxRetries:0,defaultHeaders:{'x-test-case':scenario}});
    const stream=client.messages.stream({model:'client-'+ 'm'.repeat(400),max_tokens:64,stream:true,messages:[{role:'user',content:'hello'}]});
    const events=[]; for await(const event of stream) events.push(event);
    const final=await stream.finalMessage();
    assert.equal(events.filter(e=>e.type==='message_stop').length,1);
    assert.equal(final.model,'client-'+ 'm'.repeat(400));
    if(scenario==='tools') {assert.deepEqual(final.content.map(c=>[c.name,c.input]),[['A',{a:1}],['B',{b:2}]]);assert.equal(final.stop_reason,'tool_use');}
    else {assert.equal(final.content[0].text,expectedText);assert.equal(final.stop_reason,'end_turn');}
    if(scenario==='text') {
      assert.deepEqual(events.find(e=>e.type==='message_delta').usage,{input_tokens:17,output_tokens:23});
      // SDK 0.39.0 aggregates output_tokens only from message_delta.
      assert.deepEqual(final.usage,{input_tokens:0,output_tokens:23});
    }
    console.log(`ok official @anthropic-ai/sdk 0.39.0 ${scenario} for-await and finalMessage()`);
  }
  for(const buffering of ['yes','no']) {
    sentNext=false; releaseNext=undefined;
    const response=await fetch(`http://127.0.0.1:${port}/v1/messages`,{method:'POST',headers:{'content-type':'application/json','x-test-case':'early','x-test-buffering':buffering},body:JSON.stringify({model:'test',max_tokens:4,stream:true,messages:[]})});
    const reader=response.body.getReader(); let wire='';
    while(!wire.includes('"text":"first"')) { const {value,done}=await deadline(reader.read(),'first event before upstream second packet'); assert(!done); wire+=Buffer.from(value).toString(); }
    assert.equal(sentNext,false); assert(releaseNext); releaseNext();
    while(true) {const {value,done}=await reader.read();if(done)break;wire+=Buffer.from(value).toString();}
    assert.equal((wire.match(/event: message_stop/g)||[]).length,1);
    console.log(`ok first-event visibility before mock releases second packet, X-Accel-Buffering ${buffering}`);
  }
  const start=Date.now();
  const response=await fetch(`http://127.0.0.1:${port}/v1/messages`,{method:'POST',headers:{'content-type':'application/json','x-test-case':'done-open'},body:JSON.stringify({model:'test',max_tokens:4,stream:true,messages:[]})});
  const wire=await deadline(response.text(),'DONE completion without upstream EOF',1000);
  assert(wire.includes('event: message_stop')); assert(Date.now()-start<1000);
  console.log('ok dispatched DONE completes downstream before upstream EOF');
  assert.equal(requests,6);
} catch(error) {console.error(stderr); console.error(await readFile(path.join(dir,'error.log'),'utf8').catch(()=>'')); throw error;}
finally {if(releaseNext)releaseNext();nginx.kill('SIGTERM');await deadline(nginxClosed,'nginx shutdown',2000);fixture.closeAllConnections();await new Promise(resolve=>fixture.close(resolve));await rm(dir,{recursive:true,force:true});}
