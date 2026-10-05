/* H03 route-table provenance (HTTP-07): the compiled 177-row table is the
 * binding artifact contracts/routes.json, in the artifact's exact order,
 * with the artifact's dispositions and exact C entry-point names, and the
 * development-501 registrations are exactly the rows whose owning packet
 * has not landed.
 *
 * These cases read the artifact with yyjson (the pinned strict JSON reader)
 * and fail loudly when it is missing: a missing prerequisite is never a
 * skip. */
#include "cf_test.h"

#include "context.h" /* cf_route_action seam (A00's header) */
#include "routes.h"

#include <yyjson.h>

#define ROUTES_JSON "docs/devel/implementation/contracts/routes.json"

static const char *method_name(cf_method method) {
    switch (method) {
    case CF_GET: return "GET";
    case CF_HEAD: return "HEAD";
    case CF_POST: return "POST";
    case CF_PUT: return "PUT";
    case CF_PATCH: return "PATCH";
    case CF_DELETE: return "DELETE";
    default: return "?";
    }
}

static yyjson_val *obj_get(yyjson_val *obj, const char *key) {
    return yyjson_obj_get(obj, key);
}

static bool str_eq(yyjson_val *val, const char *expected) {
    return val != NULL && yyjson_is_str(val) &&
           strcmp(yyjson_get_str(val), expected) == 0;
}

CF_TEST(route_table_matches_the_artifact_in_order) {
    yyjson_doc *doc = yyjson_read_file(ROUTES_JSON, 0, NULL, NULL);
    CF_REQUIRE(doc != NULL);
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *rows = obj_get(root, "routes");
    CF_REQUIRE(rows != NULL);
    CF_REQUIRE(yyjson_is_arr(rows));

    size_t count = 0;
    const cf_route *table = cf_routes(&count);
    CF_REQUIRE(table != NULL);
    CF_CHECK(count == 177);
    CF_CHECK(yyjson_arr_size(rows) == count);

    size_t index, max;
    yyjson_val *row;
    yyjson_arr_foreach(rows, index, max, row) {
        const cf_route *r = &table[index];
        yyjson_val *id = obj_get(row, "id");
        yyjson_val *method = obj_get(row, "method");
        CF_REQUIRE(id != NULL && yyjson_is_int(id));
        CF_CHECK(r->id == (uint32_t)yyjson_get_int(id));
        CF_CHECK(r->id == (uint32_t)(index + 1));
        CF_REQUIRE(method != NULL && yyjson_is_str(method));
        CF_CHECK(strcmp(method_name(r->method), yyjson_get_str(method)) == 0);
        CF_CHECK(str_eq(obj_get(row, "pattern"), r->pattern));
        CF_CHECK(str_eq(obj_get(row, "endpoint"), r->endpoint));
        CF_CHECK(str_eq(obj_get(row, "c_symbol"), r->c_symbol));
        CF_CHECK(str_eq(obj_get(row, "task"), r->task));
        yyjson_val *handler = obj_get(row, "reference_handler");
        CF_REQUIRE(handler != NULL && yyjson_is_str(handler));
        const char *disp = yyjson_get_str(obj_get(row, "disposition"));
        CF_REQUIRE(disp != NULL);
        if (strcmp(disp, "reference_error") == 0) {
            CF_CHECK(r->disposition == CF_ROUTE_REFERENCE_ERROR);
            const char *h = yyjson_get_str(handler);
            CF_CHECK(strcmp(h, "action_not_found") == 0 ||
                      strcmp(h, "missing_controller") == 0);
        } else {
            CF_CHECK(strcmp(disp, "implement") == 0);
            CF_CHECK(r->disposition == CF_ROUTE_IMPLEMENT);
        }
        yyjson_val *defaults = obj_get(row, "defaults");
        CF_REQUIRE(defaults != NULL && yyjson_is_obj(defaults));
        CF_CHECK(yyjson_obj_size(defaults) == r->default_count);
        size_t di, dmax;
        yyjson_val *key, *value;
        yyjson_obj_foreach(defaults, di, dmax, key, value) {
            bool seen = false;
            for (size_t k = 0; k < r->default_count; k++) {
                if (strcmp(yyjson_get_str(key), r->defaults[k].name) == 0) {
                    CF_CHECK(strcmp(yyjson_get_str(value),
                                    r->defaults[k].value) == 0);
                    seen = true;
                }
            }
            CF_CHECK(seen);
        }
        CF_CHECK(r->action != NULL);
        CF_CHECK(cf_route_action(r->id) == r->action);

        /* Every row's binding is exactly the disposition's handler: the 40
         * action_not_found rows answer the reference 404, the single
         * missing_controller row the reference 500, and the H03 built-ins
         * their ported functions. */
        if (strcmp(disp, "reference_error") == 0) {
            const char *h = yyjson_get_str(handler);
            if (strcmp(h, "action_not_found") == 0) {
                CF_CHECK(r->action == cf_action_reference_action_not_found);
            } else {
                CF_CHECK(r->action == cf_action_reference_missing_controller);
            }
        } else {
            const char *h = yyjson_get_str(handler);
            if (strcmp(h, "health::show") == 0) {
                CF_CHECK(r->action == cf_action_health_show);
            } else if (strcmp(h, "turbo_native::recede") == 0) {
                CF_CHECK(r->action == cf_action_turbo_native_recede);
            } else if (strcmp(h, "turbo_native::resume") == 0) {
                CF_CHECK(r->action == cf_action_turbo_native_resume);
            } else if (strcmp(h, "turbo_native::refresh") == 0) {
                CF_CHECK(r->action == cf_action_turbo_native_refresh);
            } else if (strcmp(h, "mailbox::ingress_not_configured") == 0) {
                CF_CHECK(r->action ==
                         cf_action_mailbox_ingress_not_configured);
            } else if (strcmp(h, "mailbox::conductor") == 0) {
                const char *method = yyjson_get_str(obj_get(row, "method"));
                if (strcmp(method, "POST") == 0) {
                    /* A01's cf_check_csrf has landed: CSRF then head 403. */
                    CF_CHECK(r->action == cf_action_mailbox_conductor_post);
                } else {
                    CF_CHECK(r->action == cf_action_mailbox_conductor_get);
                }
            }
        }
    }
    yyjson_doc_free(doc);
}

