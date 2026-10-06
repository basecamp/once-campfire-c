/* tests/integrations/test_unfurl.c — I01 unfurl acceptance (INT-01, JOB-02
 * webhook side unaffected). Page vectors copied from the pinned
 * tmp/rust-ref/crates/campfire/src/integrations/testdata/opengraph_cases.json
 * (bodies) with outcomes from opengraph_expected.json; expectations are the
 * reference's, never regenerated from this implementation. Page URLs carry
 * the ephemeral loopback port, so expected JSON is formatted with it
 * (canonical https://example.com stays verbatim: validated, never fetched).
 *
 * Transport runs against local loopback servers spawned here (Python
 * stdlib only; HTTPS via the pinned testdata/tls certs). A missing
 * prerequisite (no python3, bind failure, no ready signal, missing TLS
 * files) FAILS the case loudly — never a silent skip. Run from the
 * worktree root so the TLS fixtures resolve.
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

#include "integrations/unfurl.h"

static const char kSrv[] =
    "import base64,json,sys,time\n"
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
    "  port=str(self.server.server_address[1])\n"
    "  body=body.replace(b'@@PORT@@',port.encode())\n"
    "  if r.get('pad_to'): body=body+b' '*(r['pad_to']-len(body))\n"
    "  if r.get('gzip'): body=__import__('gzip').compress(body)\n"
    "  self.send_response(r['status'])\n"
    "  for k,v in r.get('headers',[]): self.send_header(k,v.replace('@@PORT@@',port))\n"
    "  if r.get('gzip'): self.send_header('Content-Encoding','gzip')\n"
    "  if r.get('chunked'): self.send_header('Transfer-Encoding','chunked')\n"
    "  elif not any(k.lower()=='content-length' for k,v in r.get('headers',[])):\n"
    "   self.send_header('Content-Length',str(len(body)))\n"
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

static const char kSrvTLS[] =
    "import base64,json,sys,time,ssl\n"
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
    "   f.write(json.dumps({'method':self.command,'target':self.path})+'\\n')\n"
    "  r=None\n"
    "  for x in routes:\n"
    "   if x['method']==self.command and x['path']==self.path:\n"
    "    r=x;break\n"
    "  if r is None:\n"
    "   b=b'not found';self.send_response(404);self.send_header('Content-Length',str(len(b)))\n"
    "   self.send_header('Connection','close');self.end_headers()\n"
    "   if self.command!='HEAD': self.wfile.write(b)\n"
    "   return\n"
    "  body=base64.b64decode(r.get('body_b64',''))\n"
    "  self.send_response(r['status'])\n"
    "  for k,v in r.get('headers',[]): self.send_header(k,v)\n"
    "  self.send_header('Content-Length',str(len(body)))\n"
    "  self.send_header('Connection','close');self.end_headers()\n"
    "  if self.command!='HEAD': self.wfile.write(body)\n"
    " def do_GET(self): self._run()\n"
    " def do_HEAD(self): self._run()\n"
    "srv=ThreadingHTTPServer(('127.0.0.1',0),H)\n"
    "ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)\n"
    "ctx.load_cert_chain(sys.argv[4],sys.argv[5])\n"
    "srv.socket=ctx.wrap_socket(srv.socket,server_side=True)\n"
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

static int srv_spawn(srv *s, const char *script, const char *routes_json, const char *tls_cert,
                     const char *tls_key) {
    memset(s, 0, sizeof *s);
    s->pid = -1;
    static unsigned ctr = 0;
    snprintf(s->dir, sizeof s->dir, "/tmp/opencode/i01uf-%d-%u", (int)getpid(), ctr++);
    if (mkdir_p(s->dir) != 0) return -1;
    char py[160], rj[160], pf[160];
    snprintf(py, sizeof py, "%s/srv.py", s->dir);
    snprintf(rj, sizeof rj, "%s/routes.json", s->dir);
    snprintf(s->rec, sizeof s->rec, "%s/rec.jsonl", s->dir);
    snprintf(pf, sizeof pf, "%s/port", s->dir);
    if (write_file(py, script) != 0) return -1;
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
        if (tls_cert)
            execlp("python3", "python3", py, rj, s->rec, pf, tls_cert, tls_key, (char *)NULL);
        else
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
                for (int k = 0; k < 200; k++) {
                    if (tcp_connect_ms("127.0.0.1", port, 500) == 0) {
                        /* TLS servers refuse plain probes; trust the port
                         * file (written just before serve_forever). */
                        if (tls_cert) return 0;
                        return 0;
                    }
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
            s->pid = -1;
            return -1;
        }
    }
    srv_stop(s);
    return -1;
}

static int srv_start(srv *s, const char *routes_json) {
    return srv_spawn(s, kSrv, routes_json, NULL, NULL);
}

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
    if (!t) return 0; /* no requests recorded yet */
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
        size_t rem = n - i;
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

/* Standard test config: fake DNS for the fixture hosts, loopback allowed. */
static void ucfg(cf_unfurl_config *c) {
    cf_unfurl_default_config(c);
    static const char *hosts[] = {"www.example.com", "example.com", "www.other.com",
                                  "fxtwitter.com", "twitter.com"};
    static const char *addrs[] = {"127.0.0.1", "127.0.0.1", "127.0.0.1", "127.0.0.1",
                                  "127.0.0.1"};
    c->test_hosts = hosts;
    c->test_addrs = addrs;
    c->ntest = 5;
    c->test_allow_private = true;
}

