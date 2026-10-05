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
	src/context.c \
	src/db/schema.c \
	src/db/reader.c \
	src/db/statements.c \
	src/db/writer.c \
	src/http/params.c \
	src/http/loop.c \
	src/http/request.c \
	src/http/response.c \
	src/http/output.c \
	src/routes.c \
	src/assets.c \
	src/views/escape.c \
	src/auth/before.c \
	src/auth/json.c \
	src/auth/password.c

# Remaining auth sources (crypto, message, tokens, password, session, rate)
# and the model families join APP_LIB_SRCS once R02's rich-text bridge exists
# (message.c references it); their tests link them explicitly meanwhile.
AUTH_ALL_SRCS := \
	src/auth/before.c \
	src/auth/crypto.c \
	src/auth/json.c \
	src/auth/message.c \
	src/auth/password.c \
	src/auth/rate.c \
	src/auth/session.c \
	src/auth/tokens.c

# Model sources (D01) are linked into the model test binaries and the
# application library once the A01/R02 boundary lands; see MODEL_TEST_BINS.
MODEL_SRCS := \
	src/models/types.c \
	src/models/account.c \
	src/models/active_storage.c \
	src/models/ban.c \
	src/models/boost.c \
	src/models/first_run.c \
	src/models/membership.c \
	src/models/message.c \
	src/models/push_subscription.c \
	src/models/rich_text_record.c \
	src/models/room.c \
	src/models/search.c \
	src/models/session.c \
	src/models/sound.c \
	src/models/user.c \
	src/models/webhook.c

# Test-only support doubles for the not-yet-landed A01/R02 boundaries
# (tests/models/support/, never part of the application library).
# password is A01 production code now (src/auth/password.c is model-free and
# in APP_LIB_SRCS); only the rich-text stand-in remains until R02 lands.
SUPPORT_SRCS := tests/models/support/richtext.c

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
# libxcrypt: bcrypt crypt_r for A01 and the model test-support password double.
LIBCRYPT_CLANG_LIB := vendor/build/libxcrypt-clang/.libs/libcrypt.a
LIBCRYPT_FILC_LIB := vendor/build/libxcrypt-filc/.libs/libcrypt.a
# OpenSSL: EVP/HMAC/KDF primitives for A01 (libcrypto only until P01 TLS).
OPENSSL_CLANG_LIB := vendor/build/openssl-clang/install/lib/libcrypto.a
OPENSSL_FILC_LIB := vendor/build/openssl-filc/install/lib/libcrypto.a
OPENSSL_CLANG_INCLUDE := -Ivendor/build/openssl-clang/install/include
OPENSSL_FILC_INCLUDE := -Ivendor/build/openssl-filc/install/include
# picohttpparser: single-file upstream parser (no upstream build system). It
# is compiled per mode but with upstream-appropriate flags only — never the
# application's -Werror (01-foundation-http.md F00 build restriction).
PICOHTTP_SRC := vendor/src/picohttpparser/picohttpparser.c
# recursive: MODE_OBJ is defined further down (build-variant section)
PICOHTTP_OBJ = $(MODE_OBJ)/vendor/picohttpparser.o
DEP_INCLUDES := -I$(SQLITE_INCLUDE) -I$(YYJSON_INCLUDE) -Ivendor/src/picohttpparser

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
	MODE_DEP_INCLUDES := -Ivendor/build/libxcrypt-clang $(OPENSSL_CLANG_INCLUDE)
	MODE_DEP_LIBS = $(SQLITE_CLANG_LIB) $(YYJSON_CLANG_LIB) $(LIBCRYPT_CLANG_LIB) $(OPENSSL_CLANG_LIB) $(PICOHTTP_OBJ)
else ifeq ($(MODE),bench)
	MODE_CC := $(CLANG)
	MODE_CFLAGS := -O3 -flto
	MODE_LDFLAGS := -flto
	MODE_DEP_INCLUDES := -Ivendor/build/libxcrypt-clang $(OPENSSL_CLANG_INCLUDE)
	MODE_DEP_LIBS = $(SQLITE_CLANG_LIB) $(YYJSON_CLANG_LIB) $(LIBCRYPT_CLANG_LIB) $(OPENSSL_CLANG_LIB) $(PICOHTTP_OBJ)
