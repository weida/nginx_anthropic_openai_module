CC ?= gcc
CFLAGS ?= -std=c89 -pedantic -Wall -Wdeclaration-after-statement
INCS = -I src -I deps/cJSON
CONV_SRC = src/ngx_http_anthropic_openai_json.c \
           src/ngx_http_anthropic_openai_req.c \
           src/ngx_http_anthropic_openai_sse.c \
           deps/cJSON/cJSON.c

NGX_SRC ?= $(CURDIR)/.build-src/nginx-1.26.3
export NGX_SRC
SKIP_MODULE_BUILD ?= 0
BUILDDIR ?= $(CURDIR)/build

.PHONY: test integration-test conv-test modules clean

test: conv-test stream-test schedule-test integration-test sdk-test slow-reader-test golden-test

integration-test: modules
	prove -v t/*.t

conv-test: t/conv_test
	./t/conv_test

t/conv_test: t/conv_test.c $(CONV_SRC) $(wildcard src/*.h) deps/cJSON/cJSON.h
	$(CC) $(CFLAGS) $(INCS) -DNGX_HTTP_AO_NO_NGX -o $@ t/conv_test.c $(CONV_SRC)

modules:
ifeq ($(SKIP_MODULE_BUILD),1)
	test -f "$(BUILDDIR)/ngx_http_anthropic_openai_module.so"
else
	$(MAKE) -C $(NGX_SRC) -f $(BUILDDIR)/Makefile modules
endif

clean:
	rm -f t/conv_test t/filter_schedule t/filter_schedule_asan t/filter_schedule_ubsan t/conv_test_asan t/conv_test_ubsan build/libao-conv.so

.PHONY: stream-test schedule-test sdk-test golden-test sanitize-test

build/libao-conv.so: $(CONV_SRC) $(wildcard src/*.h)
	mkdir -p build
	$(CC) $(CFLAGS) -shared -fPIC $(INCS) -DNGX_HTTP_AO_NO_NGX -o $@ $(CONV_SRC)

stream-test: build/libao-conv.so
	python3 t/stream_regression.py

schedule-test:
	NGX_SRC=$(NGX_SRC) CC=$(CC) t/build_schedule.sh
	./t/filter_schedule

sdk-test: modules
	node t/sdk/gateway-test.mjs

golden-test:
	python3 t/oracle/replay.py

# GCC ASan global registration keeps otherwise-unused nginx module metadata
# alive; disable global redzones for this isolated filter harness only.
.PHONY: ubsan-test asan-test
sanitize-test: ubsan-test asan-test

ubsan-test:
	SCHEDULE_OUTPUT=t/filter_schedule_ubsan SCHEDULE_CFLAGS='-std=c89 -pedantic -Wall -Wno-unused-parameter -g -O1 -fsanitize=undefined -fno-sanitize-recover=all' t/build_schedule.sh
	./t/filter_schedule_ubsan
	$(CC) $(CFLAGS) -g -O1 -fsanitize=undefined -fno-sanitize-recover=all $(INCS) -o t/conv_test_ubsan t/conv_test.c $(CONV_SRC)
	./t/conv_test_ubsan

asan-test:
	SCHEDULE_OUTPUT=t/filter_schedule_asan SCHEDULE_CFLAGS='-std=c89 -pedantic -Wall -Wno-unused-parameter -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer --param asan-globals=0' t/build_schedule.sh
	$(CC) $(CFLAGS) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer $(INCS) -o t/conv_test_asan t/conv_test.c $(CONV_SRC)
	ASAN_OPTIONS=detect_leaks=1 ./t/filter_schedule_asan
	ASAN_OPTIONS=detect_leaks=1 ./t/conv_test_asan

.PHONY: slow-reader-test
slow-reader-test: modules
	python3 t/slow_reader.py