static long ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* --- route builder (all bodies base64; avoids JSON escaping) ------------------ */

typedef struct {
    char *buf;
    size_t len, cap;
    bool oom;
} jbuf;

static void jb_add(jbuf *j, const char *method, const char *path, int status, const char *ct,
                   const unsigned char *body, size_t body_len, const char *extra) {
    char *b64 = b64enc(body ? body : (unsigned char *)"", body_len);
    if (!b64) {
        j->oom = true;
        return;
    }
    char ctj[256] = "";
    if (ct) snprintf(ctj, sizeof ctj, "[\"Content-Type\",\"%s\"]", ct);
    size_t need =
        j->len + strlen(method) + strlen(path) + strlen(ctj) + strlen(b64) + (extra ? strlen(extra) : 0) + 128;
    if (need > j->cap) {
        size_t cap = j->cap ? j->cap : 4096;
        while (cap < need) cap *= 2;
        char *nb = realloc(j->buf, cap);
        if (!nb) {
            free(b64);
            j->oom = true;
            return;
        }
        j->buf = nb;
        j->cap = cap;
    }
    if (j->len) j->buf[j->len++] = ',';
    j->len += (size_t)snprintf(j->buf + j->len, j->cap - j->len,
                               "{\"method\":\"%s\",\"path\":\"%s\",\"status\":%d,\"headers\":[%s],"
                               "\"body_b64\":\"%s\"%s}",
                               method, path, status, ctj, b64, extra ? extra : "");
    free(b64);
}

static void jb_raw(jbuf *j, const char *route_json) {
    size_t need = j->len + strlen(route_json) + 8;
    if (need > j->cap) {
        size_t cap = j->cap ? j->cap : 4096;
        while (cap < need) cap *= 2;
        char *nb = realloc(j->buf, cap);
        if (!nb) {
            j->oom = true;
            return;
        }
        j->buf = nb;
        j->cap = cap;
    }
    if (j->len) j->buf[j->len++] = ',';
    strcpy(j->buf + j->len, route_json);
    j->len += strlen(route_json);
}

static char *jb_routes(jbuf *j) {
    char *o = malloc(j->len + 3);
    if (!o) return NULL;
    sprintf(o, "[%s]", j->buf ? j->buf : "");
    return o;
}

/* --- pure unit cases ---------------------------------------------------------- */

CF_TEST(blocks_private_addresses_like_surfguard) {
    /* From guard.rs classifies_addresses_like_surfguard (representative). */
    const char *blocked[] = {"0.0.0.0", "10.1.2.3", "100.64.0.1", "127.0.0.1", "168.63.129.16",
                             "169.254.169.254", "172.16.0.0", "192.168.1.1", "198.51.100.1",
                             "203.0.113.1", "224.0.0.1", "::", "::1", "::ffff:192.168.1.1",
                             "::ffff:8.8.8.8", "fc00::1", "fd00::1", "fe80::1", "2001:db8::1",
                             "2001:2::1", "64:ff9b:1::1"};
    for (size_t i = 0; i < sizeof blocked / sizeof blocked[0]; i++)
        CF_CHECK(cf_unfurl_blocked_ip(blocked[i]));
    const char *pub[] = {"8.8.8.8", "1.1.1.1", "93.184.216.34", "100.128.0.1",
                         "2606:2800:220:1:248:1893:25c8:1946", "2001:3::1", "64:ff9b::808:808"};
    for (size_t i = 0; i < sizeof pub / sizeof pub[0]; i++)
        CF_CHECK(!cf_unfurl_blocked_ip(pub[i]));
}

CF_TEST(filters_media_urls) {
    /* FILES_AND_MEDIA_URL_REGEX behavior on the fixture URLs. */
    CF_CHECK(cf_unfurl_is_media_url("http://www.example.com/video.mp4"));
    CF_CHECK(cf_unfurl_is_media_url("http://www.example.com/archive.tar.gz?x=1"));
    CF_CHECK(cf_unfurl_is_media_url("http://www.example.com/archive.tar.gzip")); /* tar + boundary */
    CF_CHECK(!cf_unfurl_is_media_url("http://www.example.com/UPPER.MP4"));       /* case sensitive */
    CF_CHECK(!cf_unfurl_is_media_url("http://www.example.com/page"));
}

/* --- live unfurl cases ----------------------------------------------------------
 * Page vectors copied from opengraph_cases.json routes; outcomes from
 * opengraph_expected.json bodies. Ephemeral ports enter pages/redirects via
 * the server's @@PORT@@ substitution; expected JSON is formatted with the
 * bound port (canonical https://example.com stays verbatim: validated
 * through the guard, never fetched). */

/* Fixture page (the '/' route): og:image points at this server's HEAD route. */
#define IMG "http://example.com:@@PORT@@/image.png"
static const char kPage[] = "<html><head><meta property=\"og:url\" content=\"https://example.com\">"
                            "<meta property=\"og:title\" content=\"Hey!\">"
                            "<meta property=\"og:description\" content=\"desc..\">"
                            "<meta property=\"og:image\" content=\"" IMG "\"></head></html>";

