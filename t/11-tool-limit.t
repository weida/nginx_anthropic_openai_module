use strict; use warnings; use FindBin; use lib "$FindBin::Bin/lib";
use Test::More; use AOTest; use File::Temp qw(tempdir);
# The inherited location also verifies that a low cap survives config merging.
start_nginx('anthropic_openai_max_tools 1;', 'anthropic_openai_max_tools 2;');
for my $case (['/',1],['/configured',2]) {
    my ($path,$limit)=@$case;
    for my $n ($limit,$limit+1) {
        my $r=exchange(path=>$path,request=>message(tools=>[map {{name=>'tool'.$_}} 1..$n]));
        is($r->{status},$n==$limit ? 200 : 400,"custom/inherited cap: $path $n");
        is($r->{hits},$n==$limit ? 1 : 0,'cap enforced before upstream');
    }
}
my $dir=tempdir('ao-config-XXXXXX',TMPDIR=>1,CLEANUP=>1); mkdir "$dir/logs";
my $binary=$ENV{TEST_NGINX_BINARY} || '/usr/sbin/nginx';
my $module=$ENV{TEST_NGINX_LOAD_MODULES} || "$FindBin::Bin/../build/ngx_http_anthropic_openai_module.so";
for my $n ('0','-1','2147483648','4294967297','invalid','1','2147483647') {
    open my $f,'>',"$dir/nginx.conf" or die $!;
    print $f "load_module $module;\nerror_log stderr;\nevents {}\nhttp { anthropic_openai_max_tools $n; }\n"; close $f;
    my $pid=fork(); die $! unless defined $pid;
    if (!$pid) { open STDOUT,'>',"$dir/check.log"; open STDERR,'>&',\*STDOUT; exec $binary,'-t','-p',"$dir/",'-c',"$dir/nginx.conf"; exit 127; }
    waitpid($pid,0);
    is($?==0 ? 1 : 0,($n eq '1'||$n eq '2147483647') ? 1 : 0,"config range: $n");
}
done_testing;
