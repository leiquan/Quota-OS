from pathlib import Path
import json,sys,re
log=Path(sys.argv[1]).read_text()
def rows(tag):
    return [dict((k,int(v)) for k,v in re.findall(r'(\w+)=(\d+)',v))
            for v in re.findall(r'^\['+tag+r'\] (.*)$',log,re.M)]
def single(tag):
    events=rows(tag)
    assert len(events)==1,(tag,len(events))
    return events[0]
clock=single('frame-trace')
bounds=single('frame-window')
frames=rows('frame')
periods=rows('load-period')
health=single('load-result')
assert clock['hz']==62500000 and clock['dropped']==0 and len(frames)==clock['count']
assert len(periods)>=1000 and single('load-end')==dict(count=len(periods),dropped=0)
assert all(health[k]==0 for k in ('t1_misses','t2_short','t2_over','t3_over'))
for n,row in enumerate(periods):
    assert row['runnable'] and not row['skipped'] and row['min']<=row['used']<=row['grant'] and row['t3']<=row['cap']
    if n:
        assert row['start']-periods[n-1]['start']==625000 and row['seq']==periods[n-1]['seq']+1
streams={}
for sid,name,limit in ((0,'speed',625000),(1,'camera',2500000)):
    prev=bounds['prev_'+name]
    release=None
    gaps=[]
    for f in (f for f in frames if f['stream']==sid):
        assert release is None or f['release']-release==limit,f
        assert f['release']<=f['start']<=f['copy']<=f['commit'] and f['commit']-f['release']<=limit,f
        assert bounds['begin']<=f['commit']<=bounds['end']
        assert 0<f['commit']-prev<=limit,f
        gaps.append(f['commit']-prev)
        prev,release=f['commit'],f['release']
    assert len(gaps)==bounds[name]
    streams[name]=dict(frames=len(gaps),max_gap_ticks=max(gaps))
print(json.dumps(dict(hz=clock['hz'],periods=len(periods),streams=streams,health=health,
                      pressure=single('combined-pressure')),indent=2))
