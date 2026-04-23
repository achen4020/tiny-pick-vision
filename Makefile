# Toolchains
CC_HOST   ?= cc
CC_TARGET ?= armv7a-linux-androideabi24-clang
AR_TARGET ?= llvm-ar
STRIP     ?= llvm-strip

# Flags
CFLAGS_COMMON  = -std=c11 -Wall -Wextra -Werror -Wpedantic -Iinclude -MMD -MP
CFLAGS_HOST    = $(CFLAGS_COMMON) -g -O0 -DTPV_HOST_BUILD -D_POSIX_C_SOURCE=200809L
CFLAGS_TARGET  = $(CFLAGS_COMMON) -Os -flto -ffreestanding -fno-exceptions \
                 -fno-asynchronous-unwind-tables -fomit-frame-pointer

SRCS = src/threshold.c src/ccl_moments.c src/shape_features.c \
       src/classifier.c src/pose.c src/pipeline.c src/platform_glue.c \
       src/fixed_math.c

HOST_OBJS = $(patsubst src/%.c,build/host/%.o,$(SRCS))

TEST_FILES = $(wildcard tests/test_*.c)
TEST_BINS  = $(patsubst tests/%.c,build/%,$(TEST_FILES))

.PHONY: host target test size check-layout check-layout-target clean

host: build/libtpv-host.a
target: build/libtpv-arm.so

test: $(TEST_BINS)
	@fail=0; for b in $(TEST_BINS); do \
	   echo "=== $$b ==="; \
	   $$b || fail=1; \
	 done; exit $$fail

build:
	mkdir -p $@

build/host:
	mkdir -p $@

build/host/%.o: src/%.c | build/host
	$(CC_HOST) $(CFLAGS_HOST) -c $< -o $@

build/libtpv-host.a: $(HOST_OBJS) | build
	ar rcs $@ $(HOST_OBJS)

build/libtpv-arm.so: $(SRCS) src/model_data.c | build
	$(CC_TARGET) $(CFLAGS_TARGET) -shared -o $@ $(SRCS) src/model_data.c

build/test_%: tests/test_%.c tests/testlib.c $(SRCS) tests/stub_model_data.c | build
	$(CC_HOST) $(CFLAGS_HOST) -o $@ $^ -lm

build/replay: tools/replay.c $(SRCS) tests/stub_model_data.c | build
	$(CC_HOST) $(CFLAGS_HOST) -o $@ $^ -lm

check-layout: tests/check_layout.c | build
	$(CC_HOST) $(CFLAGS_HOST) -c $< -o build/check_layout.o
	@echo "OK: tpv_Blob layout assert held under host toolchain"

# HG5: enforce sizeof(tpv_Blob) == 80 under the actual ARM AAPCS target ABI.
# Requires Android NDK on PATH; install with:
#   sdkmanager "ndk;26.1.10909125"  (or any 24+)
#   PATH="$NDK/toolchains/llvm/prebuilt/<host>/bin:$PATH"
check-layout-target: tests/check_layout.c | build
	$(CC_TARGET) $(CFLAGS_TARGET) -c $< -o build/check_layout_arm.o
	@echo "OK: tpv_Blob 80B / 8B-align under armv7a-linux-androideabi"

size: build/libtpv-arm.so
	$(STRIP) --strip-all $<
	@file_sz=$$(wc -c < $<); \
	 echo "--- section breakdown (diagnostic) ---"; \
	 llvm-size --format=sysv $< | awk '/^\.[a-z]/ { printf "  %-20s %8d\n", $$1, $$2 }'; \
	 echo "--- final stripped file size = $$file_sz B (limit=20480) ---"; \
	 if [ $$file_sz -gt 20480 ]; then echo "FAIL: $$file_sz > 20480"; exit 1; \
	 else echo "OK: $$file_sz ≤ 20480"; fi

clean:
	rm -rf build

-include $(HOST_OBJS:.o=.d) $(TEST_BINS:=.d)
