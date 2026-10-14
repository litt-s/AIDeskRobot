import {DatabaseSync} from 'node:sqlite';
import {readFileSync} from 'node:fs';
import {describe,it,expect} from 'vitest';
import {Hono} from 'hono';
import {deviceRoutes} from '../server/device-routes';

function fixture(){
 const sqlite=new DatabaseSync(':memory:');
 for(const file of ['0001_initial.sql','0002_limits.sql','0003_generations.sql','0004_devices.sql','0005_voice.sql'])sqlite.exec(readFileSync(new URL('../migrations/'+file,import.meta.url),'utf8'));
 sqlite.exec("INSERT INTO users VALUES ('u','owner','x','x','t');INSERT INTO users VALUES ('v','other','x','x','t');");
 const wrap=(sql:string,params:any[]=[])=>({bind(...values:any[]){return wrap(sql,values);},async first(){return sqlite.prepare(sql).get(...params)||null;},async all(){return {results:sqlite.prepare(sql).all(...params)};},async run(){const r=sqlite.prepare(sql).run(...params);return {meta:{changes:Number(r.changes)}};}});
 const db={prepare:wrap,async batch(stmts:any[]){sqlite.exec('BEGIN');try{const out=[];for(const s of stmts)out.push(await s.run());sqlite.exec('COMMIT');return out;}catch(e){sqlite.exec('ROLLBACK');throw e;}}} as unknown as D1Database;
 const app=new Hono<any>();
 app.use('*',async(c,next)=>{c.set('user',{id:c.req.header('Test-User')||'u'});await next();});
 deviceRoutes(app);
 const env={DB:db,ENCRYPTION_KEY:'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA='};
 const call=(path:string,init:RequestInit={},user='u')=>app.request('http://localhost'+path,{...init,headers:{'Test-User':user,'Content-Type':'application/json',...(init.headers||{})}},env);
 const bind=async(name='robot')=>{const r=await call('/api/devices',{method:'POST',body:JSON.stringify({name})});return await r.json() as any;};
 const deviceCall=(path:string,key:string,init:RequestInit={})=>app.request('http://localhost'+path,{...init,headers:{'Content-Type':'application/json',Authorization:'Device '+key,...(init.headers||{})}},env);
 return {sqlite,call,bind,deviceCall};
}

describe('device binding',()=>{
 it('returns the key once and only stores a mask',async()=>{
  const {sqlite,call,bind}=fixture();
  const created=await bind('客厅机器人');
  expect(created.key).toMatch(/^[0-9A-Z]{4}-[0-9A-Z]{4}-[0-9A-Z]{4}$/);
  expect(created.device.key_mask).not.toContain(created.key);
  const row=sqlite.prepare('SELECT key_hash,key_mask FROM devices').get() as any;
  expect(row.key_hash).not.toBe(created.key);
  expect(row.key_mask).toBe(created.device.key_mask);
  const list=await (await call('/api/devices')).json() as any;
  expect(list.devices).toHaveLength(1);
  expect(JSON.stringify(list)).not.toContain(created.key);
 });
 it('hides devices from other accounts',async()=>{
  const {call,bind}=fixture();
  const created=await bind();
  expect((await call('/api/devices')).status).toBe(200);
  const other=await (await call('/api/devices',{},'v')).json() as any;
  expect(other.devices).toHaveLength(0);
  expect((await call('/api/devices/'+created.device.id,{method:'PATCH',body:JSON.stringify({name:'x'})},'v')).status).toBe(404);
  expect((await call('/api/devices/'+created.device.id,{method:'DELETE'},'v')).status).toBe(404);
 });
 it('stops accepting a key after the device is unbound',async()=>{
  const {call,bind,deviceCall}=fixture();
  const created=await bind();
  expect((await deviceCall('/api/device/hello',created.key,{method:'POST',body:'{}'})).status).toBe(200);
  await call('/api/devices/'+created.device.id,{method:'DELETE'});
  expect((await deviceCall('/api/device/hello',created.key,{method:'POST',body:'{}'})).status).toBe(401);
 });
});

