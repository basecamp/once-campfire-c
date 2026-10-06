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

# D01 model sources, R02 rich text, A01 auth, and the transport/infra modules
# now all link into the application library (the R02 rich-text bridge resolved
# message.c's last boundary).
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
	src/models/touch.c \
	src/models/user.c \
	src/models/webhook.c

AUTH_SRCS := \
	src/auth/before.c \
	src/auth/crypto.c \
	src/auth/json.c \
	src/auth/message.c \
	src/auth/password.c \
	src/auth/platform.c \
	src/auth/rate.c \
	src/auth/session.c \
	src/auth/tokens.c \
	src/auth/user_agent.c

RICHTEXT_SRCS := \
	src/richtext/rt_attach.c \
	src/richtext/rt_autolink.c \
	src/richtext/rt_content.c \
	src/richtext/rt_dom.c \
	src/richtext/rt_pipeline.c \
	src/richtext/rt_plain.c \
	src/richtext/rt_resolver.c \
	src/richtext/rt_richtext.c \
	src/richtext/rt_sanitize.c \
	src/richtext/rt_uri.c \
	src/richtext/rt_util.c

CABLE_SRCS := src/cable/socket.c src/cable/protocol.c \
	src/cable/pubsub.c src/cable/channels.c src/cable/broadcasts.c \
	src/cable/revocation.c
JOBS_SRCS := src/jobs/queue.c src/jobs/handlers.c
STORAGE_SRCS := src/storage/files.c src/storage/process.c \
	src/storage/active_storage.c src/storage/media.c
INTEGRATIONS_SRCS := src/integrations/http.c src/integrations/unfurl.c \
	src/integrations/webhook.c src/integrations/push.c \
	src/integrations/host_resolve.c

