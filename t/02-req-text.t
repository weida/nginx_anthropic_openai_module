use strict; use warnings; use FindBin; use lib "$FindBin::Bin/lib";
use Test::More; use AOTest;
start_nginx('proxy_set_header X-Converted-Length $http_content_length;');
for my $chunked (0,1) {
    my $r=exchange(chunked=>$chunked,request=>message(system=>[{type=>'text',text=>'system',cache_control=>{type=>'ephemeral'}}],thinking=>{type=>'enabled'},messages=>[{role=>'user',content=>'a'x18000}]));
    is($r->{status},200,"large body succeeds chunked=$chunked");
    is($r->{upstream}{model},'qwen','model override');
    is_deeply($r->{upstream}{messages},[{role=>'system',content=>'system'},{role=>'user',content=>'a'x18000}],'complete converted messages survive request buffer reuse');
    unlike($r->{upstream_body},qr/cache_control|thinking/,'approved unsupported options dropped');
    my ($n)=$r->{upstream_headers}=~/Content-Length: (\d+)/i;
    is($n,length($r->{upstream_body}),'rewritten wire content length');
    my ($explicit)=$r->{upstream_headers}=~/X-Converted-Length: (\d+)/i;
    is($explicit,length($r->{upstream_body}),'content length header variable matches converted body');
    unlike($r->{upstream_headers},qr/Transfer-Encoding:/i,'upstream is not chunked');
}
my $r=exchange(request=>message(stream=>JSON::PP::true),status=>400,response=>'{}');
is_deeply($r->{upstream}{stream_options},{include_usage=>JSON::PP::true},'stream usage enabled by default');
$r=exchange(path=>'/unbuffered',request=>message(messages=>[{role=>'user',content=>'a'x18000}]));
is($r->{status},400,'unbuffered proxy request rejected before conversion');
is($r->{json}{error}{type},'invalid_request_error','unbuffered request gets Anthropic JSON');
is($r->{hits},0,'unsupported unbuffered request never proxies');
done_testing;
