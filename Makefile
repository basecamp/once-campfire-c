# Campfire C port build (task F03).
#
# Targets
#   make                 dev build   (clang -O2, warnings as errors) -> build/dev
#   make bench           optimized   (clang -O3 + LTO)               -> build/bench
#   make filc            Fil-C toolchain per vendor/DEPS.json        -> build/filc
#   make test [MODE=dev|bench|filc]  unit tests, each its own binary
#   make sanitize        ASan+UBSan focused suite                    -> build/sanitize
#   make tsan            TSan run: threaded cases + app/config (never combined) -> build/tsan
#   make deps            explicit dependency fetch/install scripts; ordinary
#                        builds never fetch (01-foundation-http.md F00)
#   make clean           removes only this task's build outputs
#
# Strict flags apply to application code only; dependency archives are linked
# from vendor/DEPS.json's recorded artifacts with the recorded flags (SQLite
# FTS5 consumers link -lm). The integrator maintains the source/dependency
# lists below as modules land.

# ---------- application sources (integrator-maintained) ----------------------
APP_CORE_SRCS := \
	src/core/alloc.c \
	src/core/buffer.c \
	src/core/clock.c \
	src/core/error.c \
	src/core/random.c

APP_LIB_SRCS := $(APP_CORE_SRCS) \
	src/config.c \
	src/app.c \
	src/db/schema.c \
	src/db/reader.c \
	src/db/statements.c \
	src/http/params.c \
	src/views/escape.c

APP_SRCS := $(APP_LIB_SRCS) \
	src/main.c

# ---------- dependency artifacts (vendor/DEPS.json; integrator-maintained) ---
# SQLite: vendored amalgamation, FTS5 + threads; consumers link -lm (DEPS.json).
SQLITE_CLANG_LIB := vendor/build/sqlite-clang/libsqlite3.a
SQLITE_FILC_LIB := vendor/build/sqlite-filc/libsqlite3.a
SQLITE_INCLUDE := vendor/src/sqlite/sqlite-amalgamation-3530400
# yyjson: vendored amalgamation for H02's parameter JSON (DEPS.json).
YYJSON_CLANG_LIB := vendor/build/yyjson-clang/libyyjson.a
YYJSON_FILC_LIB := vendor/build/yyjson-filc/libyyjson.a
YYJSON_INCLUDE := vendor/src/yyjson/src
DEP_INCLUDES := -I$(SQLITE_INCLUDE) -I$(YYJSON_INCLUDE)

# ---------- toolchain -------------------------------------------------------
CLANG ?= clang
# Pinned Fil-C 0.685 pizfix driver (vendor/DEPS.json filc entry; vendor/README
# section 2). Every -filc dependency archive was built with this exact binary.
FILC ?= /home/msaraiva/.local/fil-c/0.685/filc-0.685-linux-x86_64/build/bin/filcc

STRICT_FLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE \
	-Wall -Wextra -Werror -pthread
APP_CPPFLAGS := -Isrc -Itests

# ---------- build variants --------------------------------------------------
MODE ?= dev
ifeq ($(MODE),dev)
	MODE_CC := $(CLANG)
	MODE_CFLAGS := -O2
	MODE_LDFLAGS :=
	MODE_DEP_LIBS := $(SQLITE_CLANG_LIB) $(YYJSON_CLANG_LIB)
else ifeq ($(MODE),bench)
	MODE_CC := $(CLANG)
	MODE_CFLAGS := -O3 -flto
	MODE_LDFLAGS := -flto
	MODE_DEP_LIBS := $(SQLITE_CLANG_LIB) $(YYJSON_CLANG_LIB)
else ifeq ($(MODE),filc)
	MODE_CC := $(FILC)
	MODE_CFLAGS := -O2
	MODE_LDFLAGS :=
	MODE_DEP_LIBS := $(SQLITE_FILC_LIB) $(YYJSON_FILC_LIB)
else ifeq ($(MODE),sanitize)
	MODE_CC := $(CLANG)
	MODE_CFLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
	MODE_LDFLAGS := -fsanitize=address,undefined
	MODE_DEP_LIBS := $(SQLITE_CLANG_LIB) $(YYJSON_CLANG_LIB)
else ifeq ($(MODE),tsan)
	MODE_CC := $(CLANG)
	MODE_CFLAGS := -O1 -g -fsanitize=thread -fno-omit-frame-pointer
	MODE_LDFLAGS := -fsanitize=thread
	MODE_DEP_LIBS := $(SQLITE_CLANG_LIB) $(YYJSON_CLANG_LIB)
else
$(error unknown MODE '$(MODE)': use dev, bench, filc, sanitize or tsan)
endif

BUILD_ROOT := build
BUILD := $(BUILD_ROOT)/$(MODE)
MODE_OBJ := $(BUILD)/obj
TESTS_DIR := $(BUILD)/tests
BIN := $(BUILD)/campfire

