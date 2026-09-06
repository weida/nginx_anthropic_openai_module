package AOStreamTest;
use strict; use warnings;
use Encode qw(encode_utf8);
use Exporter 'import'; use JSON::PP; use Test::More;
our @EXPORT=qw(start_mock events text_of tool_inputs);
my ($pid,$pipe);
sub start_mock {
    ($pid=open($pipe,'-|','python3',"$FindBin::Bin/stream_mock.py")) or die $!;
    my $port=<$pipe>; chomp $port; return $port;
}
sub events {
    my ($body)=@_; my @out; my %open; my %started; my $stop=0; my $delta=0;
    for my $raw (split /\n\n/,$body) {
        next unless length $raw;
        ok(length($raw)+2<=262144,'serialized event fits a fixed output slot');
        my ($name,$data)=$raw=~/^event: ([a-z_]+)\ndata: (.*)$/s;
        my $e=eval{decode_json($data)};
        ok($e && $name eq $e->{type},'event and JSON type agree'); next unless $e;
        ok(!$stop,'no event after message_stop');
        if ($name eq 'content_block_start') { ok(!$started{$e->{index}}++,'one start per index'); $open{$e->{index}}=1; }
        if ($name eq 'content_block_delta') { ok($open{$e->{index}},'delta belongs to open block'); }
        if ($name eq 'content_block_stop') { ok(delete $open{$e->{index}},'one stop per open block'); }
        if ($name eq 'message_delta') { ok(!$delta++,'one message_delta'); ok(!keys %open,'all blocks closed before message_delta'); }
        $stop++ if $name eq 'message_stop'; push @out,$e;
    }
    is($stop,1,'exactly one message_stop');
    is($out[0]{type},'message_start','message_start first'); is($out[1]{type},'ping','ping second');
    return \@out;
}
sub text_of {
    my ($body)=@_; my $events=events($body);
    return encode_utf8(join '',map {$_->{delta}{text} // ''} grep {$_->{type} eq 'content_block_delta'} @$events);
}
sub tool_inputs {
    my ($body)=@_; my $events=events($body); my %args; my @names;
    for my $e (@$events) {
        push @names,$e->{content_block}{name} if $e->{type} eq 'content_block_start' && $e->{content_block}{type} eq 'tool_use';
        $args{$e->{index}}.=$e->{delta}{partial_json} if $e->{type} eq 'content_block_delta' && $e->{delta}{type} eq 'input_json_delta';
    }
    return join('|',@names,map {$args{$_}} sort {$a<=>$b} keys %args);
}
END { my $status=$?; if ($pid) { kill 'TERM',$pid; close $pipe; } $?=$status; }
1;
sub error_of {
    my $events=events($_[0]);
    is(scalar(grep {$_->{type} eq 'error'} @$events),1,'one terminal error');
    is(scalar(grep {$_->{type} eq 'message_delta'} @$events),0,'no successful message_delta on error');
    is(scalar(grep {$_->{type} eq 'content_block_start' && $_->{content_block}{type} eq 'tool_use'} @$events),0,'no partially validated tool commit');
    return 'error';
}
sub usage_of {
    my $events=events($_[0]);
    my ($e)=grep {$_->{type} eq 'message_delta'} @$events;
    return "$e->{usage}{input_tokens},$e->{usage}{output_tokens}";
}
sub long_tool {
    my $events=events($_[0]); my $args=''; my $slices=0;
    for my $e (@$events) { if ($e->{type} eq 'content_block_delta' && $e->{delta}{type} eq 'input_json_delta') { $args.=$e->{delta}{partial_json}; $slices++; } }
    ok($slices>=2,'escape-aware tool arguments exceed one slot and split');
    # Exact whitespace and escape spelling are part of the contract.
    my $expected='{ "value": "'.("\x{6f22}\x{1f642}\\\"\\\\\\n" x 15000).'" }';
    is($args,$expected,'concatenated partial_json is byte-exact original JSON');
    my $parsed=eval{JSON::PP->new->decode($args)};
    ok(ref($parsed) eq 'HASH','complete argument concatenation is a JSON object');
    return 'long-tool-ok';
}
1;
