/*
 * F00 probe for the Gumbo C HTML5 parser (Nokogiri v1.19.4 gumbo-parser
 * subtree, nokogiri_gumbo.h API).
 *
 * Parses two small HTML documents and verifies:
 *   - the tree shape produced by HTML5 tree construction (root html,
 *     head/body split, implied tbody inside table, inline formatting
 *     elements, text runs, attribute name/value pairs);
 *   - serialization of a known case by a minimal walker over the parse
 *     tree, compared byte-for-byte against an expected string.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "nokogiri_gumbo.h"

static int failures;

static void check(bool cond, const char *what)
{
    if (cond) {
        printf("ok: %s\n", what);
    } else {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

struct writer {
    char buf[4096];
    size_t len;
    bool overflow;
};

static void put(struct writer *w, const char *s)
{
    size_t n = strlen(s);

    if (w->len + n >= sizeof(w->buf)) {
        w->overflow = true;
        return;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = '\0';
}

static void serialize(const GumboNode *node, struct writer *w)
{
    unsigned int i;

    switch (node->type) {
    case GUMBO_NODE_DOCUMENT:
        for (i = 0; i < node->v.document.children.length; i++)
            serialize(node->v.document.children.data[i], w);
        break;
    case GUMBO_NODE_TEMPLATE:
    case GUMBO_NODE_ELEMENT: {
        const char *tag = gumbo_normalized_tagname(node->v.element.tag);

        put(w, "<");
        put(w, tag);
        for (i = 0; i < node->v.element.attributes.length; i++) {
            const GumboAttribute *attr = node->v.element.attributes.data[i];

            put(w, " ");
            put(w, attr->name);
            put(w, "=\"");
            put(w, attr->value);
            put(w, "\"");
        }
        put(w, ">");
        for (i = 0; i < node->v.element.children.length; i++)
            serialize(node->v.element.children.data[i], w);
        put(w, "</");
        put(w, tag);
        put(w, ">");
        break;
    }
    case GUMBO_NODE_TEXT:
    case GUMBO_NODE_WHITESPACE:
        put(w, node->v.text.text);
        break;
    case GUMBO_NODE_CDATA:
        put(w, "<![CDATA[");
        put(w, node->v.text.text);
        put(w, "]]>");
        break;
    case GUMBO_NODE_COMMENT:
        put(w, "<!--");
        put(w, node->v.text.text);
        put(w, "-->");
        break;
    }
}

static const GumboNode *child(const GumboNode *node, unsigned int idx)
{
    return (const GumboNode *)node->v.element.children.data[idx];
}

static void check_serialization(const GumboNode *root, const char *expected,
                                const char *label)
{
    struct writer w = {{0}, 0, false};

    serialize(root, &w);
    if (w.overflow) {
        fprintf(stderr, "FAIL: %s: serializer buffer overflow\n", label);
        failures++;
        return;
    }
    if (strcmp(w.buf, expected) != 0) {
        fprintf(stderr, "FAIL: %s serialization\n  want: %s\n  got:  %s\n",
                label, expected, w.buf);
        failures++;
    } else {
        printf("ok: %s serialization -> %s\n", label, w.buf);
    }
}

static void probe_doc1(void)
{
    static const char html[] =
        "<!DOCTYPE html><html><head><title>Hi</title></head>"
        "<body><p class=\"a\" id=\"b\">Hi <b>world</b>!</p></body></html>";
    static const char expected[] =
        "<html><head><title>Hi</title></head>"
        "<body><p class=\"a\" id=\"b\">Hi <b>world</b>!</p></body></html>";
    GumboOutput *out = gumbo_parse(html);

    check(out != NULL, "doc1: gumbo_parse returns output");
    if (out == NULL)
        return;
    check(out->status == GUMBO_STATUS_OK, "doc1: parse status OK");
    check(out->document_error == false, "doc1: no document error");

    const GumboNode *root = out->root;
    check(root->type == GUMBO_NODE_ELEMENT && root->v.element.tag == GUMBO_TAG_HTML,
          "doc1: root element is <html>");
    check(root->v.element.children.length == 2, "doc1: html has 2 children");
    const GumboNode *head = child(root, 0);
    const GumboNode *body = child(root, 1);
    check(head->v.element.tag == GUMBO_TAG_HEAD, "doc1: first child is <head>");
    check(body->v.element.tag == GUMBO_TAG_BODY, "doc1: second child is <body>");

    const GumboNode *title = child(head, 0);
    check(title->v.element.tag == GUMBO_TAG_TITLE &&
              title->v.element.children.length == 1 &&
              strcmp(child(title, 0)->v.text.text, "Hi") == 0,
          "doc1: head > title > \"Hi\"");

    const GumboNode *p = child(body, 0);
    check(p->v.element.tag == GUMBO_TAG_P, "doc1: body first child is <p>");
    check(p->v.element.attributes.length == 2, "doc1: p has 2 attributes");
    const GumboAttribute *a0 = p->v.element.attributes.data[0];
    const GumboAttribute *a1 = p->v.element.attributes.data[1];
    check(strcmp(a0->name, "class") == 0 && strcmp(a0->value, "a") == 0,
          "doc1: p attribute class=\"a\"");
    check(strcmp(a1->name, "id") == 0 && strcmp(a1->value, "b") == 0,
          "doc1: p attribute id=\"b\"");

    check(p->v.element.children.length == 3, "doc1: p has 3 children");
    check(child(p, 0)->type == GUMBO_NODE_TEXT &&
              strcmp(child(p, 0)->v.text.text, "Hi ") == 0,
          "doc1: p text \"Hi \"");
    const GumboNode *b = child(p, 1);
    check(b->v.element.tag == GUMBO_TAG_B, "doc1: p child <b>");
    check(b->v.element.children.length == 1 &&
              child(b, 0)->type == GUMBO_NODE_TEXT &&
              strcmp(child(b, 0)->v.text.text, "world") == 0,
          "doc1: b > \"world\"");
    check(child(p, 2)->type == GUMBO_NODE_TEXT &&
              strcmp(child(p, 2)->v.text.text, "!") == 0,
          "doc1: p trailing text \"!\"");

    check_serialization(root, expected, "doc1");

    gumbo_destroy_output(out);
}

static void probe_doc2(void)
{
    static const char html[] =
        "<!DOCTYPE html><table><tr><td>cell</td></tr></table>";
    static const char expected[] =
        "<html><head></head><body><table><tbody><tr><td>cell</td>"
        "</tr></tbody></table></body></html>";
    GumboOutput *out = gumbo_parse(html);

    check(out != NULL, "doc2: gumbo_parse returns output");
    if (out == NULL)
        return;
    check(out->status == GUMBO_STATUS_OK, "doc2: parse status OK");
    check(out->document->v.document.has_doctype &&
              strcmp(out->document->v.document.name, "html") == 0,
          "doc2: document has <!DOCTYPE html>");

    const GumboNode *root = out->root;
    const GumboNode *body = child(root, 1);
    check(body->v.element.tag == GUMBO_TAG_BODY, "doc2: implied <body>");
    const GumboNode *table = child(body, 0);
    check(table->v.element.tag == GUMBO_TAG_TABLE &&
              table->v.element.children.length == 1,
          "doc2: body > table");
    const GumboNode *tbody = child(table, 0);
    check(tbody->v.element.tag == GUMBO_TAG_TBODY,
          "doc2: implied <tbody> inserted by HTML5 tree building");
    const GumboNode *tr = child(tbody, 0);
    const GumboNode *td = child(tr, 0);
    check(tr->v.element.tag == GUMBO_TAG_TR && td->v.element.tag == GUMBO_TAG_TD,
          "doc2: tbody > tr > td");
    check(td->v.element.children.length == 1 &&
              strcmp(child(td, 0)->v.text.text, "cell") == 0,
          "doc2: td > \"cell\"");

    check_serialization(root, expected, "doc2");

    gumbo_destroy_output(out);
}

int main(void)
{
    probe_doc1();
    probe_doc2();

    printf("probe_result: %s (%d failure(s))\n",
           failures ? "FAIL" : "READY", failures);
    return failures ? 1 : 0;
}
