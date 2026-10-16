import {DatabaseSync} from 'node:sqlite';
import {applyMigrations} from './migrations';
import {describe,it,expect} from 'vitest';
import {Hono} from 'hono';
import {agentRoutes,announcementText,normalizeOptions} from '../server/agent-routes';

/*
 * T042 第一期：agent 事件通道。
 * 语音只报"哪个软件 + 什么状态"（用户要求"机器人可以不用念具体的内容"），
 * 标题/细节/批准选项落库并显示在控制台——两套东西必须分得开，这套用例守的就是这条线。
 */
function fixture(){
 const sqlite=new DatabaseSync(':memory:');
 applyMigrations(sqlite);
 sqlite.exec("INSERT INTO users VALUES ('u','owner','x','x','t');");
 sqlite.exec("INSERT INTO devices (id,user_id,name,key_hash,key_mask,status,created_at,updated_at) VALUES ('d1','u','客厅机器人','h1','••••-••••-0001','offline','t','t');");
 const wrap=(sql:string,params:any[]=[]):any=>({bind(...values:any[]){return wrap(sql,values);},async first(){return sqlite.prepare(sql).get(...params)||null;},async all(){return {results:sqlite.prepare(sql).all(...params)};},async run(){const r=sqlite.prepare(sql).run(...params);return {meta:{changes:Number(r.changes)}};}});
 const db={prepare:wrap} as unknown as D1Database;
 const app=new Hono<any>();
 app.use('*',async(c,next)=>{c.set('user',{id:'u'});await next();});
 agentRoutes(app);
 const env={DB:db};
 const call=(path:string,init:RequestInit={})=>app.request('http://localhost'+path,{...init,headers:{'Content-Type':'application/json',...(init.headers||{})}},env);
 const mint=async()=>((await (await call('/api/agent/keys',{method:'POST',body:'{}'})).json()) as any).secret as string;
 const report=(key:string,body:any)=>app.request('http://localhost/api/agent/events',{method:'POST',headers:{'Content-Type':'application/json',Authorization:'Agent '+key},body:JSON.stringify(body)},env);
 const speaks=()=>sqlite.prepare("SELECT payload FROM device_commands WHERE type='speak'").all() as any[];
 const agentGet=(key:string,path:string)=>app.request('http://localhost'+path,{headers:{Authorization:'Agent '+key}},env);
 const sayOf=()=>JSON.parse(speaks()[0].payload).text as string;
 return {sqlite,call,mint,report,speaks,sayOf,agentGet};
}

describe('agent announcement text',()=>{
 it('says just the software and the state',()=>{
  expect(announcementText('Codex','needs_confirmation')).toBe('Codex 待批准');
  expect(announcementText('Codex','failed')).toBe('Codex 任务中断');
  expect(announcementText('Codex','finished')).toBe('Codex 任务完成');
 });
 it('stays short even when the software name is long',()=>{
  const say=announcementText('某'.repeat(80),'finished');
  expect(say.length).toBeLessThanOrEqual(40);
  expect(say.endsWith('…')).toBe(true);
 });
 it('drops junk options instead of failing the report',()=>{
  expect(normalizeOptions(['允许一次','','   ','x'.repeat(25),5,null,'允许一次','拒绝'])).toEqual(['允许一次','拒绝']);
  expect(normalizeOptions(['1','2','3','4','5'])).toEqual(['1','2','3','4']);
  expect(normalizeOptions('允许一次')).toEqual([]);
 });
});

describe('agent event options',()=>{
 it('stores the options but keeps them out of the announcement',async()=>{
  const {sqlite,mint,report,sayOf}=fixture();
  const key=await mint();
  const r=await report(key,{kind:'needs_confirmation',source:'Codex',title:'要执行 rm -rf dist',detail:'很长的细节，语音里不该出现',options:['允许一次','本次会话都允许','拒绝']});
  expect(r.status).toBe(201);
  const body=await r.json() as any;
  expect(body.say).toBe('Codex 待批准');
  expect(body.event.options).toEqual(['允许一次','本次会话都允许','拒绝']);
  expect((sqlite.prepare('SELECT options FROM agent_events').get() as any).options).toBe('["允许一次","本次会话都允许","拒绝"]');
  expect(sayOf()).toBe('Codex 待批准');
 });
 it('never speaks the title or the detail',async()=>{
  const {mint,report,sayOf}=fixture();
  const key=await mint();
  await report(key,{kind:'finished',source:'Codex',title:'改完 3 个文件',detail:'改了 web/src/main.tsx'});
  expect(sayOf()).toBe('Codex 任务完成');
 });
 it('ignores options that arrive with a non-confirmation event',async()=>{
  const {mint,report,sayOf}=fixture();
  const key=await mint();
  const body=await (await report(key,{kind:'failed',source:'Codex',title:'npm test 失败',options:['允许一次']})).json() as any;
  expect(body.say).toBe('Codex 任务中断');
  expect(sayOf()).toBe('Codex 任务中断');
 });
 it('turns stored options back into an array for the console',async()=>{
  const {call,mint,report}=fixture();
  const key=await mint();
  await report(key,{kind:'needs_confirmation',source:'Codex',title:'要执行 rm -rf dist',options:['允许一次','拒绝']});
  const list=await (await call('/api/agent/events')).json() as any;
  expect(list.events[0].options).toEqual(['允许一次','拒绝']);
 });
});

