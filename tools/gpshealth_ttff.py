#!/usr/bin/env python3
"""Segment gpshealth.log into GPS power cycles and measure time-to-first-fix.

The log APPENDS across reboots and across GPS power cycles, so any rate or
duration taken over the whole file is meaningless. `on=<sec>` is seconds since
the GPS rail came up, so a NON-MONOTONIC on= is the segment boundary; a START
event is one too. Everything below is computed per segment.
"""
import re, sys

LINE = re.compile(
    r'^(?P<ts>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}) on=(?P<on>\d+)s (?P<ev>\w+) '
    r'bps=(?P<bps>\d+) chars=(?P<chars>\d+) ok=(?P<ok>\d+) bad=(?P<bad>\d+) '
    # view= and cno= were added 2026-09-03 (satellites IN VIEW + strongest
    # C/N0). Optional so this still parses logs written before that.
    r'fix=(?P<fix>\d+) sats=(?P<sats>\d+) (?:view=(?P<view>\d+) cno=(?P<cno>\d+) )?'
    r'lock=(?P<lock>\d) stable=(?P<stable>\d) '
    r'state=(?P<state>.+?) mode=(?P<mode>\w+) ble=(?P<ble>-?\d+) wifi=(?P<wifi>\d) '
    r'lora=(?P<lora>\d) usb=(?P<usb>\d) chg=(?P<chg>\d) card=(?P<card>\d)$')

def parse(path):
    recs, skipped = [], []
    for n, raw in enumerate(open(path, errors='replace'), 1):
        line = raw.rstrip('\n')
        if not line or line.startswith('--'):
            skipped.append((n, line)); continue
        m = LINE.match(line)
        if not m:
            skipped.append((n, line)); continue
        d = m.groupdict()
        for k in ('on','bps','chars','ok','bad','fix','sats','ble'):
            d[k] = int(d[k])
        # Absent in pre-2026-09-03 logs; None means "not recorded", which is a
        # different thing from a recorded zero and must not be conflated.
        for k in ('view','cno'):
            d[k] = int(d[k]) if d.get(k) is not None else None
        for k in ('lock','stable','wifi','lora','usb','chg','card'):
            d[k] = bool(int(d[k]))
        d['_line'] = n
        recs.append(d)
    return recs, skipped

def segment(recs):
    """Split on START, or on a non-monotonic on= (a new rail power-up)."""
    segs, cur, prev_on = [], [], None
    for r in recs:
        boundary = (r['ev'] == 'START') or (prev_on is not None and r['on'] < prev_on)
        if boundary and cur:
            segs.append(cur); cur = []
        cur.append(r); prev_on = r['on']
    if cur: segs.append(cur)
    return segs

def fmt(s):
    s = int(s)
    return f"{s}s" if s < 60 else f"{s//60}m{s%60:02d}s"

def report(path):
    recs, skipped = parse(path)
    print(f"file: {path}")
    print(f"  parsed {len(recs)} records, {len(skipped)} non-record lines")
    for n, l in skipped:
        print(f"    L{n}: {l[:100]}")
    if not recs: return
    segs = segment(recs)
    print(f"  {len(segs)} GPS power cycle(s)\n")
    for i, sg in enumerate(segs, 1):
        first, last = sg[0], sg[-1]
        # TTFF: first record whose stable lock is set. `on` is seconds since the
        # rail came up, so it IS the elapsed acquisition time, no clock needed.
        lockrec  = next((r for r in sg if r['stable']), None)
        instrec  = next((r for r in sg if r['lock']), None)
        maxsats  = max(r['sats'] for r in sg)
        badtot   = max(r['bad'] for r in sg)
        oktot    = max(r['ok']  for r in sg)
        bpsvals  = [r['bps'] for r in sg]
        nosat    = sum(1 for r in sg if r['sats'] == 0)
        states   = {}
        for r in sg: states[r['state']] = states.get(r['state'], 0) + 1
        starts_at_zero = first['on'] <= 35   # did we actually catch the power-up?
        print(f"--- cycle {i}  (L{first['_line']}-L{last['_line']}) ---")
        print(f"  wall     {first['ts']} -> {last['ts']}")
        print(f"  on=      {first['on']}s -> {last['on']}s   ({len(sg)} ticks)"
              + ("" if starts_at_zero else "   [PARTIAL: power-up not in log]"))
        print(f"  mode     {first['mode']}  ble={first['ble']} wifi={int(first['wifi'])}"
              f" lora={int(first['lora'])} usb={int(first['usb'])} card={int(first['card'])}")
        if lockrec and (starts_at_zero or lockrec is not sg[0]):
            print(f"  TTFF     {fmt(lockrec['on'])} to STABLE lock at on={lockrec['on']}s")
            if instrec and instrec is not lockrec:
                print(f"           first instantaneous lock at on={instrec['on']}s")
        elif lockrec:
            print(f"  TTFF     UNKNOWN - already locked at the first record "
                  f"(on={first['on']}s); the acquisition is not in this file")
        else:
            print(f"  TTFF     NEVER LOCKED in {fmt(last['on'] - first['on'])} of log")
        print(f"  sats     max {maxsats} used; {nosat}/{len(sg)} ticks at 0 used")
        views = [r['view'] for r in sg if r['view'] is not None]
        if views:
            cnos = [r['cno'] for r in sg if r['cno'] is not None]
            noview = sum(1 for v in views if v == 0)
            print(f"  view     max {max(views)} in view; {noview}/{len(views)} ticks "
                  f"at 0 in view; best C/N0 {max(cnos) if cnos else 0} dB")
        else:
            print("  view     not recorded (log predates the GSV fields)")
        print(f"  nmea     bps min/med/max {min(bpsvals)}/{sorted(bpsvals)[len(bpsvals)//2]}/{max(bpsvals)}")
        print(f"  csum     ok={oktot} bad={badtot}"
              + (f"  <-- BYTE LOSS" if badtot else "  (clean)"))
        print(f"  states   " + ", ".join(f"{k}={v}" for k, v in states.items()))
        print()

if __name__ == '__main__':
    for p in sys.argv[1:]:
        report(p)
