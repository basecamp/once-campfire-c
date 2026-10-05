/* tests/integrations/test_webhook.c — I01 webhook acceptance (INT-02, JOB-01/02
 * webhook side). Reference vectors copied from the pinned
 * tmp/rust-ref/crates/campfire/src/integrations/testdata/webhook_cases.json
 * and webhook_expected.json (bodies decoded from body_b64); expected
 * outcomes are the reference's, never regenerated from this implementation.
 *
 * Transport runs against local loopback servers spawned here (Python
 * stdlib only). A missing prerequisite (no python3, bind failure, no
 * ready signal) FAILS the case loudly — never a silent skip.
 */
#include "cf_test.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "integrations/webhook.h"

/* --- embedded loopback server (Python stdlib, no framework) ------------------ */

static const char kSrv[] =
    "import base64,json,sys,time,socket\n"
    "from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer\n"
    "routes=json.load(open(sys.argv[1]))\n"
    "rec_path=sys.argv[2]\n"
    "class H(BaseHTTPRequestHandler):\n"
    " protocol_version='HTTP/1.1'\n"
    " def log_message(self,*a): pass\n"
    " def _run(self):\n"
    "  n=int(self.headers.get('Content-Length') or 0)\n"
    "  body=self.rfile.read(n) if n else b''\n"
    "  with open(rec_path,'a') as f:\n"
    "   f.write(json.dumps({'method':self.command,'target':self.path,\n"
    "    'headers':[[k,v] for k,v in self.headers.items()],\n"
    "    'body_b64':base64.b64encode(body).decode()})+'\\n')\n"
    "  r=None\n"
    "  for x in routes:\n"
    "   if x['method']==self.command and x['path']==self.path:\n"
    "    r=x;break\n"
    "  if r is None:\n"
    "   b=b'not found';self.send_response(404);self.send_header('Content-Type','text/plain')\n"
    "   self.send_header('Content-Length',str(len(b)));self.send_header('Connection','close')\n"
    "   self.end_headers()\n"
    "   if self.command!='HEAD': self.wfile.write(b)\n"
    "   return\n"
    "  if r.get('trickle'):\n"
    "   self.send_response(r['status'])\n"
    "   for k,v in r.get('headers',[]): self.send_header(k,v)\n"
    "   self.send_header('Connection','close');self.end_headers()\n"
    "   try:\n"
    "    while True:\n"
    "     self.wfile.write(b' ');self.wfile.flush();time.sleep(0.05)\n"
    "   except Exception: pass\n"
    "   return\n"
    "  time.sleep(r.get('delay_ms',0)/1000.0)\n"
    "  body=base64.b64decode(r.get('body_b64',''))\n"
    "  if r.get('gzip'): body=__import__('gzip').compress(body)\n"
    "  self.send_response(r['status'])\n"
    "  for k,v in r.get('headers',[]): self.send_header(k,v)\n"
    "  if r.get('gzip'): self.send_header('Content-Encoding','gzip')\n"
    "  if r.get('chunked'):\n"
    "   self.send_header('Transfer-Encoding','chunked')\n"
    "  else: self.send_header('Content-Length',str(len(body)))\n"
    "  self.send_header('Connection','close');self.end_headers()\n"
    "  if self.command=='HEAD': return\n"
    "  if r.get('chunked'):\n"
    "   i=0\n"
    "   while i < len(body):\n"
    "    c=body[i:i+65536];self.wfile.write(('%x\\r\\n'%len(c)).encode()+c+b'\\r\\n');i+=len(c)\n"
    "   self.wfile.write(b'0\\r\\n\\r\\n')\n"
    "  else: self.wfile.write(body)\n"
    " def do_GET(self): self._run()\n"
    " def do_HEAD(self): self._run()\n"
    " def do_POST(self): self._run()\n"
    "srv=ThreadingHTTPServer(('127.0.0.1',0),H)\n"
    "srv.daemon_threads=True\n"
    "open(sys.argv[3],'w').write(str(srv.server_address[1]))\n"
    "srv.serve_forever()\n";

typedef struct {
    pid_t pid;
    char dir[64];
    char rec[96];
    int port;
} srv;

