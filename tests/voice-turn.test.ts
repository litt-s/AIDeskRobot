import {DatabaseSync} from 'node:sqlite';
import {applyMigrations} from './migrations';
import {afterEach,beforeEach,describe,it,expect,vi} from 'vitest';
import {Hono} from 'hono';
import {HTTPException} from 'hono/http-exception';
import {encrypt,hash} from '../server/security';
import {SentenceSplitter,plainTextForSpeech,runVoiceTurn,withDeadline,VoiceEvent} from '../server/voice/pipeline';
import {StreamDemuxer,encodeJsonEvent,encodePcmChunk,STREAM_TYPE} from '../server/voice/stream';


const ENCRYPTION_KEY='AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=';

/* 语音服务是外部依赖，用假实现替换掉，测试只关心编排与分帧。 */
vi.mock('../server/voice/volc',()=>({
 voiceSettingsFromEnv:()=>null,
 transcribe:vi.fn(),
 synthesize:vi.fn(),
}));
const volc=await import('../server/voice/volc');
const {voiceRoutes}=await import('../server/voice/routes');
const {deviceRoutes}=await import('../server/device-routes');

function fixture(){
 const sqlite=new DatabaseSync(':memory:');
 applyMigrations(sqlite);
 sqlite.exec("INSERT INTO users VALUES ('u','owner','x','x','t');INSERT INTO users VALUES ('v','other','x','x','t');");
 const wrap=(sql:string,params:any[]=[])=>({bind(...values:any[]){return wrap(sql,values);},async first(){return sqlite.prepare(sql).get(...params)||null;},async all(){return {results:sqlite.prepare(sql).all(...params)};},async run(){const r=sqlite.prepare(sql).run(...params);return {meta:{changes:Number(r.changes)}};}});
 const db={prepare:wrap,async batch(stmts:any[]){sqlite.exec('BEGIN');try{const out=[];for(const s of stmts)out.push(await s.run());sqlite.exec('COMMIT');return out;}catch(e){sqlite.exec('ROLLBACK');throw e;}}} as unknown as D1Database;
 const app=new Hono<any>();
 app.use('*',async(c,next)=>{if(!c.req.path.startsWith('/api/device/'))c.set('user',{id:c.req.header('Test-User')||'u'});await next();});
 app.onError((e,c)=>c.json({error:e instanceof HTTPException?e.message:'请求失败，请重试'},e instanceof HTTPException?e.status:500));
 voiceRoutes(app);
 deviceRoutes(app);
 const env={DB:db,ENCRYPTION_KEY,PROVIDER_ALLOWED_HOSTS:'api.deepseek.com'};
 return {sqlite,app,env};
}

/** 建好账号、模型、角色、设备、语音凭据，返回设备密钥。 */
async function seed(ctx:ReturnType<typeof fixture>,user='u'){
 const {sqlite}=ctx;
 sqlite.exec(`INSERT INTO models VALUES ('m','${user}','deepseek','https://api.deepseek.com','deepseek-chat','${await encrypt('sk-secret',ENCRYPTION_KEY)}','••••cret','t','t');`);
 sqlite.exec(`INSERT INTO roles VALUES ('r','${user}','桌面助手','你是桌面机器人，回答简短口语化。','陪聊','m','t','t');`);
 sqlite.exec(`INSERT INTO voice_settings VALUES ('${user}','${await encrypt('volc-secret',ENCRYPTION_KEY)}','••••${user}','volc.seedasr.sauc.duration','seed-tts-2.0','zh_female_vv_uranus_bigtts','t','t');`);
 sqlite.exec(`INSERT INTO devices (id,user_id,name,key_hash,key_mask,firmware,battery,status,last_seen_at,created_at,updated_at,role_id,conversation_id) VALUES ('d','${user}','客厅机器人','${await hash('123456789ABC')}','••••-••••-9ABC',null,null,'offline',null,'t','t','r',NULL);`);
 return {key:'1234-5678-9ABC',sqlite};
}

