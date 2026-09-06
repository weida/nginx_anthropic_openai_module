use strict; use warnings; use utf8; use FindBin; use lib "$FindBin::Bin/lib";
BEGIN { $ENV{TEST_NGINX_LOAD_MODULES} ||= "$FindBin::Bin/../build/ngx_http_anthropic_openai_module.so"; }
use Encode qw(encode_utf8);
use Test::Nginx::Socket 'no_plan'; use AOStreamTest;
our $port=start_mock();
our $base="anthropic_openai on; proxy_buffering on; proxy_read_timeout 300ms; proxy_next_upstream off;";
run_tests();
__DATA__
=== TEST 1: real proxy SSE text and forced X-Accel-Buffering yes
--- config eval
"location /v1/messages { $::base proxy_pass http://127.0.0.1:$::port/text; }"
--- request
POST /v1/messages
{"model":"client-model","max_tokens":12,"stream":true,"messages":[{"role":"user","content":"hello"}]}
--- response_body_filters eval
\&AOStreamTest::text_of
--- response_body eval
Encode::encode_utf8("hello 世界")
--- error_code: 200
=== TEST 2: CRLF split and fragmented UTF8
--- config eval
"location /v1/messages { $::base proxy_pass http://127.0.0.1:$::port/crlf; }"
--- request
POST /v1/messages
{"model":"client-model","max_tokens":12,"stream":true,"messages":[]}
--- response_body_filters eval
\&AOStreamTest::text_of
--- response_body eval
Encode::encode_utf8("跨包🙂")
--- error_code: 200
=== TEST 3: 1200 deltas stay ordered without duplication
--- config eval
"location /v1/messages { $::base proxy_pass http://127.0.0.1:$::port/thousand; }"
--- request
POST /v1/messages
{"model":"client-model","max_tokens":12,"stream":true,"messages":[]}
--- response_body_filters eval
\&AOStreamTest::text_of
--- response_body eval
CORE::join '',map {"$_,"} 0..1199
--- error_code: 200
=== TEST 4: escaped long text slices reconstruct codepoints
--- config eval
"location /v1/messages { $::base proxy_pass http://127.0.0.1:$::port/long; }"
--- request
POST /v1/messages
{"model":"client-model","max_tokens":12,"stream":true,"messages":[]}
--- response_body_filters eval
\&AOStreamTest::text_of
--- response_body eval
Encode::encode_utf8(('漢🙂"'."\\\n\t") x 20000)
--- error_code: 200

=== TEST 5: alternating real upstream behavior
--- config eval
"location /v1/messages { $::base proxy_pass http://127.0.0.1:$::port/alternating; }"
--- request
POST /v1/messages
{"model":"client-model","max_tokens":12,"stream":true,"messages":[]}
--- response_body_filters eval
\&AOStreamTest::text_of
--- response_body eval
Encode::encode_utf8(CORE::join '',map {$_%2==0 ? 'x'x15000 : '小'} 0..39)
--- error_code: 200

=== TEST 6: json real upstream behavior
--- config eval
"location /v1/messages { $::base proxy_pass http://127.0.0.1:$::port/json; }"
--- request
POST /v1/messages
{"model":"client-model","max_tokens":12,"stream":true,"messages":[]}
--- response_body_filters eval
\&AOStreamTest::text_of
--- response_body eval
"synthetic"
--- error_code: 200

=== TEST 7: json-large real upstream behavior
--- config eval
"location /v1/messages { $::base proxy_pass http://127.0.0.1:$::port/json-large; }"
--- request
POST /v1/messages
{"model":"client-model","max_tokens":12,"stream":true,"messages":[]}
--- response_body_filters eval
\&AOStreamTest::text_of
--- response_body eval
Encode::encode_utf8(('漢🙂"'."\\\n\t") x 20000)
--- error_code: 200

=== TEST 8: json-empty real upstream behavior
--- config eval
"location /v1/messages { $::base proxy_pass http://127.0.0.1:$::port/json-empty; }"
--- request
POST /v1/messages
{"model":"client-model","max_tokens":12,"stream":true,"messages":[]}
--- response_body_filters eval
\&AOStreamTest::text_of
--- response_body eval
""
--- error_code: 200

=== TEST 9: empty real upstream behavior
--- config eval
"location /v1/messages { $::base proxy_pass http://127.0.0.1:$::port/empty; }"
--- request
POST /v1/messages
{"model":"client-model","max_tokens":12,"stream":true,"messages":[]}
--- response_body_filters eval
\&AOStreamTest::text_of
--- response_body eval
""
--- error_code: 200