static int write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    size_t n = strlen(data);
    size_t w = fwrite(data, 1, n, f);
    int c = fclose(f);
    return (w == n && c == 0) ? 0 : -1;
}

static int mkdir_p(const char *dir) {
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* Blocking-connect with a millisecond budget (readiness probe only). */
static int tcp_connect_ms(const char *ip, int port, long budget_ms) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) {
        close(s);
        return -1;
    }
    int r = connect(s, (struct sockaddr *)&a, sizeof a);
    if (r != 0 && errno != EINPROGRESS) {
        close(s);
        return -1;
    }
    fd_set w;
    FD_ZERO(&w);
    FD_SET(s, &w);
    struct timeval tv = {budget_ms / 1000, (budget_ms % 1000) * 1000};
    r = select(s + 1, NULL, &w, NULL, &tv);
    if (r <= 0) {
        close(s);
        return -1;
    }
    int err = 0;
    socklen_t n = sizeof err;
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &n) != 0 || err != 0) {
        close(s);
        return -1;
    }
    /* Tear down: shutdown (not full close-read) to avoid consuming a
     * server handler slot with a half-open probe. */
    close(s);
    return 0;
}

static void srv_stop(srv *s) {
    if (s->pid > 0) {
        kill(s->pid, SIGKILL);
        int st = 0;
        waitpid(s->pid, &st, 0);
        s->pid = -1;
    }
}

/* Start the server with the given routes JSON. Requires python3 + loopback. */
static int srv_start(srv *s, const char *routes_json) {
    memset(s, 0, sizeof *s);
    s->pid = -1;
    static unsigned ctr = 0;
    snprintf(s->dir, sizeof s->dir, "/tmp/opencode/i01wh-%d-%u", (int)getpid(), ctr++);
    if (mkdir_p(s->dir) != 0) return -1;
    char py[160], rj[160], pf[160];
    snprintf(py, sizeof py, "%s/srv.py", s->dir);
    snprintf(rj, sizeof rj, "%s/routes.json", s->dir);
    snprintf(s->rec, sizeof s->rec, "%s/rec.jsonl", s->dir);
    snprintf(pf, sizeof pf, "%s/port", s->dir);
    if (write_file(py, kSrv) != 0) return -1;
    if (write_file(rj, routes_json) != 0) return -1;
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > 2) close(fd);
        }
        execlp("python3", "python3", py, rj, s->rec, pf, (char *)NULL);
        _exit(127);
    }
    s->pid = p;
    for (int i = 0; i < 500; i++) {
        FILE *f = fopen(pf, "r");
        if (f) {
            int port = 0;
            if (fscanf(f, "%d", &port) == 1 && port > 0) {
                fclose(f);
                s->port = port;
                /* The script wrote the port just before serve_forever;
                 * poll-connect so a half-started server fails loudly. */
                for (int k = 0; k < 200; k++) {
                    if (tcp_connect_ms("127.0.0.1", port, 500) == 0) return 0;
                    struct timespec ts = {0, 10000000};
                    nanosleep(&ts, NULL);
                }
                srv_stop(s);
                return -1;
            }
            fclose(f);
        }
        struct timespec ts = {0, 10000000};
        nanosleep(&ts, NULL);
        int st = 0;
        if (waitpid(p, &st, WNOHANG) == p) {
            s->pid = -1; /* server died: missing prerequisite */
            return -1;
        }
    }
    srv_stop(s);
    return -1;
}

/* --- recorded-request inspection (raw JSONL substring matching) -------------- */

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    char *o = malloc((size_t)n + 1);
    if (!o) {
        fclose(f);
        return NULL;
    }
    size_t r = fread(o, 1, (size_t)n, f);
    fclose(f);
    o[r] = '\0';
    return o;
}