else ifeq ($(MODE),filc)
	MODE_CC := $(FILC)
	MODE_CFLAGS := -O2
	MODE_LDFLAGS :=
	MODE_DEP_INCLUDES := -Ivendor/build/libxcrypt-filc $(OPENSSL_FILC_INCLUDE)
	MODE_DEP_LIBS = $(SQLITE_FILC_LIB) $(YYJSON_FILC_LIB) $(LIBCRYPT_FILC_LIB) $(OPENSSL_FILC_LIB) $(PICOHTTP_OBJ)
else ifeq ($(MODE),sanitize)
	MODE_CC := $(CLANG)
	MODE_CFLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
	MODE_LDFLAGS := -fsanitize=address,undefined
	MODE_DEP_INCLUDES := -Ivendor/build/libxcrypt-clang $(OPENSSL_CLANG_INCLUDE)
	MODE_DEP_LIBS = $(SQLITE_CLANG_LIB) $(YYJSON_CLANG_LIB) $(LIBCRYPT_CLANG_LIB) $(OPENSSL_CLANG_LIB) $(PICOHTTP_OBJ)
else ifeq ($(MODE),tsan)
	MODE_CC := $(CLANG)
	MODE_CFLAGS := -O1 -g -fsanitize=thread -fno-omit-frame-pointer
	MODE_LDFLAGS := -fsanitize=thread
	MODE_DEP_INCLUDES := -Ivendor/build/libxcrypt-clang $(OPENSSL_CLANG_INCLUDE)
	MODE_DEP_LIBS = $(SQLITE_CLANG_LIB) $(YYJSON_CLANG_LIB) $(LIBCRYPT_CLANG_LIB) $(OPENSSL_CLANG_LIB) $(PICOHTTP_OBJ)
else
$(error unknown MODE '$(MODE)': use dev, bench, filc, sanitize or tsan)
endif
# Pinned libxcrypt headers for the mode (struct crypt_data layout must match
# the linked archive; support/password.c and A01 crypto include <crypt.h>).
DEP_INCLUDES += $(MODE_DEP_INCLUDES)
# Section folding: the live app links the routes/assets + auth subset that
# exists today; unreferenced functions (auth paths awaiting R02/models) must
# drop instead of causing undefined references. The full set joins with R02.
APP_CPPFLAGS += -ffunction-sections -fdata-sections
MODE_LDFLAGS += -Wl,--gc-sections

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
	tests/db/test_reader.c \
	tests/db/test_schema.c \
	tests/db/test_statements.c \
	tests/db/test_time.c \
	tests/http/test_params.c \
	tests/views/test_escape.c \
	tests/routes/test_routes_table.c \
	tests/routes/test_routes_recognition.c \
	tests/routes/test_builtins.c \
	tests/assets/test_assets.c
UNIT_TEST_BINS := $(patsubst tests/%.c,$(TESTS_DIR)/%,$(UNIT_TEST_SRCS))

# Auxiliary test translation units: CF_TEST cases with no CF_TEST_MAIN(),
# linked into one named test binary. params_vectors.c holds H02's pinned
# vector corpus and belongs to tests/http/test_params.c.
TEST_AUX_SRCS := tests/http/params_vectors.c
TEST_AUX_OBJS := $(patsubst tests/%.c,$(MODE_OBJ)/tests/%.o,$(TEST_AUX_SRCS))

# H01 HTTP test binaries: each links the shared tests/http/test_http_common.c
# auxiliary translation unit (no CF_TEST_MAIN) plus the library.
HTTP_TEST_SRCS := \
	tests/http/test_http_loop.c \
	tests/http/test_http_framing.c \
	tests/http/test_http_limits.c \
	tests/http/test_http_output.c \
	tests/http/test_http_file.c \
	tests/http/test_http_completion.c \
	tests/http/test_http_ipv6_host.c