static void expect_success(char *out, size_t cap, int port) {
    snprintf(out, cap,
             "{\"title\":\"Hey!\",\"url\":\"https://example.com\","
             "\"image\":\"http://example.com:%d/image.png\",\"description\":\"desc..\","
             "\"context_for_validation\":{\"context\":null},\"errors\":{}}",
             port);
}

CF_TEST(unfurls_like_the_reference) {
    jbuf j;
    memset(&j, 0, sizeof j);
    jb_add(&j, "GET", "/", 200, "text/html", (unsigned char *)kPage, strlen(kPage), NULL);
    jb_add(&j, "HEAD", "/image.png", 200, "image/png", NULL, 0, NULL);
    char *rs = jb_routes(&j);
    free(j.buf);
    CF_REQUIRE(rs != NULL && !j.oom);
    srv s;
    CF_REQUIRE(srv_start(&s, rs) == 0); /* LOUD if the loopback server fails */
    free(rs);
    cf_unfurl_config cfg;
    ucfg(&cfg);
    char url[128], want[1024];
    snprintf(url, sizeof url, "http://www.example.com:%d/", s.port);
    expect_success(want, sizeof want, s.port);
    cf_unfurl_out o;
    CF_REQUIRE(cf_unfurl(&cfg, url, &o) == 0);
    CF_CHECK(o.kind == CF_UNFURL_JSON && o.json && !strcmp(o.json, want));
    cf_unfurl_out_dispose(&o);
    /* Request shape as Net::HTTP sends it (order is curl's; the set counts). */
    char *rec = read_file(s.rec);
    CF_REQUIRE(rec != NULL);
    CF_CHECK(strstr(rec, "\"method\": \"GET\"") != NULL);
    CF_CHECK(strstr(rec, "\"target\": \"/\"") != NULL);
    CF_CHECK(strstr(rec, "[\"Accept\", \"*/*\"]") != NULL);
    CF_CHECK(strstr(rec, "[\"Accept-Encoding\", \"gzip;q=1.0,deflate;q=0.6,identity;q=0.3\"]") != NULL);
    CF_CHECK(strstr(rec, "[\"User-Agent\", \"Ruby\"]") != NULL);
    char host[64];
    snprintf(host, sizeof host, "[\"Host\", \"www.example.com:%d\"]", s.port);
    CF_CHECK(strstr(rec, host) != NULL);
    CF_CHECK(strstr(rec, "\"method\": \"HEAD\"") != NULL);
    free(rec);
    srv_stop(&s);
}

CF_TEST(relative_og_url_falls_back) {
    /* The '/relative-og-url' route: og:url "/foo" is not absolute http(s). */
    static const char page[] = "<html><head><meta property=\"og:url\" content=\"/foo\">"
                               "<meta property=\"og:title\" content=\"Hey!\">"
                               "<meta property=\"og:description\" content=\"desc..\">"
                               "<meta property=\"og:image\" content=\"" IMG "\"></head></html>";
    jbuf j;
    memset(&j, 0, sizeof j);
    jb_add(&j, "GET", "/relative-og-url", 200, "text/html", (unsigned char *)page, strlen(page),
           NULL);
    jb_add(&j, "HEAD", "/image.png", 200, "image/png", NULL, 0, NULL);
    char *rs = jb_routes(&j);
    free(j.buf);
    CF_REQUIRE(rs != NULL && !j.oom);
    srv s;
    CF_REQUIRE(srv_start(&s, rs) == 0);
    free(rs);
    cf_unfurl_config cfg;
    ucfg(&cfg);
    char url[128], want[1024];
    snprintf(url, sizeof url, "http://www.example.com:%d/relative-og-url", s.port);
    snprintf(want, sizeof want,
             "{\"title\":\"Hey!\",\"url\":\"%s\","
             "\"image\":\"http://example.com:%d/image.png\",\"description\":\"desc..\","
             "\"context_for_validation\":{\"context\":null},\"errors\":{}}",
             url, s.port);
    cf_unfurl_out o;
    CF_REQUIRE(cf_unfurl(&cfg, url, &o) == 0);
    CF_CHECK(o.kind == CF_UNFURL_JSON && o.json && !strcmp(o.json, want));
    cf_unfurl_out_dispose(&o);
    srv_stop(&s);
}

