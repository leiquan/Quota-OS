#!/usr/bin/env python3
"""Check one complete Todo pressure window against its raw guest-clock trace."""
import argparse
import json
import re
from pathlib import Path

def verify(text):
    errors = []
    def require(value, message):
        if not value:
            errors.append(message)
    def rows(tag):
        return [dict((k, int(v)) for k, v in re.findall(r'(\w+)=(\d+)', line))
                for line in re.findall(r'\[' + re.escape(tag) + r'\] ([^\r\n]+)', text)]
    def single(tag):
        found = rows(tag)
        if len(found) != 1:
            raise ValueError(f'expected one {tag}, found {len(found)}')
        return found[0]

    trace, window = single('frame-trace'), single('frame-window')
    freq = trace['hz']
    require(freq == 62500000, 'unexpected guest clock')
    frames, periods = rows('frame'), rows('load-period')
    end = single('load-end')
    require(end['count'] == len(periods) >= 999 and end['dropped'] == 0, 'incomplete periods')
    require(trace['count'] == len(frames) and trace['dropped'] == 0, 'incomplete frames')
    require(len(frames) == window['speed'] + window['camera'], 'frame counter mismatch')
    result = single('load-result')
    for key in ('t1_misses', 't2_short', 't2_over', 't3_over'):
        require(result[key] == 0, key)
    require(result['t3_throttled'] > 0 and any(p['throttled'] for p in periods), 'T3 saturation not observed')
    pressure = single('combined-pressure')
    for key in ('cpu_peak_us', 'working_set_kib', 'memory_reserved_kib', 'disk_completed'):
        require(pressure[key] > 0, key)
    require(pressure['memory_available_kib'] == 0 and
            pressure['working_set_kib'] <= pressure['memory_reserved_kib'],
            'missing reserved-memory saturation or inconsistent working set')
    require(0 < pressure['disk_completed'] <= pressure['disk_submitted'],
            'invalid completed disk work')
    require('[quota-test] 5 PASS' in text and 'Strict frame timing: PASS' in text, 'guest trial verdict')
    for i, p in enumerate(periods):
        require(not p['skipped'] and p['runnable'] and 0 < p['min'] <= p['used'] <= p['grant']
                and p['t3'] <= p['cap'], f"budget in period {p['seq']}")
        if i:
            require(p['seq'] == periods[i-1]['seq'] + 1 and
                    p['start'] - periods[i-1]['start'] == freq//100, f"period gap at {p['seq']}")
    gc = rows('v8-major')
    # Only count GC overlap inside periods with a measured exhausted T3 pool.
    # start_hi and end_lo exclude the clock calibration uncertainty.
    saturated = {p['start'] for p in periods if p['throttled'] and p['t3'] <= p['cap']}
    period_ticks = freq // 100
    require(len(saturated) == len(periods), 'T3 throttling not recorded in every complete period')
    first = periods[0]['start'] if periods else 0
    def under_load(release):
        return first + (release-first)//period_ticks*period_ticks in saturated
    streams = {}
    for stream, name in enumerate(('speed', 'camera')):
        limit = freq//100 * (1 if stream == 0 else 4)
        selected = [f for f in frames if f['stream'] == stream]
        require(len(selected) == window[name] and len(selected) > 0, name+' count')
        prev = window['prev_'+name]
        max_gap = max_latency = overlap = 0
        for f in selected:
            release, start, copy, commit = (f[k] for k in ('release', 'start', 'copy', 'commit'))
            gap, latency = commit-prev, commit-release
            require(prev > 0 and 0 < gap <= limit and release <= start <= copy <= commit
                    and latency <= limit and window['begin'] <= commit <= window['end'],
                    f'{name} release={release} gap={gap} latency={latency}')
            max_gap, max_latency = max(max_gap, gap), max(max_latency, latency)
            if under_load(release) and any(g['start_hi_ns']*freq <= release*10**9 <= g['end_lo_ns']*freq for g in gc):
                overlap += 1
            prev = commit
        require(overlap > 0, name+' has no release during confirmed major GC')
        streams[name] = dict(frames=len(selected), max_gap_ticks=max_gap,
                             max_latency_ticks=max_latency, releases_during_gc=overlap)
    return dict(passed=not errors, violations=len(errors), first_errors=errors[:12],
                tick_hz=freq, streams=streams, periods=len(periods),
                pressure=pressure, budget_counters=result,
                throttled_periods=sum(bool(p['throttled']) for p in periods))

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('serial', type=Path)
    args = parser.parse_args()
    try:
        report = verify(args.serial.read_text(errors='replace'))
    except (KeyError, ValueError) as exc:
        report = dict(passed=False, error=str(exc))
    print(json.dumps(report, indent=2))
    raise SystemExit(0 if report['passed'] else 1)