HTTP_TEST_BINS := $(patsubst tests/%.c,$(TESTS_DIR)/%,$(HTTP_TEST_SRCS))
HTTP_COMMON_OBJ := $(MODE_OBJ)/tests/http/test_http_common.o

# D01 model test binaries: each links every model source (the model set is
# mutually dependent by design) plus the test-only support doubles for the
# not-yet-landed A01/R02 boundaries.
MODEL_TEST_SRCS := \
	tests/models/account_test.c \
	tests/models/active_storage_test.c \
	tests/models/ban_test.c \
	tests/models/boost_test.c \
	tests/models/first_run_test.c \
	tests/models/membership_test.c \
	tests/models/message_test.c \
	tests/models/push_subscription_test.c \
	tests/models/rich_text_record_test.c \
	tests/models/room_test.c \
	tests/models/search_test.c \
	tests/models/session_test.c \
	tests/models/sound_test.c \
	tests/models/user_test.c \
	tests/models/webhook_test.c
MODEL_TEST_BINS := $(patsubst tests/%.c,$(TESTS_DIR)/%,$(MODEL_TEST_SRCS))
MODEL_OBJS := $(patsubst %.c,$(MODE_OBJ)/%.o,$(MODEL_SRCS))
SUPPORT_OBJS := $(patsubst %.c,$(MODE_OBJ)/%.o,$(SUPPORT_SRCS))

# A00 app tests: the classic binaries link the tests/app/support route double
# (tests/app/support/route_double.c) instead of src/routes.c; the wiring binary
# links the real 177-row table. src/assets.c stays in both (dispatch calls the
# front mount). Auth test binaries link every model + auth source plus the
# model support doubles, mirroring tests/auth/tools/build.sh.
APP_DOUBLE_TEST_SRCS := \
	tests/app/test_app.c \
	tests/app/test_app_pool.c \
	tests/app/test_context.c \
	tests/app/test_formats.c
APP_DOUBLE_TEST_BINS := $(patsubst tests/%.c,$(TESTS_DIR)/%,$(APP_DOUBLE_TEST_SRCS))
APP_SERVE_TEST_SRC := tests/app/test_serve.c
APP_SERVE_TEST_BIN := $(patsubst tests/%.c,$(TESTS_DIR)/%,$(APP_SERVE_TEST_SRC))
APP_WIRING_TEST_SRC := tests/app/test_dispatch_wiring.c
APP_WIRING_TEST_BIN := $(patsubst tests/%.c,$(TESTS_DIR)/%,$(APP_WIRING_TEST_SRC))
APP_DOUBLE_OBJ := $(MODE_OBJ)/tests/app/support/route_double.o
APP_HARNESS_OBJ := $(MODE_OBJ)/tests/app/support/serve_harness.o
APP_NOROUTE_OBJS := $(filter-out $(MODE_OBJ)/src/routes.o,$(APP_LIB_OBJS))
APP_ALL_TEST_BINS := $(APP_DOUBLE_TEST_BINS) $(APP_SERVE_TEST_BIN) $(APP_WIRING_TEST_BIN)

AUTH_TEST_SRCS := \
	tests/auth/test_before.c \
	tests/auth/test_crypto.c \
	tests/auth/test_password.c \
	tests/auth/test_rate.c \
	tests/auth/test_session.c \
	tests/auth/test_tokens.c
AUTH_TEST_BINS := $(patsubst tests/%.c,$(TESTS_DIR)/%,$(AUTH_TEST_SRCS))
AUTH_OBJS := $(patsubst %.c,$(MODE_OBJ)/%.o,$(AUTH_ALL_SRCS))
# Auth test links carry every auth source, so remove the two auth objects that
# are already inside APP_LIB_OBJS (and routes.o, replaced by the double).
APP_AUTH_BASE_OBJS := $(filter-out $(MODE_OBJ)/src/routes.o \
	$(MODE_OBJ)/src/auth/before.o $(MODE_OBJ)/src/auth/json.o \
	$(MODE_OBJ)/src/auth/password.o,$(APP_LIB_OBJS))

