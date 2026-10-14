import {DatabaseSync} from 'node:sqlite';
import {readFileSync} from 'node:fs';
import {describe,it,expect} from 'vitest';
import {Hono} from 'hono';
import {chatRoutes} from '../server/chat-routes';

function fixture(){
 const sqlite=new DatabaseSync(':memory:');
 for(const file of ['0001_initial.sql','0002_limits.sql','0003_generations.sql'])sqlite.exec(readFileSync(new URL('../migrations/'+file,import.meta.url),'utf8'));
 sqlite.exec("INSERT INTO users VALUES ('u','user','x','x','t');INSERT INTO conversations VALUES ('c','u',NULL,'title','t','t');INSERT INTO messages VALUES ('m1','c','u','user','first question','complete','2026-10-14T00:00:00.000Z');INSERT INTO messages VALUES ('m2','c','u','assistant','first answer','complete','2026-10-14T00:00:01.000Z');INSERT INTO messages VALUES ('m3','c','u','user','second question','complete','2026-10-14T00:00:02.000Z');INSERT INTO messages VALUES ('m4','c','u','assistant','partial answer','interrupted','2026-10-14T00:00:03.000Z');INSERT INTO models VALUES ('m','u','fixture','https://api.openai.com/v1','fixture','cipher','masked','t','t');INSERT INTO roles VALUES ('r','u','fixture','sys','','m','t','t');UPDATE conversations SET role_id='r';");
 const wrap=(sql:string,params:any[]=[])=>({bind(...values:any[]){return wrap(sql,values);},async first(){return sqlite.prepare(sql).get(...params)||null;},async all(){return {results:sqlite.prepare(sql).all(...params)};},async run(){const r=sqlite.prepare(sql).run(...params);return {meta:{changes:Number(r.changes)}};}});
 const db={prepare:wrap,async batch(stmts:any[]){sqlite.exec('BEGIN');try{const out=[];for(const s of stmts)out.push(await s.run());sqlite.exec('COMMIT');return out;}catch(e){sqlite.exec('ROLLBACK');throw e;}}} as unknown as D1Database;
 const app=new Hono<any>();
 app.use('*',async(c,next)=>{c.set('user',{id:c.req.header('Test-User')||'u'});await next();});
 chatRoutes(app);
 const call=(path:string,user='u')=>app.request('http://localhost/api/conversations/c'+path,{method:'POST',headers:{'Test-User':user,'Content-Type':'application/json'},body:'{}'},{DB:db,ENCRYPTION_KEY:'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA='});
 return {sqlite,call};
}

describe('rewind',()=>{
 it('removes the last user question and its reply, returns the undone text and clears generation',async()=>{
  const {sqlite,call}=fixture();
  sqlite.exec("INSERT INTO chat_generations VALUES ('c','u','m4','tok','paused','[]','partial answer',9999999999999,'t',NULL)");
  const res=await call('/rewind');
  expect(res.status).toBe(200);
  expect((await res.json() as any).removed).toBe('second question');
  expect(sqlite.prepare('SELECT id FROM messages ORDER BY rowid').all().map((r:any)=>r.id)).toEqual(['m1','m2']);
  expect(sqlite.prepare('SELECT count(*) AS n FROM chat_generations').get()?.n).toBe(0);
 });
 it('refuses to rewind while a reply is still generating',async()=>{
  const {sqlite,call}=fixture();
  sqlite.exec("INSERT INTO chat_generations VALUES ('c','u','m4','tok','running','[]','partial answer',9999999999999,'t',NULL)");
  expect((await call('/rewind')).status).toBe(409);
  expect(sqlite.prepare('SELECT count(*) AS n FROM messages').get()?.n).toBe(4);
 });
 it('reports nothing to undo on an empty history and denies other tenants',async()=>{
  const {sqlite,call}=fixture();
  expect((await call('/rewind','other')).status).toBe(404);
  sqlite.exec('DELETE FROM messages');
  expect((await call('/rewind')).status).toBe(409);
 });
});

describe('regenerate',()=>{
 it('replaces the previous answer, keeps the existing question and reuses prior context',async()=>{
  const {sqlite,call}=fixture();
  const res=await call('/regenerate');
  expect(res.status).toBe(200);
  const g=sqlite.prepare('SELECT assistant_id,state,partial,context_json FROM chat_generations').get() as any;
  expect(g.state).toBe('running');
  expect(g.partial).toBe('');
  expect(g.assistant_id).not.toBe('m4');
  const ctx=JSON.parse(g.context_json);
  expect(ctx[0].role).toBe('system');
  expect(ctx.at(-1)).toEqual({role:'user',content:'second question'});
  expect(JSON.stringify(ctx)).not.toContain('partial answer');
  expect(ctx.some((x:any)=>x.content==='first answer')).toBe(true);
  const rows=sqlite.prepare('SELECT role,content,status FROM messages ORDER BY rowid').all() as any[];
  expect(rows.map(r=>r.role)).toEqual(['user','assistant','user','assistant']);
  expect(rows.filter(r=>r.role==='user').length).toBe(2);
  expect(rows.at(-1)).toMatchObject({role:'assistant',content:'',status:'generating'});
  await res.body?.cancel();
 });
 it('refuses to regenerate while a reply is still generating',async()=>{
  const {sqlite,call}=fixture();
  sqlite.exec("INSERT INTO chat_generations VALUES ('c','u','m4','tok','running','[]','partial answer',9999999999999,'t',NULL)");
  expect((await call('/regenerate')).status).toBe(409);
  expect(sqlite.prepare('SELECT content FROM messages ORDER BY rowid DESC LIMIT 1').get()?.content).toBe('partial answer');
 });
 it('reports nothing to regenerate without a question and denies other tenants',async()=>{
  const {sqlite,call}=fixture();
  expect((await call('/regenerate','other')).status).toBe(404);
  sqlite.exec("DELETE FROM messages WHERE role='user'");
  expect((await call('/regenerate')).status).toBe(409);
 });
});