static int rec_count(const char *rec) {
    char *t = read_file(rec);
    if (!t) return -1;
    int n = 0;
    for (char *p = t; (p = strchr(p, '\n')) != NULL; p++) n++;
    free(t);
    return n;
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *b64enc(const unsigned char *p, size_t n) {
    char *o = malloc((n + 2) / 3 * 4 + 1);
    if (!o) return NULL;
    size_t w = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)p[i] << 16;
        int rem = (int)(n - i);
        if (rem > 1) v |= (unsigned)p[i + 1] << 8;
        if (rem > 2) v |= p[i + 2];
        o[w++] = kB64[(v >> 18) & 63];
        o[w++] = kB64[(v >> 12) & 63];
        o[w++] = rem > 1 ? kB64[(v >> 6) & 63] : '=';
        o[w++] = rem > 2 ? kB64[v & 63] : '=';
    }
    o[w] = '\0';
    return o;
}

/* One route JSON object. body_b64/body choice, flags. Caller frees. */
static char *route(const char *method, const char *path, int status, const char *hdrs,
                   const char *body_b64, const char *extra) {
    char *o = malloc(4096 + (body_b64 ? strlen(body_b64) : 0));
    if (!o) return NULL;
    snprintf(o, 4096 + (body_b64 ? strlen(body_b64) : 0),
             "{\"method\":\"%s\",\"path\":\"%s\",\"status\":%d,\"headers\":[%s],\"body_b64\":\"%s\"%s}",
             method, path, status, hdrs ? hdrs : "", body_b64 ? body_b64 : "",
             extra ? extra : "");
    return o;
}

static char *routes1(const char *r1) {
    char *o = malloc(strlen(r1) + 4);
    if (!o) return NULL;
    sprintf(o, "[%s]", r1);
    return o;
}

/* --- pure classifier + MIME unit cases (reference vectors) ------------------- */

CF_TEST(mime_lookup_like_rails) {
    const char *sym = NULL, *spell = NULL;
    CF_REQUIRE(cf_webhook_mime_lookup("image/jpeg", &sym, &spell));
    CF_CHECK(sym && !strcmp(sym, "jpeg") && !strcmp(spell, "image/jpeg"));
    CF_REQUIRE(cf_webhook_mime_lookup("application/x-gzip", &sym, &spell));
    CF_CHECK(sym && !strcmp(sym, "gzip") && !strcmp(spell, "application/gzip"));
    CF_REQUIRE(cf_webhook_mime_lookup("IMAGE/PNG", &sym, &spell));
    CF_CHECK(sym == NULL); /* unregistered but valid: kept verbatim */
    CF_REQUIRE(cf_webhook_mime_lookup("text/html; charset=utf-8", &sym, &spell));
    CF_CHECK(sym && !strcmp(sym, "html") && !strcmp(spell, "text/html"));
    CF_REQUIRE(cf_webhook_mime_lookup("video/quicktime", &sym, &spell));
    CF_CHECK(sym == NULL);
    CF_CHECK(!cf_webhook_mime_lookup("image", NULL, NULL));
    CF_CHECK(!cf_webhook_mime_lookup("", NULL, NULL));
    CF_CHECK(!cf_webhook_mime_lookup("text/html, text", NULL, NULL));
    CF_CHECK(!cf_webhook_mime_lookup("a/b c", NULL, NULL));
}

