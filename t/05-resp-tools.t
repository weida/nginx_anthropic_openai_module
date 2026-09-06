use strict; use warnings; use FindBin; use lib "$FindBin::Bin/lib";
use Test::More; use AOTest;
start_nginx('');
for my $args ('{}',''," {}\r\n",'{"cmd":"echo\\nhello"}','[]','null','{}junk','{}{}',"{\0}","{\x0b}","{\"x\":\"a\nb\"}","{\"x\":\"a\tb\"}","{\"x\":\"a\rb\"}") {
 my $valid=$args eq '{}' || $args eq '' || $args eq " {}\r\n" || $args eq '{"cmd":"echo\\nhello"}';
 my $r=exchange(response=>encode_json(completion(choices=>[{message=>{content=>undef,tool_calls=>[{id=>'call_1',type=>'function',function=>{name=>'Bash',arguments=>$args}}]},finish_reason=>'tool_calls'}])));
 is($r->{status},$valid?200:502,'strict tool arguments status');
 if ($valid) { is($r->{json}{content}[0]{type},'tool_use','tool response block'); is($r->{json}{stop_reason},'tool_use','tool finish'); is_deeply($r->{json}{content}[0]{input},length($args)?decode_json($args):{},'tool arguments parsed'); }
 else { is($r->{json}{error}{type},'api_error','invalid arguments are protocol error'); }
}
for my $call ({function=>{name=>'Bash',arguments=>'{}'}},{id=>'call_1',function=>{arguments=>'{}'}},{id=>'call_1',function=>{name=>'Bash',arguments=>[]}}) {
 my $r=exchange(response=>encode_json(completion(choices=>[{message=>{tool_calls=>[$call]},finish_reason=>'tool_calls'}])));
 is($r->{status},502,'tool id, name and string arguments are required');
}

for my $case (
 ['NUL prefix', "\0{}", 502, undef],
 ['NUL suffix', "{}\0junk", 502, undef],
 ['NUL only', "\0", 502, undef],
 ['decoded NUL object value unsupported by cJSON', encode_json({x=>"a\0hidden"}), 502, undef],
 ['decoded NUL object key unsupported by cJSON', encode_json({"x\0hidden"=>1}), 502, undef],
 ['literal backslash-u0000 is preserved', encode_json({x=>'\u0000'}), 200, {x=>'\u0000'}],
 ['representable escaped controls are preserved', encode_json({x=>"a\n\t\rb"}), 200, {x=>"a\n\t\rb"}],
 ['actually empty arguments remain valid', '', 200, {}],
 ['empty object remains valid', '{}', 200, {}]
) {
 my $r=exchange(response=>encode_json(completion(choices=>[{message=>{tool_calls=>[{id=>'call_1',function=>{name=>'Bash',arguments=>$case->[1]}}]},finish_reason=>'tool_calls'}])));
 is($r->{status},$case->[2],$case->[0]);
 if ($case->[2]==200) { is_deeply($r->{json}{content}[0]{input},$case->[3],'all argument value bytes preserved'); }
 else { is($r->{json}{error}{type},'api_error','no false successful tool invocation'); }
}
done_testing;
