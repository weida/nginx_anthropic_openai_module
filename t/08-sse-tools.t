use strict; use warnings; use FindBin; use lib "$FindBin::Bin/lib";
BEGIN { $ENV{TEST_NGINX_LOAD_MODULES} ||= "$FindBin::Bin/../build/ngx_http_anthropic_openai_module.so"; }
use Test::Nginx::Socket 'no_plan'; use AOStreamTest;
our $port=start_mock();
run_tests();
__DATA__
=== TEST 1: interleaved tools commit sorted after validating complete arguments
--- config eval
"location /v1/messages { anthropic_openai on; proxy_pass http://127.0.0.1:$::port/tools; }"
--- request
POST /v1/messages
{"model":"client-model","max_tokens":12,"stream":true,"messages":[]}
--- response_body_filters eval
\&AOStreamTest::tool_inputs
--- response_body: A|B|{"a":1}|{"b":2}
--- error_code: 200

=== TEST 2: tool-long
--- config eval
"location /v1/messages { anthropic_openai on; proxy_pass http://127.0.0.1:$::port/tool-long; }"
--- request
POST /v1/messages
{"model":"client-model","max_tokens":12,"stream":true,"messages":[]}
--- response_body_filters eval
\&AOStreamTest::long_tool
--- response_body: long-tool-ok
--- error_code: 200

=== TEST 3: tools-invalid
--- config eval
"location /v1/messages { anthropic_openai on; proxy_pass http://127.0.0.1:$::port/tools-invalid; }"
--- request
POST /v1/messages
{"model":"client-model","max_tokens":12,"stream":true,"messages":[]}
--- response_body_filters eval
\&AOStreamTest::error_of
--- response_body: error
--- error_code: 200

=== TEST 4: tools-incomplete
--- config eval
"location /v1/messages { anthropic_openai on; proxy_pass http://127.0.0.1:$::port/tools-incomplete; }"
--- request
POST /v1/messages
{"model":"client-model","max_tokens":12,"stream":true,"messages":[]}
--- response_body_filters eval
\&AOStreamTest::error_of
--- response_body: error
--- error_code: 200
