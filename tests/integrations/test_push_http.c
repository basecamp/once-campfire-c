#define _POSIX_C_SOURCE 200809L
#include "cf_test.h"
#include "integrations/push_http.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#define TLS_DIR "tests/fixtures/crates/campfire/src/integrations/testdata/tls/"
static const char script[] =
"import ssl,sys\n"
"from http.server import HTTPServer,BaseHTTPRequestHandler\n"
"class H(BaseHTTPRequestHandler):\n"
" def log_message(self,*a): pass\n"
" def do_POST(self):\n"
"  b=self.rfile.read(int(self.headers['Content-Length']))\n"
"  good=b==bytes([0,255,1,2]) and self.headers.get('Authorization')=='vapid t=test,k=key' and self.headers.get('Content-Encoding')=='aes128gcm' and self.path.startswith('/push')\n"
"  code=302 if self.path=='/push/redirect' else (700 if self.path=='/push/status' else (201 if good else 400))\n"
"  self.send_response(code,'Push Accepted' if good else 'Bad Push')\n"
"  if self.path=='/push/redirect': self.send_header('Location','/push')\n"
"  if self.path=='/push/headers':\n"
"   for i in range(100): self.send_header('X-Pad', 'x'*200)\n"
"  self.send_header('Content-Length','65537' if self.path=='/push/body' else '0');self.end_headers()\n"
"  if self.path=='/push/body':\n"
"   try: self.wfile.write(b'x'*65537)\n"
"   except Exception: pass\n"
"class QuietServer(HTTPServer):\n"
" def handle_error(self,*a): pass\n"
"s=QuietServer(('127.0.0.1',0),H)\n"
"c=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);c.load_cert_chain(sys.argv[1],sys.argv[2]);s.socket=c.wrap_socket(s.socket,server_side=True)\n"
"print(s.server_address[1],flush=True);s.serve_forever()\n";
typedef struct {pid_t pid; unsigned port;} server;
static server start(void) {
    int p[2]; CF_REQUIRE(pipe(p)==0);
    pid_t pid=fork(); CF_REQUIRE(pid>=0);
    if (!pid) {close(p[0]); dup2(p[1],STDOUT_FILENO); close(p[1]);
        execlp("python3","python3","-c",script,TLS_DIR "server.pem",TLS_DIR "server.key",(char*)NULL); _exit(127);}
    close(p[1]); FILE *f=fdopen(p[0],"r"); CF_REQUIRE(f); unsigned port=0;
    int got=fscanf(f,"%u",&port); fclose(f);
    if(got!=1){kill(pid,SIGTERM);waitpid(pid,NULL,0);} CF_REQUIRE(got==1);
    return (server){pid,port};
}
static void stop(server s){kill(s.pid,SIGTERM);waitpid(s.pid,NULL,0);}
static cf_err exchange(server s,const char *host,const char *ip,const char *path,
    const char *ca,unsigned *status,char *reason,size_t cap,cf_push_transport_error *err){
    char url[256];snprintf(url,sizeof url,"https://%s:%u%s",host,s.port,path);
    cf_push_header headers[]={{"Authorization","vapid t=test,k=key"},{"Content-Encoding","aes128gcm"},{"Content-Type","application/octet-stream"}};
    unsigned char body[]={0,255,1,2};
    cf_push_request r={.endpoint_url=url,.host=(char*)host,.port=(uint16_t)s.port,.resolved_ip=(char*)ip,.target=(char*)path,.headers={headers,3},.body=body,.body_len=sizeof body};
    cf_push_http_context c={ca};return cf_push_http_exchange(&c,&r,status,reason,cap,err);
}
CF_TEST(push_http_posts_binary_with_pinned_ip_and_verified_tls){
    server s=start();unsigned status=0;char reason[64];cf_push_transport_error err;
    cf_err e=exchange(s,"www.example.com","127.0.0.1","/push",TLS_DIR "ca.pem",&status,reason,sizeof reason,&err);
    stop(s);CF_CHECK(e==CF_OK);CF_CHECK(err==CF_PUSH_TRANSPORT_OK);CF_CHECK(status==201);CF_CHECK(!strcmp(reason,"Push Accepted"));
}
CF_TEST(push_http_rejects_untrusted_ca_and_hostname){
    server s=start();unsigned status;char reason[32];cf_push_transport_error err;
    CF_CHECK(exchange(s,"www.example.com","127.0.0.1","/push",NULL,&status,reason,sizeof reason,&err)==CF_OK);
    CF_CHECK(err==CF_PUSH_TRANSPORT_TLS);CF_CHECK(status==0);
    CF_CHECK(exchange(s,"wrong.example","127.0.0.1","/push",TLS_DIR "ca.pem",&status,reason,sizeof reason,&err)==CF_OK);
    stop(s);CF_CHECK(err==CF_PUSH_TRANSPORT_TLS);CF_CHECK(status==0);
}
CF_TEST(push_http_bounds_headers_and_response_body){
    server s=start();unsigned status;char reason[32];cf_push_transport_error err;
    CF_CHECK(exchange(s,"www.example.com","127.0.0.1","/push/body",TLS_DIR "ca.pem",&status,reason,sizeof reason,&err)==CF_OK);
    CF_CHECK(err==CF_PUSH_TRANSPORT_IO);CF_CHECK(status==0);
    CF_CHECK(exchange(s,"www.example.com","127.0.0.1","/push/headers",TLS_DIR "ca.pem",&status,reason,sizeof reason,&err)==CF_OK);
    stop(s);CF_CHECK(err==CF_PUSH_TRANSPORT_IO);CF_CHECK(status==0);
}
CF_TEST(push_http_rejects_nonliteral_ip_before_network){
    unsigned status;char reason[2];cf_push_transport_error err;
    CF_CHECK(exchange((server){0,443},"www.example.com","evil.example","/push",NULL,&status,reason,sizeof reason,&err)==CF_INVALID);
}
CF_TEST(push_http_does_not_follow_redirects_and_truncates_reason){
    server s=start();unsigned status;char reason[5];cf_push_transport_error err;
    cf_err e=exchange(s,"www.example.com","127.0.0.1","/push/redirect",TLS_DIR "ca.pem",&status,reason,sizeof reason,&err);
    stop(s);CF_CHECK(e==CF_OK);CF_CHECK(err==CF_PUSH_TRANSPORT_OK);CF_CHECK(status==302);CF_CHECK(!strcmp(reason,"Push"));
}
CF_TEST(push_http_ignores_proxy_environment_for_pinned_address){
    server s=start();unsigned status;char reason[32];cf_push_transport_error err;
    const char *previous=getenv("https_proxy");char *saved=previous ? strdup(previous) : NULL;
    CF_REQUIRE(setenv("https_proxy","http://127.0.0.1:1",1)==0);
    cf_err e=exchange(s,"www.example.com","127.0.0.1","/push",TLS_DIR "ca.pem",&status,reason,sizeof reason,&err);
    if(saved){setenv("https_proxy",saved,1);free(saved);}else unsetenv("https_proxy");
    stop(s);CF_CHECK(e==CF_OK);CF_CHECK(err==CF_PUSH_TRANSPORT_OK);CF_CHECK(status==201);
}
CF_TEST(push_http_rejects_out_of_range_http_status){
    server s=start();unsigned status;char reason[32];cf_push_transport_error err;
    cf_err e=exchange(s,"www.example.com","127.0.0.1","/push/status",TLS_DIR "ca.pem",&status,reason,sizeof reason,&err);
    stop(s);CF_CHECK(e==CF_OK);CF_CHECK(err==CF_PUSH_TRANSPORT_IO);CF_CHECK(status==0);
}
CF_TEST(push_http_accepts_maximum_ciphertext_with_record_framing){
    server s=start();char url[128];snprintf(url,sizeof url,"https://www.example.com:%u/push",s.port);
    unsigned char body[CF_PUSH_MAX_RECORD_BYTES+86]={0};
    cf_push_header headers[]={{"Authorization","vapid t=test,k=key"},{"Content-Encoding","aes128gcm"}};
    cf_push_request r={.endpoint_url=url,.host="www.example.com",.port=(uint16_t)s.port,.resolved_ip="127.0.0.1",.target="/push",.headers={headers,2},.body=body,.body_len=sizeof body};
    cf_push_http_context c={TLS_DIR "ca.pem"};unsigned status;char reason[32];cf_push_transport_error err;
    cf_err e=cf_push_http_exchange(&c,&r,&status,reason,sizeof reason,&err);
    stop(s);CF_CHECK(e==CF_OK);CF_CHECK(err==CF_PUSH_TRANSPORT_OK);CF_CHECK(status==400);
}
CF_TEST_MAIN()