# P01 front (configured TLS + HTTP/2): non-blocking handshake/ALPN policy
# (tls.c) and the nghttp2 session policy (h2.c). Both link into the
# application library; the loop seam in src/http/loop.c owns the wiring.
FRONT_SRCS := src/front/tls.c src/front/h2.c

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
	src/cache.c \
	src/encoding.c \
	src/gzip.c \
	src/views/escape.c \
	src/views/ctx.c \
	src/views/render.c \
	src/views/translations.c \
	src/views/view_assets.c \
	src/views/layout.c \
	src/views/session.c \
	src/views/first_run.c \
	src/views/welcome.c \
	src/views/rooms.c \
	src/views/messages.c \
	src/views/pwa.c \
	src/views/model.c \
	src/views/users_sidebars.c \
	src/views/searches.c \
	src/views/users_avatars.c \
	src/views/qr_svg.c \
	src/views/users.c \
	src/views/bots.c \
	src/views/accounts.c \
	src/views/users_profiles.c \
	src/views/users_push.c \
	src/views/rooms_forms.c \
	src/views/messages_json.c \
	src/presenters/accounts.c \
	src/presenters/layout.c \
	src/presenters/messages.c \
	src/presenters/rooms.c \
	src/presenters/sidebars.c \
	src/presenters/searches.c \
	src/presenters/bots.c \
	src/actions/first_runs.c \
	src/actions/messages.c \
	src/actions/messages/boosts.c \
	src/actions/messages/boosts/by_bots.c \
	src/actions/messages/by_bots.c \
	src/actions/unfurl_links.c \
	src/actions/pwa.c \
	src/actions/qr_code.c \
	src/actions/autocompletable/users.c \
	src/actions/sessions/transfers.c \
	src/actions/accounts.c \
	src/actions/accounts/users.c \
	src/actions/accounts/bots.c \
	src/actions/accounts/bots/keys.c \
	src/actions/accounts/join_codes.c \
	src/actions/accounts/logos.c \
	src/actions/accounts/custom_styles.c \
	src/actions/users.c \
	src/actions/users/avatars_destroy.c \
	src/actions/users/profiles.c \
	src/actions/users/push_subscriptions.c \
	src/actions/users/push_subscriptions/test_notifications.c \
	src/actions/rooms.c \
	src/actions/rooms/refreshes.c \
	src/actions/rooms/involvements.c \
	src/actions/rooms/opens.c \
	src/actions/rooms/closeds.c \
	src/actions/rooms/directs.c \
	src/actions/sessions.c \
	src/actions/users/bans.c \
	src/actions/users/sidebars.c \
	src/actions/searches.c \
	src/actions/active_storage/blobs.c \
	src/actions/active_storage/representations.c \
	src/actions/active_storage/disk.c \
	src/actions/active_storage/direct_uploads.c \
	src/cache_key.c \
	src/actions/users/avatars.c \
	src/actions/welcome.c \
	$(AUTH_SRCS) \
	$(MODEL_SRCS) \
	$(RICHTEXT_SRCS) \
	$(CABLE_SRCS) \
	$(JOBS_SRCS) \
	$(STORAGE_SRCS) \
	$(INTEGRATIONS_SRCS) \
	$(FRONT_SRCS)

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
# Gumbo: Nokogiri's C HTML5 parser for R02 richtext.
GUMBO_CLANG_LIB := vendor/build/gumbo-clang/libgumbo.a
GUMBO_FILC_LIB := vendor/build/gumbo-filc/libgumbo.a
GUMBO_INCLUDE := -Ivendor/src/gumbo/gumbo-parser/src
# zlib-ng: deflate for C01 (and K01 later); headers are generated per build.
ZLIB_CLANG_LIB := vendor/build/zlib-ng-clang/libz.a
ZLIB_FILC_LIB := vendor/build/zlib-ng-filc/libz.a
ZLIB_CLANG_INCLUDE := -Ivendor/build/zlib-ng-clang
ZLIB_FILC_INCLUDE := -Ivendor/build/zlib-ng-filc
# libcurl: I01 outbound HTTP/unfurl/webhook (static archives, built against
# the vendored OpenSSL above; needs libssl.a alongside libcrypto.a).
CURL_CLANG_LIB := vendor/build/curl-clang/install/lib/libcurl.a
CURL_FILC_LIB := vendor/build/curl-filc/install/lib/libcurl.a
CURL_CLANG_INCLUDE := -Ivendor/build/curl-clang/install/include
CURL_FILC_INCLUDE := -Ivendor/build/curl-filc/install/include
LIBSSL_CLANG_LIB := vendor/build/openssl-clang/install/lib/libssl.a
LIBSSL_FILC_LIB := vendor/build/openssl-filc/install/lib/libssl.a
# nghttp2: P01 HTTP/2 framing/HPACK (static archives, library-only builds).
# Headers are generated per build: the source tree
# (vendor/src/nghttp2/lib/includes) AND the build tree
# (vendor/build/nghttp2-<mode>/lib/includes, nghttp2.h version macros) must
# both precede any system path — the system's 1.69.0 headers must never win.
NGHTTP2_CLANG_LIB := vendor/build/nghttp2-clang/lib/libnghttp2.a
NGHTTP2_FILC_LIB := vendor/build/nghttp2-filc/lib/libnghttp2.a
NGHTTP2_CLANG_INCLUDE := -Ivendor/src/nghttp2/lib/includes -Ivendor/build/nghttp2-clang/lib/includes
NGHTTP2_FILC_INCLUDE := -Ivendor/src/nghttp2/lib/includes -Ivendor/build/nghttp2-filc/lib/includes
# qrcodegen: single-file upstream QR encoder for A-qr_code's cf_qr_code_svg
# (src/views/qr_svg.c). Compiled per mode with upstream-only flags, like
# picohttpparser below.
QRCODEGEN_SRC := vendor/src/qrcodegen/c/qrcodegen.c
QRCODEGEN_INCLUDE := -Ivendor/src/qrcodegen/c
# recursive: MODE_OBJ is defined further down (build-variant section)
QRCODEGEN_OBJ = $(MODE_OBJ)/vendor/qrcodegen.o
# picohttpparser: single-file upstream parser (no upstream build system). It
# is compiled per mode but with upstream-appropriate flags only — never the
# application's -Werror (01-foundation-http.md F00 build restriction).
PICOHTTP_SRC := vendor/src/picohttpparser/picohttpparser.c
# recursive: MODE_OBJ is defined further down (build-variant section)
PICOHTTP_OBJ = $(MODE_OBJ)/vendor/picohttpparser.o
DEP_INCLUDES := -I$(SQLITE_INCLUDE) -I$(YYJSON_INCLUDE) -Ivendor/src/picohttpparser $(QRCODEGEN_INCLUDE)