CF_TEST(route_table_counts_and_public_lookup) {
    size_t count = 0;
    const cf_route *table = cf_routes(&count);
    CF_REQUIRE(table != NULL);
    CF_CHECK(count == 177);
    size_t implement = 0, reference_error = 0;
    for (size_t i = 0; i < count; i++) {
        if (table[i].disposition == CF_ROUTE_IMPLEMENT) implement++;
        if (table[i].disposition == CF_ROUTE_REFERENCE_ERROR) reference_error++;
        CF_CHECK(table[i].action != NULL);
        CF_REQUIRE(cf_route_by_id(table[i].id) == &table[i]);
    }
    CF_CHECK(implement == 136);
    CF_CHECK(reference_error == 41);
    CF_CHECK(cf_route_by_id(0) == NULL);
    CF_CHECK(cf_route_by_id(178) == NULL);
    CF_CHECK(cf_route_action(0) == NULL);
    CF_CHECK(cf_route_action(178) == NULL);
}

CF_TEST(dev_501_registrations_are_exactly_the_unlanded_packets) {
    yyjson_doc *doc = yyjson_read_file(ROUTES_JSON, 0, NULL, NULL);
    CF_REQUIRE(doc != NULL);
    yyjson_val *rows = obj_get(yyjson_doc_get_root(doc), "routes");
    CF_REQUIRE(rows != NULL && yyjson_is_arr(rows));
    CF_REQUIRE(yyjson_arr_size(rows) == 177);

    bool expected_501[178] = {false};
    size_t expected_count = 0;
    size_t index, max;
    yyjson_val *row;
    yyjson_arr_foreach(rows, index, max, row) {
        yyjson_val *id = obj_get(row, "id");
        const char *disp = yyjson_get_str(obj_get(row, "disposition"));
        const char *task = yyjson_get_str(obj_get(row, "task"));
        const char *handler = yyjson_get_str(obj_get(row, "reference_handler"));
        const char *method = yyjson_get_str(obj_get(row, "method"));
        CF_REQUIRE(id != NULL && disp != NULL && task != NULL &&
                   handler != NULL && method != NULL);
        if (strcmp(disp, "implement") != 0) continue;
        /* Every H03 row is bound (reference errors, the small built-ins and
         * both conductor methods); the rest are packets not yet landed. */
        bool bound = strcmp(task, "H03") == 0;
        if (!bound) {
            expected_501[yyjson_get_int(id)] = true;
            expected_count++;
        }
    }
    yyjson_doc_free(doc);

    size_t count = 0;
    const cf_route *table = cf_routes(&count);
    size_t actual_count = 0;
    for (size_t i = 0; i < count; i++) {
        bool is_501 = table[i].action == cf_action_not_landed_501;
        CF_CHECK(is_501 == expected_501[table[i].id]);
        if (is_501) actual_count++;
    }
    CF_CHECK(actual_count == expected_count);
    /* The four POST conductor rows are bound now (A01's cf_check_csrf);
     * everything else still unbound is a packet that has not landed yet. */
    CF_CHECK(actual_count == 118);
}

CF_TEST_MAIN()
