use strict; use warnings; use utf8; use FindBin; use lib "$FindBin::Bin/lib";
use Test::More; use AOTest;
start_nginx('');
for my $c ([{messages=>[{role=>'user',content=>'abcdefgh'}]},2],[{system=>'abcd',messages=>[{role=>'user',content=>'中文'}]},2],[{messages=>[{role=>'user',content=>'a'}]},1],[{messages=>[]},0],[{messages=>[{role=>'user',content=>'x'x20000}]},5000]) {
 for my $path ('/v1/messages/count_tokens','/file/count_tokens') {
  my $r=exchange(path=>$path,request=>$c->[0]); is($r->{status},200,'count needs no max_tokens'); is_deeply($r->{json},{input_tokens=>$c->[1]},'raw content UTF8 bytes/4 heuristic, including file buffers'); is($r->{hits},0,'count is entirely local');
 }
}
my $r=exchange(path=>'/off/count_tokens',request=>{messages=>[]}); is($r->{status},501,'off returns 501'); is($r->{json}{error}{type},'not_found_error','off JSON'); is($r->{hits},0,'off never proxies');
$r=exchange(path=>'/v1/messages/count_tokens',raw=>'{}junk'); is($r->{status},400,'count rejects malformed JSON'); is($r->{hits},0,'bad count never proxies');
$r=exchange(path=>'/v1/messages/count_tokens',request=>{system=>[{type=>'text',text=>'abcd'}],messages=>[{role=>'assistant',content=>[{type=>'tool_use',input=>{x=>1}}]},{role=>'user',content=>[{type=>'tool_result',content=>'abc'}]}],tools=>[{name=>'x'}]});
is_deeply($r->{json},{input_tokens=>7},'text 4 + input JSON 7 + result 3 + tools JSON 14 = 28 bytes');
done_testing;