# TSan run of the threaded cases (buffer + worker join, writer queue, HTTP
# completion queue) and the app/config suite touched by the shutdown path.
TSAN_TEST_SRCS := tests/core/test_buffer.c tests/config/test_config.c \
	tests/db/test_writer.c
TSAN_TEST_BINS := $(patsubst tests/%.c,$(TESTS_DIR)/%,$(TSAN_TEST_SRCS))
TSAN_HTTP_TEST_SRCS := tests/http/test_http_completion.c
TSAN_HTTP_TEST_BINS := $(patsubst tests/%.c,$(TESTS_DIR)/%,$(TSAN_HTTP_TEST_SRCS))
# Threaded A00 binaries; built by the app static-pattern rules above.
TSAN_APP_TEST_BINS := $(TESTS_DIR)/app/test_app_pool \
	$(TESTS_DIR)/app/test_formats $(TESTS_DIR)/app/test_serve

# Test translation units are compiled with -MMD -MP so header changes rebuild
# the affected test binaries; all test objects share the mode's object tree.
TEST_OBJS := $(patsubst tests/%.c,$(MODE_OBJ)/tests/%.o,$(UNIT_TEST_SRCS)) \
	$(TEST_AUX_OBJS) $(patsubst tests/%.c,$(MODE_OBJ)/tests/%.o,$(HTTP_TEST_SRCS)) \
	$(patsubst tests/%.c,$(MODE_OBJ)/tests/%.o,$(MODEL_TEST_SRCS))
TSAN_TEST_OBJS := $(patsubst tests/%.c,$(MODE_OBJ)/tests/%.o,$(TSAN_TEST_SRCS)) \
	$(patsubst tests/%.c,$(MODE_OBJ)/tests/%.o,$(TSAN_HTTP_TEST_SRCS))

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
$(SQLITE_CLANG_LIB) $(SQLITE_FILC_LIB) $(YYJSON_CLANG_LIB) $(YYJSON_FILC_LIB) \
$(LIBCRYPT_CLANG_LIB) $(LIBCRYPT_FILC_LIB) $(OPENSSL_CLANG_LIB) $(OPENSSL_FILC_LIB):
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

# Upstream single-file dependency: upstream flags only, no -Werror.
$(PICOHTTP_OBJ): $(PICOHTTP_SRC)
	@mkdir -p $(dir $@)
	$(MODE_CC) -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -pthread \
		$(MODE_CFLAGS) -MMD -MP -c $< -o $@

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

# H01 binaries share the test_http_common.c auxiliary translation unit.
$(HTTP_TEST_BINS): $(TESTS_DIR)/http/%: $(MODE_OBJ)/tests/http/%.o \
		$(HTTP_COMMON_OBJ) $(APP_LIB_OBJS) $(MODE_DEP_LIBS) | check-cc
	@mkdir -p $(dir $@)
	$(MODE_CC) $(STRICT_FLAGS) $(MODE_CFLAGS) $(MODE_LDFLAGS) \
		$(MODE_OBJ)/tests/http/$*.o $(HTTP_COMMON_OBJ) \
		$(APP_LIB_OBJS) $(MODE_DEP_LIBS) -lm -o $@

# D01 model binaries link all model sources plus the support doubles.
$(MODEL_TEST_BINS): $(TESTS_DIR)/models/%: $(MODE_OBJ)/tests/models/%.o \
		$(MODEL_OBJS) $(SUPPORT_OBJS) $(APP_LIB_OBJS) $(MODE_DEP_LIBS) | check-cc
	@mkdir -p $(dir $@)
	$(MODE_CC) $(STRICT_FLAGS) $(MODE_CFLAGS) $(MODE_LDFLAGS) \
		$(MODE_OBJ)/tests/models/$*.o $(MODEL_OBJS) $(SUPPORT_OBJS) \
		$(APP_LIB_OBJS) $(MODE_DEP_LIBS) -lm -o $@