# ---------- toolchain -------------------------------------------------------
CLANG ?= clang
# Pinned Fil-C 0.685 pizfix driver (vendor/DEPS.json filc entry; vendor/README
# section 2). Every -filc dependency archive was built with this exact binary.
FILC ?= /home/msaraiva/.local/fil-c/0.685/filc-0.685-linux-x86_64/build/bin/filcc

STRICT_FLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE \
	-Wall -Wextra -Werror -pthread
# Application version baked at build time.  APP_VERSION is the single
# Makefile variable: it feeds views.h's CF_VIEWS_APP_VERSION and A01's
# set_version_headers X-Version (CF_APP_VERSION).  X-Rev is emitted only when
# a nonempty GIT_REVISION is given (`make GIT_REVISION=<rev>`), absent by
# default (IMPLEMENTATION-ROADMAP 2026-10-05 A01/A02 completion (e)).
APP_VERSION ?= 0.1.0
GIT_REVISION ?=
APP_CPPFLAGS := -Isrc -Itests -DCF_VIEWS_APP_VERSION='"$(APP_VERSION)"' \
	-DCF_APP_VERSION='"$(APP_VERSION)"'
ifneq ($(strip $(GIT_REVISION)),)
APP_CPPFLAGS += -DCF_GIT_REVISION='"$(GIT_REVISION)"'
endif
# Same flags without the views version define (golden buckets redefine it).
APP_TEST_CPPFLAGS := $(filter-out -DCF_VIEWS_APP_VERSION='"$(APP_VERSION)"',$(APP_CPPFLAGS))

# ---------- build variants --------------------------------------------------
MODE ?= dev
ifeq ($(MODE),dev)
	MODE_CC := $(CLANG)
	MODE_CFLAGS := -O2
	MODE_LDFLAGS :=
	MODE_DEP_INCLUDES := -Ivendor/build/libxcrypt-clang $(OPENSSL_CLANG_INCLUDE) $(GUMBO_INCLUDE) $(ZLIB_CLANG_INCLUDE) $(CURL_CLANG_INCLUDE) $(NGHTTP2_CLANG_INCLUDE)
	MODE_DEP_LIBS = $(SQLITE_CLANG_LIB) $(YYJSON_CLANG_LIB) $(LIBCRYPT_CLANG_LIB) $(CURL_CLANG_LIB) $(LIBSSL_CLANG_LIB) $(OPENSSL_CLANG_LIB) $(NGHTTP2_CLANG_LIB) $(GUMBO_CLANG_LIB) $(ZLIB_CLANG_LIB) $(PICOHTTP_OBJ) $(QRCODEGEN_OBJ)
else ifeq ($(MODE),bench)
	MODE_CC := $(CLANG)
	MODE_CFLAGS := -O3 -flto
	MODE_LDFLAGS := -flto
	MODE_DEP_INCLUDES := -Ivendor/build/libxcrypt-clang $(OPENSSL_CLANG_INCLUDE) $(GUMBO_INCLUDE) $(ZLIB_CLANG_INCLUDE) $(CURL_CLANG_INCLUDE) $(NGHTTP2_CLANG_INCLUDE)
	MODE_DEP_LIBS = $(SQLITE_CLANG_LIB) $(YYJSON_CLANG_LIB) $(LIBCRYPT_CLANG_LIB) $(CURL_CLANG_LIB) $(LIBSSL_CLANG_LIB) $(OPENSSL_CLANG_LIB) $(NGHTTP2_CLANG_LIB) $(GUMBO_CLANG_LIB) $(ZLIB_CLANG_LIB) $(PICOHTTP_OBJ) $(QRCODEGEN_OBJ)