CF_TEST(follows_absolute_redirects_and_denies_the_rest) {
    jbuf j;
    memset(&j, 0, sizeof j);
    jb_add(&j, "GET", "/", 200, "text/html", (unsigned char *)kPage, strlen(kPage), NULL);
    jb_add(&j, "HEAD", "/image.png", 200, "image/png", NULL, 0, NULL);
    /* Absolute http(s) target with the loopback port substituted. */
    jb_raw(&j, "{\"method\":\"GET\",\"path\":\"/redirect\",\"status\":302,"
               "\"headers\":[[\"Location\",\"http://www.other.com:@@PORT@@/\"]],\"body_b64\":\"\"}");
    jb_raw(&j, "{\"method\":\"GET\",\"path\":\"/relative-redirect\",\"status\":301,"
               "\"headers\":[[\"Location\",\"/\"]],\"body_b64\":\"\"}");
    jb_raw(&j, "{\"method\":\"GET\",\"path\":\"/redirect-no-location\",\"status\":302,"
               "\"headers\":[],\"body_b64\":\"\"}");
    jb_raw(&j, "{\"method\":\"GET\",\"path\":\"/redirect-ftp\",\"status\":302,"
               "\"headers\":[[\"Location\",\"ftp://www.other.com/\"]],\"body_b64\":\"\"}");
    char *rs = jb_routes(&j);
    free(j.buf);
    CF_REQUIRE(rs != NULL && !j.oom);
    srv s;
    CF_REQUIRE(srv_start(&s, rs) == 0);
    free(rs);
    cf_unfurl_config cfg;
    ucfg(&cfg);
    char url[128], want[1024];
    cf_unfurl_out o;
    /* Absolute redirect to another (mapped) host is followed. */
    snprintf(url, sizeof url, "http://www.example.com:%d/redirect", s.port);
    expect_success(want, sizeof want, s.port);
    CF_REQUIRE(cf_unfurl(&cfg, url, &o) == 0);
    CF_CHECK(o.kind == CF_UNFURL_JSON && o.json && !strcmp(o.json, want));
    cf_unfurl_out_dispose(&o);
    /* Relative target, missing Location, and ftp target: no unfurl. */
    snprintf(url, sizeof url, "http://www.example.com:%d/relative-redirect", s.port);
    CF_REQUIRE(cf_unfurl(&cfg, url, &o) == 0);
    CF_CHECK(o.kind == CF_UNFURL_NONE);
    cf_unfurl_out_dispose(&o);
    snprintf(url, sizeof url, "http://www.example.com:%d/redirect-no-location", s.port);
    CF_REQUIRE(cf_unfurl(&cfg, url, &o) == 0);
    CF_CHECK(o.kind == CF_UNFURL_NONE);
    cf_unfurl_out_dispose(&o);
    snprintf(url, sizeof url, "http://www.example.com:%d/redirect-ftp", s.port);
    CF_REQUIRE(cf_unfurl(&cfg, url, &o) == 0);
    CF_CHECK(o.kind == CF_UNFURL_NONE);
    cf_unfurl_out_dispose(&o);
    srv_stop(&s);
}

CF_TEST(ten_redirects_are_too_many) {
    /* Eleven-link chain: 10 requests, then TooManyRedirects => 204. */
    jbuf j;
    memset(&j, 0, sizeof j);
    for (int i = 0; i <= 10; i++) {
        char path[16], loc[512];
        snprintf(path, sizeof path, "/c%d", i);
        if (i < 10)
            snprintf(loc, sizeof loc,
                     "{\"method\":\"GET\",\"path\":\"%s\",\"status\":302,"
                     "\"headers\":[[\"Location\",\"http://www.example.com:@@PORT@@/c%d\"]],"
                     "\"body_b64\":\"\"}",
                     path, i + 1);
        else
            snprintf(loc, sizeof loc,
                     "{\"method\":\"GET\",\"path\":\"%s\",\"status\":200,"
                     "\"headers\":[[\"Content-Type\",\"text/html\"]],\"body_b64\":\"eA==\"}",
                     path);
        jb_raw(&j, loc);
    }
    char *rs = jb_routes(&j);
    free(j.buf);
    CF_REQUIRE(rs != NULL && !j.oom);
    srv s;
    CF_REQUIRE(srv_start(&s, rs) == 0);
    free(rs);
    cf_unfurl_config cfg;
    ucfg(&cfg);
    char url[128];
    snprintf(url, sizeof url, "http://www.example.com:%d/c0", s.port);
    cf_unfurl_out o;
    CF_REQUIRE(cf_unfurl(&cfg, url, &o) == 0);
    CF_CHECK(o.kind == CF_UNFURL_NONE);
    cf_unfurl_out_dispose(&o);
    CF_CHECK(rec_count(s.rec) == 10);
    srv_stop(&s);
}

CF_TEST(rejects_non_documents) {
    jbuf j;
    memset(&j, 0, sizeof j);
    jb_add(&j, "GET", "/plain", 200, "text/plain", (unsigned char *)"I'm not HTML!", 13, NULL);
    jb_add(&j, "GET", "/created", 201, "text/html", (unsigned char *)kPage, strlen(kPage), NULL);
    jb_add(&j, "GET", "/forbidden", 403, "text/html", (unsigned char *)kPage, strlen(kPage), NULL);
    jb_add(&j, "GET", "/no-content-type", 200, NULL, (unsigned char *)kPage, strlen(kPage), NULL);
    jb_add(&j, "GET", "/html-charset", 200, "Text/HTML ; charset=utf-8", (unsigned char *)kPage,
           strlen(kPage), NULL);
    char *rs = jb_routes(&j);
    free(j.buf);
    CF_REQUIRE(rs != NULL && !j.oom);
    srv s;
    CF_REQUIRE(srv_start(&s, rs) == 0);
    free(rs);
    cf_unfurl_config cfg;
    ucfg(&cfg);
    const char *paths[] = {"/plain", "/created", "/forbidden", "/no-content-type", "/html-charset"};
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        char url[128];
        snprintf(url, sizeof url, "http://www.example.com:%d%s", s.port, paths[i]);
        cf_unfurl_out o;
        CF_REQUIRE(cf_unfurl(&cfg, url, &o) == 0);
        CF_CHECK(o.kind == CF_UNFURL_NONE);
        cf_unfurl_out_dispose(&o);
    }
    srv_stop(&s);
}