# A00 app binaries: the classic set uses the route double instead of routes.c.
$(APP_DOUBLE_TEST_BINS): $(TESTS_DIR)/app/%: $(MODE_OBJ)/tests/app/%.o \
		$(APP_DOUBLE_OBJ) $(APP_NOROUTE_OBJS) $(MODE_DEP_LIBS) | check-cc
	@mkdir -p $(dir $@)
	$(MODE_CC) $(STRICT_FLAGS) $(MODE_CFLAGS) $(MODE_LDFLAGS) \
		$(MODE_OBJ)/tests/app/$*.o $(APP_DOUBLE_OBJ) \
		$(APP_NOROUTE_OBJS) $(MODE_DEP_LIBS) -lm -o $@

$(APP_SERVE_TEST_BIN): $(MODE_OBJ)/tests/app/test_serve.o \
		$(APP_DOUBLE_OBJ) $(APP_HARNESS_OBJ) $(APP_NOROUTE_OBJS) \
		$(MODE_DEP_LIBS) | check-cc
	@mkdir -p $(dir $@)
	$(MODE_CC) $(STRICT_FLAGS) $(MODE_CFLAGS) $(MODE_LDFLAGS) \
		$(MODE_OBJ)/tests/app/test_serve.o $(APP_DOUBLE_OBJ) \
		$(APP_HARNESS_OBJ) $(APP_NOROUTE_OBJS) $(MODE_DEP_LIBS) -lm -o $@

$(APP_WIRING_TEST_BIN): $(MODE_OBJ)/tests/app/test_dispatch_wiring.o \
		$(APP_HARNESS_OBJ) $(APP_LIB_OBJS) $(MODE_DEP_LIBS) | check-cc
	@mkdir -p $(dir $@)
	$(MODE_CC) $(STRICT_FLAGS) $(MODE_CFLAGS) $(MODE_LDFLAGS) \
		$(MODE_OBJ)/tests/app/test_dispatch_wiring.o $(APP_HARNESS_OBJ) \
		$(APP_LIB_OBJS) $(MODE_DEP_LIBS) -lm -o $@

# Auth binaries link all auth + model sources and the test doubles.
$(AUTH_TEST_BINS): $(TESTS_DIR)/auth/%: $(MODE_OBJ)/tests/auth/%.o \
		$(AUTH_OBJS) $(MODEL_OBJS) $(SUPPORT_OBJS) $(APP_DOUBLE_OBJ) \
		$(APP_AUTH_BASE_OBJS) $(MODE_DEP_LIBS) | check-cc
	@mkdir -p $(dir $@)
	$(MODE_CC) $(STRICT_FLAGS) $(MODE_CFLAGS) $(MODE_LDFLAGS) \
		$(MODE_OBJ)/tests/auth/$*.o $(AUTH_OBJS) $(MODEL_OBJS) \
		$(SUPPORT_OBJS) $(APP_DOUBLE_OBJ) $(APP_AUTH_BASE_OBJS) \
		$(MODE_DEP_LIBS) -lm -o $@

# ---------- test execution -------------------------------------------------
# Every test binary runs even when an earlier one fails; the aggregate exit
# is nonzero if any binary, or the app checks, failed.
test-impl: $(BIN) $(UNIT_TEST_BINS) $(HTTP_TEST_BINS) $(MODEL_TEST_BINS) \
		$(APP_ALL_TEST_BINS) $(AUTH_TEST_BINS) $(TEST_OBJS)
	@fail=0; \
	for t in $(UNIT_TEST_BINS) $(HTTP_TEST_BINS) $(MODEL_TEST_BINS) \
		$(APP_ALL_TEST_BINS) $(AUTH_TEST_BINS); do \
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

tsan-impl: $(TSAN_TEST_BINS) $(TSAN_HTTP_TEST_BINS) $(TSAN_APP_TEST_BINS) $(TSAN_TEST_OBJS)
	@fail=0; \
	for t in $(TSAN_TEST_BINS) $(TSAN_HTTP_TEST_BINS) $(TSAN_APP_TEST_BINS); do \
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
-include $(MODEL_OBJS:.o=.d) $(SUPPORT_OBJS:.o=.d) $(AUTH_OBJS:.o=.d)
-include $(MODE_OBJ)/tests/app/*.d $(MODE_OBJ)/tests/app/support/*.d \
	$(MODE_OBJ)/tests/auth/*.d
