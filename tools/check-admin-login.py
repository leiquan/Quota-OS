from pathlib import Path
import subprocess,os,re,sys,secrets,tempfile,select,json,time
ed='Web-UI'
base=Path(__file__).resolve().parents[1]
img=base/'build'/('quotaos-lvgl.bin' if ed=='UI' else 'quotaos-node-web.bin')
root=Path(tempfile.mkdtemp(prefix='quota-auth-'+ed+'-',dir='/tmp'));print(root,flush=True)
disk=root/'user.raw'
subprocess.run(['python3',str(base/'tools/init-admin-disk.py'),str(disk)],check=True)
pw=secrets.token_hex(5)
for boot in (1,2):
 log=root/f'boot-{boot}.log'
 cmd=['qemu-system-aarch64','-machine','virt,gic-version=2','-cpu','cortex-a35','-m','64M' if ed=='UI' else '1G','-display','none','-nic','none','-global','virtio-mmio.force-legacy=false','-device','virtio-rng-device','-device','ramfb','-device','virtio-tablet-device','-device','virtio-keyboard-device','-drive',f'if=none,id=notes,format=raw,file={disk}','-device','virtio-blk-device,drive=notes,serial=QUOTA-NOTES','-icount','shift=4,sleep=off','-kernel',str(img),'-append','apps-video','-serial','file:'+str(log),'-qmp','stdio']
 vm=subprocess.Popen(cmd,stdin=-1,stdout=-1);vm.stdout.readline()
 def call(name,args={}):
    vm.stdin.write((json.dumps({'execute':name,'arguments':args,'id':1})+'\n').encode());vm.stdin.flush()
    while line:=vm.stdout.readline():
        reply=json.loads(line)
        if reply.get('id')==1:assert 'error' not in reply,reply;return
    raise RuntimeError('QMP closed')
 def wait(marker,offset=0):
    end=time.monotonic()+65
    while time.monotonic()<end:
        if marker in log.read_text(errors='replace')[offset:]:return
        time.sleep(.08)
    raise RuntimeError(log.read_text(errors='replace')[-1800:])
 def enter(value):
    for c in [*value,'ret']:
        call('send-key',{'keys':[{'type':'qcode','data':c}],'hold-time':45});time.sleep(.11)
 def click(x,y):
    call('input-send-event',{'events':[{'type':'abs','data':{'axis':a,'value':round(v*32767/n)}} for a,v,n in [('x',x,1279),('y',y,799)]]});time.sleep(.2)
    for down in (True,False):
        call('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':down}}]});time.sleep(.22)
 try:
    call('qmp_capabilities')
    if boot==1:
        wait('[session] state=1');enter(pw);wait('[session] state=2');enter(pw)
    wait('[session] state=4');enter('wrong');time.sleep(.5);assert '[session] state=5' not in log.read_text()
    enter(pw);wait('[session] state=5')
    call('screendump',{'filename':str(root/f'desktop-{boot}.ppm')})
    n=log.stat().st_size;click(32,20);click(80,139);click(80,139);wait('[session] state=4',n);enter(pw);wait('[session] state=5',log.read_text().rfind('[session] state=4'))
 finally:
    call('quit');vm.wait()
 assert pw.encode() not in log.read_bytes() and pw.encode() not in disk.read_bytes()
 rows=re.findall(r'state=\d+ t1_misses=(\d+) t2_short=(\d+) frames=(\d+),(\d+) long=(\d+),(\d+)',log.read_text())
 print(rows,flush=True)
 assert int(rows[-1][2])>int(rows[0][2]) and int(rows[-1][3])>int(rows[0][3])
 assert all(r[0:2]==('0','0') and r[4:]==rows[0][4:] for r in rows)