CF_TEST(enforces_the_body_cap) {
    /* 5 MiB + 1 chunked bytes => none; exactly 5 MiB => unfurled. */
    unsigned char *big = malloc(5 * 1024 * 1024 + 1);
    CF_REQUIRE(big != NULL);
    memset(big, 'x', 5 * 1024 * 1024 + 1);
    char *b64 = b64enc(big, 5 * 1024 * 1024 + 1);
    free(big);
    CF_REQUIRE(b64 != NULL);
    jbuf j;
    memset(&j, 0, sizeof j);
    size_t need = strlen(b64) + 512;
    char *rbig = malloc(need);
    CF_REQUIRE(rbig != NULL);
    snprintf(rbig, need,
             "{\"method\":\"GET\",\"path\":\"/big\",\"status\":200,"
             "\"headers\":[[\"Content-Type\",\"text/html\"]],\"body_b64\":\"%s\",\"chunked\":true}",
             b64);
    free(b64);
    jb_raw(&j, rbig);
    free(rbig);
    jb_add(&j, "GET", "/exact", 200, "text/html", (unsigned char *)kPage, strlen(kPage),
           ",\"pad_to\":5242880,\"chunked\":true");
    jb_add(&j, "HEAD", "/image.png", 200, "image/png", NULL, 0, NULL);
    char *rs = jb_routes(&j);
    free(j.buf);
    CF_REQUIRE(rs != NULL && !j.oom);
    srv s;
    CF_REQUIRE(srv_start(&s, rs) == 0);
    free(rs);
    cf_unfurl_config cfg;
    ucfg(&cfg);
    char url[128], want[1024];
    cf_unfurl_out o;
    snprintf(url, sizeof url, "http://www.example.com:%d/big", s.port);
    CF_REQUIRE(cf_unfurl(&cfg, url, &o) == 0);
    CF_CHECK(o.kind == CF_UNFURL_NONE);
    cf_unfurl_out_dispose(&o);
    snprintf(url, sizeof url, "http://www.example.com:%d/exact", s.port);
    expect_success(want, sizeof want, s.port);
    CF_REQUIRE(cf_unfurl(&cfg, url, &o) == 0);
    CF_CHECK(o.kind == CF_UNFURL_JSON && o.json && !strcmp(o.json, want));
    cf_unfurl_out_dispose(&o);
    srv_stop(&s);
}

CF_TEST(inflates_gzip_bodies) {
    jbuf j;
    memset(&j, 0, sizeof j);
    static const char zpage[] = "<html><head><meta property=\"og:url\" content=\"https://example.com\">"
                                "<meta property=\"og:title\" content=\"Zipped\">"
                                "<meta property=\"og:description\" content=\"desc..\">"
                                "<meta property=\"og:image\" content=\"" IMG "\"></head></html>";
    jb_add(&j, "GET", "/gzip", 200, "text/html", (unsigned char *)zpage, strlen(zpage),
           ",\"gzip\":true");
    jb_add(&j, "HEAD", "/image.png", 200, "image/png", NULL, 0, NULL);
    char *rs = jb_routes(&j);
    free(j.buf);
    CF_REQUIRE(rs != NULL && !j.oom);
    srv s;
    CF_REQUIRE(srv_start(&s, rs) == 0);
    free(rs);
    cf_unfurl_config cfg;
    ucfg(&cfg);
    char url[128], want[1024];
    snprintf(url, sizeof url, "http://www.example.com:%d/gzip", s.port);
    snprintf(want, sizeof want,
             "{\"title\":\"Zipped\",\"url\":\"https://example.com\","
             "\"image\":\"http://example.com:%d/image.png\",\"description\":\"desc..\","
             "\"context_for_validation\":{\"context\":null},\"errors\":{}}",
             s.port);
    cf_unfurl_out o;
    CF_REQUIRE(cf_unfurl(&cfg, url, &o) == 0);
    CF_CHECK(o.kind == CF_UNFURL_JSON && o.json && !strcmp(o.json, want));
    cf_unfurl_out_dispose(&o);
    srv_stop(&s);
}