APP_OBJS := $(patsubst %.c,$(MODE_OBJ)/%.o,$(APP_SRCS))
APP_LIB_OBJS := $(patsubst %.c,$(MODE_OBJ)/%.o,$(APP_LIB_SRCS))

# Unit tests: one binary per listed tests/<area>/test_*.c, linked with the
# library sources. Explicit list per 01's build rules: the integrator adds an
# area here when that module and its sources land (wildcard discovery would
# build other tasks' in-flight tests before their modules exist). Integration
# cases (tests/integration/) are never part of `make test`.
UNIT_TEST_SRCS := \
	tests/core/test_buffer.c \
	tests/core/test_clock.c \
	tests/core/test_error.c \
	tests/core/test_random.c \
	tests/config/test_config.c \
	tests/app/test_app.c \
	tests/db/test_reader.c \
	tests/db/test_schema.c \
	tests/db/test_statements.c \
	tests/db/test_time.c \
	tests/http/test_params.c \
	tests/views/test_escape.c
UNIT_TEST_BINS := $(patsubst tests/%.c,$(TESTS_DIR)/%,$(UNIT_TEST_SRCS))

# Auxiliary test translation units: CF_TEST cases with no CF_TEST_MAIN(),
# linked into one named test binary. params_vectors.c holds H02's pinned
# vector corpus and belongs to tests/http/test_params.c.
TEST_AUX_SRCS := tests/http/params_vectors.c
TEST_AUX_OBJS := $(patsubst tests/%.c,$(MODE_OBJ)/tests/%.o,$(TEST_AUX_SRCS))

# TSan run of the threaded cases (buffer + worker join) and the app/config
# suite touched by the shutdown-path change.
TSAN_TEST_SRCS := tests/core/test_buffer.c tests/config/test_config.c \
	tests/app/test_app.c
TSAN_TEST_BINS := $(patsubst tests/%.c,$(TESTS_DIR)/%,$(TSAN_TEST_SRCS))

# Test translation units are compiled with -MMD -MP so header changes rebuild
# the affected test binaries; all test objects share the mode's object tree.
TEST_OBJS := $(patsubst tests/%.c,$(MODE_OBJ)/tests/%.o,$(UNIT_TEST_SRCS)) \
	$(TEST_AUX_OBJS)
TSAN_TEST_OBJS := $(patsubst tests/%.c,$(MODE_OBJ)/tests/%.o,$(TSAN_TEST_SRCS))

