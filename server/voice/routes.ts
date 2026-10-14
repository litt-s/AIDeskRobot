/*
 * 语音通道。
 *
 *   /api/voice/settings   控制台用（会话鉴权）：配置本账号的火山语音凭据
 *   /api/device/voice     机器人用（设备密钥鉴权）：一次请求完成一轮对话
 *
 * 机器人只上传一段 PCM、只下载一段带分帧的 PCM，不需要 WebSocket，
 * 也不需要理解模型协议；换供应商只改这里的适配层。
 */
import {HTTPException} from 'hono/http-exception';
import {decrypt,encrypt,maskKey} from '../security';
import {deviceAuth,fail,limit} from '../device-auth';
import {fetchProvider} from '../provider';
import {providerURL} from '../provider-policy';
import {SSELines} from '../generation';
import {ChatMessage,VoiceDeps,plainTextForSpeech,runVoiceTurn,withDeadline,SPEAK_DEADLINE_MS} from './pipeline';
import {encodeJsonEvent,encodePcmChunk} from './stream';
import {transcribe,synthesize,voiceSettingsFromEnv,VoiceSettings} from './volc';

const now=()=>new Date().toISOString();
/* 10 秒是够长的口令，30 秒已远超一次对话的合理长度；再长多半是异常重传。 */
const MAX_AUDIO_BYTES=32000*30;
const DEFAULT_ASR_RESOURCE='volc.seedasr.sauc.duration';
const DEFAULT_TTS_RESOURCE='seed-tts-2.0';
const DEFAULT_SPEAKER='zh_female_vv_uranus_bigtts';

type Env={DB:D1Database;ENCRYPTION_KEY:string;PROVIDER_ALLOWED_HOSTS?:string;VOLC_API_KEY?:string;VOLC_ASR_RESOURCE?:string;VOLC_TTS_RESOURCE?:string;VOLC_TTS_SPEAKER?:string};

function field(value:unknown,label:string,max:number,required=true){if(typeof value!=='string'||value.length>max||(required&&!value.trim()))throw new HTTPException(400,{message:`${label}无效`});return value.trim();}

/**
 * 账号自带凭据优先，否则回落到平台环境变量。
 * 回落让"自用先跑通"和"以后开放给多用户"不必分成两套代码。
 */
export async function resolveVoiceSettings(db:D1Database,uid:string,env:Env):Promise<{settings:VoiceSettings;source:'account'|'platform'}|null>{
 const row=await db.prepare('SELECT * FROM voice_settings WHERE user_id=?').bind(uid).first() as any;
 if(row)return {source:'account',settings:{apiKey:await decrypt(row.api_key_cipher,env.ENCRYPTION_KEY),asrResourceId:row.asr_resource,ttsResourceId:row.tts_resource,speaker:row.speaker,sampleRate:16000}};
 const fallback=voiceSettingsFromEnv(env);
 return fallback?{settings:fallback,source:'platform'}:null;
}

/** 流式请求模型并逐段回调。与网页端共用 SSELines，保证两边对同一份协议的解析一致。 */
async function completeOnce(env:Env,messages:ChatMessage[],cfg:any,onDelta:(delta:string)=>void):Promise<string>{
 const secret=await decrypt(cfg.key_cipher,env.ENCRYPTION_KEY);
 const response=await fetchProvider(providerURL(cfg.base_url,env.PROVIDER_ALLOWED_HOSTS)+'/chat/completions',{method:'POST',redirect:'manual',signal:AbortSignal.timeout(120000),headers:{'Content-Type':'application/json',Authorization:'Bearer '+secret},body:JSON.stringify({model:cfg.model,stream:true,messages})},secret);
 if(!response.body)throw new Error('模型服务未返回内容');
 const reader=response.body.getReader(),decoder=new TextDecoder(),parser=new SSELines();
 let answer='',success=false;
 for(;;){const {done,value}=await reader.read();const lines=parser.feed(done?decoder.decode():decoder.decode(value,{stream:true}),done);for(const raw of lines){if(raw==='[DONE]'){success=true;continue;}const data=JSON.parse(raw);if(data.error)throw new Error('模型服务返回错误');const delta=data.choices?.[0]?.delta?.content;if(typeof delta==='string'&&delta){answer+=delta;onDelta(delta);}if(data.choices?.[0]?.finish_reason)success=true;}if(done)break;}
 if(!success&&!answer)throw new Error('模型服务未返回内容');
 return answer;
}

