import {DatabaseSync} from 'node:sqlite';
import {applyMigrations} from './migrations';
import {afterEach,beforeEach,describe,it,expect,vi} from 'vitest';
import {Hono} from 'hono';
import {HTTPException} from 'hono/http-exception';
import {encrypt} from '../server/security';
import {meetingRoutes,parseTranscript} from '../server/meeting-routes';

const ENCRYPTION_KEY='AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=';

vi.mock('../server/voice/volc',()=>({
  voiceSettingsFromEnv:()=>null,
  transcribe:vi.fn(),
}));
const volc=await import('../server/voice/volc');

function fixture(){
  const sqlite=new DatabaseSync(':memory:');
  applyMigrations(sqlite);
  sqlite.exec("INSERT INTO users VALUES ('u','owner','x','x','t');INSERT INTO users VALUES ('v','other','x','x','t');");
  const wrap=(sql:string,params:any[]=[]):any=>({
    bind(...values:any[]){return wrap(sql,values);},
    async first(){return sqlite.prepare(sql).get(...params)||null;},
    async all(){return {results:sqlite.prepare(sql).all(...params)};},
    async run(){const r=sqlite.prepare(sql).run(...params);return {meta:{changes:Number(r.changes)}};},
  });
  const db={prepare:wrap,async batch(statements:any[]){sqlite.exec('BEGIN');try{const out=[];for(const statement of statements)out.push(await statement.run());sqlite.exec('COMMIT');return out;}catch(error){sqlite.exec('ROLLBACK');throw error;}}} as unknown as D1Database;
  const app=new Hono<any>();
  app.use('*',async(c,next)=>{c.set('user',{id:c.req.header('Test-User')||'u'});await next();});
  app.onError((error,c)=>c.json({error:error instanceof HTTPException?error.message:'请求失败'},error instanceof HTTPException?error.status:500));
  meetingRoutes(app);
  const env={DB:db,ENCRYPTION_KEY,PROVIDER_ALLOWED_HOSTS:'api.deepseek.com'};
  const call=async(path:string,init:RequestInit={},user='u'):Promise<Response>=>await app.request('http://localhost'+path,{...init,headers:{...(init.headers||{}),'Test-User':user}},env) as Response;
  return {sqlite,env,call};
}

async function seed(ctx:ReturnType<typeof fixture>){
  const cipher=await encrypt('sk-fixture-secret',ENCRYPTION_KEY);
  ctx.sqlite.exec(`INSERT INTO models VALUES ('m','u','DeepSeek','https://api.deepseek.com','deepseek-chat','${cipher}','••••cret','t','t');`);
  ctx.sqlite.exec(`INSERT INTO voice_settings VALUES ('u','${await encrypt('volc-secret',ENCRYPTION_KEY)}','••••u','asr-resource','tts-resource','speaker','t','t');`);
}

beforeEach(()=>{vi.mocked(volc.transcribe).mockReset();});
afterEach(()=>{vi.unstubAllGlobals();vi.restoreAllMocks();});

describe('meeting transcript parser',()=>{
  it('keeps explicit speaker labels and estimates turn boundaries',()=>{
    const turns=parseTranscript('[张三] 目标是周五上线。\r\n李四：我负责回归。');
    expect(turns.map(turn=>[turn.speaker_label,turn.content])).toEqual([['张三','目标是周五上线。'],['李四','我负责回归。']]);
    expect(turns[1].started_ms).toBe(turns[0].ended_ms);
    expect(turns.every(turn=>turn.content.length>0)).toBe(true);
  });
});

