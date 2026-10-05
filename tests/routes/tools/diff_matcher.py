#!/usr/bin/env python3
"""Differential check: the H03 matcher vs the reference regex semantics of
controllers.rs::compile/recognize.

Enumerates the pinned recognition vectors, every artifact pattern filled with
parameter variants, their near-miss variants, and a seeded random corpus;
computes the expected first match (Python re, same translation as the Rust
compile()) and compares the C matcher's row id and decoded capture set.

    tests/routes/tools/diff_matcher.py [--harness build/h03/match_harness]

Exit 0 when every case agrees; nonzero on the first mismatch summary.
"""
import argparse, json, os, re, subprocess, sys, urllib.parse, random

REPO=os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
rows=json.load(open(REPO+'/docs/devel/implementation/contracts/routes.json'))['routes']
vecs=json.load(open(REPO+'/docs/devel/implementation/contracts/route-recognition.json'))

def compile_pattern(pattern):
    rx='^'; names=[]
    i=0
    while i < len(pattern):
        c=pattern[i]
        if c=='(':
            rx+='(?:'; i+=1
        elif c==')':
            rx+=')?'; i+=1
        elif c in ':*':
            j=i+1
            while j<len(pattern) and (pattern[j].isalnum() or pattern[j]=='_'): j+=1
            name=pattern[i+1:j]
            rx += '([^/.?]+)' if c==':' else '(.+?)'
            names.append(name); i=j
        else:
            rx+=re.escape(c); i+=1
    rx+=r'\Z'
    return re.compile(rx), names

COMPILED=[(r['id'], r['method'], r['pattern'], r['defaults'])+compile_pattern(r['pattern']) for r in rows]

def normalize(path):
    out='/' + path
    res=''
    for c in out:
        if c=='/' and res.endswith('/'): continue
        res+=c
    while len(res)>1 and res.endswith('/'): res=res[:-1]
    return re.sub(r'%[a-fA-F0-9]{2}', lambda m: m.group(0).upper(), res)

def pct_decode(s):
    b=urllib.parse.unquote_to_bytes(s)
    try:
        return b.decode('utf-8'), None
    except UnicodeDecodeError:
        return None, 'invalid'

def expected(verb, path):
    p=normalize(path)
    v='GET' if verb=='HEAD' else verb
    for rid, method, pattern, defaults, rx, names in COMPILED:
        if method != v: continue
        m=rx.match(p)
        if not m: continue
        params={}
        for k,val in defaults.items(): params[k]=val
        for idx,name in enumerate(names):
            g=m.group(idx+1)
            if g is None: continue
            dec,err=pct_decode(g)
            if err: return 'err='+str(-3)  # CF_INVALID
            params[name]=dec
        params['controller']=None
        return 'id=%d'%rid + ''.join(
            '|%s='%k + (''.join('%02x'%b for b in params[k].encode('utf-8')) if params[k] is not None else '')
            for k in params if k not in ('controller','action'))
    return 'none'

def filled_in(pattern):
    out=[]
    for fmt in ['', '.json']:
        pat=pattern.replace('(.:format)', fmt)
        for (param, glob) in [("1","photo"),("opens","dir/photo.tar.gz"),("caf%C3%A9","a%2Fb/c%20d"),("café","\U0001F600/dir/é.tar")]:
            p=pat
            # replace captures left to right
            res=''; i=0
            while i < len(p):
                c=p[i]
                if c in ':*':
                    j=i+1
                    while j<len(p) and (p[j].isalnum() or p[j]=='_'): j+=1
                    name=p[i+1:j]
                    res += 'json' if (c==':' and name=='format') else (param if c==':' else glob)
                    i=j
                else:
                    res+=c; i+=1
            out.append(res)
    return out

def variants(path):
    vs=[path, path.upper(), path.replace('/', '//', 2), path.lstrip('/')]
    for suffix in ['.json','.turbo_stream','.1.2','.','/','//','x','/x','/new','/edit','?q=1','%FF','%2F','é','\U0001F600.json']:
        vs.append(path+suffix)
    if path:
        vs.append(path[:-1])
    if '/' in path:
        parent=path.rsplit('/',1)[0]
        vs.extend([parent, parent+'/%E9', parent+'/@42', parent+'/opens'])
    return vs

parser=argparse.ArgumentParser()
parser.add_argument('--harness', default='build/h03/match_harness')
args=parser.parse_args()

corpus=set()
for v in vecs:
    corpus.add(v['path'])
    for p in variants(v['path']): corpus.add(p)
for r in rows:
    for p in filled_in(r['pattern']):
        corpus.add(p)
        for q in variants(p): corpus.add(q)
random.seed(7)
alphabet=['a','b','1','/','.','%2F','%C3%A9','-','_','@','*','~','x','.json','/photo','%FF','%4','.tar.gz',' ']
for _ in range(3000):
    corpus.add(''.join(random.choice(alphabet) for _ in range(random.randint(1,12))))
corpus=sorted(corpus)
print('corpus paths:', len(corpus), file=sys.stderr)

verbs=['GET','HEAD','POST','PATCH','PUT','DELETE','OPTIONS']
lines=[]
exp=[]
for verb in verbs:
    # OPTIONS: Rust's Method::from_bytes accepts it, no routes -> none
    for path in corpus:
        lines.append('%s %s'%(verb,path))
        exp.append(expected(verb,path))
# remove paths containing newlines (impossible in a request line) to keep the harness line-based
keep=[i for i,l in enumerate(lines) if '\n' not in l]
lines=[lines[i] for i in keep]; exp=[exp[i] for i in keep]
inp='\n'.join(lines)+'\n'
proc=subprocess.run([args.harness],input=inp.encode(),capture_output=True)
if proc.returncode!=0:
    print('harness failed', proc.returncode, proc.stderr.decode()[:500]); sys.exit(1)
got=proc.stdout.decode().splitlines()
if len(got)!=len(lines):
    print('line count mismatch', len(got), len(lines)); sys.exit(1)
bad=0
for i,(l,e,g) in enumerate(zip(lines,exp,got)):
    if e.startswith('err') and g.startswith('err'): continue
    if e!=g:
        bad+=1
        if bad<=20: print('MISMATCH %r expected %s got %s'%(l,e,g))
print('checked', len(lines), 'cases; mismatches', bad)
sys.exit(1 if bad else 0)