CF_TEST(handles_charsets_like_the_reference) {
    /* Bodies copied from opengraph_cases.json; outcomes from expected. */
    static const char no_meta[] = "<html><head>"
                                  "<meta property=\"og:title\" content=\"Caf\xc3\xa9 \xf0\x9f\x98\x80 ok\">"
                                  "<meta property=\"og:description\" content=\"na\xc3\xafve &eacute; &#233; x\">"
                                  "</head></html>";
    static const char meta[] = "<html><head><meta charset=\"utf-8\">"
                               "<meta property=\"og:title\" content=\"Caf\xc3\xa9 \xf0\x9f\x98\x80 ok\">"
                               "<meta property=\"og:description\" content=\"na\xc3\xafve &eacute; x\">"
                               "</head></html>";
    jbuf j;
    memset(&j, 0, sizeof j);
    jb_add(&j, "GET", "/utf8-no-meta", 200, "text/html", (unsigned char *)no_meta, strlen(no_meta),
           NULL);
    jb_add(&j, "GET", "/utf8-meta", 200, "text/html", (unsigned char *)meta, strlen(meta), NULL);
    char *rs = jb_routes(&j);
    free(j.buf);
    CF_REQUIRE(rs != NULL && !j.oom);
    srv s;
    CF_REQUIRE(srv_start(&s, rs) == 0);
    free(rs);
    cf_unfurl_config cfg;
    ucfg(&cfg);
    char url[128], want[1024];
    cf_unfurl_out o;
    /* Without a meta charset, non-ASCII is dropped. */
    snprintf(url, sizeof url, "http://www.example.com:%d/utf8-no-meta", s.port);
    snprintf(want, sizeof want,
             "{\"title\":\"Caf  ok\",\"description\":\"nave   x\",\"url\":\"%s\",\"image\":null,"
             "\"context_for_validation\":{\"context\":null},\"errors\":{}}",
             url);
    CF_REQUIRE(cf_unfurl(&cfg, url, &o) == 0);
    CF_CHECK(o.kind == CF_UNFURL_JSON && o.json && !strcmp(o.json, want));
    cf_unfurl_out_dispose(&o);
    /* With one, everything is kept. */
    snprintf(url, sizeof url, "http://www.example.com:%d/utf8-meta", s.port);
    snprintf(want, sizeof want,
             "{\"title\":\"Caf\xc3\xa9 \xf0\x9f\x98\x80 ok\","
             "\"description\":\"na\xc3\xafve \xc3\xa9 x\",\"url\":\"%s\",\"image\":null,"
             "\"context_for_validation\":{\"context\":null},\"errors\":{}}",
             url);
    CF_REQUIRE(cf_unfurl(&cfg, url, &o) == 0);
    CF_CHECK(o.kind == CF_UNFURL_JSON && o.json && !strcmp(o.json, want));
    cf_unfurl_out_dispose(&o);
    srv_stop(&s);
}

CF_TEST(strips_markup_in_validation) {
    /* The '/script' route: encoded tags in the title, a live script in the
     * description; both reduce to text before validation. */
    static const char page[] = "<html><head><meta property=\"og:url\" content=\"https://example.com\">"
                               "<meta property=\"og:title\" content=\"Hey!&lt;script&gt;alert('hi')&lt;/script&gt;\">"
                               "<meta property=\"og:description\" content=\"Hello<script>alert('hi')</script>\">"
                               "<meta property=\"og:image\" content=\"" IMG "\"></head></html>";
    jbuf j;
    memset(&j, 0, sizeof j);
    jb_add(&j, "GET", "/script", 200, "text/html", (unsigned char *)page, strlen(page), NULL);
    jb_add(&j, "HEAD", "/image.png", 200, "image/png", NULL, 0, NULL);
    char *rs = jb_routes(&j);
    free(j.buf);
    CF_REQUIRE(rs != NULL && !j.oom);
    srv s;
    CF_REQUIRE(srv_start(&s, rs) == 0);
    free(rs);
    cf_unfurl_config cfg;
    ucfg(&cfg);
    char url[128], want[1024];
    snprintf(url, sizeof url, "http://www.example.com:%d/script", s.port);
    snprintf(want, sizeof want,
             "{\"title\":\"Hey!alert('hi')\",\"url\":\"https://example.com\","
             "\"image\":\"http://example.com:%d/image.png\",\"description\":\"Helloalert('hi')\","
             "\"context_for_validation\":{\"context\":null},\"errors\":{}}",
             s.port);
    cf_unfurl_out o;
    CF_REQUIRE(cf_unfurl(&cfg, url, &o) == 0);
    CF_CHECK(o.kind == CF_UNFURL_JSON && o.json && !strcmp(o.json, want));
    cf_unfurl_out_dispose(&o);
    srv_stop(&s);
}

CF_TEST(gives_up_on_stalled_pages) {
    jbuf j;
    memset(&j, 0, sizeof j);
    jb_raw(&j, "{\"method\":\"GET\",\"path\":\"/slow\",\"status\":200,"
               "\"headers\":[[\"Content-Type\",\"text/html\"]],\"body_b64\":\"eA==\","
               "\"delay_ms\":5000}");
    jb_raw(&j, "{\"method\":\"GET\",\"path\":\"/trickle\",\"status\":200,"
               "\"headers\":[[\"Content-Type\",\"text/html\"]],\"trickle\":true}");
    char *rs = jb_routes(&j);
    free(j.buf);
    CF_REQUIRE(rs != NULL && !j.oom);
    srv s;
    CF_REQUIRE(srv_start(&s, rs) == 0);
    free(rs);
    cf_unfurl_config cfg;
    ucfg(&cfg);
    cfg.connect_timeout_ms = 1000;
    cfg.read_timeout_ms = 300;
    cfg.deadline_ms = 2000;
    char url[128];
    cf_unfurl_out o;
    long t0 = ms_now();
    snprintf(url, sizeof url, "http://www.example.com:%d/slow", s.port);
    CF_REQUIRE(cf_unfurl(&cfg, url, &o) == 0);
    CF_CHECK(o.kind == CF_UNFURL_NONE); /* timeout => no unfurl, not an error */
    cf_unfurl_out_dispose(&o);
    snprintf(url, sizeof url, "http://www.example.com:%d/trickle", s.port);
    CF_REQUIRE(cf_unfurl(&cfg, url, &o) == 0);
    CF_CHECK(o.kind == CF_UNFURL_NONE);
    cf_unfurl_out_dispose(&o);
    CF_CHECK(ms_now() - t0 < 5000);
    srv_stop(&s);
}