function deviceCall(ctx:ReturnType<typeof fixture>,path:string,key:string,body:BodyInit|null=null){
 return ctx.app.request('http://localhost'+path,{method:'POST',headers:{Authorization:'Device '+key},body},ctx.env as any);
}

/* 模型侧用真的 SSE 响应，这样 provider 解析路径也在覆盖范围内。 */
function stubProvider(chunks:string[]){
 vi.stubGlobal('fetch',vi.fn(async()=>new Response(new ReadableStream({start(controller){const encoder=new TextEncoder();for(const chunk of chunks)controller.enqueue(encoder.encode(chunk));controller.close();}}),{status:200,headers:{'Content-Type':'text/event-stream'}})));
}

const sse=(delta:string)=>`data: ${JSON.stringify({choices:[{delta:{content:delta}}]})}\n\n`;

beforeEach(()=>{
 vi.mocked(volc.transcribe).mockReset();
 vi.mocked(volc.synthesize).mockReset();
/* 合成是"边收边回调"的，返回统计信息；这里只关心回调，所以忽略返回值。 */
 vi.mocked(volc.synthesize).mockImplementation((async(_s:any,_t:string,onChunk:any)=>{await onChunk(new Uint8Array([1,2]));await onChunk(new Uint8Array([3,4]));}) as any);
});

describe('sentence splitting',()=>{
 it('emits a sentence as soon as its terminator arrives',()=>{
  const splitter=new SentenceSplitter();
  expect(splitter.push('你好')).toEqual([]);
  expect(splitter.push('，今天天气不错。')).toEqual(['你好，今天天气不错。']);
  expect(splitter.push('要不要')).toEqual([]);
  expect(splitter.flush()).toEqual(['要不要']);
 });
 it('breaks a long run-on at a weak punctuation so the first audio is not delayed',()=>{
  const splitter=new SentenceSplitter();
  const long='这是一段没有任何句号的很长的话它在逗号处应该被切开否则第一句话要等整段生成完才能开口，后面还有很多内容要继续说下去';
  const parts=splitter.push(long);
  expect(parts.length).toBeGreaterThan(0);
  expect(parts[0].length).toBeLessThan(long.length);
  expect(splitter.flush().join('')).not.toContain('undefined');
 });
 it('treats whitespace-only deltas as nothing to say',()=>{
  const splitter=new SentenceSplitter();
  expect(splitter.push('   ')).toEqual([]);
  expect(splitter.flush()).toEqual([]);
 });
});

describe('speech cleanup',()=>{
 it('strips markdown that should not be read aloud',()=>{
  expect(plainTextForSpeech('## 标题\n- **要点**：`code`')).toBe('标题 要点：code');
  expect(plainTextForSpeech('看这里 [链接](https://example.com) 就好')).toBe('看这里 链接 就好');
  expect(plainTextForSpeech('```js\nconst a=1\n```\n以上')).toContain('代码省略');
 });
});

describe('device stream framing',()=>{
 it('round-trips both frame types',()=>{
  const demuxer=new StreamDemuxer();
  const frames=demuxer.push(encodeJsonEvent({type:'asr',text:'你好'}));
  expect(frames).toHaveLength(1);
  expect(frames[0].type).toBe(STREAM_TYPE.JSON);
  expect(JSON.parse(new TextDecoder().decode(frames[0].payload))).toEqual({type:'asr',text:'你好'});
  const pcm=demuxer.push(encodePcmChunk(new Uint8Array([1,2,3])));
  expect(pcm[0].type).toBe(STREAM_TYPE.PCM);
  expect([...pcm[0].payload]).toEqual([1,2,3]);
 });
 it('waits for the rest of a frame that arrived split across reads',()=>{
  const demuxer=new StreamDemuxer();
  const frame=encodeJsonEvent({type:'done'});
  expect(demuxer.push(frame.subarray(0,3))).toEqual([]);
  const frames=demuxer.push(frame.subarray(3));
  expect(frames).toHaveLength(1);
  expect(JSON.parse(new TextDecoder().decode(frames[0].payload))).toEqual({type:'done'});
 });
});

