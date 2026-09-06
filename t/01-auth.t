use strict; use warnings; use FindBin; use lib "$FindBin::Bin/lib";
use Test::More; use AOTest;
start_nginx('');
for my $c (["x-api-key: test-client\r\n",'/v1/messages','Bearer test-client'],["x-api-key: test-client\r\nAuthorization: Bearer old\r\n",'/v1/messages','Bearer test-client'],["x-api-key: test-client\r\nAuthorization: Bearer old\r\n",'/configured','Bearer test-config'],["Authorization: Bearer old\r\n",'/v1/messages','Bearer old']) {
    my $r=exchange(headers=>$c->[0]."anthropic-version: 2023-06-01\r\nx-stainless-lang: js\r\nAccept-Encoding: gzip\r\n",path=>$c->[1]);
    is($r->{status},200,'authenticated proxy succeeds');
    my @auth=$r->{upstream_headers}=~/^Authorization: (.*)\r?$/gmi; s/\r$// for @auth;
    is_deeply(\@auth,[$c->[2]],'exactly one Authorization with configured > api-key > original priority');
    unlike($r->{upstream_headers},qr/^(?:x-api-key|anthropic-|x-stainless-|Accept-Encoding)/mi,'private SDK headers and compression suppressed');
}
my $r=exchange(method=>'GET',raw=>'',headers=>"x-api-key: test-get\r\n",response=>'passthrough');
is($r->{body},'passthrough','non-POST response passes through');
like($r->{upstream_headers},qr/x-api-key: test-get/i,'non-POST headers unchanged');
done_testing;