/**
 * Summarise the PCM a device uploaded.
 *
 * Recognition answering "no speech here" leaves us with several very different
 * situations that look identical from the outside - the microphone captured
 * nothing at all, it captured the room but missed the speaker, or what arrived
 * is not speech at all (a real signal driven far past full scale, or noise from
 * an input that was never really connected). They need opposite fixes, so the
 * numbers go into the failure the user sees rather than being guessed at from
 * the symptom.
 *
 * Peak and RMS alone cannot separate the last two: a signal amplified well past
 * full scale and a full-scale noise floor both sit at a pinned peak with a very
 * high "voiced" share. The other two figures do separate them.
 *   clipped - share of samples at full scale. High means a real signal arrived
 *             with far too much gain; a noise floor only touches the rails by
 *             chance, so it stays near zero.
 *   crossing - share of zero crossings. Speech stays well under 20% even when
 *             it is clipping; noise sits near 50%, because consecutive samples
 *             are uncorrelated.
 */
function describeAudio(pcm: Uint8Array, sampleRate: number): string {
  /* DataView instead of Int16Array: no alignment assumption on the buffer. */
  const view = new DataView(pcm.buffer, pcm.byteOffset, pcm.length);
  const count = Math.floor(pcm.length / 2);
  let peak = 0;
  let sum = 0;
  let active = 0;
  let clipped = 0;
  let crossings = 0;
  let previous = 0;
  for (let index = 0; index < count; index += 1) {
    const sample = view.getInt16(index * 2, true);
    const value = Math.abs(sample);
    if (value > peak) peak = value;
    sum += value * value;
    if (value > 300) active += 1;
    /* Both rails count: clamping saturates at +32767 and -32768. */
    if (value >= 32767) clipped += 1;
    if (index > 0 && sample < 0 !== previous < 0) crossings += 1;
    previous = sample;
  }
  const rms = count ? Math.round(Math.sqrt(sum / count)) : 0;
  const voiced = count ? Math.round((active / count) * 100) : 0;
  const clippedPercent = count ? Math.round((clipped / count) * 100) : 0;
  const crossingPercent = count > 1 ? Math.round((crossings / (count - 1)) * 100) : 0;
  return `收到 ${(count / sampleRate).toFixed(1)} 秒音频，峰值 ${peak}，均方根 ${rms}，有声占比 ${voiced}%，削顶 ${clippedPercent}%，过零 ${crossingPercent}%`;
}

/**
 * Capture rate the device reports, so both ends agree on what the audio is.
 * Deliberately narrow: the rate has to be one the speech service accepts, and
 * accepting arbitrary values would let a broken device make the server declare
 * nonsense. Falls back to the server default when absent or implausible.
 */
function captureSampleRate(header: string | undefined, fallback: number): number {
  const value = Number(header);
  if (!Number.isInteger(value) || value < 8000 || value > 48000) return fallback;
  return value;
}

