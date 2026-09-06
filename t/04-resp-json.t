use strict; use warnings; use FindBin; use lib "$FindBin::Bin/lib";
use Test::More; use AOTest;
start_nginx('');
my $r=exchange(response_headers=>"openai-model: private\r\nx-ratelimit-requests: 123\r\nx-request-id: test-request-id\r\n");
unlike($r->{headers},qr/^(?:openai-|x-ratelimit-|x-request-id:)/mi,'upstream protocol headers hidden');
like($r->{headers},qr/^request-id: test-request-id\r?$/mi,'request-id renamed');
is($r->{status},200,'nonstream response succeeds');
is($r->{json}{model},'client-model','client model echoed');
is_deeply($r->{json}{content},[{type=>'text',text=>'hello'}],'text response converted');
is_deeply($r->{json}{usage},{input_tokens=>7,output_tokens=>2},'usage converted');
for my $finish (['length','max_tokens'],['stop','end_turn']) {
 my $r=exchange(response=>encode_json(completion(choices=>[{message=>{content=>'ok'},finish_reason=>$finish->[0]}],usage=>undef)));
 is($r->{json}{stop_reason},$finish->[1],'finish reason maps'); is_deeply($r->{json}{usage},{input_tokens=>0,output_tokens=>0},'missing usage defaults zero');
}
for my $c ({response=>'not-json'},{response=>'{}'},{response=>'{"choices":',incomplete=>1},{response=>'{"choices":',incomplete=>1,timeout=>1},{response=>encode_json(completion(choices=>[{message=>{content=>'x'x(16*1024*1024)},finish_reason=>'stop'}]))}) {
 my $r=exchange(%$c); is($r->{status},502,'invalid, incomplete, timed out or oversized upstream becomes 502'); is($r->{json}{error}{type},'api_error','Anthropic protocol error'); ok(!$r->{timeout},'response terminates without hanging');
}
my $long_model='client-'.('m'x400);
$r=exchange(request=>message(model=>$long_model));
is($r->{json}{model},$long_model,'model echo is not truncated at 255 bytes');
for my $bad ({choices=>[{}]}, {choices=>[{message=>[]}]}) {
 $r=exchange(response=>encode_json($bad)); is($r->{status},502,'missing or invalid completion message rejected');
}
done_testing;
