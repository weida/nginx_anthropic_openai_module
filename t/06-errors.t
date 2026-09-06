use strict; use warnings; use FindBin; use lib "$FindBin::Bin/lib";
use Test::More; use AOTest;
start_nginx('');
for my $stream (0,1) {
 for my $c ([400,'invalid_request_error'],[401,'authentication_error'],[403,'permission_error'],[404,'not_found_error'],[429,'rate_limit_error'],[500,'api_error'],[502,'api_error'],[529,'overloaded_error']) {
  my $r=exchange(request=>message(stream=>$stream?JSON::PP::true:JSON::PP::false),status=>$c->[0],response=>'upstream secret: test-private');
  is($r->{status},$c->[0],'upstream status preserved'); is($r->{json}{error}{type},$c->[1],'Anthropic error type'); unlike($r->{body},qr/test-private/,'raw upstream body is not disclosed');
 }
}
for my $raw ('{}','{}junk',"{\0}","{\x0b}",'{"max_tokens":1,"messages":[],"x":"raw' . "\n" . 'newline"}') {
 my $r=exchange(raw=>$raw); is($r->{status},400,'invalid request rejected'); is($r->{json}{error}{type},'invalid_request_error','client receives Anthropic JSON'); is($r->{hits},0,'invalid body does not reach upstream');
}
for my $number ('01','1.','1e','1e+') {
 my $r=exchange(raw=>'{' . '"max_tokens":'.$number.',"messages":[]' . '}');
 is($r->{status},400,'JSON number grammar is strict'); is($r->{hits},0,'malformed number never reaches upstream');
}

for my $stream (0,1) {
 for my $failure ('eof','timeout','oversized') {
  my $response=$failure eq 'oversized' ? 'test-private'.('x'x(16*1024*1024)) : 'test-private';
  my $r=exchange(request=>message(stream=>$stream?JSON::PP::true:JSON::PP::false),status=>429,response=>$response,incomplete=>$failure ne 'oversized',timeout=>$failure eq 'timeout');
  is($r->{status},429,"known upstream status survives $failure stream=$stream");
  is_deeply($r->{json},{type=>'error',error=>{type=>'rate_limit_error',message=>'request could not be completed'}},'exactly one complete mapped Anthropic error JSON');
  unlike($r->{body},qr/test-private/,'abnormal upstream error body is never disclosed');
  ok(!$r->{timeout},'abnormal error response terminates');
 }
}
done_testing;