describe('voice turn orchestration',()=>{
 const collect=()=>{const events:(VoiceEvent|Uint8Array)[]=[];return {events,emit:(e:any)=>{events.push(e);}};};

 it('emits the transcript, then audio, then the reply, then done',async()=>{
  const {events,emit}=collect();
  const deps={
   transcribe:async()=>'今天天气怎么样',
   synthesize:async(_t:string,onChunk:any)=>{await onChunk(new Uint8Array([9]));},
   complete:async(_m:any,onDelta:any)=>{onDelta('今天晴天。');return '今天晴天。';},
  };
  const result=await runVoiceTurn(deps as any,[],new Uint8Array([0,0]),emit);
  expect(result).toMatchObject({transcript:'今天天气怎么样',reply:'今天晴天。',spoken:1,audioBytes:1});
  expect(events[0]).toEqual({type:'asr',text:'今天天气怎么样'});
  expect(events[1]).toBeInstanceOf(Uint8Array);
  expect(events[2]).toEqual({type:'reply',text:'今天晴天。'});
  expect(events[3]).toEqual({type:'done'});
 });

 it('starts synthesizing the first sentence before the model has finished',async()=>{
  const order:string[]=[];
  const deps={
   transcribe:async()=>'讲个故事',
   synthesize:async(text:string,onChunk:any)=>{order.push('tts:'+text);await onChunk(new Uint8Array([1]));},
   complete:async(_m:any,onDelta:any)=>{
    onDelta('从前有座山。');      // 第一句在这里就应该送去合成
    await new Promise(resolve=>setTimeout(resolve,5));
    order.push('model:still-generating');
    onDelta('山里有个庙。');
    return '从前有座山。山里有个庙。';
   },
  };
  await runVoiceTurn(deps as any,[],new Uint8Array([0,0]),async()=>{});
  expect(order).toEqual(['tts:从前有座山。','model:still-generating','tts:山里有个庙。']);
 });

 it('reports a failed recognition instead of staying silent',async()=>{
  const {events,emit}=collect();
  const deps={transcribe:async()=>{throw new Error('连接超时');},synthesize:async()=>{},complete:async()=>''};
  const result=await runVoiceTurn(deps as any,[],new Uint8Array([0,0]),emit);
  expect(JSON.stringify(events)).toContain('语音识别失败');
  expect(events.at(-1)).toEqual({type:'done'});
  expect(result.transcript).toBe('');
 });

 it('asks the user to repeat when nothing was recognized',async()=>{
  const {events,emit}=collect();
  const deps={transcribe:async()=>'   ',synthesize:async()=>{},complete:async()=>{throw new Error('不应调用模型');}};
  await runVoiceTurn(deps as any,[],new Uint8Array([0,0]),emit);
  expect(events).toEqual([{type:'asr',text:''},{type:'error',message:'没有听清，请再说一遍。'},{type:'done'}]);
 });

 it('still speaks what the model already produced when the stream breaks',async()=>{
  const {events,emit}=collect();
  const deps={
   transcribe:async()=>'你好',
   synthesize:async(_t:string,onChunk:any)=>{await onChunk(new Uint8Array([7]));},
   complete:async(_m:any,onDelta:any)=>{onDelta('你好呀，我是小助手。');throw new Error('模型服务暂时不可用');},
  };
  await runVoiceTurn(deps as any,[],new Uint8Array([0,0]),emit);
  expect(events.some(e=>e instanceof Uint8Array)).toBe(true);
  expect(JSON.stringify(events)).toContain('模型请求失败');
  expect(events.at(-1)).toEqual({type:'done'});
 });

 it('keeps reporting the conversation when synthesis itself fails',async()=>{
  const {events,emit}=collect();
  const deps={
   transcribe:async()=>'在吗',
   synthesize:async()=>{throw new Error('语音合成失败：HTTP 429');},
   complete:async(_m:any,onDelta:any)=>{onDelta('在的。');return '在的。';},
  };
  await runVoiceTurn(deps as any,[],new Uint8Array([0,0]),emit);
  expect(JSON.stringify(events)).toContain('语音合成失败');
  expect(events.at(-1)).toEqual({type:'done'});
 });

 /* 实测：向外部语音服务发起 WebSocket 握手时，AbortSignal 打不断那次 fetch。
  * 机器人等不起，所以每一步都必须有墙钟兜底。 */
 it('gives up on a step that never settles instead of leaving the robot waiting',async()=>{
  await expect(withDeadline(new Promise(()=>{}),20,'语音识别')).rejects.toThrow('语音识别超时');
  await expect(withDeadline(Promise.resolve('ok'),20,'模型请求')).resolves.toBe('ok');
  await expect(withDeadline(Promise.reject(new Error('上游拒绝')),20,'模型请求')).rejects.toThrow('上游拒绝');
 });

 it('reports a recognition timeout through the normal error path',async()=>{
  vi.useFakeTimers();
  try{
   const {events,emit}=collect();
   const deps={transcribe:()=>new Promise(()=>{}),synthesize:async()=>{},complete:async()=>''};
   const turn=runVoiceTurn(deps as any,[],new Uint8Array([0,0]),emit);
   await vi.advanceTimersByTimeAsync(26000);
   await turn;
   expect(JSON.stringify(events)).toContain('语音识别超时');
   expect(events.at(-1)).toEqual({type:'done'});
  }finally{vi.useRealTimers();}
 });
});

