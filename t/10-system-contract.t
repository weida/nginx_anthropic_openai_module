use strict; use warnings; use FindBin; use lib "$FindBin::Bin/lib";
use Test::More; use AOTest;
start_nginx('');
for my $content ('new rule', [{type=>'text',text=>'new'},{type=>'text',text=>'rule'}]) {
    my $r=exchange(request=>message(system=>'initial',messages=>[
        {role=>'user',content=>'hi'}, {role=>'system',content=>$content},
        {role=>'assistant',content=>'ok'}, {role=>'user',content=>'next'}]));
    is($r->{status},200,'system text succeeds');
    is_deeply([map {$_->{role}} @{$r->{upstream}{messages}}],
        [qw(system user system assistant user)],'system position preserved');
    is($r->{upstream}{messages}[2]{content},ref($content) ? "new\nrule" : $content,'system text preserved');
}
for my $msg (
    {role=>'robot',content=>'important'},
    {role=>'user',content=>[{type=>'text',text=>'keep me'},{type=>'document'}]},
    {role=>'user',content=>[{type=>'tool_result',tool_use_id=>'call_1',content=>'result'},{type=>'document'}]},
    {role=>'assistant',content=>[{type=>'document'}]},
    (map {{role=>'system',content=>$_}} (17,{},undef,[{type=>'image'}],[{type=>'document'}],[{type=>'tool_use'}],[{type=>'text',text=>17}],[undef])),
    {role=>'system',content=>'temporary',clear_at=>'next_user_message'},
    {role=>'system',content=>'rule',output_config=>{effort=>'high'}}
) {
    my $r=exchange(request=>message(messages=>[{role=>'user',content=>'hi'},$msg]));
    is($r->{status},400,'unsupported message rejected');
    is($r->{hits},0,'no partial conversation reaches upstream');
    is($r->{json}{error}{type},'invalid_request_error','Anthropic error type');
    isnt($r->{json}{error}{message},'request could not be completed','specific rejection reason');
}
for my $n (33,256,257) {
    my $r=exchange(request=>message(tools=>[map {{name=>'tool'.$_}} 1..$n]));
    is($r->{status},$n<=256 ? 200 : 400,"default cap: $n tools");
    is($r->{hits},$n<=256 ? 1 : 0,'cap checked before upstream');
}
done_testing;
