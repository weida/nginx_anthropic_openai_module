package AOTest;
use strict;
use warnings;
use Exporter 'import';
use IO::Socket::INET;
use File::Temp qw(tempdir);
use FindBin;
use JSON::PP;
use POSIX qw(_exit WNOHANG);
use Time::HiRes qw(sleep);
our @EXPORT = qw(start_nginx exchange message completion encode_json decode_json);
my ($dir, $nginx, $port, $listener, $serial, $owner);
sub message { return {model=>'client-model',max_tokens=>64,messages=>[{role=>'user',content=>'hello'}], @_}; }
sub completion { return {id=>'chatcmpl-test',model=>'backend-model',choices=>[{index=>0,message=>{role=>'assistant',content=>'hello'},finish_reason=>'stop'}],usage=>{prompt_tokens=>7,completion_tokens=>2}, @_}; }
sub start_nginx {
    my ($extra)=@_;
    $owner=$$;
    $dir=tempdir('ao-integration-XXXXXX',TMPDIR=>1,CLEANUP=>1);
    mkdir "$dir/logs";
    $listener=IO::Socket::INET->new(LocalAddr=>'127.0.0.1',LocalPort=>0,Listen=>16,ReuseAddr=>1) or die $!;
    my $up=$listener->sockport;
    my $reserve=IO::Socket::INET->new(LocalAddr=>'127.0.0.1',LocalPort=>0,Listen=>1) or die $!;
    $port=$reserve->sockport; close $reserve;
    my $module=$ENV{TEST_NGINX_LOAD_MODULES} || "$FindBin::Bin/../build/ngx_http_anthropic_openai_module.so";
    open my $cf,'>',"$dir/nginx.conf" or die $!;
    print $cf qq{load_module $module;
daemon off; master_process off; pid $dir/nginx.pid;
error_log $dir/error.log info;
events { worker_connections 128; }
http { access_log off; client_body_temp_path $dir/body;
proxy_temp_path $dir/proxy; client_max_body_size 32m;
server { listen 127.0.0.1:$port;
client_body_buffer_size 1k;
location / { anthropic_openai on; anthropic_openai_model qwen;
proxy_pass http://127.0.0.1:$up/v1/chat/completions;
proxy_buffering off; proxy_request_buffering on;
proxy_read_timeout 300ms; proxy_next_upstream off;
$extra
}
location /configured { anthropic_openai on; anthropic_openai_api_key test-config;
proxy_pass http://127.0.0.1:$up/v1/chat/completions; }
location /unbuffered { anthropic_openai on; proxy_request_buffering off;
proxy_http_version 1.1; proxy_pass http://127.0.0.1:$up/v1/chat/completions; }
location /off/count_tokens { anthropic_openai on; anthropic_openai_count_tokens off;
proxy_pass http://127.0.0.1:$up; }
location /file/count_tokens { anthropic_openai on; client_body_in_file_only clean;
proxy_pass http://127.0.0.1:$up; }
}}};
    close $cf;
    $nginx=fork(); die $! unless defined $nginx;
    if (!$nginx) { exec($ENV{TEST_NGINX_BINARY} || '/usr/sbin/nginx','-p',"$dir/",'-c',"$dir/nginx.conf") or _exit(127); }
    for (1..100) {
        my $s=IO::Socket::INET->new(PeerAddr=>'127.0.0.1',PeerPort=>$port);
        if ($s) { close $s; return; }
        sleep .02;
    }
    die "nginx did not start: $dir/error.log";
}
sub exchange {
    my (%o)=@_;
    my $capture="$dir/capture-".++$serial;
    my $pid=fork(); die $! unless defined $pid;
    if (!$pid) {
        $SIG{ALRM}=sub { _exit(0) }; alarm 8;
        my $s=$listener->accept() or _exit(1);
        $s->autoflush(1);
        my $request='';
        while ($request !~ /\r\n\r\n/) { my $n=sysread($s,my $b,4096); last unless $n; $request.=$b; }
        my ($h,$body)=split /\r\n\r\n/,$request,2; $body //='';
        my ($n)=$h=~/Content-Length:\s*(\d+)/i; $n //=0;
        while (length($body)<$n) { my $got=sysread($s,my $b,$n-length($body)); last unless $got; $body.=$b; }
        open my $f,'>',$capture or _exit(2); binmode $f; print $f "$h\r\n\r\n$body"; close $f;
        my $resp=exists $o{response} ? $o{response} : encode_json(completion());
        my $status=$o{status} || 200;
        my $length=length($resp)+($o{incomplete} ? 100 : 0);
        my $headers=$o{response_headers} || '';
        print $s "HTTP/1.1 $status Test\r\nContent-Type: application/json\r\nContent-Length: $length\r\nConnection: close\r\n$headers\r\n$resp";
        sleep 1 if $o{timeout};
        close $s; _exit(0);
    }
    my $s=IO::Socket::INET->new(PeerAddr=>'127.0.0.1',PeerPort=>$port) or die $!;
    $s->autoflush(1);
    my $body=exists $o{raw} ? $o{raw} : encode_json($o{request} || message());
    my $method=$o{method} || 'POST'; my $path=$o{path} || '/v1/messages';
    my $headers=$o{headers} || '';
    my $wire="$method $path HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nContent-Type: application/json\r\n$headers";
    if ($o{chunked}) { $wire.="Transfer-Encoding: chunked\r\n\r\n"; while(length $body) { my $chunk=substr($body,0,137,''); $wire.=sprintf("%x\r\n",length $chunk).$chunk."\r\n"; } $wire.="0\r\n\r\n"; }
    else { $wire.='Content-Length: '.length($body)."\r\n\r\n$body"; }
    print $s $wire;
    my $response=''; my $timedout=0;
    { local $SIG{ALRM}=sub { die "timeout\n" }; eval { alarm 4; while(sysread($s,my $buf,65536)) { $response.=$buf; } alarm 0; }; $timedout=1 if $@; alarm 0; }
    close $s;
    kill 'TERM',$pid; waitpid($pid,0);
    my $captured=''; if (open my $f,'<',$capture) { local $/; $captured=<$f>; close $f; }
    my ($rh,$rb)=split /\r\n\r\n/,$response,2; $rh //=''; $rb //='';
    if ($rh =~ /Transfer-Encoding: chunked/i) { my $decoded=''; while($rb =~ s/^([0-9a-f]+)[^\r]*\r\n//i) { my $n=hex($1); last unless $n; $decoded.=substr($rb,0,$n,''); $rb =~ s/^\r\n//; } $rb=$decoded; }
    my ($status)=$rh=~/^HTTP\/\S+ (\d+)/;
    my ($uh,$ub)=split /\r\n\r\n/,$captured,2;
    return {status=>$status || 0,body=>$rb,headers=>$rh,json=>scalar eval{decode_json($rb)},upstream_headers=>$uh || '',upstream_body=>$ub || '',upstream=>scalar eval{decode_json($ub || '')},hits=>length($captured)?1:0,timeout=>$timedout};
}
END { if ($owner && $$==$owner && $nginx) { kill 'TERM',$nginx; waitpid($nginx,0); } }
1;