describe('console test alert',()=>{
 it('fires a test event through the very same path',async()=>{
  const {call,speaks,sayOf}=fixture();
  const response=await call('/api/agent/events/test',{method:'POST',body:'{}'});
  expect(response.status).toBe(201);
  const body=await response.json() as any;
  expect(body.event.source).toBe('测试');
  expect(body.event.options).toEqual(['允许一次','本次会话都允许','拒绝']);
  expect(body.say).toBe('测试 待批准');
  expect(body.announced).toBe(1);
  expect(speaks()).toHaveLength(1);
  expect(sayOf()).toBe('测试 待批准');
  const list=await (await call('/api/agent/events')).json() as any;
  expect(list.events[0].title).toContain('测试提醒');
  expect(list.events[0].detail).toContain('手机');
  expect(list.events[0].status).toBe('open');
 });
});
describe('phone watch heartbeat',()=>{
 it('records the phone and reports it back for the console',async()=>{
  const {call}=fixture();
  const empty=await (await call('/api/agent/watch')).json() as any;
  expect(empty.watch).toBeNull();
  const posted=await call('/api/agent/watch/heartbeat',{method:'POST',body:JSON.stringify({label:'Pixel 8',notified:3,error:''})});
  expect(posted.status).toBe(200);
  const body=await (await call('/api/agent/watch')).json() as any;
  expect(body.watch.label).toBe('Pixel 8');
  expect(body.watch.notified_count).toBe(3);
  expect(body.watch.last_error).toBeNull();
  expect(typeof body.watch.last_seen_at).toBe('string');
 });
 it('keeps the last error so the console can say why the phone went quiet',async()=>{
  const {call}=fixture();
  await call('/api/agent/watch/heartbeat',{method:'POST',body:JSON.stringify({label:'Pixel 8',notified:0,error:'没拿到登录态'})});
  await call('/api/agent/watch/heartbeat',{method:'POST',body:JSON.stringify({label:'Pixel 8',notified:5,error:''})});
  const body=await (await call('/api/agent/watch')).json() as any;
  expect(body.watch.notified_count).toBe(5);
  expect(body.watch.last_error).toBeNull();
 });
});
describe('remote approval',()=>{
 it('lets the phone decide once and only once',async()=>{
  const {call,mint,report,sqlite}=fixture();
  const key=await mint();
  const created=await (await report(key,{kind:'needs_confirmation',source:'Codex',title:'rm -rf dist',options:['允许一次','拒绝']})).json() as any;
  const id=created.event.id;
  const first=await call(`/api/agent/events/${id}/decision`,{method:'POST',body:JSON.stringify({decision:'deny',from:'phone'})});
  expect(first.status).toBe(200);
  expect(await first.json()).toMatchObject({decision:'deny',decided_from:'phone'});
  const again=await call(`/api/agent/events/${id}/decision`,{method:'POST',body:JSON.stringify({decision:'allow'})});
  expect(again.status).toBe(409);
  expect(sqlite.prepare('SELECT decision,decided_from,status FROM agent_events WHERE id=?').get(id)).toMatchObject({decision:'deny',decided_from:'phone',status:'acked'});
 });
 it('refuses a decision that arrives after Codex stopped waiting',async()=>{
  const {call,mint,report,sqlite}=fixture();
  const key=await mint();
  const created=await (await report(key,{kind:'needs_confirmation',source:'Codex',title:'x'})).json() as any;
  const id=created.event.id;
  sqlite.prepare('UPDATE agent_events SET created_at=? WHERE id=?').run(new Date(Date.now()-10*60*1000).toISOString(),id);
  const late=await call(`/api/agent/events/${id}/decision`,{method:'POST',body:JSON.stringify({decision:'allow'})});
  expect(late.status).toBe(409);
  expect(sqlite.prepare('SELECT decision FROM agent_events WHERE id=?').get(id)).toMatchObject({decision:null});
 });
 it('hands the decision to the waiting hook, and says null when nobody answered',async()=>{
  const {agentGet,mint,report,sqlite}=fixture();
  const key=await mint();
  const created=await (await report(key,{kind:'needs_confirmation',source:'Codex',title:'x'})).json() as any;
  const id=created.event.id;
  expect(await (await agentGet(key,`/api/agent/events/${id}/decision?wait=0`)).json()).toMatchObject({decision:null});
  expect((await agentGet('not-a-real-key',`/api/agent/events/${id}/decision?wait=0`)).status).toBe(401);
  sqlite.prepare("UPDATE agent_events SET decision='allow',decided_at=?,decided_from='phone' WHERE id=?").run(new Date().toISOString(),id);
  expect(await (await agentGet(key,`/api/agent/events/${id}/decision?wait=0`)).json()).toMatchObject({decision:'allow',decided_from:'phone'});
 });
 it('carries the decision back to the console list',async()=>{
  const {call,mint,report}=fixture();
  const key=await mint();
  const created=await (await report(key,{kind:'needs_confirmation',source:'Codex',title:'x'})).json() as any;
  await call(`/api/agent/events/${created.event.id}/decision`,{method:'POST',body:JSON.stringify({decision:'allow',from:'mini'})});
  const list=await (await call('/api/agent/events')).json() as any;
  expect(list.events[0]).toMatchObject({decision:'allow',decided_from:'mini'});
 });
});