=== TEST 10: usage real upstream behavior
--- config eval
"location /v1/messages { $::base proxy_pass http://127.0.0.1:$::port/usage; }"
--- request
POST /v1/messages
{"model":"client-model","max_tokens":12,"stream":true,"messages":[]}
--- response_body_filters eval
\&AOStreamTest::usage_of
--- response_body eval
"17,23"
--- error_code: 200

=== TEST 11: finish-eof real upstream behavior
--- config eval
"location /v1/messages { $::base proxy_pass http://127.0.0.1:$::port/finish-eof; }"
--- request
POST /v1/messages
{"model":"client-model","max_tokens":12,"stream":true,"messages":[]}
--- response_body_filters eval
\&AOStreamTest::text_of
--- response_body eval
"finished"
--- error_code: 200

=== TEST 12: premature real upstream behavior
--- config eval
"location /v1/messages { $::base proxy_pass http://127.0.0.1:$::port/premature; }"
--- request
POST /v1/messages
{"model":"client-model","max_tokens":12,"stream":true,"messages":[]}
--- response_body_filters eval
\&AOStreamTest::error_of
--- response_body eval
"error"
--- error_code: 200

=== TEST 13: timeout real upstream behavior
--- config eval
"location /v1/messages { $::base proxy_pass http://127.0.0.1:$::port/timeout; }"
--- request
POST /v1/messages
{"model":"client-model","max_tokens":12,"stream":true,"messages":[]}
--- response_body_filters eval
\&AOStreamTest::error_of
--- response_body eval
"error"
--- error_code: 200

=== TEST 14: partial real upstream behavior
--- config eval
"location /v1/messages { $::base proxy_pass http://127.0.0.1:$::port/partial; }"
--- request
POST /v1/messages
{"model":"client-model","max_tokens":12,"stream":true,"messages":[]}
--- response_body_filters eval
\&AOStreamTest::error_of
--- response_body eval
"error"
--- error_code: 200

=== TEST 15: error real upstream behavior
--- config eval
"location /v1/messages { $::base proxy_pass http://127.0.0.1:$::port/error; }"
--- request
POST /v1/messages
{"model":"client-model","max_tokens":12,"stream":true,"messages":[]}
--- response_body_filters eval
\&AOStreamTest::error_of
--- response_body eval
"error"
--- error_code: 200

=== TEST 16: json-invalid real upstream behavior
--- config eval
"location /v1/messages { $::base proxy_pass http://127.0.0.1:$::port/json-invalid; }"
--- request
POST /v1/messages
{"model":"client-model","max_tokens":12,"stream":true,"messages":[]}
--- response_body_filters eval
\&AOStreamTest::error_of
--- response_body eval
"error"
--- error_code: 200

=== TEST 17: json-incomplete must use held-stream SSE error contract
--- config eval
"location /v1/messages { $::base proxy_pass http://127.0.0.1:$::port/json-incomplete; }"
--- request
POST /v1/messages
{"model":"client-model","max_tokens":12,"stream":true,"messages":[]}
--- response_body_filters eval
\&AOStreamTest::error_of
--- response_body: error
--- error_code: 200

=== TEST 18: json-timeout must use held-stream SSE error contract
--- config eval
"location /v1/messages { $::base proxy_pass http://127.0.0.1:$::port/json-timeout; }"
--- request
POST /v1/messages
{"model":"client-model","max_tokens":12,"stream":true,"messages":[]}
--- response_body_filters eval
\&AOStreamTest::error_of
--- response_body: error
--- error_code: 200

=== TEST 19: json-oversize must use held-stream SSE error contract
--- config eval
"location /v1/messages { $::base proxy_pass http://127.0.0.1:$::port/json-oversize; }"
--- request
POST /v1/messages
{"model":"client-model","max_tokens":12,"stream":true,"messages":[]}
--- response_body_filters eval
\&AOStreamTest::error_of
--- response_body: error
--- error_code: 200

=== TEST 20: timeout after dispatched finish still emits error without successful message_delta
--- config eval
"location /v1/messages { $::base proxy_pass http://127.0.0.1:$::port/timeout-finish; }"
--- request
POST /v1/messages
{"model":"client-model","max_tokens":12,"stream":true,"messages":[]}
--- response_body_filters eval
\&AOStreamTest::error_of
--- response_body: error
--- error_code: 200
