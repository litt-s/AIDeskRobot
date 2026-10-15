import {DatabaseSync} from 'node:sqlite';
import {applyMigrations} from './migrations';
import {describe,it,expect} from 'vitest';
import {Hono} from 'hono';
import {agentRoutes,announcementText,normalizeOptions} from '../server/agent-routes';

/*
 * T042 第一期的补充：需要确认的事件要念出"当时的选项"，跟 Codex 弹窗里那几项一致。
 * 这套用例守三件事：选项进得了库、进得了播报、坏数据不会把上报本身搞失败。
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
 return {sqlite,call,mint,report,speaks};
}

describe('agent announcement text',()=>{
 it('keeps the old wording when there are no options',()=>{
  expect(announcementText('Codex','needs_confirmation','要执行 rm -rf dist')).toBe('Codex 需要你确认：要执行 rm -rf dist');
  expect(announcementText('Codex','finished','改完 3 个文件')).toBe('Codex 的任务完成了：改完 3 个文件');
 });
 it('reads the options out for a confirmation',()=>{
  const say=announcementText('Codex','needs_confirmation','要执行 rm -rf dist',['允许一次','本次会话都允许','拒绝']);
  expect(say).toBe('Codex 需要你确认：要执行 rm -rf dist。可选：允许一次，本次会话都允许，拒绝');
 });
 it('gives the options room but never rambles',()=>{
  const say=announcementText('Codex','needs_confirmation','要'.repeat(60),['允许一次','本次会话都允许','拒绝']);
  expect(say.length).toBeLessThanOrEqual(90);
  expect(say).toContain('可选：');
  expect(announcementText('Codex','failed','x'.repeat(200)).length).toBeLessThanOrEqual(60);
 });
 it('drops junk options instead of failing the report',()=>{
  expect(normalizeOptions(['允许一次','','   ','x'.repeat(25),5,null,'允许一次','拒绝'])).toEqual(['允许一次','拒绝']);
  expect(normalizeOptions(['1','2','3','4','5'])).toEqual(['1','2','3','4']);
  expect(normalizeOptions('允许一次')).toEqual([]);
 });
});

describe('agent event options',()=>{
 it('stores the options and speaks them',async()=>{
  const {sqlite,mint,report,speaks}=fixture();
  const key=await mint();
  const r=await report(key,{kind:'needs_confirmation',source:'Codex',title:'要执行 rm -rf dist',options:['允许一次','本次会话都允许','拒绝']});
  expect(r.status).toBe(201);
  const body=await r.json() as any;
  expect(body.say).toContain('可选：允许一次，本次会话都允许，拒绝');
  expect(body.event.options).toEqual(['允许一次','本次会话都允许','拒绝']);
  expect((sqlite.prepare('SELECT options FROM agent_events').get() as any).options).toBe('["允许一次","本次会话都允许","拒绝"]');
  expect(JSON.parse(speaks()[0].payload).text).toBe(body.say);
 });
 it('ignores options that arrive with a non-confirmation event',async()=>{
  const {mint,report}=fixture();
  const key=await mint();
  const body=await (await report(key,{kind:'finished',source:'Codex',title:'改完 3 个文件',options:['允许一次']})).json() as any;
  expect(body.say).toBe('Codex 的任务完成了：改完 3 个文件');
 });
 it('turns stored options back into an array for the console',async()=>{
  const {call,mint,report}=fixture();
  const key=await mint();
  await report(key,{kind:'needs_confirmation',source:'Codex',title:'要执行 rm -rf dist',options:['允许一次','拒绝']});
  const list=await (await call('/api/agent/events')).json() as any;
  expect(list.events[0].options).toEqual(['允许一次','拒绝']);
 });
});