CF_TEST(classify_like_reference) {
    cf_webhook_delivery d;
    /* text/200: exact bodies incl. HTML passthrough and charset case. */
    CF_REQUIRE(cf_webhook_classify(200, "text/plain", (unsigned char *)"Hello back!", 11, &d) ==
               CF_WEBHOOK_OK);
    CF_CHECK(d.kind == CF_WEBHOOK_REPLY_TEXT && !strcmp(d.text, "Hello back!"));
    cf_webhook_delivery_dispose(&d);
    CF_REQUIRE(cf_webhook_classify(200, "text/plain", (unsigned char *)"H\xc3\xa9llo \xf0\x9f\x98\x80",
                                   11, &d) == CF_WEBHOOK_OK);
    CF_CHECK(d.kind == CF_WEBHOOK_REPLY_TEXT && !strcmp(d.text, "H\xc3\xa9llo \xf0\x9f\x98\x80"));
    cf_webhook_delivery_dispose(&d);
    /* Invalid UTF-8 is lossy (U+FFFD), like from_utf8_lossy. */
    CF_REQUIRE(cf_webhook_classify(200, "text/plain", (unsigned char *)"caf\xe9", 4, &d) ==
               CF_WEBHOOK_OK);
    CF_CHECK(d.kind == CF_WEBHOOK_REPLY_TEXT && !strcmp(d.text, "caf\xef\xbf\xbd"));
    cf_webhook_delivery_dispose(&d);
    /* Case-sensitive text match: Text/Plain is an attachment, kept verbatim. */
    CF_REQUIRE(cf_webhook_classify(200, "Text/Plain", (unsigned char *)"not text", 8, &d) ==
               CF_WEBHOOK_OK);
    CF_CHECK(d.kind == CF_WEBHOOK_REPLY_ATTACHMENT && !strcmp(d.filename, "attachment.") &&
             !strcmp(d.content_type, "Text/Plain") && d.data_len == 8 &&
             !memcmp(d.data, "not text", 8));
    cf_webhook_delivery_dispose(&d);
    /* Synonym: attachment.m4a with the registered spelling audio/aac. */
    CF_REQUIRE(cf_webhook_classify(200, "audio/mp4", (unsigned char *)"mp4", 3, &d) ==
               CF_WEBHOOK_OK);
    CF_CHECK(d.kind == CF_WEBHOOK_REPLY_ATTACHMENT && !strcmp(d.filename, "attachment.m4a") &&
             !strcmp(d.content_type, "audio/aac"));
    cf_webhook_delivery_dispose(&d);
    /* Non-200 text/html is an attachment, not text. */
    CF_REQUIRE(cf_webhook_classify(500, "text/html", (unsigned char *)"Internal Error!", 15, &d) ==
               CF_WEBHOOK_OK);
    CF_CHECK(d.kind == CF_WEBHOOK_REPLY_ATTACHMENT && !strcmp(d.filename, "attachment.html") &&
             !strcmp(d.content_type, "text/html"));
    cf_webhook_delivery_dispose(&d);
    /* 201 text/plain is an attachment (attachment.text). */
    CF_REQUIRE(cf_webhook_classify(201, "text/plain", (unsigned char *)"created", 7, &d) ==
               CF_WEBHOOK_OK);
    CF_CHECK(d.kind == CF_WEBHOOK_REPLY_ATTACHMENT && !strcmp(d.filename, "attachment.text"));
    cf_webhook_delivery_dispose(&d);
    /* Absent/empty/slashless content types. */
    CF_REQUIRE(cf_webhook_classify(500, NULL, (unsigned char *)"x", 1, &d) == CF_WEBHOOK_OK);
    CF_CHECK(d.kind == CF_WEBHOOK_REPLY_NONE);
    cf_webhook_delivery_dispose(&d);
    CF_CHECK(cf_webhook_classify(200, "image", (unsigned char *)"x", 1, &d) ==
             CF_WEBHOOK_INVALID_MIME);
    CF_CHECK(cf_webhook_classify(200, "", (unsigned char *)"x", 1, &d) == CF_WEBHOOK_INVALID_MIME);
}

/* --- live delivery cases ------------------------------------------------------- */

static const char kPayload[] = "{\"message\":\"hi\"}"; /* Content-Length 16 */

