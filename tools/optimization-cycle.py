#!/usr/bin/env python3
"""Run bounded optimization-cycle regressions. Failures stay in summary.json."""
import argparse,hashlib,json,os,re,runpy,select,subprocess,time
from pathlib import Path

def serial_case(kernel,out,boot,mode):
    out.mkdir(parents=True,exist_ok=True)
    cmd=['qemu-system-aarch64','-machine','virt,gic-version='+('3' if mode=='native' else '2'),'-cpu','host' if mode=='native' else 'cortex-a72','-m','128M','-nographic','-nic','none','-kernel',str(kernel),'-append',boot]
    cmd+=['-accel','hvf'] if mode=='native' else ['-accel','tcg','-icount','shift=4,sleep=off']
    (out/'command.json').write_text(json.dumps(cmd,indent=2))
    p=subprocess.Popen(cmd,stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    data=b'';end=time.monotonic()+100
    try:
        with (out/'serial.log').open('wb') as f:
            while time.monotonic()<end and b'QA DONE' not in data:
                ready,_,_=select.select([p.stdout],[],[],.2)
                if ready:
                    chunk=os.read(p.stdout.fileno(),65536)
                    if not chunk:break
                    f.write(chunk);f.flush();data+=chunk
    finally:p.kill();p.wait(timeout=5)
    text=data.decode('utf-8','replace').replace('\r','')
    failed=[l for l in text.splitlines() if re.search(r'\bFAIL\b|PANIC',l)]
    result={'completed':'QA DONE' in text,'failed_lines':failed,'passed':'QA DONE' in text and not failed}
    phases=['[boot] calibration stage;', '[boot] calibration complete;', '[boot] shell task running']
    positions=[text.find(x) for x in phases]
    result['boot_order']=all(x>=0 for x in positions) and positions==sorted(positions)
    result['passed'] &= result['boot_order']
    if boot=='e1':
        result['unverified_rejected']='E1 unverified PASS' in text
        result['passed'] &= result['unverified_rejected']

    if boot=='logtest':
        checks={'partial_once':text.count('[klog-test] partial record')==1,'second_once':text.count('[klog-test] second record')==1,'overflow24':len(re.findall(r'^\[klog-test\] overflow \d+$',text,re.M))==24,'lost3':text.count('[log] 3 more lines not kept')==1,'pass_marker':'[klog-test] PASS' in text}
        result['checks']=checks;result['passed'] &= all(checks.values())
    return result

def main():
    ap=argparse.ArgumentParser();ap.add_argument('kernel',type=Path);ap.add_argument('out',type=Path);ap.add_argument('--rounds',type=int,default=100);a=ap.parse_args()
    if a.rounds < 1:
        ap.error('--rounds must be positive')
    root=a.out.resolve();root.mkdir(parents=True,exist_ok=True);kernel=a.kernel.resolve();rt=runpy.run_path('tools/realtime-rounds.py')
    summary={'sha256':hashlib.sha256(kernel.read_bytes()).hexdigest(),'cases':{},'rounds':[]}
    def save(): (root/'summary.json').write_text(json.dumps(summary,indent=2))
    for mode,boot in [('native','logtest'),('icount','logtest'),('icount','e1'),('icount','e3'),('icount','e2h'),('icount','e4')]:
        name=boot+'-'+mode
        try:r=serial_case(kernel,root/name,boot,mode)
        except Exception as e:r={'passed':False,'error':repr(e)}
        summary['cases'][name]=r;save();print(name,json.dumps(r),flush=True)
    for n in range(1,a.rounds+1):
        row={'round':n}
        for mode,accel,cpu,machine,clock in [('native','hvf','host','virt,gic-version=3','host'),('icount','tcg','cortex-a72','virt,gic-version=2','icount')]:
            if mode == 'native' and n > 10:
                continue
            os.environ.update(QUOTA_QEMU_ACCEL=accel,QUOTA_QEMU_CPU=cpu,QUOTA_QEMU_MACHINE=machine,QUOTA_GUI_LONGEVITY_SECONDS='0')
            try:r=rt['gui'](kernel,root/('round-%02d'%n)/mode,clock, task_manager=(mode == 'icount'))
            except Exception as e:r={'passed':False,'error':repr(e)}
            row[mode]=r;print(n,mode,json.dumps(r),flush=True)
        summary['rounds'].append(row);save()
    for mode,accel,cpu,machine,clock in [('native','hvf','host','virt,gic-version=3','host'),('icount','tcg','cortex-a72','virt,gic-version=2','icount')]:
        os.environ.update(QUOTA_QEMU_ACCEL=accel,QUOTA_QEMU_CPU=cpu,QUOTA_QEMU_MACHINE=machine)
        try:r=rt['isolate'](kernel,root/('isolate-'+mode),clock)
        except Exception as e:r={'passed':False,'error':repr(e)}
        summary['cases']['isolate-'+mode]=r;save();print('isolate',mode,json.dumps(r),flush=True)
    summary['acceptance_passed'] = (
        len(summary['rounds']) == a.rounds
        and all(row['icount']['passed'] for row in summary['rounds'])
        and all(result['passed'] for name, result in summary['cases'].items()
                if name.endswith('-icount'))
    )
    save()
    return 0 if summary['acceptance_passed'] else 1
if __name__=='__main__':raise SystemExit(main())
