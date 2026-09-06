# vi:filetype=perl
use FindBin;
BEGIN {
    $ENV{TEST_NGINX_LOAD_MODULES} ||=
        "$FindBin::Bin/../build/ngx_http_anthropic_openai_module.so";
}
use Test::Nginx::Socket 'no_plan';

run_tests();

__DATA__

=== TEST 1: module loads, flag off is pass-through
--- config
location /t {
    anthropic_openai off;
    return 200 "ok";
}
--- request
GET /t
--- response_body: ok
--- error_code: 200
