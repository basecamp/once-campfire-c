/* src/auth/rate.c — the SessionsController sign-in rate limit
 * (sessions.rs: `rate_limit to: 10, within: 3.minutes, only: :create`),
 * keyed by request.remote_ip.
 *
 * The pinned Rust map grows without bound; 02-data-auth.md A01 fixes it at
 * CF_AUTH_SIGN_IN_MAP_CAP IPs, removed of expired entries, with new IPs
 * rejected with 429 while full (no eviction bypass).
 */
#include "internal.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *ip;
    size_t ip_len;
    uint64_t count;
    int64_t expires_at_us; /* fixed window start + 180 s */
} auth_rate_entry;

typedef struct {
    auth_rate_entry entries[CF_AUTH_SIGN_IN_MAP_CAP];
    size_t len;
} auth_rate_map;

static auth_rate_map auth_rate_limits;
static pthread_mutex_t auth_rate_mutex = PTHREAD_MUTEX_INITIALIZER;

static ssize_t auth_rate_find(const auth_rate_map *map, cf_span ip) {
    for (size_t i = 0; i < map->len; i++) {
        if (map->entries[i].ip_len == ip.len &&
            memcmp(map->entries[i].ip, ip.ptr, ip.len) == 0) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static void auth_rate_remove_at(auth_rate_map *map, size_t index) {
    free(map->entries[index].ip);
    map->entries[index] = map->entries[map->len - 1];
    map->len--;
}

/* Test-only hook for the rate-limit tests: forget every window. */
void cf_auth_rate_limit_test_reset(void);

void cf_auth_rate_limit_test_reset(void) {
    pthread_mutex_lock(&auth_rate_mutex);
    for (size_t i = 0; i < auth_rate_limits.len; i++) {
        free(auth_rate_limits.entries[i].ip);
    }
    auth_rate_limits.len = 0;
    pthread_mutex_unlock(&auth_rate_mutex);
}

cf_err cf_auth_sign_in_rate_limit(cf_ctx *ctx, bool *limited) {
    if (ctx == NULL || limited == NULL) return CF_INVALID;
    *limited = false;
    if (ctx->request == NULL) return CF_INVALID;
    cf_span ip = ctx->request->peer_ip;
    if (ip.len != 0 && ip.ptr == NULL) return CF_INVALID;
    int64_t now = cf_now_us(ctx->app);

    pthread_mutex_lock(&auth_rate_mutex);
    auth_rate_map *map = &auth_rate_limits;

    /* The Rust map drops expired entries on every increment. */
    for (size_t i = 0; i < map->len;) {
        if (map->entries[i].expires_at_us <= now) auth_rate_remove_at(map, i);
        else i++;
    }

    ssize_t at = auth_rate_find(map, ip);
    bool rejected = false;
    if (at < 0) {
        if (map->len >= CF_AUTH_SIGN_IN_MAP_CAP) {
            /* Bounded map: a new IP is rejected, never evicting a live one. */
            rejected = true;
        } else {
            char *copy = malloc(ip.len + 1);
            if (copy == NULL) {
                pthread_mutex_unlock(&auth_rate_mutex);
                return CF_NOMEM;
            }
            if (ip.len != 0) memcpy(copy, ip.ptr, ip.len);
            copy[ip.len] = '\0';
            map->entries[map->len++] = (auth_rate_entry){
                .ip = copy,
                .ip_len = ip.len,
                .count = 1,
                .expires_at_us = now + CF_AUTH_SIGN_IN_WINDOW_US,
            };
        }
    } else {
        map->entries[at].count++;
        if (map->entries[at].count > CF_AUTH_SIGN_IN_LIMIT) rejected = true;
    }
    pthread_mutex_unlock(&auth_rate_mutex);

    if (rejected) {
        *limited = true;
        cf_ctx_set_error_status(ctx, 429);
    }
    return CF_OK;
}
