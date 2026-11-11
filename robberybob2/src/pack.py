# Run from the port/ directory: python robberybob2/src/pack.py  (writes robberybob2.zip)
import zipfile,os
files=['Robbery Bob 2.sh']
for root,dirs,fs in os.walk('robberybob2'):
    dirs[:]=[d for d in dirs if d not in('src','savedata','screenshots')]
    for f in fs:
        if f=='log.txt' or f.endswith('.apk'):continue
        files.append(os.path.join(root,f).replace(os.sep,'/'))
with zipfile.ZipFile('robberybob2.zip','w',zipfile.ZIP_DEFLATED) as z:
    for f in sorted(files):
        data=open(f,'rb').read()
        if f.endswith(('.sh','.conf','.md','.txt','.xml','.json')):
            data=data.replace(b'\r\n',b'\n')
        zi=zipfile.ZipInfo(f,date_time=(2026,11,11,12,0,0))
        zi.compress_type=zipfile.ZIP_DEFLATED
        zi.external_attr=(0o100755 if f.endswith(('.sh','rb2loader')) else 0o100644)<<16
        z.writestr(zi,data)
        print(f,len(data))