describe('meeting API',()=>{
  it('creates a meeting, imports speaker turns, renames speakers and isolates users',async()=>{
    const ctx=fixture();await seed(ctx);
    const created=await (await ctx.call('/api/meetings',{method:'POST',body:JSON.stringify({title:'产品周会',model_id:'m'})})).json() as any;
    expect(created.meeting.status).toBe('new');
    const imported=await (await ctx.call(`/api/meetings/${created.meeting.id}/transcript`,{method:'PUT',body:JSON.stringify({text:'[张三] 目标是周五上线。\n李四：我负责回归。'})})).json() as any;
    expect(imported.meeting.status).toBe('transcribed');
    expect(imported.turns).toHaveLength(2);
    expect((await ctx.call(`/api/meetings/${created.meeting.id}`,{},'v')).status).toBe(404);
    const renamed=await (await ctx.call(`/api/meetings/${created.meeting.id}/speakers`,{method:'POST',body:JSON.stringify({from:'张三',to:'项目经理'})})).json() as any;
    expect(renamed.turns[0].speaker_label).toBe('项目经理');
    expect(renamed.meeting.transcript).toContain('张三');
    const patched=await (await ctx.call(`/api/meetings/${created.meeting.id}/turns/${renamed.turns[0].id}`,{method:'PATCH',body:JSON.stringify({content:'目标是周五 18 点上线。'})})).json() as any;
    expect(patched.turns[0].content).toBe('目标是周五 18 点上线。');
    expect(ctx.sqlite.prepare('SELECT COUNT(*) AS n FROM meetings').get()).toEqual({n:1});
  });

  it('transcribes PCM chunks, keeps only text, and marks the meeting complete',async()=>{
    const ctx=fixture();await seed(ctx);
    const created=await (await ctx.call('/api/meetings',{method:'POST',body:JSON.stringify({title:'评审会',model_id:'m'})})).json() as any;
    vi.mocked(volc.transcribe).mockResolvedValue({text:'先上线登录。',utterances:[],elapsedMs:10});
    const audio=await ctx.call(`/api/meetings/${created.meeting.id}/audio`,{method:'POST',headers:{'X-Audio-Duration-Ms':'2000','Content-Type':'application/octet-stream'},body:new Uint8Array([1,0,2,0])});

    expect(audio.status).toBe(200);
    const data=await audio.json() as any;
    expect(data.meeting.status).toBe('transcribing');
    expect(data.meeting.source).toBe('audio');
    expect(data.turns[0].content).toBe('先上线登录。');
    expect(data.turns[0].speaker_label).toBe('片段 01');
    expect(ctx.sqlite.prepare("SELECT name FROM pragma_table_info('meetings') WHERE name LIKE '%audio%'").all()).toEqual([]);
    const completed=await (await ctx.call(`/api/meetings/${created.meeting.id}/complete`,{method:'POST',body:JSON.stringify({duration_seconds:120})})).json() as any;
    expect(completed.meeting.status).toBe('transcribed');
    expect(completed.meeting.duration_seconds).toBe(120);
    expect(vi.mocked(volc.transcribe).mock.calls[0]?.[0]).toMatchObject({sampleRate:16000});
  });

  it('validates audio and rejects an empty transcript before model generation',async()=>{
    const ctx=fixture();await seed(ctx);
    const created=await (await ctx.call('/api/meetings',{method:'POST',body:JSON.stringify({title:'空会议',model_id:'m'})})).json() as any;
    expect((await ctx.call(`/api/meetings/${created.meeting.id}/audio`,{method:'POST',headers:{'X-Audio-Duration-Ms':'1000'},body:new Uint8Array([1])})).status).toBe(400);
    expect((await ctx.call(`/api/meetings/${created.meeting.id}/minutes`,{method:'POST',body:JSON.stringify({model_id:'m'})})).status).toBe(400);
  });

  it('generates and persists structured minutes through the selected model',async()=>{
    const ctx=fixture();await seed(ctx);
    const created=await (await ctx.call('/api/meetings',{method:'POST',body:JSON.stringify({title:'上线会',model_id:'m'})})).json() as any;
    await ctx.call(`/api/meetings/${created.meeting.id}/transcript`,{method:'PUT',body:JSON.stringify({text:'张三：周五上线。'})});
    vi.stubGlobal('fetch',vi.fn(async()=>new Response(JSON.stringify({choices:[{message:{content:'# 会议纪要\n\n周五上线。'}}]}),{status:200,headers:{'Content-Type':'application/json'}})));
    const response=await ctx.call(`/api/meetings/${created.meeting.id}/minutes`,{method:'POST',body:JSON.stringify({model_id:'m'})});
    expect(response.status).toBe(200);
    const data=await response.json() as any;
    expect(data.meeting.minutes_status).toBe('ready');
    expect(data.meeting.minutes).toContain('周五上线');
    expect(ctx.sqlite.prepare('SELECT minutes_status FROM meetings WHERE id=?').get(created.meeting.id)).toEqual({minutes_status:'ready'});
    const fetchMock=vi.mocked(fetch);
    const init=fetchMock.mock.calls[0]?.[1] as RequestInit;
    expect(String(init.body)).toContain('张三：周五上线');
  });
});