CF_TEST(delivers_like_the_reference) {
    /* Vectors: text, html, gzip, json, synonym, error-html, error-bare,
     * created-text, no-content-type (copied names/shapes from
     * webhook_cases.json). */
    char *b_text = b64enc((unsigned char *)"Hello back!", 11);
    char *b_html = b64enc((unsigned char *)"<b>Hi</b> & bye", 15);
    char *b_zip = b64enc((unsigned char *)"Zipped reply", 12);
    char *b_json = b64enc((unsigned char *)"{\"a\":1}", 7);
    char *b_mp4 = b64enc((unsigned char *)"mp4", 3);
    char *b_err = b64enc((unsigned char *)"Internal Error!", 15);
    char *b_cr = b64enc((unsigned char *)"created", 7);
    CF_REQUIRE(b_text && b_html && b_zip && b_json && b_mp4 && b_err && b_cr);
    char rs[8192];
    snprintf(rs, sizeof rs,
             "["
             "{\"method\":\"POST\",\"path\":\"/text\",\"status\":200,"
             "\"headers\":[[\"Content-Type\",\"text/plain\"]],\"body_b64\":\"%s\"},"
             "{\"method\":\"POST\",\"path\":\"/html\",\"status\":200,"
             "\"headers\":[[\"Content-Type\",\"text/html\"]],\"body_b64\":\"%s\"},"
             "{\"method\":\"POST\",\"path\":\"/gzip\",\"status\":200,"
             "\"headers\":[[\"Content-Type\",\"text/plain\"]],\"body_b64\":\"%s\",\"gzip\":true},"
             "{\"method\":\"POST\",\"path\":\"/json\",\"status\":200,"
             "\"headers\":[[\"Content-Type\",\"application/json\"]],\"body_b64\":\"%s\"},"
             "{\"method\":\"POST\",\"path\":\"/synonym\",\"status\":200,"
             "\"headers\":[[\"Content-Type\",\"audio/mp4\"]],\"body_b64\":\"%s\"},"
             "{\"method\":\"POST\",\"path\":\"/error-html\",\"status\":500,"
             "\"headers\":[[\"Content-Type\",\"text/html\"]],\"body_b64\":\"%s\"},"
             "{\"method\":\"POST\",\"path\":\"/error-bare\",\"status\":500,\"headers\":[],\"body_b64\":\"\"},"
             "{\"method\":\"POST\",\"path\":\"/created-text\",\"status\":201,"
             "\"headers\":[[\"Content-Type\",\"text/plain\"]],\"body_b64\":\"%s\"},"
             "{\"method\":\"POST\",\"path\":\"/no-content-type\",\"status\":200,\"headers\":[],\"body_b64\":\"\"}"
             "]",
             b_text, b_html, b_zip, b_json, b_mp4, b_err, b_cr);
    free(b_text);
    free(b_html);
    free(b_zip);
    free(b_json);
    free(b_mp4);
    free(b_err);
    free(b_cr);
    srv s;
    CF_REQUIRE(srv_start(&s, rs) == 0); /* LOUD if the loopback server fails */
    cf_webhook_config cfg;
    cf_webhook_default_config(&cfg);
    char url[128];
    cf_webhook_delivery d;
    /* text + html => Text with exact bytes. */
    snprintf(url, sizeof url, "http://127.0.0.1:%d/text", s.port);
    CF_REQUIRE(cf_webhook_deliver(&cfg, url, (unsigned char *)kPayload, 16, &d) == CF_WEBHOOK_OK);
    CF_CHECK(d.has_status && d.status == 200 && d.kind == CF_WEBHOOK_REPLY_TEXT &&
             !strcmp(d.text, "Hello back!"));
    cf_webhook_delivery_dispose(&d);
    snprintf(url, sizeof url, "http://127.0.0.1:%d/html", s.port);
    CF_REQUIRE(cf_webhook_deliver(&cfg, url, (unsigned char *)kPayload, 16, &d) == CF_WEBHOOK_OK);
    CF_CHECK(d.kind == CF_WEBHOOK_REPLY_TEXT && d.text && !strcmp(d.text, "<b>Hi</b> & bye"));
    cf_webhook_delivery_dispose(&d);
    /* gzip reply is inflated like Net::HTTP. */
    snprintf(url, sizeof url, "http://127.0.0.1:%d/gzip", s.port);
    CF_REQUIRE(cf_webhook_deliver(&cfg, url, (unsigned char *)kPayload, 16, &d) == CF_WEBHOOK_OK);
    CF_CHECK(d.kind == CF_WEBHOOK_REPLY_TEXT && !strcmp(d.text, "Zipped reply"));
    cf_webhook_delivery_dispose(&d);
    /* json + synonym attachments. */
    snprintf(url, sizeof url, "http://127.0.0.1:%d/json", s.port);
    CF_REQUIRE(cf_webhook_deliver(&cfg, url, (unsigned char *)kPayload, 16, &d) == CF_WEBHOOK_OK);
    CF_CHECK(d.kind == CF_WEBHOOK_REPLY_ATTACHMENT && !strcmp(d.filename, "attachment.json") &&
             !strcmp(d.content_type, "application/json") && d.data_len == 7);
    cf_webhook_delivery_dispose(&d);
    snprintf(url, sizeof url, "http://127.0.0.1:%d/synonym", s.port);
    CF_REQUIRE(cf_webhook_deliver(&cfg, url, (unsigned char *)kPayload, 16, &d) == CF_WEBHOOK_OK);
    CF_CHECK(d.kind == CF_WEBHOOK_REPLY_ATTACHMENT && !strcmp(d.filename, "attachment.m4a") &&
             !strcmp(d.content_type, "audio/aac"));
    cf_webhook_delivery_dispose(&d);
    /* error-html (500 text) => attachment.html; error-bare => none. */
    snprintf(url, sizeof url, "http://127.0.0.1:%d/error-html", s.port);
    CF_REQUIRE(cf_webhook_deliver(&cfg, url, (unsigned char *)kPayload, 16, &d) == CF_WEBHOOK_OK);
    CF_CHECK(d.status == 500 && d.kind == CF_WEBHOOK_REPLY_ATTACHMENT &&
             !strcmp(d.filename, "attachment.html"));
    cf_webhook_delivery_dispose(&d);
    snprintf(url, sizeof url, "http://127.0.0.1:%d/error-bare", s.port);
    CF_REQUIRE(cf_webhook_deliver(&cfg, url, (unsigned char *)kPayload, 16, &d) == CF_WEBHOOK_OK);
    CF_CHECK(d.status == 500 && d.kind == CF_WEBHOOK_REPLY_NONE);
    cf_webhook_delivery_dispose(&d);
    /* created-text (201) => attachment.text; no-content-type => none. */
    snprintf(url, sizeof url, "http://127.0.0.1:%d/created-text", s.port);
    CF_REQUIRE(cf_webhook_deliver(&cfg, url, (unsigned char *)kPayload, 16, &d) == CF_WEBHOOK_OK);
    CF_CHECK(d.status == 201 && d.kind == CF_WEBHOOK_REPLY_ATTACHMENT &&
             !strcmp(d.filename, "attachment.text"));
    cf_webhook_delivery_dispose(&d);
    snprintf(url, sizeof url, "http://127.0.0.1:%d/no-content-type", s.port);
    CF_REQUIRE(cf_webhook_deliver(&cfg, url, (unsigned char *)kPayload, 16, &d) == CF_WEBHOOK_OK);
    CF_CHECK(d.status == 200 && d.kind == CF_WEBHOOK_REPLY_NONE);
    cf_webhook_delivery_dispose(&d);
    /* Request exactness on the /text delivery: headers + body as Net::HTTP
     * sends them (order is curl's; the SET of headers is asserted). */
    char *rec = read_file(s.rec);
    CF_REQUIRE(rec != NULL);
    char want[512];
    snprintf(want, sizeof want, "[\"Host\", \"127.0.0.1:%d\"]", s.port);
    CF_CHECK(strstr(rec, "[\"Content-Type\", \"application/json\"]") != NULL);
    CF_CHECK(strstr(rec, "[\"Accept-Encoding\", \"gzip;q=1.0,deflate;q=0.6,identity;q=0.3\"]") !=
             NULL);
    CF_CHECK(strstr(rec, "[\"Accept\", \"*/*\"]") != NULL);
    CF_CHECK(strstr(rec, "[\"User-Agent\", \"Ruby\"]") != NULL);
    CF_CHECK(strstr(rec, want) != NULL);
    CF_CHECK(strstr(rec, "[\"Connection\", \"close\"]") != NULL);
    CF_CHECK(strstr(rec, "[\"Content-Length\", \"16\"]") != NULL);
    CF_CHECK(strstr(rec, "\"body_b64\": \"eyJtZXNzYWdlIjoiaGkifQ==\"") != NULL);
    CF_CHECK(strstr(rec, "\"target\": \"/text\"") != NULL);
    free(rec);
    srv_stop(&s);
}

