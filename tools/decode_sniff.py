import sys, re, statistics as st
def load(path):
    traces=[]; cur=None
    for l in open(path, errors='replace'):
        l=re.sub(r'\x1b\[[0-9;]*m','',l)
        m=re.search(r'sniff #(\d+): (\d+) changes, first state SDA=(\d) SCL=(\d)', l)
        if m:
            cur={'n':int(m.group(1)),'first':(int(m.group(3)),int(m.group(4))),'items':[]}; traces.append(cur); continue
        m=re.search(r'thermia:\d+\]:\s+((?:\d+:[01][01]\s*)+)$', l)
        if m and cur is not None:
            for t in m.group(1).split():
                d,lv=t.split(':'); cur['items'].append((int(d),int(lv[0]),int(lv[1])))
    return traces

def decode(tr):
    sda,scl=tr['first']; t=0; ev=[]; bits=[]; out=[]; rises=[]
    started = (sda==0 and scl==1)
    if started: out.append('S')
    cur=[]; last_rise=None
    for d,nsda,nscl in tr['items']:
        t+=d
        if nscl==1 and scl==0:            # SCL rising: sample SDA
            cur.append(nsda)
            if last_rise is not None: rises.append(t-last_rise)
            last_rise=t
        elif nscl==1 and scl==1 and nsda!=sda:   # SDA change while SCL high
            if cur: out.append(('partial',cur)); cur=[]
            out.append('P' if nsda==1 else 'Sr')
            started = nsda==0
        sda,scl=nsda,nscl
        while len(cur)==9:
            b=0
            for x in cur[:8]: b=(b<<1)|x
            out.append('%02X%s'%(b,'a' if cur[8]==0 else 'n')); cur=[]
    if cur: out.append(('partial',cur))
    return out, rises

traces=load(sys.argv[1])
print('traces:',len(traces))
allr=[]
for tr in traces:
    o,r=decode(tr); allr+=r
    print('#%d'%tr['n'], ' '.join(x if isinstance(x,str) else 'partial%s'%''.join(map(str,x[1])) for x in o))
if allr:
    print('SCL rise-to-rise: median %d us  min %d  max %d'%(st.median(allr),min(allr),max(allr)))
