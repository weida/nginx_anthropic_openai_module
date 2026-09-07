use strict; use warnings; use FindBin; use lib "$FindBin::Bin/lib";
use Test::More; use AOTest;
start_nginx('');
my $schema={type=>'object',properties=>{cmd=>{type=>'string'}}};
for my $type (undef,'custom') {
    my $tool={name=>'Bash',input_schema=>$schema}; $tool->{type}=$type if defined $type;
    my $r=exchange(request=>message(tools=>[$tool],tool_choice=>{type=>'tool',name=>'Bash',disable_parallel_tool_use=>JSON::PP::true},messages=>[{role=>'assistant',content=>[{type=>'text',text=>'running'},{type=>'tool_use',id=>'call_1',name=>'Bash',input=>{cmd=>'ls'}}]},{role=>'user',content=>[{type=>'text',text=>'continue'},{type=>'tool_result',tool_use_id=>'call_1',content=>'oops',is_error=>JSON::PP::true}]},{role=>'user',content=>'next'}]));
    is($r->{status},200,'tools request succeeds');
    is_deeply($r->{upstream}{tools},[{type=>'function',function=>{name=>'Bash',parameters=>$schema}}],'custom/default tools map equivalently');
    is_deeply($r->{upstream}{tool_choice},{type=>'function',function=>{name=>'Bash'}},'named tool choice');
    is($r->{upstream}{parallel_tool_calls},JSON::PP::false,'parallel tools disabled');
    is_deeply([map {$_->{role}} @{$r->{upstream}{messages} || []}],[qw(assistant tool user user)],'tool results remain within their user turn');
    is($r->{upstream}{messages}[1]{content},'ERROR: oops','tool error prefix');
    is_deeply(decode_json($r->{upstream}{messages}[0]{tool_calls}[0]{function}{arguments}),{cmd=>'ls'},'tool input serialized');
}
for my $choice ([auto=>'auto'],[any=>'required'],[none=>'none']) {
 my $r=exchange(request=>message(tool_choice=>{type=>$choice->[0]})); is($r->{upstream}{tool_choice},$choice->[1],'tool choice '.$choice->[0]); ok(!exists $r->{upstream}{parallel_tool_calls},'parallel default omitted');
}
my $r=exchange(request=>message(messages=>[{role=>'user',content=>[{type=>'image',source=>{type=>'url',url=>'https://example.invalid/image.png'}},{type=>'image',source=>{type=>'base64',media_type=>'image/png',data=>'YWJj'}}]}]));
is_deeply($r->{upstream}{messages}[0]{content},[{type=>'image_url',image_url=>{url=>'https://example.invalid/image.png'}},{type=>'image_url',image_url=>{url=>'data:image/png;base64,YWJj'}}],'URL and base64 images convert without fetching');
for my $request (message(tools=>[{type=>'bash',name=>'Bash'}]),message(tools=>[map {{name=>'tool'.$_}} 1..257]),message(messages=>[{role=>'user',content=>[{type=>'document'}]}]),message(messages=>[{role=>'user',content=>[{type=>'tool_result',tool_use_id=>'1',content=>[{type=>'image'}]}]}])) {
 my $r=exchange(request=>$request); is($r->{status},400,'unsupported block or too many tools rejected'); is($r->{json}{error}{type},'invalid_request_error','Anthropic request error'); is($r->{hits},0,'invalid request never reaches upstream');
}

for my $content (undef,17,{},JSON::PP::true,[{type=>'tool_use'}],[{type=>'unknown'}],[{type=>'thinking'}],[{type=>'text',text=>'ok'},{type=>'image'}],[undef],[17],[{}],[{type=>'text',text=>17}]) {
 my $r=exchange(request=>message(messages=>[{role=>'user',content=>[{type=>'tool_result',tool_use_id=>'call_1',content=>$content}]}]));
 is($r->{status},400,'unsupported tool_result content shape is rejected');
 is($r->{json}{error}{type},'invalid_request_error','invalid tool result gets Anthropic JSON');
 is($r->{hits},0,'unsupported tool_result never reaches upstream');
}
for my $case ([{},''],[{content=>''},''],[{content=>[]},''],[{content=>[{type=>'text',text=>''},{type=>'text',text=>'second'},{type=>'text',text=>''}]},"\nsecond\n"],[{content=>[{type=>'text',text=>'first'},{type=>'text',text=>'second'}]},"first\nsecond"],[{content=>[{type=>'text',text=>'first'},{type=>'text',text=>'second'}],is_error=>JSON::PP::true},"ERROR: first\nsecond"]) {
 my $r=exchange(request=>message(messages=>[{role=>'user',content=>[{type=>'tool_result',tool_use_id=>'call_1',%{$case->[0]}}]}]));
 is($r->{status},200,'supported tool_result content succeeds');
 is($r->{upstream}{messages}[0]{content},$case->[1],'omitted/empty content and ordered text joining');
}
done_testing;