CF_TEST(malformed_reply_is_not_retried) {
    char *r = route("POST", "/no-slash", 200, "[\"Content-Type\", \"image\"]", "", NULL);
    char *rs = routes1(r);
    free(r);
    CF_REQUIRE(rs != NULL);
    srv s;
    CF_REQUIRE(srv_start(&s, rs) == 0);
    free(rs);
    cf_webhook_config cfg;
    cf_webhook_default_config(&cfg);
    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/no-slash", s.port);
    cf_webhook_delivery d;
    /* One POST, one failure, no retry: the server saw exactly one delivery. */
    CF_CHECK(cf_webhook_deliver(&cfg, url, (unsigned char *)kPayload, 16, &d) ==
             CF_WEBHOOK_INVALID_MIME);
    CF_CHECK(rec_count(s.rec) == 1);
    /* A second explicit deliver is a second POST (caller-driven, not a retry). */
    CF_CHECK(cf_webhook_deliver(&cfg, url, (unsigned char *)kPayload, 16, &d) ==
             CF_WEBHOOK_INVALID_MIME);
    CF_CHECK(rec_count(s.rec) == 2);
    srv_stop(&s);
}

CF_TEST(rejects_replies_over_the_limit) {
    /* 256 KiB zeros with a 64 KiB test cap (production cap stays 100 MB);
     * plus a gzip bomb (128 KiB zeros gzipped to ~100 bytes) that must
     * abort on DECODED bytes, having read little more than the limit. */
    unsigned char *big = malloc(256 * 1024);
    CF_REQUIRE(big != NULL);
    memset(big, 0, 256 * 1024);
    char *b64 = b64enc(big, 256 * 1024);
    unsigned char *zeros = malloc(128 * 1024);
    CF_REQUIRE(zeros != NULL);
    memset(zeros, 0, 128 * 1024);
    /* gzip via python at serve time instead: send raw, flag gzip. */
    char *b64z = b64enc(zeros, 128 * 1024);
    free(big);
    free(zeros);
    CF_REQUIRE(b64 && b64z);
    size_t rn = strlen(b64) + strlen(b64z) + 512;
    char *rs = malloc(rn + 1);
    CF_REQUIRE(rs != NULL);
    snprintf(rs, rn + 1,
             "[{\"method\":\"POST\",\"path\":\"/big\",\"status\":200,"
             "\"headers\":[[\"Content-Type\",\"image/png\"]],\"body_b64\":\"%s\"},"
             "{\"method\":\"POST\",\"path\":\"/bomb\",\"status\":200,"
             "\"headers\":[[\"Content-Type\",\"image/png\"]],\"body_b64\":\"%s\",\"gzip\":true}]",
             b64, b64z);
    free(b64);
    free(b64z);
    srv s;
    CF_REQUIRE(srv_start(&s, rs) == 0);
    free(rs);
    cf_webhook_config cfg;
    cf_webhook_default_config(&cfg);
    cfg.http.max_bytes = 64 * 1024; /* test-only override */
    char url[128];
    cf_webhook_delivery d;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    snprintf(url, sizeof url, "http://127.0.0.1:%d/big", s.port);
    CF_CHECK(cf_webhook_deliver(&cfg, url, (unsigned char *)kPayload, 16, &d) ==
             CF_WEBHOOK_REPLY_LARGE);
    snprintf(url, sizeof url, "http://127.0.0.1:%d/bomb", s.port);
    CF_CHECK(cf_webhook_deliver(&cfg, url, (unsigned char *)kPayload, 16, &d) ==
             CF_WEBHOOK_REPLY_LARGE);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    CF_CHECK(ms < 5000); /* aborted at the cap, not after full inflation */
    srv_stop(&s);
}

