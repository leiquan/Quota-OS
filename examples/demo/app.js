const fs=require('node:fs'),p='/app/data/note.txt';
rpc.set('load',()=>{try{return fs.readFileSync(p,'utf8')}catch(e){if(e.code==='ENOENT')return '';throw e}});
rpc.set('save',async text=>{if(typeof text!=='string'||Buffer.byteLength(text,'utf8')>496)throw Error('Note exceeds 496 UTF-8 bytes');fs.writeFileSync(p,text);await rpc.get('saved','Saved');return true});
