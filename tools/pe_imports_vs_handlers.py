import re,glob,struct,sys
names=set()
for f in glob.glob('SomeBoxOne/src/*.hpp'):
    s=open(f,encoding='utf8',errors='ignore').read()
    names|=set(re.findall(r'\bH\("([^"]+)"',s))
    names|=set(re.findall(r'handlers_\["([^"]+)"\]',s))
d=open(sys.argv[1],'rb').read()
pe=struct.unpack_from('<I',d,0x3c)[0]
nsec=struct.unpack_from('<H',d,pe+6)[0]; osz=struct.unpack_from('<H',d,pe+20)[0]
opt=pe+24
imp_rva,imp_sz=struct.unpack_from('<II',d,opt+0x78)
secs=[];sp=opt+osz
for i in range(nsec):
    n,vs,va,rs,rp=struct.unpack_from('<8sIIII',d,sp+i*40)
    secs.append((va,max(vs,rs),rp))
def off(r):
    for va,sz,rp in secs:
        if va<=r<va+sz: return r-va+rp
def cs(r):
    o=off(r);e=d.index(b'\0',o);return d[o:e].decode('latin1')
p=off(imp_rva);res={}
while True:
    ol,_,_,nm,ft=struct.unpack_from('<IIIII',d,p)
    if not nm:break
    dll=cs(nm).lower();t=off(ol or ft);i=0
    while True:
        v=struct.unpack_from('<Q',d,t+i*8)[0]
        if not v:break
        nme='#%d'%(v&0xffff) if v>>63 else cs((v&0x7fffffff)+2)
        res.setdefault(dll,[]).append(nme);i+=1
    p+=20
for dll,l in res.items():
    miss=[x for x in l if x not in names]
    print(dll,len(l),"missing",len(miss))
    if len(sys.argv)>2 and (dll.startswith(sys.argv[2])): print("   ",miss)
