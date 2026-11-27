from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import struct
import subprocess
import sys
import time

repo = Path.cwd()
base = Path(__file__).resolve().parent
mode = sys.argv[1]
directory = base / sys.argv[2]
directory.mkdir()
disk = directory / "notes.raw"
root = directory / "root.ext4"
kernel = repo / "build/quotaos-node-web-volume.bin"
runtime = base / "runtime.raw"
(directory / "inputs.json").write_text(json.dumps({
    name: hashlib.file_digest(path.open('rb'), 'sha256').hexdigest()
    for name, path in (('kernel',kernel), ('runtime',runtime))
}, indent=2))
subprocess.run(["python3", "tools/init-admin-disk.py", str(disk)], check=True)
password = secrets.token_hex(5)
marker = "qa" + secrets.token_hex(3)
subprocess.run(["mke2fs", "-q", "-F", "-t", "ext4", "-b", "4096", "-I", "256",
                "-O", "none,filetype,extent,has_journal", "-J", "size=4",
                "-E", "lazy_itable_init=0,lazy_journal_init=0", str(root), "4096"], check=True)

def fnv(body, generation):
    value = ((2166136261 ^ generation) * 16777619) & 0xffffffff
    for byte in body:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value

def current_record():
    raw = disk.read_bytes()
    found = []
    for offset in (3072,3584):
        magic, generation, length, check = struct.unpack_from("<4I", raw, offset)
        body = raw[offset+16:offset+16+length]
        if magic == 0x514e4431 and length <= 496 and check == fnv(body,generation):
            if body:
                found.append((generation,json.loads(body)))
    return max(found, default=(0,[]), key=lambda rec:rec[0])

if mode == "pressure":
    body = json.dumps([[f"task{n:02}",False] for n in range(24)],separators=(",",":")).encode()
    assert len(body) <= 496
    with disk.open("r+b") as stream:
        stream.seek(3072)
        stream.write(struct.pack("<4I",0x514e4431,1,len(body),fnv(body,1))+body)

def until(action, label, timeout=90, interval=.2):
    cutoff = time.monotonic() + timeout
    while time.monotonic() < cutoff:
        result = action()
        if result:
            return result
        time.sleep(interval)
    raise TimeoutError(label)

class Guest:
    def __init__(self, process, serial):
        self.proc, self.serial = process, serial
        json.loads(self.proc.stdout.readline())
        self.call("qmp_capabilities")

    def call(self, command, **args):
        self.proc.stdin.write(json.dumps(dict(execute=command,arguments=args,id=1))+"\n")
        self.proc.stdin.flush()
        for line in self.proc.stdout:
            msg=json.loads(line)
            if msg.get("id") == 1:
                if "error" in msg:
                    raise RuntimeError(msg)
                return msg["return"]
        raise RuntimeError("QMP closed")

    def log(self):
        return self.serial.read_text(errors="replace")

    def expect(self, marker, timeout=90):
        return until(lambda: marker in self.log(),marker,timeout)

    def move(self,x,y):
        self.call("input-send-event",events=[
            dict(type="abs",data=dict(axis="x",value=round(x*32767/899))),
            dict(type="abs",data=dict(axis="y",value=round(y*32767/1599))),
        ])

    def button(self,down):
        self.call("input-send-event",events=[dict(type="btn",data=dict(button="left",down=down))])

    def tap(self,x,y):
        self.move(x,y)
        time.sleep(.27)
        self.button(True)
        time.sleep(.032)
        self.button(False)
        time.sleep(.27)

    def type(self,value,interval=.27):
        for key in value:
            self.call("send-key",keys=[dict(type="qcode",data=key)],**{"hold-time":55})
            time.sleep(interval)

    def photo(self, file):
        self.call("screendump", filename=str(file))
        header = b"P6\n900 1600\n255\n"
        data = file.read_bytes()
        assert data.startswith(header)
        return data[len(header):]

    def login(self, number):
        if number == 1:
            for state in (1,2):
                self.expect(f"[session] state={state} ")
                self.type([*password,"ret"])
        self.expect("[session] state=4 ")
        self.type([*password,"ret"])
        self.expect("[session] state=5 ")

    def clocks(self,label):
        wanted = {"display_max_gap":625000,"video_max_gap":2500000,
                  "speed_late_gaps":0,"video_late_gaps":0,"video_missed":0}
        symbols = subprocess.check_output(["/opt/homebrew/opt/llvm/bin/llvm-nm","-S",str(kernel.with_suffix(".elf"))], text=True)
        result={}
        for row in symbols.splitlines():
            parts=row.split()
            if len(parts)==4 and parts[-1] in wanted:
                addr,size,_,name=parts
                target=directory/f"{label}-{name}.bin"
                self.call("pmemsave",val=int(addr,16),size=int(size,16),filename=str(target))
                result[name]=int.from_bytes(target.read_bytes(),"little")
        (directory/f"{label}-clocks.json").write_text(json.dumps(result,indent=2))
        assert result.keys()==wanted.keys() and all((0 < result[name] <= limit) if limit else result[name]==0 for name,limit in wanted.items()),result

    def open_todo(self):
        if "[files] verified slot=6" not in self.log():
            raw = disk.read_bytes()
            if not any(struct.unpack_from("<I4xI",raw,offset) == (0x51415031,3853)
                       for offset in (9216,13312)):
                self.expect("[files] verified slot=6")
        time.sleep(.6)
        for _ in range(10):
            self.tap(47,94)
            if "[wap] launch entries" in self.log():
                break
        self.expect("[wap] launch entries")
        self.expect("[wap] page frame complete",1800)