else ifeq ($(MODE),filc)
	MODE_CC := $(FILC)
	MODE_CFLAGS := -O2
	MODE_LDFLAGS :=
	MODE_DEP_INCLUDES := -Ivendor/build/libxcrypt-filc $(OPENSSL_FILC_INCLUDE) $(GUMBO_INCLUDE) $(ZLIB_FILC_INCLUDE) $(CURL_FILC_INCLUDE) $(NGHTTP2_FILC_INCLUDE)
	MODE_DEP_LIBS = $(SQLITE_FILC_LIB) $(YYJSON_FILC_LIB) $(LIBCRYPT_FILC_LIB) $(CURL_FILC_LIB) $(LIBSSL_FILC_LIB) $(OPENSSL_FILC_LIB) $(NGHTTP2_FILC_LIB) $(GUMBO_FILC_LIB) $(ZLIB_FILC_LIB) $(PICOHTTP_OBJ) $(QRCODEGEN_OBJ)
else ifeq ($(MODE),sanitize)
	MODE_CC := $(CLANG)
	MODE_CFLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
	MODE_LDFLAGS := -fsanitize=address,undefined
	MODE_DEP_INCLUDES := -Ivendor/build/libxcrypt-clang $(OPENSSL_CLANG_INCLUDE) $(GUMBO_INCLUDE) $(ZLIB_CLANG_INCLUDE) $(CURL_CLANG_INCLUDE) $(NGHTTP2_CLANG_INCLUDE)
	MODE_DEP_LIBS = $(SQLITE_CLANG_LIB) $(YYJSON_CLANG_LIB) $(LIBCRYPT_CLANG_LIB) $(CURL_CLANG_LIB) $(LIBSSL_CLANG_LIB) $(OPENSSL_CLANG_LIB) $(NGHTTP2_CLANG_LIB) $(GUMBO_CLANG_LIB) $(ZLIB_CLANG_LIB) $(PICOHTTP_OBJ) $(QRCODEGEN_OBJ)
else ifeq ($(MODE),tsan)
	MODE_CC := $(CLANG)
	MODE_CFLAGS := -O1 -g -fsanitize=thread -fno-omit-frame-pointer
	MODE_LDFLAGS := -fsanitize=thread
	MODE_DEP_INCLUDES := -Ivendor/build/libxcrypt-clang $(OPENSSL_CLANG_INCLUDE) $(GUMBO_INCLUDE) $(ZLIB_CLANG_INCLUDE) $(CURL_CLANG_INCLUDE) $(NGHTTP2_CLANG_INCLUDE)
	MODE_DEP_LIBS = $(SQLITE_CLANG_LIB) $(YYJSON_CLANG_LIB) $(LIBCRYPT_CLANG_LIB) $(CURL_CLANG_LIB) $(LIBSSL_CLANG_LIB) $(OPENSSL_CLANG_LIB) $(NGHTTP2_CLANG_LIB) $(GUMBO_CLANG_LIB) $(ZLIB_CLANG_LIB) $(PICOHTTP_OBJ) $(QRCODEGEN_OBJ)
