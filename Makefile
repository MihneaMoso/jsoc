CC ?= gcc

# --- Flags -----------------------------------------------------------------
STD      = -std=c2x
WARN     = -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wvla -Wdouble-promotion
CPPFLAGS = -Iinclude

ifeq ($(PORTABLE),1)
  SIMD_FLAGS = -DJSOC_NO_SIMD
else
  SIMD_FLAGS =
endif

# Auto-vectorisation of short byte-scan loops is a pessimisation for JSON:
# it adds peel/remainder branches per token.  The explicit SIMD in simd.h is
# the only vector code we want.  VECTORIZE=1 re-enables the compiler's.
ifeq ($(VECTORIZE),1)
  VEC_FLAGS =
else
  VEC_FLAGS = -fno-tree-vectorize
endif

CFLAGS  = $(STD) $(WARN) $(CPPFLAGS) $(SIMD_FLAGS) $(VEC_FLAGS) -fPIC $(EXTRA_CFLAGS)
LDFLAGS =
LDLIBS  =

# --- Layout ----------------------------------------------------------------
BUILD_DIR   = build
SRC_DIR     = src
INCLUDE_DIR = include
TEST_DIR    = test
BENCH_DIR   = bench

SOURCES = $(SRC_DIR)/arena.c $(SRC_DIR)/error.c $(SRC_DIR)/token.c \
          $(SRC_DIR)/tokenizer.c $(SRC_DIR)/dom.c $(SRC_DIR)/path.c \
          $(SRC_DIR)/writer.c $(SRC_DIR)/serialize.c $(SRC_DIR)/cursor.c \
          $(SRC_DIR)/decode.c