describe('device voice endpoint',()=>{
 beforeEach(()=>{stubProvider([sse('今天'),sse('晴天。'),'data: [DONE]\n\n']);});
 afterEach(()=>{vi.unstubAllGlobals();});

 it('runs a full turn and returns framed audio plus the reply',async()=>{
  const ctx=fixture();
  const {key}=await seed(ctx);
  vi.mocked(volc.transcribe).mockResolvedValue({text:'今天天气怎么样',utterances:[],elapsedMs:10});
  const response=await deviceCall(ctx,'/api/device/voice',key,new Uint8Array([0,0,0,0]));
  expect(response.status).toBe(200);
  expect(response.headers.get('X-Sample-Rate')).toBe('16000');
  const frames=new StreamDemuxer().push(new Uint8Array(await response.arrayBuffer()));
  const kinds=frames.map(f=>f.type);
  expect(kinds[0]).toBe(STREAM_TYPE.JSON);
  expect(kinds).toContain(STREAM_TYPE.PCM);
  const json=frames.filter(f=>f.type===STREAM_TYPE.JSON).map(f=>JSON.parse(new TextDecoder().decode(f.payload)));
  expect(json[0]).toEqual({type:'asr',text:'今天天气怎么样'});
  expect(json.at(-1)).toEqual({type:'done'});
  expect(json.some(e=>e.type==='reply'&&e.text==='今天晴天。')).toBe(true);
  /* 音频只在 PCM 帧里，且按句子顺序拼起来是完整的合成输出。 */
  const pcm=frames.filter(f=>f.type===STREAM_TYPE.PCM).flatMap(f=>[...f.payload]);
  expect(pcm).toEqual([1,2,3,4]);
 });

 it('writes the turn into a conversation the console can open',async()=>{
  const ctx=fixture();
  const {key,sqlite}=await seed(ctx);
  vi.mocked(volc.transcribe).mockResolvedValue({text:'把灯关掉',utterances:[],elapsedMs:10});
  await (await deviceCall(ctx,'/api/device/voice',key,new Uint8Array([0,0]))).arrayBuffer();
  const device=sqlite.prepare('SELECT conversation_id,role_id,status FROM devices WHERE id=?').get('d') as any;
  expect(device.conversation_id).toBeTruthy();
  expect(device.role_id).toBe('r');
  expect(device.status).toBe('online');
  const messages=sqlite.prepare('SELECT role,content,status FROM messages WHERE conversation_id=? ORDER BY rowid').all(device.conversation_id) as any[];
  expect(messages).toEqual([
   {role:'user',content:'把灯关掉',status:'complete'},
   {role:'assistant',content:'今天晴天。',status:'complete'},
  ]);
  /* 第二次对话复用同一条会话，历史随请求带上。 */
  const first=vi.mocked(volc.transcribe).mock.results.length;
  await (await deviceCall(ctx,'/api/device/voice',key,new Uint8Array([0,0]))).arrayBuffer();
  expect(vi.mocked(volc.transcribe).mock.results.length).toBe(first+1);
  expect(sqlite.prepare('SELECT COUNT(*) AS n FROM conversations').get() as any).toEqual({n:1});
  expect(sqlite.prepare('SELECT COUNT(*) AS n FROM messages').get() as any).toEqual({n:4});
 });

 it('records why a turn produced no answer, so the console is not just silent',async()=>{
  const ctx=fixture();
  const {key,sqlite}=await seed(ctx);
  /* Recognition is what fails here; without this the robot simply never
   * speaks and there is nothing anywhere for the user to act on. */
  vi.mocked(volc.transcribe).mockRejectedValue(new Error('语音识别超时（25 秒）'));
  await (await deviceCall(ctx,'/api/device/voice',key,new Uint8Array([0,0]))).arrayBuffer();
  const device=sqlite.prepare('SELECT conversation_id FROM devices WHERE id=?').get('d') as any;
  const messages=sqlite.prepare('SELECT role,content,status FROM messages WHERE conversation_id=? ORDER BY rowid').all(device.conversation_id) as any[];
  expect(messages).toHaveLength(1);
  expect(messages[0].role).toBe('assistant');
  expect(messages[0].status).toBe('failed');
  expect(messages[0].content).toContain('语音识别超时');
  /* The audio we actually sent has to be in the record: "captured silence" and
   * "captured the room but missed the speaker" look identical otherwise, and
   * they need opposite fixes. */
  expect(messages[0].content).toContain('秒音频');
  expect(messages[0].content).toContain('峰值');
 });

 it('reports the level of the audio when recognition heard nothing in it',async()=>{
  const ctx=fixture();
  const {key,sqlite}=await seed(ctx);
  vi.mocked(volc.transcribe).mockResolvedValue({text:'',utterances:[],elapsedMs:10});
  /* Silence: every sample zero. */
  const silence=new Uint8Array(16000*2);
  await (await deviceCall(ctx,'/api/device/voice',key,silence)).arrayBuffer();
  let device=sqlite.prepare('SELECT conversation_id FROM devices WHERE id=?').get('d') as any;
  let messages=sqlite.prepare('SELECT content FROM messages WHERE conversation_id=? ORDER BY rowid').all(device.conversation_id) as any[];
  expect(messages[0].content).toContain('没有听清');
  expect(messages[0].content).toMatch(/峰值 0/);
  expect(messages[0].content).toMatch(/有声占比 0%/);
  expect(messages[0].content).toMatch(/削顶 0%/);
  expect(messages[0].content).toMatch(/过零 0%/);

  /* One second of a loud tone: the level must show up as real signal. */
  vi.mocked(volc.transcribe).mockResolvedValue({text:'',utterances:[],elapsedMs:10});
  const tone=new Uint8Array(16000*2);
  const view=new DataView(tone.buffer);
  for(let i=0;i<16000;i+=1)view.setInt16(i*2,Math.round(20000*Math.sin(i/8)),true);
  await (await deviceCall(ctx,'/api/device/voice',key,tone)).arrayBuffer();
  device=sqlite.prepare('SELECT conversation_id FROM devices WHERE id=?').get('d') as any;
  messages=sqlite.prepare('SELECT content FROM messages WHERE conversation_id=? ORDER BY rowid').all(device.conversation_id) as any[];
  expect(messages[1].content).toMatch(/峰值 20\d\d\d/);
  expect(messages[1].content).toMatch(/有声占比 \d?\d%/);
 });

/*
 * Peak and RMS cannot tell "amplified far past full scale" from "noise floor on
 * an input that was never really connected" - both pin the peak and report a
 * near-total "voiced" share. The two situations need opposite fixes, so the
 * figures that separate them are pinned here in both directions.
 */
it('separates a clipped signal from a noise floor',async()=>{
  const ctx=fixture();
  const {key,sqlite}=await seed(ctx);
  vi.mocked(volc.transcribe).mockResolvedValue({text:'',utterances:[],elapsedMs:10});

  /* A real waveform driven far past full scale, then clamped the way the
   * firmware clamps it: loud, mostly at the rails, but still a waveform. */
  const loud=new Uint8Array(16000*2);
  const loudView=new DataView(loud.buffer);
  for(let i=0;i<16000;i+=1){
    const value=Math.round(200000*Math.sin(i/8));
    loudView.setInt16(i*2,Math.max(-32768,Math.min(32767,value)),true);
  }
  await (await deviceCall(ctx,'/api/device/voice',key,loud)).arrayBuffer();

  /* Uncorrelated full-range samples: consecutive values share no shape, so it
   * crosses zero about half the time and touches a rail only by chance. */
  const noise=new Uint8Array(16000*2);
  const noiseView=new DataView(noise.buffer);
  let state=12345;
  for(let i=0;i<16000;i+=1){
    state=(Math.imul(state,1103515245)+12345)|0;
    noiseView.setInt16(i*2,state>>16,true);
  }
  await (await deviceCall(ctx,'/api/device/voice',key,noise)).arrayBuffer();

  const device=sqlite.prepare('SELECT conversation_id FROM devices WHERE id=?').get('d') as any;
  const messages=sqlite.prepare('SELECT content FROM messages WHERE conversation_id=? ORDER BY rowid').all(device.conversation_id) as any[];
  /* Clipped but waveform-like: most samples at a rail, few zero crossings. */
  expect(messages[0].content).toMatch(/削顶 [89]\d%/);
  expect(messages[0].content).toMatch(/过零 \d%/);
  /* Noise: never at a rail, crossing zero about every other sample. */
  expect(messages[1].content).toMatch(/削顶 0%/);
  expect(messages[1].content).toMatch(/过零 [45]\d%/);
});

 it('records what the model said even when nothing could be synthesised',async()=>{
  const ctx=fixture();
  const {key,sqlite}=await seed(ctx);
  vi.mocked(volc.transcribe).mockResolvedValue({text:'今天天气怎么样',utterances:[],elapsedMs:10});
  vi.mocked(volc.synthesize).mockRejectedValue(new Error('语音合成失败：HTTP 429'));
  await (await deviceCall(ctx,'/api/device/voice',key,new Uint8Array([0,0]))).arrayBuffer();
  const device=sqlite.prepare('SELECT conversation_id FROM devices WHERE id=?').get('d') as any;
  const messages=sqlite.prepare('SELECT role,content,status FROM messages WHERE conversation_id=? ORDER BY rowid').all(device.conversation_id) as any[];
  expect(messages).toEqual([
   {role:'user',content:'今天天气怎么样',status:'complete'},
   {role:'assistant',content:'今天晴天。',status:'complete'},
  ]);
 });

 it('speaks a sentence without recording anything',async()=>{
  const ctx=fixture();
  const {key}=await seed(ctx);
  const response=await ctx.app.request('http://localhost/api/device/say',{method:'POST',headers:{Authorization:'Device '+key,'Content-Type':'application/json'},body:JSON.stringify({text:'你好呀'})},ctx.env as any);
  expect(response.status).toBe(200);
  expect(response.headers.get('X-Sample-Rate')).toBe('16000');
  const frames=new StreamDemuxer().push(new Uint8Array(await response.arrayBuffer()));
  expect(frames.some(f=>f.type===STREAM_TYPE.PCM)).toBe(true);
  const json=frames.filter(f=>f.type===STREAM_TYPE.JSON).map(f=>JSON.parse(new TextDecoder().decode(f.payload)));
  expect(json.at(-1)).toEqual({type:'done'});
  /* The announcement path must not touch the microphone or the model. */
  expect(volc.transcribe).not.toHaveBeenCalled();
  expect(vi.mocked(volc.synthesize).mock.calls[0][1]).toBe('你好呀');
 });

 it('rejects an empty or oversized announcement',async()=>{
  const ctx=fixture();
  const {key}=await seed(ctx);
  const say=(body:unknown)=>ctx.app.request('http://localhost/api/device/say',{method:'POST',headers:{Authorization:'Device '+key,'Content-Type':'application/json'},body:JSON.stringify(body)},ctx.env as any);
  expect((await say({text:'   '})).status).toBe(400);
  expect((await say({text:'长'.repeat(201)})).status).toBe(400);
  expect((await say({})).status).toBe(400);
 });

 it('recognises at the rate the device reports, not the server default',async()=>{
  const ctx=fixture();
  const {key,sqlite}=await seed(ctx);
  vi.mocked(volc.transcribe).mockResolvedValue({text:'',utterances:[],elapsedMs:10});
  /* One second of audio captured at 24 kHz: 24000 samples. Reporting the
   * wrong rate would either stretch or squash every duration we compute. */
  const pcm=new Uint8Array(24000*2);
  const response=await ctx.app.request('http://localhost/api/device/voice',{method:'POST',headers:{Authorization:'Device '+key,'X-Sample-Rate':'24000'},body:pcm},ctx.env as any);
  await response.arrayBuffer();
  expect(vi.mocked(volc.transcribe).mock.calls[0][0]).toMatchObject({sampleRate:24000});
  const device=sqlite.prepare('SELECT conversation_id FROM devices WHERE id=?').get('d') as any;
  const messages=sqlite.prepare('SELECT content FROM messages WHERE conversation_id=? ORDER BY rowid').all(device.conversation_id) as any[];
  expect(messages[0].content).toContain('收到 1.0 秒音频');
 });

 it('ignores a nonsensical reported rate instead of trusting it',async()=>{
  const ctx=fixture();
  const {key}=await seed(ctx);
  vi.mocked(volc.transcribe).mockResolvedValue({text:'',utterances:[],elapsedMs:10});
  for(const header of ['0','-1','999999','abc','']){
   await (await ctx.app.request('http://localhost/api/device/voice',{method:'POST',headers:{Authorization:'Device '+key,'X-Sample-Rate':header},body:new Uint8Array(320)},ctx.env as any)).arrayBuffer();
  }
  expect(vi.mocked(volc.transcribe).mock.calls.every(c=>(c[0] as any).sampleRate===16000)).toBe(true);
 });

 it('rejects a device that has no voice role yet, before any audio is processed',async()=>{
  const ctx=fixture();
  const {key,sqlite}=await seed(ctx);
  sqlite.exec("UPDATE devices SET role_id=NULL WHERE id='d'");
  vi.mocked(volc.transcribe).mockResolvedValue({text:'x',utterances:[],elapsedMs:1});
  const response=await deviceCall(ctx,'/api/device/voice',key,new Uint8Array([0,0]));
  expect(response.status).toBe(409);
  expect(await response.json()).toMatchObject({error:expect.stringContaining('语音角色')});
  expect(volc.transcribe).not.toHaveBeenCalled();
 });

 it('rejects an account with no voice credentials configured',async()=>{
  const ctx=fixture();
  const {key,sqlite}=await seed(ctx);
  sqlite.exec('DELETE FROM voice_settings');
  const response=await deviceCall(ctx,'/api/device/voice',key,new Uint8Array([0,0]));
  expect(response.status).toBe(409);
  expect(await response.json()).toMatchObject({error:expect.stringContaining('语音服务')});
 });

 it('rejects empty, unaligned and oversized uploads',async()=>{
  const ctx=fixture();
  const {key}=await seed(ctx);
  expect((await deviceCall(ctx,'/api/device/voice',key,new Uint8Array(0))).status).toBe(400);
  expect((await deviceCall(ctx,'/api/device/voice',key,new Uint8Array([1]))).status).toBe(400);
  expect((await deviceCall(ctx,'/api/device/voice',key,new Uint8Array(32000*31))).status).toBe(413);
 });

 it('requires a valid device key',async()=>{
  const ctx=fixture();
  await seed(ctx);
  expect((await deviceCall(ctx,'/api/device/voice','','')).status).toBe(401);
  expect((await deviceCall(ctx,'/api/device/voice','ZZZZ-ZZZZ-ZZZZ',new Uint8Array([0,0]))).status).toBe(401);
 });
});