else
$(error unknown MODE '$(MODE)': use dev, bench, filc, sanitize or tsan)
endif
# Pinned libxcrypt headers for the mode (struct crypt_data layout must match
# the linked archive; support/password.c and A01 crypto include <crypt.h>).
DEP_INCLUDES += $(MODE_DEP_INCLUDES)
# Section folding: the live app links the routes/assets + auth subset that
# exists today; unreferenced functions (auth paths awaiting R02/models) must
# drop instead of causing undefined references. The full set joins with R02.
APP_CPPFLAGS += -ffunction-sections -fdata-sections -Itests/views
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
	tests/http/test_gzip.c \
	tests/http/test_encoding.c \
	tests/views/test_escape.c \
	tests/views/test_messages_json.c \
	tests/views/test_presenters.c \
	tests/views/test_presenters_messages.c \
	tests/routes/test_routes_table.c \
	tests/routes/test_routes_recognition.c \
	tests/routes/test_builtins.c \
	tests/assets/test_assets.c \
	tests/cache/test_cache.c \
	tests/cache/test_cache_threads.c \
	tests/cache/test_cache_key.c \
	tests/richtext/test_corpus.c \
	tests/richtext/test_richtext.c \
	tests/cable/test_cable_handshake.c \
	tests/cable/test_cable_frames.c \
	tests/cable/test_cable_protocol.c \
	tests/cable/test_cable_deflate.c \
	tests/cable/test_cable_queue.c \
	tests/cable/test_cable_session.c \
	tests/cable/test_cable_loop.c \
	tests/cable/test_cable_channels.c \
	tests/cable/test_cable_wiring.c \
	tests/cable/test_cable_broadcasts.c \
	tests/cable/test_cable_pubsub.c \
	tests/cable/test_cable_revocation.c \
	tests/cable/test_cable_live.c \
	tests/jobs/test_jobs.c \
	tests/jobs/test_jobs_writer.c \
	tests/jobs/test_handlers.c \
	tests/storage/test_files.c \
	tests/storage/test_process.c \
	tests/storage/test_active_storage.c \
	tests/storage/test_media.c \
	tests/integrations/test_unfurl.c \
	tests/integrations/test_webhook.c \
	tests/integrations/test_push.c \
	tests/integrations/test_host_resolve.c \
	tests/front/test_tls.c \
	tests/front/test_h2.c \
	tests/models/test_touch.c \
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
	tests/http/test_tls_loop.c \
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

# Views render-suite bucket: one binary built from six case TUs plus the
# support harness (test_main.c carries its CF_TEST_MAIN; static_root_double.c
# replaces cf_static_root so routes.o/assets.o are excluded from the link).
VIEWS_MAIN_SRC := tests/views/support/test_main.c
VIEWS_SUPPORT_SRCS := \
	tests/views/support/golden.c \
	tests/views/support/golden_b.c \
	tests/views/support/facts.c \
	tests/views/support/static_root_double.c
VIEWS_BUCKET_SRCS := \
	tests/views/test_sessions.c \
	tests/views/test_first_run.c \
	tests/views/test_welcome.c \
	tests/views/test_layout.c \
	tests/views/test_rooms.c \
	tests/views/test_messages.c \
	tests/views/test_users_avatars.c \
	tests/views/test_users_sidebars.c \
	tests/views/test_searches.c \
	tests/views/test_users.c \
	tests/views/test_bots.c \
	tests/views/test_accounts.c \
	tests/views/test_users_profiles.c \
	tests/views/test_rooms_forms.c
VIEWS_TEST_BIN := $(TESTS_DIR)/views/test_views
VIEWS_MAIN_OBJ := $(patsubst tests/%.c,$(MODE_OBJ)/tests/%.o,$(VIEWS_MAIN_SRC))
VIEWS_SUPPORT_OBJS := $(patsubst tests/%.c,$(MODE_OBJ)/tests/%.o,$(VIEWS_SUPPORT_SRCS))
VIEWS_BUCKET_OBJS := $(patsubst tests/%.c,$(MODE_OBJ)/tests/%.o,$(VIEWS_BUCKET_SRCS))
APP_NOROUTE_ASSETS_OBJS := $(filter-out $(MODE_OBJ)/src/routes.o \
	$(MODE_OBJ)/src/assets.o,$(APP_LIB_OBJS))
# Golden buckets compare against fixtures captured with the reference's
# "parity" build version, so they link a parity-compiled presenters/layout.c
# (the same pattern as static_root_double.c).
PARITY_LAYOUT_OBJ := $(MODE_OBJ)/tests/support/presenters_layout_parity.o
APP_GOLDEN_BUCKET_OBJS := $(filter-out $(MODE_OBJ)/src/presenters/layout.o \
	$(MODE_OBJ)/src/views/qr_svg.o,\
	$(APP_NOROUTE_ASSETS_OBJS)) $(PARITY_LAYOUT_OBJ)

