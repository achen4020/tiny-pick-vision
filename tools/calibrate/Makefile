# tools/calibrate/Makefile — calibration tool, isolated from runtime build
ROOT    := $(abspath ../..)
CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -I$(ROOT)/include -g -O0 \
           -D_POSIX_C_SOURCE=200809L -DTPV_CAL_BUILD
LDFLAGS := -lm

# calibrate.c carries main() — split it from the reusable lib so test binaries
# can link the lib without colliding with their own main.
CAL_LIB_SRCS = stats.c separability.c codegen.c frame_io.c cal_stub.c
CAL_MAIN_SRC = calibrate.c

# The runtime feature extractor is shared verbatim — calibration and the
# embedded module compute features the same way.
SHARED  = $(ROOT)/src/threshold.c $(ROOT)/src/ccl_moments.c \
          $(ROOT)/src/shape_features.c $(ROOT)/src/fixed_math.c

TEST_FILES = $(wildcard tests/test_*.c)
TEST_BINS  = $(patsubst tests/%.c,build/%,$(TEST_FILES))

.PHONY: all test clean
all:  build/calibrate
test: $(TEST_BINS)
	@fail=0; for b in $(TEST_BINS); do echo "=== $$b ==="; $$b || fail=1; done; exit $$fail

build:
	mkdir -p $@

build/calibrate: $(CAL_LIB_SRCS) $(CAL_MAIN_SRC) $(SHARED) | build
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

build/test_%: tests/test_%.c tests/testlib_cal.c $(CAL_LIB_SRCS) $(SHARED) | build
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -rf build