describe('device channel',()=>{
 it('rejects missing and unknown credentials',async()=>{
  const {deviceCall}=fixture();
  expect((await deviceCall('/api/device/hello','',{method:'POST',body:'{}'})).status).toBe(401);
  expect((await deviceCall('/api/device/hello','not-a-real-key',{method:'POST',body:'{}'})).status).toBe(401);
 });
 it('accepts the hand-typed form of the key but nothing else',async()=>{
  const {bind,deviceCall}=fixture();
  const created=await bind();
  const typed=created.key.replace(/-/g,'').toLowerCase();
  expect((await deviceCall('/api/device/hello',typed,{method:'POST',body:'{}'})).status).toBe(200);
  const swapped=created.key.slice(0,-1)+(created.key.endsWith('Z')?'1':'Z');
  expect((await deviceCall('/api/device/hello',swapped,{method:'POST',body:'{}'})).status).toBe(401);
  expect((await deviceCall('/api/device/hello',typed.slice(0,-2),{method:'POST',body:'{}'})).status).toBe(401);
 });
 it('marks the device online on hello and records reported status',async()=>{
  const {call,bind,deviceCall}=fixture();
  const created=await bind();
  await deviceCall('/api/device/hello',created.key,{method:'POST',body:JSON.stringify({firmware:'0.1.0'})});
  await deviceCall('/api/device/status',created.key,{method:'POST',body:JSON.stringify({battery:77,firmware:'0.1.1'})});
  const list=await (await call('/api/devices')).json() as any;
  expect(list.devices[0]).toMatchObject({online:true,status:'online',battery:77,firmware:'0.1.1'});
  expect(list.devices[0].last_seen_at).toBeTruthy();
 });
 it('delivers a console command through polling and reflects the device result',async()=>{
  const {call,bind,deviceCall}=fixture();
  const created=await bind();
  const posted=await call('/api/devices/'+created.device.id+'/commands',{method:'POST',body:JSON.stringify({type:'emotion',payload:{name:'happy'}})});
  expect(posted.status).toBe(201);
  const first=await (await deviceCall('/api/device/commands',created.key)).json() as any;
  expect(first.commands).toHaveLength(1);
  expect(first.commands[0]).toMatchObject({type:'emotion',payload:'{"name":"happy"}'});
  const second=await (await deviceCall('/api/device/commands',created.key)).json() as any;
  expect(second.commands).toHaveLength(0);
  const ack=await deviceCall('/api/device/commands/'+first.commands[0].id+'/ack',created.key,{method:'POST',body:JSON.stringify({status:'done',result:'ok'})});
  expect(ack.status).toBe(200);
  const log=await (await call('/api/devices/'+created.device.id+'/commands')).json() as any;
  expect(log.commands[0]).toMatchObject({status:'done',result:'ok'});
  expect((await deviceCall('/api/device/commands/'+first.commands[0].id+'/ack',created.key,{method:'POST',body:'{}'})).status).toBe(404);
 });
 it('validates command payloads and rejects unknown types',async()=>{
  const {call,bind}=fixture();
  const created=await bind();
  const post=(body:unknown)=>call('/api/devices/'+created.device.id+'/commands',{method:'POST',body:JSON.stringify(body)});
  expect((await post({type:'emotion',payload:{name:'bogus'}})).status).toBe(400);
  expect((await post({type:'volume',payload:{value:200}})).status).toBe(400);
  expect((await post({type:'launch_missile',payload:{}})).status).toBe(400);
  expect((await post({type:'speak',payload:{text:'你好'}})).status).toBe(201);
  expect((await post({type:'listen',payload:{}})).status).toBe(201);
  expect((await post({type:'volume',payload:{value:42}})).status).toBe(201);
 });
});