# Controller action tests: route-double + views support + no-routes library.
ACTIONS_TEST_SRCS := \
	tests/actions/first_runs_test.c \
	tests/actions/messages_test.c \
	tests/actions/rooms_test.c \
	tests/actions/users_bans_test.c \
	tests/actions/users_avatars_test.c \
	tests/actions/users_sidebars_test.c \
	tests/actions/searches_test.c \
	tests/actions/pwa_test.c \
	tests/actions/qr_code_test.c \
	tests/actions/sessions_transfers_test.c \
	tests/actions/users_test.c \
	tests/actions/autocompletable_users_test.c \
	tests/actions/accounts_test.c \
	tests/actions/accounts_users_test.c \
	tests/actions/accounts_join_codes_test.c \
	tests/actions/accounts_logos_test.c \
	tests/actions/accounts_custom_styles_test.c \
	tests/actions/accounts_bots_test.c \
	tests/actions/accounts_bots_keys_test.c \
	tests/actions/users_profiles_test.c \
	tests/actions/users_push_subscriptions_test.c \
	tests/actions/users_push_subscriptions_test_notifications_test.c \
	tests/actions/users_avatars_destroy_test.c \
	tests/actions/rooms_refreshes_test.c \
	tests/actions/rooms_involvements_test.c \
	tests/actions/rooms_opens_test.c \
	tests/actions/rooms_closeds_test.c \
	tests/actions/rooms_directs_test.c \
	tests/actions/messages_boosts_test.c \
	tests/actions/messages_boosts_by_bots_test.c \
	tests/actions/messages_by_bots_test.c \
	tests/actions/unfurl_links_test.c \
	tests/actions/active_storage_blobs_test.c \
	tests/actions/active_storage_representations_test.c \
	tests/actions/active_storage_disk_test.c \
	tests/actions/active_storage_direct_uploads_test.c \
	tests/actions/welcome_test.c \
	tests/actions/sessions_test.c
ACTIONS_TEST_BINS := $(patsubst tests/%.c,$(TESTS_DIR)/%,$(ACTIONS_TEST_SRCS))

# Auth test binaries use the route double's test helpers (auth_env.h) instead
# of src/routes.c, so they link the no-routes library plus the double.
AUTH_TEST_SRCS := \
	tests/auth/test_before.c \
	tests/auth/test_crypto.c \
	tests/auth/test_password.c \
	tests/auth/test_platform.c \
	tests/auth/test_rate.c \
	tests/auth/test_session.c \
	tests/auth/test_tokens.c
AUTH_TEST_BINS := $(patsubst tests/%.c,$(TESTS_DIR)/%,$(AUTH_TEST_SRCS))


# TSan run of the threaded cases (buffer + worker join, writer queue, HTTP
# completion queue) and the app/config suite touched by the shutdown path.
TSAN_TEST_SRCS := tests/core/test_buffer.c tests/config/test_config.c \
	tests/db/test_writer.c tests/jobs/test_jobs_writer.c \
	tests/cable/test_cable_queue.c tests/cable/test_cable_live.c \
	tests/cache/test_cache_threads.c
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
	$(HTTP_COMMON_OBJ) $(APP_DOUBLE_OBJ) $(APP_HARNESS_OBJ)
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
$(LIBCRYPT_CLANG_LIB) $(LIBCRYPT_FILC_LIB) $(OPENSSL_CLANG_LIB) $(OPENSSL_FILC_LIB) \
$(GUMBO_CLANG_LIB) $(GUMBO_FILC_LIB) $(ZLIB_CLANG_LIB) $(ZLIB_FILC_LIB) \
$(CURL_CLANG_LIB) $(CURL_FILC_LIB) $(LIBSSL_CLANG_LIB) $(LIBSSL_FILC_LIB) \
$(NGHTTP2_CLANG_LIB) $(NGHTTP2_FILC_LIB):
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

