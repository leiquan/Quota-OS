import collections
import json
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
log=(root/"boot-1.log").read_text()
period_ticks=625000
errors=[]

def need(ok, reason):
    if not ok:
        errors.append(reason)

def rows(pattern):
    return [{k:int(v) for k,v in re.findall(r"(\w+)=(-?\d+)", line)}
            for line in re.findall(pattern,log,re.M)]

def match(pattern):
    found=rows(pattern)
    if len(found)!=1:
        raise ValueError((pattern,len(found)))
    return found[0]

def trace(header, group):
    result=rows(group)
    count=match(header)
    need(count['count']==len(result) and count['dropped']==0, f"missing {header}")
    return result

need(bool(re.search(r'\[touch-ext4\] io-end .* result=PASS cold=0',log)), "ext4 probe")
status=match(r'^\[touch-ext4\] (speed_deadlines=.*)$')
need(all(v==0 for v in status.values()), "probe counter")
health=re.findall(r"t1_misses=(\d+).*?t2_short=(\d+)",log)
need(bool(health) and set(health)=={("0","0")},"lifetime deadline/shortfall counter")
window=match(r'^\[frame-window\] (.*)$')
clock=match(r'^\[frame-trace\] (.*)$')
need(clock['hz']==62_500_000,"guest clock")
frames=trace(r'^\[frame-trace\] (.*)$',r'^\[frame\] (.*)$')
need(len(frames)==window['speed']+window['camera'],"frame counter mismatch")
streams={}
for stream,name,deadline in ((0,"speed",period_ticks),(1,"camera",period_ticks*4)):
    segment=[row for row in frames if row['stream']==stream]
    prev=window['prev_'+name]
    last_release=None
    gaps=[]
    latencies=[]
    for row in segment:
        release,start,copy,commit=(row[key] for key in ('release','start','copy','commit'))
        gaps.append(commit-prev)
        latencies.append(commit-release)
        need(last_release is None or release-last_release==deadline, f"{name} release discontinuity")
        need(release<=start<=copy<=commit and window['begin']<=commit<=window['end'],
             f"{name} timestamp order")
        prev,last_release=commit,release
    need(bool(segment) and len(segment)==window[name] and
         all(0<gap<=deadline for gap in gaps) and all(delay<=deadline for delay in latencies),
         f"{name} exceeds {deadline} ticks")
    streams[name]=dict(frames=len(segment),limit_ticks=deadline,
        max_gap_ticks=max(gaps,default=None),max_latency_ticks=max(latencies,default=None))
periods=trace(r'^\[touch-quota\] (.*)$',r'^\[touch-period\] (.*)$')
need(bool(periods),"no budget periods")
for index,row in enumerate(periods):
    need(not(row['short'] or row['skipped'] or row['remote_incomplete']) and
         row['t2_min']<=row['t2_used']<=row['t2_grant'] and
         row['cpu0']+row['remote']<=row['cap'],f"budget in {row['seq']}")
    if index:
        prev=periods[index-1]
        need(row['seq']-prev['seq']==1 and row['start']-prev['start']==period_ticks,"period gap")
need(any(row['t2_grant']>0 and row['t2_used']>0 for row in periods),"no active Tier 2 grant")
# The quota recorder starts at the next full period. A worker can start
# in the partial first period, which still has its own lease and pool ledger.
waves={row['period']:row for row in rows(r'^\[wave\] (.*)$')}
remote_leases=[]
for worker in rows(r'^\[worker\] (.*)$'):
    wave=waves.get(worker['period'])
    if worker['affinity']!=1 or not(window['begin']<=worker['first']<worker['last']<=window['end']):
        continue
    need(0<worker['ticks']==worker['last']-worker['first']<=worker['grant'] and
         worker['iterations']>0 and wave is not None and wave['finished']==1 and not wave['dropped'] and
         wave['remote']==worker['ticks'] and wave['total']==wave['cpu0_t3']+wave['remote']<=wave['cap'],
         "CPU1 lease accounting")
    remote_leases.append(dict(worker=worker,wave=wave))
need(bool(remote_leases),"no charged CPU1 work")
sweeps=match(r'^\[touch-ext4\] (concurrent_memory_sweeps=.*)$')
need(sweeps['concurrent_memory_sweeps']>0 and sweeps['bytes_per_sweep']>0,"no memory activity")
bus=trace(r'^\[touch-ext4\] bus-trace (.*)$',r'^\[touch-ext4\] (bus-op=.*)$')
inputs=trace(r'^\[touch-ext4\] scroll-trace (.*)$',r'^\[touch-ext4\] scroll-step (.*)$')
outputs=trace(r'^\[touch-ext4\] page-trace (.*)$',r'^\[touch-ext4\] page-frame (.*)$')
stages=trace(r'^\[touch-ext4\] page-stage-trace (.*)$',r'^\[touch-ext4\] (page-stage=.*)$')
need(bool(inputs) and bool(outputs),"missing input or visible page flush")
changes=[]
first=inputs[0]['ticks'] if inputs else 0
for out in outputs:
    end=out['ticks']
    ops=collections.Counter(row['op'] for row in bus if row['start']<end and first<row['end'])
    if {0,1} <= ops.keys():
        changes.append(dict(generation=out['generation'],begin=first,display=end,disk_ops=dict(ops)))
need(bool(changes) and all(end['ticks']<=window['end'] for end in outputs),
     "no viewport presentation spanning actual reads and writes")
visual=json.loads((root/"visual.json").read_text())
need(bool(visual["in_window"]),"no verified cursor-free pixel displacement sampled while active")
result=dict(passed=not errors, errors=errors[:20], violation_count=len(errors), frame_window=window,
            streams=streams,tick_hz=clock['hz'],periods=len(periods),remote_periods=sum(r['remote']>0 for r in periods),
            health=status,remote_leases=remote_leases,memory=sweeps,physical_ops=dict(collections.Counter(r['op'] for r in bus)),
            input_steps=len(inputs),page_frames=len(outputs),stage_counts=dict(collections.Counter(r['stage'] for r in stages)),
            overlapping_scroll=changes,visual=visual)
print(json.dumps(result,indent=2))
raise SystemExit(bool(errors))