CF_TEST(timeout_becomes_text_reply) {
    char rs[] = "[{\"method\":\"POST\",\"path\":\"/slow\",\"status\":200,"
                "\"headers\":[[\"Content-Type\",\"text/plain\"]],\"body_b64\":\"dG9vIGxhdGU=\","
                "\"delay_ms\":3000}]";
    srv s;
    CF_REQUIRE(srv_start(&s, rs) == 0);
    cf_webhook_config cfg;
    cf_webhook_default_config(&cfg);
    cfg.http.connect_timeout_ms = 1000;
    cfg.http.read_timeout_ms = 300;
    cfg.http.deadline_ms = 10000;
    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/slow", s.port);
    cf_webhook_delivery d;
    /* A stalled endpoint is answered with the 7-second text, status None. */
    CF_REQUIRE(cf_webhook_deliver(&cfg, url, (unsigned char *)kPayload, 16, &d) == CF_WEBHOOK_OK);
    CF_CHECK(d.timed_out && !d.has_status && d.timeout_secs == 7 && d.kind == CF_WEBHOOK_REPLY_NONE);
    cf_webhook_delivery_dispose(&d);
    srv_stop(&s);
}

CF_TEST(trickling_reply_hits_the_deadline) {
    char rs[] = "[{\"method\":\"POST\",\"path\":\"/hook\",\"status\":200,"
                "\"headers\":[[\"Content-Type\",\"image/png\"]],\"trickle\":true}]";
    srv s;
    CF_REQUIRE(srv_start(&s, rs) == 0);
    cf_webhook_config cfg;
    cf_webhook_default_config(&cfg);
    cfg.http.connect_timeout_ms = 1000;
    cfg.http.read_timeout_ms = 10000; /* trickle beats the read timeout */
    cfg.http.deadline_ms = 1500;      /* but not the delivery deadline */
    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/hook", s.port);
    cf_webhook_delivery d;
    CF_REQUIRE(cf_webhook_deliver(&cfg, url, (unsigned char *)kPayload, 16, &d) == CF_WEBHOOK_OK);
    CF_CHECK(d.timed_out && !d.has_status && d.timeout_secs == 1);
    cf_webhook_delivery_dispose(&d);
    srv_stop(&s);
}