DEPS_SCRIPTS := $(sort $(wildcard vendor/scripts/*.sh))

ifeq ($(MODE),sanitize)
	RUN_ENV := ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1
else
	RUN_ENV :=
endif

# ---------- top-level targets ----------------------------------------------
.PHONY: all dev bench filc test sanitize tsan deps clean
.PHONY: build-app test-impl tsan-impl check-cc

all: dev

dev:
	+$(MAKE) MODE=dev build-app

bench:
	+$(MAKE) MODE=bench build-app

filc:
	+$(MAKE) MODE=filc build-app

VALID_TEST_MODES := dev bench filc
ifneq ($(filter $(MODE),$(VALID_TEST_MODES)),)
TEST_MODE_OK := 1
endif

test:
ifdef TEST_MODE_OK
	+$(MAKE) MODE=$(MODE) test-impl
else
	@echo "error: make test MODE must be dev, bench or filc (got '$(MODE)');" >&2
	@echo "       use 'make sanitize' or 'make tsan' for sanitizer builds." >&2
	@exit 2
endif

sanitize:
	+$(MAKE) MODE=sanitize test-impl

tsan:
	+$(MAKE) MODE=tsan tsan-impl

deps:
	@set -e; for script in $(DEPS_SCRIPTS); do \
		echo "== $$script"; \
		if [ -x "$$script" ]; then \
			"$$script"; \
		else \
			bash "$$script"; \
		fi; \
	done; \
	echo "make deps: all pinned fetch/install scripts verified (idempotent)"

# The vendor scripts declare Bash (some use ${BASH_SOURCE[0]}), so they are
# never run through `sh`: executable scripts are executed directly so their
# shebang decides the interpreter, and non-executable ones go through bash.

# Removes only the outputs this Makefile owns: other tasks' build trees
# (build/f01, ...) are left untouched in this shared checkout.
CLEAN_DIRS := $(BUILD_ROOT)/dev $(BUILD_ROOT)/bench $(BUILD_ROOT)/filc \
	$(BUILD_ROOT)/sanitize $(BUILD_ROOT)/tsan
clean:
	rm -rf $(CLEAN_DIRS)

# ---------- build rules ----------------------------------------------------
build-app: $(BIN)

# Missing dependency artifacts fail with the recorded recipe instead of
# silently skipping (07-verification.md: missing prerequisites fail).
$(SQLITE_CLANG_LIB) $(SQLITE_FILC_LIB) $(YYJSON_CLANG_LIB) $(YYJSON_FILC_LIB):
	@echo "error: missing dependency artifact '$@'" >&2
	@echo "       build it with the recorded F00 recipe in vendor/DEPS.json" >&2
	@echo "       (see vendor/README.md); ordinary builds never fetch." >&2
	@exit 1

check-cc:
ifeq ($(MODE),filc)
	@test -x "$(FILC)" || { \
		echo "error: Fil-C compiler not found at $(FILC)" >&2; \
		echo "       run vendor/scripts/filc.sh first (idempotent)." >&2; \
		exit 1; \
	}
else
	@command -v $(CLANG) >/dev/null 2>&1 || test -x "$(CLANG)" || { \
		echo "error: C compiler '$(CLANG)' not found" >&2; exit 1; \
	}
endif

$(BIN): $(APP_OBJS) $(MODE_DEP_LIBS) | check-cc
	$(MODE_CC) $(STRICT_FLAGS) $(MODE_CFLAGS) $(MODE_LDFLAGS) \
		$(APP_OBJS) $(MODE_DEP_LIBS) -lm -o $@

$(MODE_OBJ)/%.o: %.c
	@mkdir -p $(dir $@)
	$(MODE_CC) $(STRICT_FLAGS) $(MODE_CFLAGS) $(APP_CPPFLAGS) \
		$(DEP_INCLUDES) -MMD -MP -c $< -o $@

$(MODE_OBJ)/tests/%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(MODE_CC) $(STRICT_FLAGS) $(MODE_CFLAGS) $(APP_CPPFLAGS) \
		$(DEP_INCLUDES) -MMD -MP -c $< -o $@

$(TESTS_DIR)/%: $(MODE_OBJ)/tests/%.o $(APP_LIB_OBJS) $(MODE_DEP_LIBS) | check-cc
	@mkdir -p $(dir $@)
	$(MODE_CC) $(STRICT_FLAGS) $(MODE_CFLAGS) $(MODE_LDFLAGS) $< \
		$(APP_LIB_OBJS) $(MODE_DEP_LIBS) -lm -o $@

# tests/http/test_params is the one binary with an auxiliary case translation
# unit (params_vectors.c); its explicit rule overrides the generic one above.
$(TESTS_DIR)/http/test_params: $(MODE_OBJ)/tests/http/test_params.o \
		$(MODE_OBJ)/tests/http/params_vectors.o $(APP_LIB_OBJS) \
		$(MODE_DEP_LIBS) | check-cc
	@mkdir -p $(dir $@)
	$(MODE_CC) $(STRICT_FLAGS) $(MODE_CFLAGS) $(MODE_LDFLAGS) \
		$(MODE_OBJ)/tests/http/test_params.o \
		$(MODE_OBJ)/tests/http/params_vectors.o \
		$(APP_LIB_OBJS) $(MODE_DEP_LIBS) -lm -o $@

# ---------- test execution -------------------------------------------------
# Every test binary runs even when an earlier one fails; the aggregate exit
# is nonzero if any binary, or the app checks, failed.
test-impl: $(BIN) $(UNIT_TEST_BINS) $(TEST_OBJS)
	@fail=0; \
	for t in $(UNIT_TEST_BINS); do \
		echo "-- $$t"; \
		$(RUN_ENV) "$$t" || fail=1; \
	done; \
	echo "-- $(BIN) --version"; \
	$(BIN) --version | grep -q '^campfire ' || fail=1; \
	echo "-- $(BIN) --help"; \
	$(BIN) --help | grep -q '^usage: campfire ' || fail=1; \
	if out=$$(env -i "$(BIN)" 2>&1); then \
		echo "bad-config startup unexpectedly succeeded: $$out"; fail=1; \
	else \
		echo "$$out" | grep -q PUBLIC_ORIGIN || { \
			echo "bad-config startup did not name PUBLIC_ORIGIN: $$out"; \
			fail=1; \
		}; \
	fi; \
	if [ $$fail -ne 0 ]; then \
		echo "unit tests ($(MODE)): FAILED"; \
		exit 1; \
	fi; \
	echo "unit tests ($(MODE)): all passed"

tsan-impl: $(TSAN_TEST_BINS) $(TSAN_TEST_OBJS)
	@fail=0; \
	for t in $(TSAN_TEST_BINS); do \
		echo "-- $$t"; \
		$$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then \
		echo "tsan threaded tests ($(MODE)): FAILED"; \
		exit 1; \
	fi; \
	echo "tsan threaded tests ($(MODE)): all passed"

# Header dependencies (-MMD -MP).
-include $(APP_OBJS:.o=.d)
-include $(TEST_OBJS:.o=.d) $(TSAN_TEST_OBJS:.o=.d)