export function voiceRoutes(app:any){
 app.get('/api/voice/settings',async(c:any)=>{const uid=c.get('user').id;const row=await c.env.DB.prepare('SELECT key_mask,asr_resource,tts_resource,speaker,updated_at FROM voice_settings WHERE user_id=?').bind(uid).first() as any;const fallback=voiceSettingsFromEnv(c.env);return c.json({voice:row?{configured:true,source:'account',key_mask:row.key_mask,asr_resource:row.asr_resource,tts_resource:row.tts_resource,speaker:row.speaker,updated_at:row.updated_at}:{configured:!!fallback,source:fallback?'platform':'none',key_mask:null,asr_resource:fallback?.asrResourceId??DEFAULT_ASR_RESOURCE,tts_resource:fallback?.ttsResourceId??DEFAULT_TTS_RESOURCE,speaker:fallback?.speaker??DEFAULT_SPEAKER,updated_at:null}});});
 /* apiKey 可以省略：只改音色或资源号时不必把密钥再贴一遍。 */
 app.put('/api/voice/settings',async(c:any)=>{const uid=c.get('user').id,b=await c.req.json();const existing=await c.env.DB.prepare('SELECT * FROM voice_settings WHERE user_id=?').bind(uid).first() as any;const apiKey=b.apiKey?field(b.apiKey,'语音 API key',500):(existing?await decrypt(existing.api_key_cipher,c.env.ENCRYPTION_KEY):fail('请填写语音 API key'));const asrResource=field(b.asr_resource??existing?.asr_resource??DEFAULT_ASR_RESOURCE,'语音识别资源号',120);const ttsResource=field(b.tts_resource??existing?.tts_resource??DEFAULT_TTS_RESOURCE,'语音合成资源号',120);const speaker=field(b.speaker??existing?.speaker??DEFAULT_SPEAKER,'音色',120);const t=now();await c.env.DB.prepare('INSERT INTO voice_settings VALUES (?,?,?,?,?,?,?,?) ON CONFLICT(user_id) DO UPDATE SET api_key_cipher=excluded.api_key_cipher,key_mask=excluded.key_mask,asr_resource=excluded.asr_resource,tts_resource=excluded.tts_resource,speaker=excluded.speaker,updated_at=excluded.updated_at').bind(uid,await encrypt(apiKey,c.env.ENCRYPTION_KEY),maskKey(apiKey),asrResource,ttsResource,speaker,existing?.created_at??t,t).run();return c.json({voice:{configured:true,source:'account',key_mask:maskKey(apiKey),asr_resource:asrResource,tts_resource:ttsResource,speaker,updated_at:t}});});
 app.delete('/api/voice/settings',async(c:any)=>{await c.env.DB.prepare('DELETE FROM voice_settings WHERE user_id=?').bind(c.get('user').id).run();const fallback=voiceSettingsFromEnv(c.env);return c.json({voice:{configured:!!fallback,source:fallback?'platform':'none'}});});

 /* 播报一句话：只做合成，不走模型。设备不依赖 AIUI 会话也能出声。 */
 app.post('/api/device/say',async(c:any)=>{
  const device=await deviceAuth(c);
  await limit(c.env.DB,'device-say:'+device.id,30);
  const resolved=await resolveVoiceSettings(c.env.DB,device.user_id,c.env);
  if(!resolved)fail('账号尚未配置语音服务，请在控制台填写',409);
  const body=await c.req.json().catch(()=>({})) as Record<string,unknown>;
  const text=typeof body.text==='string'?body.text.trim():'';
  if(!text)fail('缺少播报文本',400);
  if(text.length>200)fail('播报文本请控制在 200 字以内',400);

  const settings=resolved.settings;
  const spoken=plainTextForSpeech(text)||text;
  const stream=new ReadableStream<Uint8Array>({
   async start(controller){
    let closed=false;
    const emit=async(event:any)=>{if(closed)return;try{controller.enqueue(event instanceof Uint8Array?encodePcmChunk(event):encodeJsonEvent(event));}catch{closed=true;}};
    let bytes=0;
    try{
     await withDeadline(synthesize(settings,spoken,async(chunk)=>{bytes+=chunk.length;await emit(chunk);}),SPEAK_DEADLINE_MS,'语音合成');
     if(!bytes)await emit({type:'error',message:'语音合成没有返回音频'});
    }catch(error){
     await emit({type:'error',message:`语音合成失败：${error instanceof Error?error.message:String(error)}`});
    }
    await emit({type:'done'});
    closed=true;
    try{controller.close();}catch{/* 客户端已断开 */}
   },
  },{highWaterMark:0});
  return new Response(stream,{headers:{'Content-Type':'application/octet-stream','Cache-Control':'no-store','X-Content-Type-Options':'nosniff','X-Sample-Rate':String(settings.sampleRate)}});
 });

 app.post('/api/device/voice',async(c:any)=>{
  const device=await deviceAuth(c);
  await limit(c.env.DB,'device-voice:'+device.id,30);
  const resolved=await resolveVoiceSettings(c.env.DB,device.user_id,c.env);
  if(!resolved)fail('账号尚未配置语音服务，请在控制台填写',409);
  if(!device.role_id)fail('请在控制台为这台设备选择语音角色',409);

  const cfg=await c.env.DB.prepare('SELECT r.system_prompt,r.task,m.base_url,m.model,m.key_cipher FROM roles r JOIN models m ON m.id=r.model_id AND m.user_id=r.user_id WHERE r.id=? AND r.user_id=?').bind(device.role_id,device.user_id).first() as any;
  if(!cfg)fail('设备绑定的角色或其关联模型不存在',409);

  const audio=new Uint8Array(await c.req.arrayBuffer());
  if(!audio.length)fail('未收到音频数据',400);
  if(audio.length>MAX_AUDIO_BYTES)fail('音频过长，请控制在 30 秒以内',413);
  /* 16 位采样按 2 字节对齐；奇数长度说明不是 16 位 PCM。 */
  if(audio.length%2)fail('音频必须是 16 位 PCM',400);

  const conversation=await ensureConversation(c,device);
  const history=(await c.env.DB.prepare("SELECT role,content FROM messages WHERE conversation_id=? AND user_id=? AND status='complete' ORDER BY created_at DESC,rowid DESC LIMIT 100").bind(conversation,device.user_id).all()).results.reverse() as ChatMessage[];
  const context:ChatMessage[]=[{role:'system',content:cfg.system_prompt+'\n任务：'+cfg.task},...history];

  /*
   * Recognition must be told the rate the device actually captured at; a
   * mismatch makes the service decode the samples at the wrong speed. Synthesis
   * keeps the fixed playback rate, because that half of the device never moves.
   */
  const settings=resolved.settings;
  const captureRate=captureSampleRate(c.req.header('X-Sample-Rate'),settings.sampleRate);
  const asrSettings={...settings,sampleRate:captureRate};
  const deps:VoiceDeps={transcribe:(pcm)=>transcribe(asrSettings,pcm).then(r=>r.text),synthesize:(text,onChunk)=>synthesize(settings,text,onChunk).then(()=>undefined),complete:(messages,onDelta)=>completeOnce(c.env,messages,cfg,onDelta)};

  const encoder=new TextEncoder();
  const stream=new ReadableStream<Uint8Array>({
   async start(controller){
    let closed=false;
    const emit=async(event:any)=>{if(closed)return;try{controller.enqueue(event instanceof Uint8Array?encodePcmChunk(event):encodeJsonEvent(event));}catch{closed=true;}};
    try{
     const turn=await runVoiceTurn(deps,context,audio,emit);
     /*
      * A turn that produced no answer used to leave no trace anywhere the user
      * could see: the robot just stayed silent and the console showed nothing.
      * Record what happened in the device's own conversation so the reason is
      * visible in the workbench (and in the serial log, which the device gets
      * through the framed error event).
      */
     if(turn.transcript||turn.failure){
      const t=now();
      const writes=[];
      if(turn.transcript)writes.push(c.env.DB.prepare('INSERT INTO messages VALUES (?,?,?,?,?,?,?)').bind(crypto.randomUUID(),conversation,device.user_id,'user',turn.transcript.slice(0,10000),'complete',t));
      /* When recognition heard nothing, what we actually sent is the only way
       * to tell "microphone captured silence" from "captured the room but not
       * the speaker". Both look the same from outside. */
      const answer=turn.reply.trim()||(turn.failure?`${turn.failure}（${describeAudio(audio,captureRate)}）`:'');
      if(answer)writes.push(c.env.DB.prepare('INSERT INTO messages VALUES (?,?,?,?,?,?,?)').bind(crypto.randomUUID(),conversation,device.user_id,'assistant',answer.slice(0,10000),turn.reply.trim()?'complete':'failed',t));
      if(turn.transcript)writes.push(c.env.DB.prepare("UPDATE conversations SET title=CASE WHEN title LIKE '设备对话%' AND (SELECT COUNT(*) FROM messages WHERE conversation_id=? AND role='user')<=1 THEN ? ELSE title END,updated_at=? WHERE id=? AND user_id=?").bind(conversation,turn.transcript.slice(0,40),t,conversation,device.user_id));
      else writes.push(c.env.DB.prepare('UPDATE conversations SET updated_at=? WHERE id=? AND user_id=?').bind(t,conversation,device.user_id));
      await c.env.DB.batch(writes);
     }
     await c.env.DB.prepare('UPDATE devices SET last_seen_at=?,status=?,updated_at=? WHERE id=?').bind(now(),'online',now(),device.id).run();
    }catch(error){
     await emit({type:'error',message:error instanceof Error?error.message:'语音对话失败，请重试'});
     await emit({type:'done'});
    }
    closed=true;
    try{controller.close();}catch{/* 客户端已断开 */}
   },
  },{highWaterMark:0});
  return new Response(stream,{headers:{'Content-Type':'application/octet-stream','Cache-Control':'no-store','X-Content-Type-Options':'nosniff','X-Sample-Rate':'16000'}});
 });
}

/** 每台设备固定一条对话，控制台里就能直接看到机器人聊了什么。 */
async function ensureConversation(c:any,device:any):Promise<string>{
 const existing=device.conversation_id as string|null;
 if(existing){
  const owned=await c.env.DB.prepare('SELECT id FROM conversations WHERE id=? AND user_id=?').bind(existing,device.user_id).first();
  if(owned)return existing;
 }
 const id=crypto.randomUUID(),t=now();
 await c.env.DB.prepare('INSERT INTO conversations VALUES (?,?,?,?,?,?)').bind(id,device.user_id,device.role_id,'设备对话 · '+device.name,t,t).run();
 await c.env.DB.prepare('UPDATE devices SET conversation_id=? WHERE id=?').bind(id,device.id).run();
 return id;
}
