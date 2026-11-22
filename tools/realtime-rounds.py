#!/usr/bin/env python3
"""Run identical GUI+T1/T2/T3 and EL0 tests with honest timing assertions."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import time

class QMP:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        end = time.monotonic() + 5
        while True:
            try:
                self.s.connect(path)
                break
            except OSError:
                if time.monotonic() >= end:
                    raise
                time.sleep(.02)
        self.s.settimeout(5)
        self.f = self.s.makefile('rb')
        self.f.readline()
        self.seq = 0
        self.call('qmp_capabilities')
    def call(self, name, args=None):
        self.seq += 1
        msg = {'execute': name, 'id': self.seq}
        if args is not None:
            msg['arguments'] = args
        self.s.sendall((json.dumps(msg)+'\n').encode())
        while True:
            r = json.loads(self.f.readline())
            if r.get('id') == self.seq:
                if 'error' in r:
                    raise RuntimeError(r['error'])
                return r['return']
    def close(self):
        self.f.close()
        self.s.close()

def contents(path):
    return path.read_text(errors='replace') if path.exists() else ''

def gui(kernel, out, mode, *, pointer_traffic=True, pause_ms=0):
    out.mkdir(parents=True, exist_ok=True)
    sock = '/tmp/quota-rt-%d.sock' % os.getpid()
    Path(sock).unlink(missing_ok=True)
    log = out/'serial.log'
    cmd = ['qemu-system-aarch64', '-machine', os.environ.get('QUOTA_QEMU_MACHINE','virt,gic-version=2'), '-cpu', os.environ.get('QUOTA_QEMU_CPU','cortex-a72'),
           '-m', '128M', '-display', 'none', '-nic', 'none', '-device', 'ramfb',
           '-device', 'virtio-keyboard-device', '-device', 'virtio-tablet-device',
           '-kernel', str(kernel), '-serial', 'file:'+str(log),
           '-qmp', 'unix:'+sock+',server=on,wait=off']
    if os.environ.get('QUOTA_QEMU_ACCEL'):
        cmd += ['-accel',os.environ['QUOTA_QEMU_ACCEL']]
    if mode == 'icount':
        cmd += ['-icount', 'shift=4,sleep=off']
    (out/'command.json').write_text(json.dumps(cmd, indent=2))
    q = None
    with (out/'stderr.log').open('wb') as err:
        p = subprocess.Popen(cmd, stdout=err, stderr=err)
        try:
            q = QMP(sock)
            end = time.monotonic()+60
            while '[lvgl] ready' not in contents(log):
                if time.monotonic() > end or p.poll() is not None:
                    raise TimeoutError('LVGL boot')
                time.sleep(.01)
            text = contents(log)
            geometry = re.search(r'ramfb published (\d+)x(\d+)', text)
            if not geometry: raise AssertionError('framebuffer geometry missing')
            sw, sh = map(int, geometry.groups())
            m = re.search(r'\[lvgl\] load9 box (\d+),(\d+) (\d+)x(\d+)', text)
            if not m:
                raise AssertionError('load9 button missing')
            x,y,w,h = map(int,m.groups())
            def move(x,y):
                q.call('input-send-event', {'events':[
                    {'type':'abs','data':{'axis':'x','value':x*32767//(sw-1)}},
                    {'type':'abs','data':{'axis':'y','value':y*32767//(sh-1)}}]})
            move(x+w//2,y+h//2)
            time.sleep(.12)
            q.call('human-monitor-command',{'command-line':'mouse_button 1'})
            time.sleep(.12)
            q.call('human-monitor-command',{'command-line':'mouse_button 0'})
            end = time.monotonic()+90
            events = 0
            pause = None
            while '[menu] done 9' not in contents(log):
                if time.monotonic()>end or p.poll() is not None:
                    raise TimeoutError('GUI stress completion')
                # Continual real virtio pointer traffic redraws the software
                # cursor while Tier2 minima and 32 Tier3 hogs compete.
                if pause_ms and pause is None and '[menu] running 9' in contents(log):
                    q.call('stop')
                    stopped = time.monotonic_ns()
                    try:
                        time.sleep(pause_ms / 1000)
                    finally:
                        q.call('cont')
                    pause = (time.monotonic_ns() - stopped) / 1_000_000
                if pointer_traffic:
                    move(280+(events*29)%340,45+(events*13)%390)
                    events += 1
                time.sleep(.025)
            # Include the final statistics panel, not just its serial marker.
            time.sleep(.12)
            q.call('screendump', {'filename':str(out/'result.ppm')})
            longevity_ok = None
            hold = int(os.environ.get('QUOTA_GUI_LONGEVITY_SECONDS','0'))
            if hold:
                end = time.monotonic()+hold
                while time.monotonic()<end:
                    move(280+(events*29)%340,45+(events*13)%390)
                    events += 1
                    time.sleep(.1)
                def click(px,py):
                    move(px,py)
                    time.sleep(.1)
                    q.call('human-monitor-command',{'command-line':'mouse_button 1'})
                    time.sleep(.1)
                    q.call('human-monitor-command',{'command-line':'mouse_button 0'})
                    time.sleep(.2)
                t = contents(log)
                m = re.search(r'\[lvgl\] input box (\d+),(\d+) (\d+)x(\d+)',t)
                x,y,w,h = map(int,m.groups())
                click(x+w//2,y+h//2)
                m = re.search(r'\[lvgl\] plus box (\d+),(\d+) (\d+)x(\d+)',t)
                x,y,w,h = map(int,m.groups())
                click(x+w//2,y+h//2)
                for key in ['a','b']:
                    q.call('human-monitor-command',{'command-line':'sendkey '+key})
                    time.sleep(.2)
                t = contents(log)
                longevity_ok = ('[lvgl] click count 1' in t and
                                '[lvgl] key code 48 value 1 char 98' in t and
                                '[lvgl] demo returned' not in t)
                q.call('screendump',{'filename':str(out/'after-50s.ppm')})
            t = contents(log)
            a = re.search(r'\[menu\] T1 miss (\d+) class a (\d+) class b (\d+) skipped (\d+)',t)
            b = re.search(r'Tier 2 低于最低值 (\d+) 个周期，audio-mix 错过 (\d+) 次',t)
            if not a or not b:
                raise AssertionError('missing T1/T2 results')
            vals = list(map(int,a.groups()+b.groups()))
            return dict(zip(['t1_miss','class_a','class_b','skipped','t2_short','t2_job_miss'],vals),
                        input_moves=events, requested_pause_ms=pause_ms, host_stop_to_cont_ms=pause, completed=True, longevity_seconds=hold, longevity_pass=longevity_ok, passed=not any(vals) and (events > 0 or not pointer_traffic) and 'PANIC' not in t and longevity_ok is not False,
                        panic='PANIC' in t)
        finally:
            p.kill()
            p.wait(timeout=5)
            if q:
                q.close()
            Path(sock).unlink(missing_ok=True)

def isolate(kernel,out,mode):
    out.mkdir(parents=True,exist_ok=True)
    cmd=['python3','tools/run-isolate.py',str(kernel),str(out)]
    if mode=='icount':
        cmd.append('icount')
    with (out/'runner.log').open('wb') as f:
        p=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,timeout=120)
    t=contents(out/'serial.log')
    result={'exit_code':p.returncode,'done':'[isolate] DONE' in t,
            'restart16':'[exec-re] bad=0 of 16' in t,
            'classes_ok':len(re.findall(r'class [\w-]+ match_ec=1 match_far=1 match_name=1 ledger=1',t))==5,
            'empty':'final procs=0 pages_free=16 pages_used=0 queue=0 holds=0 mem_kb=0 running=0 refusals=0' in t,
            'irq_contract':'[irq-contract] nested_mask=1 restored=1' in t}
    for kind in ['kill','fault']:
        m=re.search(rf'\[isolate\] {kind}_rounds (\d+) ok (\d+) late (\d+) stale_reject (\d+) us_min (\d+) us_max (\d+)',t)
        if not m:
            raise AssertionError('missing '+kind+' summary')
        v=list(map(int,m.groups()))
        result[kind]=dict(zip(['rounds','ok','late','stale_reject','us_min','us_max'],v),passed=v[:4]==[100,100,0,100] and v[5]<=20000)
    m=re.search(r'deadline_us=(\d+) misses (\d+) -> (\d+) irq_off_max_us=(\d+)',t)
    if not m:
        raise AssertionError('missing display timing')
    v=list(map(int,m.groups()))
    result['timing']=dict(zip(['deadline_us','before','after','irq_off_max_us'],v),passed=v[1]==v[2])
    result['functional_pass']=all(result[k] for k in ['done','restart16','classes_ok','empty','irq_contract']) and all(result[k]['passed'] for k in ['kill','fault']) and p.returncode==0
    result['passed']=result['functional_pass'] and result['timing']['passed']
    return result

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('kernel',type=Path)
    ap.add_argument('out',type=Path)
    ap.add_argument('--rounds',type=int,default=10)
    ap.add_argument('--longevity-seconds',type=int,default=0)
    ap.add_argument('--modes',nargs='+',choices=['host','icount'],default=['host','icount'])
    ap.add_argument('--accel',choices=['tcg','hvf'],default='tcg')
    ap.add_argument('--cpu',default='cortex-a72')
    ap.add_argument('--machine',default='virt,gic-version=2')
    args=ap.parse_args()
    if args.accel=='hvf' and 'icount' in args.modes:
        ap.error('HVF does not use instruction-count time')
    os.environ['QUOTA_QEMU_ACCEL']=args.accel
    os.environ['QUOTA_QEMU_CPU']=args.cpu
    os.environ['QUOTA_QEMU_MACHINE']=args.machine
    root=args.out.resolve();root.mkdir(parents=True,exist_ok=True)
    kernel=args.kernel.resolve()
    summary={'kernel':str(kernel),'sha256':hashlib.sha256(kernel.read_bytes()).hexdigest(),'config':{'accel':args.accel,'cpu':args.cpu,'machine':args.machine,'modes':args.modes},'rounds':[]}
    (root/'versions.txt').write_text(subprocess.check_output(['qemu-system-aarch64','--version'],text=True))
    for r in range(1,args.rounds+1):
        row={'round':r}
        for mode in args.modes:
            for test,fn in [('gui',gui),('isolate',isolate)]:
                key=test+'-'+mode
                os.environ['QUOTA_GUI_LONGEVITY_SECONDS']=str(args.longevity_seconds if r==args.rounds and mode=='host' else 0)
                try:
                    row[key]=fn(kernel,root/('round-%02d'%r)/key,mode)
                except Exception as e:
                    row[key]={'passed':False,'error':repr(e)}
                print(json.dumps({'round':r,'test':key,**row[key]},ensure_ascii=False),flush=True)
                (root/'summary.json').write_text(json.dumps(summary|{'rounds':summary['rounds']+[row]},ensure_ascii=False,indent=2))
        summary['rounds'].append(row)
    (root/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2))
    return 0 if all(row[key].get('passed') for row in summary['rounds'] for key in [t+'-'+m for m in args.modes for t in ['gui','isolate']]) else 1

if __name__=='__main__':
    raise SystemExit(main())