CF_TEST(skips_media_urls_without_fetching) {
    jbuf j;
    memset(&j, 0, sizeof j);
    jb_add(&j, "GET", "/video.mp4", 200, "text/html", (unsigned char *)kPage, strlen(kPage), NULL);
    char *rs = jb_routes(&j);
    free(j.buf);
    CF_REQUIRE(rs != NULL && !j.oom);
    srv s;
    CF_REQUIRE(srv_start(&s, rs) == 0);
    free(rs);
    cf_unfurl_config cfg;
    ucfg(&cfg);
    char url[128];
    snprintf(url, sizeof url, "http://www.example.com:%d/video.mp4", s.port);
    cf_unfurl_out o;
    CF_REQUIRE(cf_unfurl(&cfg, url, &o) == 0);
    CF_CHECK(o.kind == CF_UNFURL_NONE);
    cf_unfurl_out_dispose(&o);
    CF_CHECK(rec_count(s.rec) == 0); /* resolved, never fetched */
    srv_stop(&s);
}

CF_TEST(rejects_bad_urls) {
    cf_unfurl_config cfg;
    ucfg(&cfg);
    cf_unfurl_out o;
    /* Non-http scheme: resolved, never fetched. */
    CF_REQUIRE(cf_unfurl(&cfg, "ftp://www.example.com/", &o) == 0);
    CF_CHECK(o.kind == CF_UNFURL_NONE);
    cf_unfurl_out_dispose(&o);
    /* Relative, blank-padded, and non-ASCII inputs never fetch. */
    CF_REQUIRE(cf_unfurl(&cfg, "httpfake", &o) == 0);
    CF_CHECK(o.kind == CF_UNFURL_NONE);
    cf_unfurl_out_dispose(&o);
    CF_REQUIRE(cf_unfurl(&cfg, " foo", &o) == 0);
    CF_CHECK(o.kind == CF_UNFURL_NONE);
    cf_unfurl_out_dispose(&o);
    CF_REQUIRE(cf_unfurl(&cfg, "http://www.example.com/\xc3\xa9", &o) == 0);
    CF_CHECK(o.kind == CF_UNFURL_NONE);
    cf_unfurl_out_dispose(&o);
    /* mailto raises InvalidComponent (a 500 in the controller). */
    CF_REQUIRE(cf_unfurl(&cfg, "mailto:foo", &o) == 0);
    CF_CHECK(o.kind == CF_UNFURL_RAISED && o.raised && !strcmp(o.raised, "URI::InvalidComponentError"));
    cf_unfurl_out_dispose(&o);
}

CF_TEST(blocks_private_targets_without_fetching) {
    /* Guard enforced end-to-end (private addresses never reach the server).
     * Uses a strict config: no loopback exception. */
    cf_unfurl_config cfg;
    cf_unfurl_default_config(&cfg);
    static const char *hosts[] = {"www.example.com"};
    static const char *addrs[] = {"127.0.0.1"};
    cfg.test_hosts = hosts;
    cfg.test_addrs = addrs;
    cfg.ntest = 1;
    cfg.test_allow_private = false;
    jbuf j;
    memset(&j, 0, sizeof j);
    jb_add(&j, "GET", "/", 200, "text/html", (unsigned char *)kPage, strlen(kPage), NULL);
    char *rs = jb_routes(&j);
    free(j.buf);
    CF_REQUIRE(rs != NULL && !j.oom);
    srv s;
    CF_REQUIRE(srv_start(&s, rs) == 0);
    free(rs);
    char url[128];
    snprintf(url, sizeof url, "http://www.example.com:%d/", s.port);
    cf_unfurl_out o;
    CF_REQUIRE(cf_unfurl(&cfg, url, &o) == 0);
    CF_CHECK(o.kind == CF_UNFURL_NONE);
    cf_unfurl_out_dispose(&o);
    /* Decimal/hex IP literals collapse to loopback: blocked, never fetched. */
    CF_REQUIRE(cf_unfurl(&cfg, "http://2130706433/", &o) == 0);
    CF_CHECK(o.kind == CF_UNFURL_NONE);
    cf_unfurl_out_dispose(&o);
    CF_REQUIRE(cf_unfurl(&cfg, "http://0x7f.1/", &o) == 0);
    CF_CHECK(o.kind == CF_UNFURL_NONE);
    cf_unfurl_out_dispose(&o);
    CF_CHECK(rec_count(s.rec) == 0);
    srv_stop(&s);
}

