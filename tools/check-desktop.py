#!/usr/bin/env python3
"""Exercise the 1280x800 desktop via /tmp/quota-desktop-wide.sock.
Run from the repo root after launching QEMU with that QMP socket.
"""
from pathlib import Path
import json,re,runpy,time
out=Path('../../outputs/desktop-1280').resolve();mod=runpy.run_path('tools/realtime-rounds.py');q=mod['QMP']('/tmp/quota-desktop-wide.sock');t=(out/'serial.log').read_text();m=re.search(r'ramfb published (\d+)x(\d+)',t);w,h=map(int,m.groups());assert(w,h)==(1280,800)
def move(x,y):q.call('input-send-event',{'events':[{'type':'abs','data':{'axis':'x','value':x*32767//(w-1)}},{'type':'abs','data':{'axis':'y','value':y*32767//(h-1)}}]})
def button(n):q.call('human-monitor-command',{'command-line':'mouse_button '+str(n)})
def click(x,y):
 move(x,y);time.sleep(.15);button(1);time.sleep(.15);button(0);time.sleep(.25)
def shot(name):q.call('screendump',{'filename':str(out/(name+'.ppm'))})
move(w-1,h-1);time.sleep(.2);shot('desktop')
click((w-306)//2+152,h-49)
for key in 'a better place to work':
 q.call('human-monitor-command',{'command-line':'sendkey '+('spc' if key==' ' else key)});time.sleep(.13)
m=re.search(r'\[lvgl\] plus box (\d+),(\d+) (\d+)x(\d+)',t);x,y,bw,bh=map(int,m.groups());click(x+bw//2,y+bh//2)
move(970,259);time.sleep(.1);button(1);time.sleep(.1)
for xy in [(978,267),(986,275),(994,283),(1002,291),(1010,303)]:move(*xy);time.sleep(.1)
button(0);time.sleep(.2);t=(out/'serial.log').read_text();m=re.findall(r'\[desktop\] moved (\d+),(\d+)',t);assert m,'drag missing';x,y=map(int,m[-1]);assert x>650 and y>238
shot('windows');click(x+23,y+21);time.sleep(.2);click((w-306)//2+152,h-49);shot('restored')
move(1205,680);time.sleep(.2);shot('readme-desktop')
t=(out/'serial.log').read_text();r={'size':[w,h],'edge_pointer':bool(re.search(r'pointer 127[7-9],79[7-9]',t)),'notes_open':'[lvgl] input page' in t,'typed':'key code 48 value 1 char 98' in t,'count1':'click count 1' in t,'dragged':[x,y],'closed':'[desktop] window hidden' in t,'restored':t.count('[lvgl] input page')>=2,'panic':'PANIC' in t,'status':q.call('query-status')};(out/'interaction-results.json').write_text(json.dumps(r,indent=2));print(json.dumps(r,indent=2));assert all(r[k] for k in ['edge_pointer','notes_open','typed','count1','closed','restored']) and not r['panic'];q.close()