# Golden-bucket parity object: presenters/layout.c with the captured version.
$(PARITY_LAYOUT_OBJ): src/presenters/layout.c | check-cc
	@mkdir -p $(dir $@)
	$(MODE_CC) $(STRICT_FLAGS) $(MODE_CFLAGS) $(APP_TEST_CPPFLAGS) $(DEP_INCLUDES) \
		-DCF_VIEWS_APP_VERSION='"parity"' -MMD -MP -c $< -o $@

$(MODE_OBJ)/tests/%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(MODE_CC) $(STRICT_FLAGS) $(MODE_CFLAGS) $(APP_CPPFLAGS) \
		$(DEP_INCLUDES) -MMD -MP -c $< -o $@

# Upstream single-file dependency: upstream flags only, no -Werror.
$(PICOHTTP_OBJ): $(PICOHTTP_SRC)
	@mkdir -p $(dir $@)
	$(MODE_CC) -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -pthread \
		$(MODE_CFLAGS) -MMD -MP -c $< -o $@

# Upstream single-file QR encoder (same no--Werror treatment).
$(QRCODEGEN_OBJ): $(QRCODEGEN_SRC)
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

# K01c's dispatch-level cache admission suite builds against the route
# double + views support (like the actions bucket), not the plain unit link.
CACHE_ADMISSION_TEST_BIN := $(TESTS_DIR)/cache/test_cache_admission
$(CACHE_ADMISSION_TEST_BIN): $(MODE_OBJ)/tests/cache/test_cache_admission.o \
		$(APP_DOUBLE_OBJ) $(VIEWS_SUPPORT_OBJS) $(APP_GOLDEN_BUCKET_OBJS) \
		$(MODE_DEP_LIBS) | check-cc
	@mkdir -p $(dir $@)
	$(MODE_CC) $(STRICT_FLAGS) $(MODE_CFLAGS) $(MODE_LDFLAGS) -Itests/views \
		$(MODE_OBJ)/tests/cache/test_cache_admission.o $(APP_DOUBLE_OBJ) \
		$(VIEWS_SUPPORT_OBJS) $(APP_GOLDEN_BUCKET_OBJS) \
		$(MODE_DEP_LIBS) -lm -o $@

# H01 binaries share the test_http_common.c auxiliary translation unit.
$(HTTP_TEST_BINS): $(TESTS_DIR)/http/%: $(MODE_OBJ)/tests/http/%.o \
		$(HTTP_COMMON_OBJ) $(APP_LIB_OBJS) $(MODE_DEP_LIBS) | check-cc
	@mkdir -p $(dir $@)
	$(MODE_CC) $(STRICT_FLAGS) $(MODE_CFLAGS) $(MODE_LDFLAGS) \
		$(MODE_OBJ)/tests/http/$*.o $(HTTP_COMMON_OBJ) \
		$(APP_LIB_OBJS) $(MODE_DEP_LIBS) -lm -o $@


$(VIEWS_TEST_BIN): $(VIEWS_BUCKET_OBJS) $(VIEWS_MAIN_OBJ) $(VIEWS_SUPPORT_OBJS) \
		$(APP_GOLDEN_BUCKET_OBJS) $(MODE_DEP_LIBS) | check-cc
	@mkdir -p $(dir $@)
	$(MODE_CC) $(STRICT_FLAGS) $(MODE_CFLAGS) $(MODE_LDFLAGS) -Itests/views \
		$(VIEWS_BUCKET_OBJS) $(VIEWS_MAIN_OBJ) $(VIEWS_SUPPORT_OBJS) \
		$(APP_GOLDEN_BUCKET_OBJS) $(MODE_DEP_LIBS) -lm -o $@