CF_TEST(rewrites_tweets_through_fxtwitter) {
    /* Tweet page copied from the fxtwitter.com route (no og:url/image). */
    static const char tweet[] = "<html><head>"
                                "<meta property=\"og:title\" content=\"DHH \xf0\x9f\x98\x80 \xe2\x80\x9cquoted\xe2\x80\x9d\">"
                                "<meta property=\"og:description\" content=\"tweet \xc3\xa9\">"
                                "</head></html>";
    jbuf j;
    memset(&j, 0, sizeof j);
    jb_add(&j, "GET", "/dhh/status/1", 200, "text/html", (unsigned char *)tweet, strlen(tweet),
           NULL);
    char *rs = jb_routes(&j);
    free(j.buf);
    CF_REQUIRE(rs != NULL && !j.oom);
    srv s;
    CF_REQUIRE(srv_start(&s, rs) == 0);
    free(rs);
    cf_unfurl_config cfg;
    ucfg(&cfg);
    char url[128], want[1024];
    snprintf(url, sizeof url, "http://twitter.com:%d/dhh/status/1", s.port);
    /* Canonical falls back to the ORIGINAL twitter URL; no meta charset, so
     * non-ASCII is stripped exactly like the reference. */
    snprintf(want, sizeof want,
             "{\"title\":\"DHH  quoted\",\"description\":\"tweet \",\"url\":\"%s\",\"image\":null,"
             "\"context_for_validation\":{\"context\":null},\"errors\":{}}",
             url);
    cf_unfurl_out o;
    CF_REQUIRE(cf_unfurl(&cfg, url, &o) == 0);
    CF_CHECK(o.kind == CF_UNFURL_JSON && o.json && !strcmp(o.json, want));
    cf_unfurl_out_dispose(&o);
    /* The fetch went to fxtwitter, never to twitter.com. */
    char *rec = read_file(s.rec);
    CF_REQUIRE(rec != NULL);
    CF_CHECK(strstr(rec, "\"target\": \"/dhh/status/1\"") != NULL);
    char host[64];
    snprintf(host, sizeof host, "[\"Host\", \"fxtwitter.com:%d\"]", s.port);
    CF_CHECK(strstr(rec, host) != NULL);
    CF_CHECK(rec_count(s.rec) == 1);
    free(rec);
    srv_stop(&s);
}

CF_TEST(verifies_tls_against_the_test_ca) {
    /* Pinned fixtures: SAN covers www.example.com (CN fcm.googleapis.com). */
    const char *ca = "tests/fixtures/crates/campfire/src/integrations/testdata/tls/ca.pem";
    const char *cert = "tests/fixtures/crates/campfire/src/integrations/testdata/tls/server.pem";
    const char *key = "tests/fixtures/crates/campfire/src/integrations/testdata/tls/server.key";
    FILE *f = fopen(ca, "r");
    CF_REQUIRE(f != NULL); /* LOUD: run from the worktree root */
    if (f) fclose(f);
    static const char page[] = "<html><head><meta property=\"og:url\" content=\"https://example.com\">"
                               "<meta property=\"og:title\" content=\"TLS\">"
                               "<meta property=\"og:description\" content=\"d\"></head></html>";
    char *b64 = b64enc((unsigned char *)page, strlen(page));
    CF_REQUIRE(b64 != NULL);
    size_t n = strlen(b64) + 256;
    char *rs = malloc(n);
    CF_REQUIRE(rs != NULL);
    snprintf(rs, n,
             "[{\"method\":\"GET\",\"path\":\"/\",\"status\":200,"
             "\"headers\":[[\"Content-Type\",\"text/html\"]],\"body_b64\":\"%s\"}]",
             b64);
    free(b64);
    srv s;
    CF_REQUIRE(srv_spawn(&s, kSrvTLS, rs, cert, key) == 0);
    free(rs);
    cf_unfurl_config cfg;
    ucfg(&cfg);
    cfg.ca_path = ca;
    char url[128], want[512];
    snprintf(url, sizeof url, "https://www.example.com:%d/", s.port);
    snprintf(want, sizeof want,
             "{\"title\":\"TLS\",\"url\":\"https://example.com\",\"description\":\"d\",\"image\":null,"
             "\"context_for_validation\":{\"context\":null},\"errors\":{}}");
    cf_unfurl_out o;
    CF_REQUIRE(cf_unfurl(&cfg, url, &o) == 0);
    CF_CHECK(o.kind == CF_UNFURL_JSON && o.json && !strcmp(o.json, want));
    cf_unfurl_out_dispose(&o);
    /* An untrusted CA is a failed fetch (no unfurl), never an exception. */
    char bogus[128];
    snprintf(bogus, sizeof bogus, "%s/bogus-ca.pem", s.dir);
    CF_REQUIRE(write_file(bogus, "not a certificate") == 0);
    cfg.ca_path = bogus;
    CF_REQUIRE(cf_unfurl(&cfg, url, &o) == 0);
    CF_CHECK(o.kind == CF_UNFURL_NONE);
    cf_unfurl_out_dispose(&o);
    srv_stop(&s);
}

CF_TEST_MAIN()