CF_TEST(connection_refused_fails_without_retry) {
    /* Bind-then-close a port so the address refuses cleanly. */
    srv s;
    char rs[] = "[]";
    CF_REQUIRE(srv_start(&s, rs) == 0);
    int port = s.port;
    srv_stop(&s);
    cf_webhook_config cfg;
    cf_webhook_default_config(&cfg);
    cfg.http.connect_timeout_ms = 1000;
    cfg.http.read_timeout_ms = 1000;
    cfg.http.deadline_ms = 5000;
    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/hook", port);
    cf_webhook_delivery d;
    /* ECONNREFUSED-like: a failed delivery (job fails), not a timeout
     * reply and not retried (single attempt returns). */
    CF_CHECK(cf_webhook_deliver(&cfg, url, (unsigned char *)kPayload, 16, &d) ==
             CF_WEBHOOK_TRANSPORT);
    CF_CHECK(!d.timed_out);
}

/* The J02 consumer contract: re-check the bot before applying any reply.
 * Wiring lives with the integrator; this pins the seam both sides honor. */
static int g_applied = 0;
static bool g_bot_alive = false;

static bool fake_bot_alive(void) {
    return g_bot_alive;
}

static void fake_consumer_apply(const cf_webhook_delivery *d) {
    if (!cf_webhook_needs_apply(d)) return;
    if (!fake_bot_alive()) return; /* removed in flight: no resurrection */
    g_applied++;
}

CF_TEST(removed_bot_gets_no_reply) {
    cf_webhook_delivery d;
    CF_REQUIRE(cf_webhook_classify(200, "text/plain", (unsigned char *)"hi", 2, &d) ==
               CF_WEBHOOK_OK);
    CF_REQUIRE(cf_webhook_needs_apply(&d));
    g_applied = 0;
    g_bot_alive = false; /* bot removed while the request was in flight */
    fake_consumer_apply(&d);
    CF_CHECK(g_applied == 0);
    g_bot_alive = true;
    fake_consumer_apply(&d);
    CF_CHECK(g_applied == 1);
    cf_webhook_delivery_dispose(&d);
    CF_REQUIRE(cf_webhook_classify(500, NULL, (unsigned char *)"", 0, &d) == CF_WEBHOOK_OK);
    CF_CHECK(!cf_webhook_needs_apply(&d));
    g_applied = 0;
    g_bot_alive = true;
    fake_consumer_apply(&d);
    CF_CHECK(g_applied == 0);
    cf_webhook_delivery_dispose(&d);
}

CF_TEST_MAIN()