$(ACTIONS_TEST_BINS): $(TESTS_DIR)/actions/%: $(MODE_OBJ)/tests/actions/%.o \
		$(APP_DOUBLE_OBJ) $(VIEWS_SUPPORT_OBJS) $(APP_GOLDEN_BUCKET_OBJS) \
		$(MODE_DEP_LIBS) | check-cc
	@mkdir -p $(dir $@)
	$(MODE_CC) $(STRICT_FLAGS) $(MODE_CFLAGS) $(MODE_LDFLAGS) -Itests/views \
		$(MODE_OBJ)/tests/actions/$*.o $(APP_DOUBLE_OBJ) \
		$(VIEWS_SUPPORT_OBJS) $(APP_GOLDEN_BUCKET_OBJS) \
		$(MODE_DEP_LIBS) -lm -o $@

# Auth binaries: route-double helpers + the no-routes app library.
$(AUTH_TEST_BINS): $(TESTS_DIR)/auth/%: $(MODE_OBJ)/tests/auth/%.o \
		$(APP_DOUBLE_OBJ) $(APP_NOROUTE_OBJS) $(MODE_DEP_LIBS) | check-cc
	@mkdir -p $(dir $@)
	$(MODE_CC) $(STRICT_FLAGS) $(MODE_CFLAGS) $(MODE_LDFLAGS) \
		$(MODE_OBJ)/tests/auth/$*.o $(APP_DOUBLE_OBJ) \
		$(APP_NOROUTE_OBJS) $(MODE_DEP_LIBS) -lm -o $@

# Cable broadcasts tests exercise dispatch with the route-double helpers.
$(TESTS_DIR)/cable/test_cable_broadcasts: $(MODE_OBJ)/tests/cable/test_cable_broadcasts.o \
		$(APP_DOUBLE_OBJ) $(APP_NOROUTE_OBJS) $(MODE_DEP_LIBS) | check-cc
	@mkdir -p $(dir $@)
	$(MODE_CC) $(STRICT_FLAGS) $(MODE_CFLAGS) $(MODE_LDFLAGS) \
		$(MODE_OBJ)/tests/cable/test_cable_broadcasts.o $(APP_DOUBLE_OBJ) \
		$(APP_NOROUTE_OBJS) $(MODE_DEP_LIBS) -lm -o $@

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

# ---------- test execution -------------------------------------------------
# Every test binary runs even when an earlier one fails; the aggregate exit
# is nonzero if any binary, or the app checks, failed.
test-impl: $(BIN) $(UNIT_TEST_BINS) $(HTTP_TEST_BINS) \
		$(APP_ALL_TEST_BINS) $(AUTH_TEST_BINS) $(VIEWS_TEST_BIN) \
		$(ACTIONS_TEST_BINS) $(CACHE_ADMISSION_TEST_BIN) $(TEST_OBJS)
	@fail=0; \
	for t in $(UNIT_TEST_BINS) $(HTTP_TEST_BINS) \
		$(APP_ALL_TEST_BINS) $(AUTH_TEST_BINS) $(VIEWS_TEST_BIN) \
		$(ACTIONS_TEST_BINS) $(CACHE_ADMISSION_TEST_BIN); do \
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

tsan-impl: $(TSAN_TEST_BINS) $(TSAN_HTTP_TEST_BINS) $(TSAN_APP_TEST_BINS) \
		$(ACTIONS_TEST_BINS) $(AUTH_TEST_BINS) $(TSAN_TEST_OBJS)
	@fail=0; \
	for t in $(TSAN_TEST_BINS) $(TSAN_HTTP_TEST_BINS) $(TSAN_APP_TEST_BINS) \
		$(ACTIONS_TEST_BINS) $(AUTH_TEST_BINS); do \
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
-include $(MODE_OBJ)/tests/app/*.d $(MODE_OBJ)/tests/app/support/*.d \
	$(MODE_OBJ)/tests/auth/*.d $(MODE_OBJ)/tests/richtext/*.d \
	$(MODE_OBJ)/tests/cable/*.d $(MODE_OBJ)/tests/jobs/*.d \
	$(MODE_OBJ)/tests/storage/*.d $(MODE_OBJ)/tests/actions/*.d \
	$(MODE_OBJ)/tests/views/*.d $(MODE_OBJ)/tests/views/support/*.d \
	$(MODE_OBJ)/tests/support/*.d $(MODE_OBJ)/tests/cache/*.d
