# Toolchains
CC_HOST   ?= cc
CC_TARGET ?= armv7a-linux-androideabi24-clang
AR_TARGET ?= llvm-ar
STRIP     ?= llvm-strip

# arm64 Android debug .so for bench test APP (not a production deliverable)
CC_TARGET_ARM64 ?= aarch64-linux-android24-clang

# Flags
CFLAGS_COMMON  = -std=c11 -Wall -Wextra -Werror -Wpedantic -Iinclude -MMD -MP
CFLAGS_HOST    = $(CFLAGS_COMMON) -g -O0 -DTPV_HOST_BUILD -D_POSIX_C_SOURCE=200809L
CFLAGS_TARGET  = $(CFLAGS_COMMON) -Os -flto -ffreestanding -fno-exceptions \
                 -fno-asynchronous-unwind-tables -fomit-frame-pointer -fPIC

# Reuse the target flags but turn on debug features and drop -flto (since
# the .so isn't going through the size gate — keep link-time errors easy to
# read instead).
CFLAGS_TARGET_ARM64 = $(CFLAGS_COMMON) -Os -ffreestanding -fno-exceptions \
                      -fno-asynchronous-unwind-tables -fomit-frame-pointer \
                      -fPIC -DTPV_DEBUG_FEATURES

# Pick the right SHA-256 tool (macOS has shasum, most Linux has sha256sum).
SHA256_CMD := $(shell command -v shasum >/dev/null 2>&1 && echo "shasum -a 256" || echo "sha256sum")

SRCS = src/threshold.c src/ccl_moments.c src/shape_features.c \
       src/classifier.c src/pose.c src/pipeline.c src/platform_glue.c \
       src/fixed_math.c

HOST_OBJS = $(patsubst src/%.c,build/host/%.o,$(SRCS))

TEST_FILES = $(filter-out tests/test_debug_api.c,$(wildcard tests/test_*.c))
TEST_BINS  = $(patsubst tests/%.c,build/%,$(TEST_FILES)) build/test_debug_api

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

# test_debug_api needs TPV_DEBUG_FEATURES turned on so the debug
# function is visible. Everything else on the host toolchain.
build/test_debug_api: tests/test_debug_api.c tests/testlib.c $(SRCS) tests/stub_model_data.c | build
	$(CC_HOST) $(CFLAGS_HOST) -DTPV_DEBUG_FEATURES -o $@ $^ -lm

# Host-side regression replay MUST link the real calibrated model_data.c so
# the CSV represents actual production decisions. Linking tests/stub_model_data
# would make every frame land as REJECTED with confidence 0, turning the
# "bit-identical decision across hosts" diff meaningless. If src/model_data.c
# is absent, make stops with "No rule to make target" — the correct signal to
# run the calibration tool first.
build/replay: tools/replay.c $(SRCS) src/model_data.c | build
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

# arm64 debug .so. Links the real src/model_data.c (same as build/replay);
# missing model_data.c should fail with "No rule to make target".
build/libtpv-arm64-debug.so: $(SRCS) src/model_data.c | build
	$(CC_TARGET_ARM64) $(CFLAGS_TARGET_ARM64) -shared -o $@ $(SRCS) src/model_data.c

# android-so: place the .so under the APK's jniLibs/ and emit the model_data.c
# SHA-256 as an APK asset. tpv_jni.c is built by Android Studio's CMake and
# cannot share -D macros with libtpv.so, so the SHA travels via an asset
# file instead of a compile-time constant.
ANDROID_JNI_LIBS    = android/app/src/main/jniLibs/arm64-v8a
ANDROID_ASSETS      = android/app/src/main/assets
.PHONY: android-so android-apk
android-so: build/libtpv-arm64-debug.so
	mkdir -p $(ANDROID_JNI_LIBS) && \
	cp $< $(ANDROID_JNI_LIBS)/libtpv.so && \
	mkdir -p $(ANDROID_ASSETS) && \
	$(SHA256_CMD) src/model_data.c | awk '{print $$1}' > $(ANDROID_ASSETS)/tpv_model_sha.txt
	@echo "OK: libtpv.so at $(ANDROID_JNI_LIBS)/libtpv.so"
	@echo "OK: model sha at $(ANDROID_ASSETS)/tpv_model_sha.txt (`cat $(ANDROID_ASSETS)/tpv_model_sha.txt`)"

android-apk: android-so
	cd android && ./gradlew assembleDebug
	@echo "APK at android/app/build/outputs/apk/debug/app-debug.apk"

-include $(HOST_OBJS:.o=.d) $(TEST_BINS:=.d)