@contextmanager
def boot(number):
    log=directory/f"boot-{number}.log"
    cmd=["/bin/bash","tools/run-web-node-volume.sh","-display","none","-qmp","stdio"]
    if mode=="pressure":
        cmd += ["-append","apps-touch-ext4","-set","drive.root.throttling.iops-total=25"]
    if os.environ.get("QA_ICOUNT")=="4":
        cmd += ["-icount","shift=4,sleep=off"]
    env={**os.environ,"QUOTA_RUNTIME_VOLUME":str(runtime),"QUOTA_KERNEL":str(kernel),
         "QUOTA_NOTES_DISK":str(disk),"QUOTA_ROOT_DISK":str(root),"QUOTA_SERIAL":f"file:{log}",
         "QUOTA_TOUCH_CPUS":"2","QUOTA_QEMU_RAM":"512M","QUOTA_TOUCH_HVF":"0"}
    (directory/f"command-{number}.json").write_text(json.dumps(cmd,indent=2))
    with (directory/f"qemu-{number}.log").open('w') as err:
        proc=subprocess.Popen(cmd,env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err,text=True)
        guest=None
        try:
            guest=Guest(proc,log)
            machines=[guest.call("query-cpus-fast"),guest.call("query-memory-size-summary")]
            assert len(machines[0])==2 and machines[1]["base-memory"]==512*1024**2
            (directory/f"machine-{number}.json").write_text(json.dumps(machines,indent=2))
            yield guest
        finally:
            try:
                if guest:
                    guest.call("quit")
                else:
                    proc.terminate()
            except RuntimeError:
                proc.terminate()
            proc.wait(timeout=15)
    assert password not in log.read_text(errors="replace")

def add_position(frame):
    for y in range(110,300):
        row=frame[y*2700:(y+1)*2700]
        for color in ("177768","146558"):
            offset=row.find(bytes.fromhex(color)*14)
            if offset>=0 and offset%3==0:
                return offset//3+12, y+5

def crop(frame):
    return [frame[(y*900+175)*3:(y*900+638)*3] for y in range(128,335)]

def scroll_displacement(a,b):
    first,second=crop(a),crop(b)
    positions={row:i for i,row in enumerate(first) if (
        row.count(bytes.fromhex("20364a"))>=3 and first.count(row)==1)}
    shifts={}
    for y,row in enumerate(second):
        offset=positions.get(row,y)-y
        if abs(offset)>15:
            shifts[offset]=shifts.get(offset,0)+1
    return next(((k,v) for k,v in sorted(shifts.items(),key=lambda p:-p[1]) if v>=4),None)

def ready(g, number):
    g.login(number)
    g.open_todo()
    g.move(850,500)
    f=directory/f"todo-{number}.ppm"
    return until(lambda:add_position(g.photo(f)),"enabled Todo Add",100,2)

def check_counters(log):
    samples = re.findall(r"t1_misses=(\d+).*?t2_short=(\d+)", log)
    assert samples and set(samples)=={("0","0")}, set(samples)

if mode == "cold":
    with boot(1) as g:
        x,y=ready(g,1)
        g.tap(x-140,y)
        g.type(marker,1.3)
        g.tap(x,y)
        write=until(lambda: (record if (record:=current_record())[1]==[[marker,False]] else None),"exact Todo save",120)
        g.expect(f"backend disk flush generation={write[0]}")
        g.photo(directory/"saved.ppm")
        g.clocks("saved")
        check_counters(g.log())
    with boot(2) as g:
        x,y=ready(g,2)
        time.sleep(4)
        g.photo(directory/"restored-before-edit.ppm")
        for xx, yy in ((176,231),(174,232),(180,231)):
            g.tap(xx,yy)
            time.sleep(4)
            if current_record()[1]==[[marker,True]]:
                break
        cold=until(lambda: (record if (record:=current_record())[1]==[[marker,True]] else None),"cold Todo readback and checkbox",100)
        assert cold[0] > write[0]
        check_counters(g.log())
        g.photo(directory/"cold-readback.ppm")
        g.clocks("cold")
    (directory/"cold.json").write_text(json.dumps(dict(marker=marker,saved=write,readback=cold),indent=2))

if mode=="pressure":
    with boot(1) as g:
        ready(g,1)
        old=g.photo(directory/"before-scroll.ppm")
        g.move(325,325)
        time.sleep(.25)
        g.button(True)
        for y in range(317,243,-8):
            g.move(325,y)
            time.sleep(.08)
        g.button(False)
        g.expect("pressure_worker=")
        active=directory/"scroll-active.ppm"
        visual=None
        end=time.monotonic()+180
        while "io-end ticks=" not in g.log() and time.monotonic()<end:
            displacement=scroll_displacement(old,g.photo(active))
            if displacement and "io-end ticks=" not in g.log():
                visual=displacement
                break
            time.sleep(.12)
        g.expect("io-end ticks=",300)
        until(lambda: re.search(r"scroll-trace count=(\d+) dropped=(\d+)", g.log()),"scroll trace",180)
        time.sleep(2)
        g.photo(directory/"after-scroll.ppm")
        check_counters(g.log())
        (directory/"visual.json").write_text(json.dumps(dict(in_window=visual,
             final=scroll_displacement(old,g.photo(directory/"after-scroll.ppm")))))
    subprocess.run(["e2fsck","-fn",str(root)],check=True,
                   stdout=(directory/"e2fsck.log").open("w"),stderr=subprocess.STDOUT)
    body=subprocess.check_output(["debugfs","-R","cat touch-keep.txt",str(root)],stderr=subprocess.DEVNULL)
    assert body==b"Quota OS Touch UI journaled replacement and cold readback",body
print("Completed",directory,flush=True)
