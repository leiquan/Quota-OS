#!/usr/bin/env python3
"""Check the live desktop started with -qmp unix:/tmp/quota-desktop.sock,server=on,wait=off.
Run from the repository root; evidence goes to this task outputs/desktop folder.
"""
from pathlib import Path
import socket,json,time,re
out=Path('../../outputs/desktop').resolve();s=socket.socket(socket.AF_UNIX);s.connect('/tmp/quota-desktop.sock');s.settimeout(4);f=s.makefile('rb');f.readline();seq=0
def q(c,a=None):
 global seq
 seq+=1;s.sendall((json.dumps({'execute':c,'id':seq,**({'arguments':a} if a else {})})+'\n').encode())
 while True:
  r=json.loads(f.readline())
  if r.get('id')==seq:
   if 'error' in r:raise RuntimeError(r)
   return r['return']
q('qmp_capabilities')
def move(x,y):q('input-send-event',{'events':[{'type':'abs','data':{'axis':'x','value':x*32767//639}},{'type':'abs','data':{'axis':'y','value':y*32767//479}}]})
def button(n):q('human-monitor-command',{'command-line':'mouse_button '+str(n)})
def click(x,y):
 move(x,y);time.sleep(.15);button(1);time.sleep(.15);button(0);time.sleep(.25)
def shot(n):q('screendump',{'filename':str(out/(n+'.ppm'))})
shot('desktop');click(320,438)
for k in ['a','b']:q('human-monitor-command',{'command-line':'sendkey '+k});time.sleep(.2)
click(167,308);shot('notes');move(350,104);time.sleep(.1);button(1);time.sleep(.1)
for x,y in [(356,110),(362,116),(368,122),(374,128),(380,136)]:move(x,y);time.sleep(.12)
button(0);time.sleep(.25);shot('notes-moved');t=(out/'serial.log').read_text();m=re.findall(r'\[desktop\] moved (\d+),(\d+)',t);assert m,'drag event missing';x,y=map(int,m[-1]);assert x>100 and y>87,(x,y)
click(x+18,y+16);shot('notes-closed');click(320,438);shot('notes-restored');click(392,438);shot('about');click(123,102);click(x+18,y+16);click(40,79);shot('wallpaper');click(247,438);shot('desktop-final')
t=(out/'serial.log').read_text();r={'notes_open':'[lvgl] input page' in t,'typing_ab':'key code 30 value 1 char 97' in t and 'key code 48 value 1 char 98' in t,'count1':'click count 1' in t,'dragged_to':[x,y],'hidden_events':t.count('[desktop] window hidden'),'restored':t.count('[lvgl] input page')>=2,'about':'[lvgl] coverage page' in t,'panic':'PANIC' in t,'running':q('query-status')};(out/'interaction-results.json').write_text(json.dumps(r,indent=2));print(json.dumps(r,indent=2));assert all(r[k] for k in ['notes_open','typing_ab','count1','restored','about']) and not r['panic'];s.close()