describe('voice settings endpoint',()=>{
 it('never returns the stored key, only a mask',async()=>{
  const ctx=fixture();
  await seed(ctx);
  const response=await ctx.app.request('http://localhost/api/voice/settings',{headers:{'Test-User':'u'}},ctx.env as any);
  const body=await response.json() as any;
  expect(body.voice).toMatchObject({configured:true,source:'account',key_mask:'••••u'});
  expect(JSON.stringify(body)).not.toContain('volc-secret');
 });

 it('saves a key, keeps the model-only edits from re-sending it, and can be cleared',async()=>{
  const ctx=fixture();
  const put=(body:unknown,user='u')=>ctx.app.request('http://localhost/api/voice/settings',{method:'PUT',headers:{'Test-User':user,'Content-Type':'application/json'},body:JSON.stringify(body)},ctx.env as any);
  const first=await put({apiKey:'volc-new-secret'});
  expect((await first.json() as any).voice.key_mask).toBe('••••••••cret');
  const second=await put({speaker:'zh_male_x'});
  expect(second.status).toBe(200);
  expect((await second.json() as any).voice).toMatchObject({speaker:'zh_male_x',key_mask:'••••••••cret'});
  const back=await put({apiKey:'volc-new-secret',speaker:'zh_male_x'});
  expect((await back.json() as any).voice.speaker).toBe('zh_male_x');
  expect((await ctx.app.request('http://localhost/api/voice/settings',{method:'DELETE',headers:{'Test-User':'u'}},ctx.env as any)).status).toBe(200);
  expect((await (await ctx.app.request('http://localhost/api/voice/settings',{headers:{'Test-User':'u'}},ctx.env as any)).json() as any).voice.configured).toBe(false);
 });

 it('does not let one account overwrite or read another account settings',async()=>{
  const ctx=fixture();
  await seed(ctx,'v');
  const put=await ctx.app.request('http://localhost/api/voice/settings',{method:'PUT',headers:{'Test-User':'u','Content-Type':'application/json'},body:JSON.stringify({apiKey:'mine'})},ctx.env as any);
  expect(put.status).toBe(200);
  const rows=ctx.sqlite.prepare('SELECT user_id FROM voice_settings ORDER BY user_id').all() as any[];
  expect(rows).toEqual([{user_id:'u'},{user_id:'v'}]);
  const other=await (await ctx.app.request('http://localhost/api/voice/settings',{headers:{'Test-User':'v'}},ctx.env as any)).json() as any;
  expect(other.voice.key_mask).toBe('••••v');
 });
});

describe('device role binding',()=>{
 it('clears the conversation when the role changes so the new role starts fresh',async()=>{
  const ctx=fixture();
  const {key,sqlite}=await seed(ctx);
  vi.mocked(volc.transcribe).mockResolvedValue({text:'你好',utterances:[],elapsedMs:1});
  await deviceCall(ctx,'/api/device/voice',key,new Uint8Array([0,0]));
  const before=sqlite.prepare('SELECT conversation_id FROM devices WHERE id=?').get('d') as any;
  expect(before.conversation_id).toBeTruthy();
  sqlite.exec("INSERT INTO roles VALUES ('r2','u','另一个角色','换了个角色','','m','t','t')");
  const patched=await ctx.app.request('http://localhost/api/devices/d',{method:'PATCH',headers:{'Test-User':'u','Content-Type':'application/json'},body:JSON.stringify({name:'客厅机器人',role_id:'r2'})},ctx.env as any);
  expect(patched.status).toBe(200);
  const after=sqlite.prepare('SELECT conversation_id FROM devices WHERE id=?').get('d') as any;
  expect(after.conversation_id).toBeNull();
 });});
