/*
 * F00 probe for libxcrypt v4.5.2 (bcrypt via crypt_r).
 *
 * Requires an explicit $2b$ bcrypt salt with an explicit cost and must
 * verify known-answer vectors without touching the global crypt() state.
 *
 * Vector A provenance: tmp/rust-ref/vectors/rails_compat.json
 *   ("passwords.digests[0]", password "secret123456", digest
 *    "$2a$12$I/2qP3ixlNN..JU2dXSN0umG7/Folk3aUaqgSHn/Rl0G82O9WYk66").
 *   Asserted with a $2b$ prefix: libxcrypt documents that $2a$ and $2b$
 *   agree except for the historic >72-byte wraparound case, and this
 *   password is 12 bytes. The hash body must be byte-identical.
 *
 * Vector B provenance: the standard published OpenBSD bcrypt regress
 *   vector (password "U*U", salt "$2a$05$CCCCCCCCCCCCCCCCCCCCC.",
 *   digest "$2a$05$CCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOeW");
 *   the $2b$ form of the same vector is asserted as the primary case and
 *   the $2a$ form as a cross-check.
 */
#define _GNU_SOURCE 1
#include <crypt.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define NTHREADS 4
#define ROUNDS 25

static int failures;

static void expect(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL: %s\n  want: %s\n  got:  %s\n", what, want, got);
        failures++;
    } else {
        printf("ok: %s -> %s\n", what, got);
    }
}

struct thread_case {
    const char *password;
    const char *salt;
    char expected[128];
    int ok;
};

static void run_crypt(struct crypt_data *data, const char *password,
                      const char *salt, char out[128])
{
    char *r;

    memset(data, 0, sizeof(*data));
    r = crypt_r(password, salt, data);
    if (r == NULL) {
        snprintf(out, 128, "<crypt_r returned NULL>");
        return;
    }
    snprintf(out, 128, "%s", r);
}

static void *thread_main(void *arg)
{
    struct thread_case *tc = arg;
    struct crypt_data data;
    char out[128];
    int i;

    for (i = 0; i < ROUNDS; i++) {
        run_crypt(&data, tc->password, tc->salt, out);
        if (strcmp(out, tc->expected) != 0) {
            tc->ok = 0;
            return NULL;
        }
    }
    tc->ok = 1;
    return NULL;
}

int main(void)
{
    struct crypt_data data;
    char out[128];
    pthread_t threads[NTHREADS];
    struct thread_case cases[NTHREADS] = {
        {"thread-pass-0", "$2b$04$aaaabbbbccccddddeeeeff", {0}, 0},
        {"thread-pass-1", "$2b$04$ffffgggghhhhiiiijjjjkk", {0}, 0},
        {"thread-pass-2", "$2b$04$llllmmmmnnnnooooppppqq", {0}, 0},
        {"thread-pass-3", "$2b$04$rrrrssssttttuuuuvvvvww", {0}, 0},
    };
    int i;

    /* Vector A: rust-ref digest, $2b$ prefix, explicit cost 12. */
    run_crypt(&data, "secret123456", "$2b$12$I/2qP3ixlNN..JU2dXSN0u", out);
    expect("rust-ref vector as $2b$ (cost 12)",
           out,
           "$2b$12$I/2qP3ixlNN..JU2dXSN0umG7/Folk3aUaqgSHn/Rl0G82O9WYk66");

    /* Vector B: published OpenBSD regress vector in explicit $2b$ form. */
    run_crypt(&data, "U*U", "$2b$05$CCCCCCCCCCCCCCCCCCCCC.", out);
    expect("OpenBSD regress vector as $2b$ (cost 5)",
           out,
           "$2b$05$CCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOeW");

    /* Same salt letters with the $2a$ prefix must give the same body:
     * cross-check that the published $2a$ body is reproduced exactly. */
    run_crypt(&data, "U*U", "$2a$05$CCCCCCCCCCCCCCCCCCCCC.", out);
    expect("published OpenBSD $2a$ body (cost 5)",
           out,
           "$2a$05$CCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOeW");

    /* Determinism: a second call with the same inputs matches the first. */
    run_crypt(&data, "U*U", "$2b$05$CCCCCCCCCCCCCCCCCCCCC.", out);
    expect("repeated call is deterministic",
           out,
           "$2b$05$CCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOeW");

    /* No global crypt state: concurrent crypt_r calls on distinct buffers
     * must each reproduce their precomputed single-thread result. */
    for (i = 0; i < NTHREADS; i++)
        run_crypt(&data, cases[i].password, cases[i].salt, cases[i].expected);
    for (i = 0; i < NTHREADS; i++) {
        if (pthread_create(&threads[i], NULL, thread_main, &cases[i]) != 0) {
            fprintf(stderr, "FAIL: pthread_create\n");
            failures++;
            return 1;
        }
    }
    for (i = 0; i < NTHREADS; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            fprintf(stderr, "FAIL: pthread_join\n");
            failures++;
            return 1;
        }
    }
    for (i = 0; i < NTHREADS; i++) {
        if (!cases[i].ok) {
            fprintf(stderr, "FAIL: concurrent crypt_r mismatch for %s\n",
                    cases[i].password);
            failures++;
        }
    }
    if (failures == 0)
        printf("ok: %d concurrent crypt_r x %d rounds are state-independent\n",
               NTHREADS, ROUNDS);

    printf("probe_result: %s (%d failure(s))\n", failures ? "FAIL" : "READY",
           failures);
    return failures ? 1 : 0;
}