OBJECTS = $(SOURCES:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
DEPS    = $(OBJECTS:.o=.d)

STATIC_LIB = $(BUILD_DIR)/libjsoc.a
SHARED_LIB = $(BUILD_DIR)/libjsoc.so

TEST_SOURCES = $(TEST_DIR)/test_tokenizer.c
TEST_BIN     = $(BUILD_DIR)/test_tokenizer

DOM_TEST_SOURCES = $(TEST_DIR)/test_dom.c
DOM_TEST_BIN     = $(BUILD_DIR)/test_dom

CORPUS_SOURCES = $(TEST_DIR)/corpus.c
CORPUS_BIN     = $(BUILD_DIR)/corpus

BENCH_SOURCES = $(BENCH_DIR)/bench.c
BENCH_BIN     = $(BUILD_DIR)/bench

TARGETS = $(STATIC_LIB) $(SHARED_LIB) $(TEST_BIN) $(DOM_TEST_BIN) $(CORPUS_BIN) $(BENCH_BIN)

# Default benchmark inputs (generated under /tmp/opencode).
BENCH_ARGS ?= /tmp/opencode/bench_compact.json /tmp/opencode/bench_pretty.json

.PHONY: all clean test bench install uninstall debug release release-native \
        asan asan-native tsan ubsan format check size FORCE

all: release

# --- Build configurations --------------------------------------------------
debug:         EXTRA_CFLAGS += -O0 -g3 -DDEBUG
release:       EXTRA_CFLAGS += -O3 -DNDEBUG
release-native: EXTRA_CFLAGS += -O3 -march=native -DNDEBUG

asan:          EXTRA_CFLAGS += -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
asan:          LDFLAGS      += -fsanitize=address,undefined
asan-native:   EXTRA_CFLAGS += -O1 -g -march=native -fsanitize=address,undefined -fno-omit-frame-pointer
asan-native:   LDFLAGS      += -fsanitize=address,undefined

tsan:          EXTRA_CFLAGS += -O1 -g -fsanitize=thread
tsan:          LDFLAGS      += -fsanitize=thread
ubsan:         EXTRA_CFLAGS += -O1 -g -fsanitize=undefined
ubsan:         LDFLAGS      += -fsanitize=undefined

debug release release-native asan asan-native tsan ubsan: $(TARGETS)

# --- Rules -----------------------------------------------------------------
# Stamp of the effective command line.  Changing optimisation target (or
# PORTABLE) invalidates every object, even though the build directory is
# shared.  The recipe only rewrites the file when the contents differ, so
# unchanged builds keep their timestamps.
$(BUILD_DIR)/.cflags: FORCE
	@mkdir -p $(BUILD_DIR)
	@printf '%s\n' '$(CC) $(CFLAGS) $(LDFLAGS)' | cmp -s - $@ 2>/dev/null || \
	  printf '%s\n' '$(CC) $(CFLAGS) $(LDFLAGS)' > $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(BUILD_DIR)/.cflags
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(STATIC_LIB): $(OBJECTS)
	@rm -f $@
	ar rcs $@ $^

$(SHARED_LIB): $(OBJECTS)
	$(CC) -shared -o $@ $^ $(LDFLAGS)

# The differential fuzz test includes tokenizer_internal.h to drive the SIMD
# and portable backends directly.
$(TEST_BIN): $(TEST_SOURCES) $(SRC_DIR)/tokenizer_internal.h $(STATIC_LIB) $(BUILD_DIR)/.cflags
	$(CC) $(CFLAGS) -I$(SRC_DIR) -o $@ $(TEST_SOURCES) $(STATIC_LIB) $(LDFLAGS) $(LDLIBS)

$(BENCH_BIN): $(BENCH_SOURCES) $(STATIC_LIB) $(BUILD_DIR)/.cflags
	$(CC) $(CFLAGS) -o $@ $(BENCH_SOURCES) $(STATIC_LIB) $(LDFLAGS) $(LDLIBS)

$(DOM_TEST_BIN): $(DOM_TEST_SOURCES) $(INCLUDE_DIR)/dom.h $(SRC_DIR)/dom_internal.h $(STATIC_LIB) $(BUILD_DIR)/.cflags
	$(CC) $(CFLAGS) -I$(SRC_DIR) -o $@ $(DOM_TEST_SOURCES) $(STATIC_LIB) $(LDFLAGS) $(LDLIBS)

$(CORPUS_BIN): $(CORPUS_SOURCES) $(SRC_DIR)/dom_internal.h $(STATIC_LIB) $(BUILD_DIR)/.cflags
	$(CC) $(CFLAGS) -I$(SRC_DIR) -o $@ $(CORPUS_SOURCES) $(STATIC_LIB) $(LDFLAGS) $(LDLIBS)

-include $(DEPS)

# --- Convenience -----------------------------------------------------------
test: release
	./$(TEST_BIN)
	./$(DOM_TEST_BIN)

bench: release
	./$(BENCH_BIN) $(BENCH_ARGS)

# Acceptance corpora.  Fetch them first (see README) or override CORPUS_ARGS.
CORPUS_ARGS ?= /tmp/opencode/simdjson-data/jsonchecker /tmp/opencode/JSONTestSuite/test_parsing
corpus: release
	./$(CORPUS_BIN) $(CORPUS_ARGS)

check: asan
	./$(TEST_BIN)
	./$(DOM_TEST_BIN)

install: release
	install -d $(DESTDIR)/usr/local/include/jsoc
	install -m 644 $(INCLUDE_DIR)/*.h $(DESTDIR)/usr/local/include/jsoc/
	install -d $(DESTDIR)/usr/local/lib
	install -m 644 $(STATIC_LIB) $(DESTDIR)/usr/local/lib/
	install -m 755 $(SHARED_LIB) $(DESTDIR)/usr/local/lib/
	install -d $(DESTDIR)/usr/local/lib/pkgconfig
	@echo 'prefix=/usr/local' > $(BUILD_DIR)/jsoc.pc
	@echo 'exec_prefix=$${prefix}' >> $(BUILD_DIR)/jsoc.pc
	@echo 'libdir=$${exec_prefix}/lib' >> $(BUILD_DIR)/jsoc.pc
	@echo 'includedir=$${prefix}/include' >> $(BUILD_DIR)/jsoc.pc
	@echo 'Name: jsoc' >> $(BUILD_DIR)/jsoc.pc
	@echo 'Description: Fast JSON tokenizer/decoder in C23' >> $(BUILD_DIR)/jsoc.pc
	@echo 'Version: 0.1.0' >> $(BUILD_DIR)/jsoc.pc
	@echo 'Libs: -L$${libdir} -ljsoc' >> $(BUILD_DIR)/jsoc.pc
	@echo 'Cflags: -I$${includedir}' >> $(BUILD_DIR)/jsoc.pc
	install -m 644 $(BUILD_DIR)/jsoc.pc $(DESTDIR)/usr/local/lib/pkgconfig/

uninstall:
	rm -f $(DESTDIR)/usr/local/include/jsoc/*.h
	rmdir $(DESTDIR)/usr/local/include/jsoc 2>/dev/null || true
	rm -f $(DESTDIR)/usr/local/lib/libjsoc.a
	rm -f $(DESTDIR)/usr/local/lib/libjsoc.so
	rm -f $(DESTDIR)/usr/local/lib/pkgconfig/jsoc.pc

clean:
	rm -rf $(BUILD_DIR)

FORMAT_SOURCES = $(SRC_DIR)/*.c $(INCLUDE_DIR)/*.h $(TEST_DIR)/*.c \
                 $(BENCH_DIR)/*.c
format:
	clang-format -i $(FORMAT_SOURCES) 2>/dev/null || true

size: release
	@size $(STATIC_LIB)
	@echo "--- Token struct size ---"
	@printf '#include "token.h"\n#include <stdio.h>\nint main(void){printf("jsoc_token = %%zu bytes\\n", sizeof(jsoc_token));return 0;}\n' \
	  | $(CC) $(CFLAGS) -x c - -o /tmp/jsoc_size_check && /tmp/jsoc_size_check

FORCE:
