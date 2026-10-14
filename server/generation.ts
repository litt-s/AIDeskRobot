import {fetchProvider,providerResponse} from './provider';
import {decrypt} from './security';
import {providerURL} from './provider-policy';

export type GenerationRow={conversation_id:string;user_id:string;assistant_id:string;token:string;state:'running'|'paused'|'completed'|'failed';context_json:string;partial:string;lease_until:number;updated_at:string;error:string|null};
export const LEASE_MS=15000;
const time=()=>new Date().toISOString();
export async function reconcile(db:D1Database,id:string,uid:string){
 await db.prepare("UPDATE chat_generations SET state='paused',error='连接已失联，可基于已保存内容继续生成',updated_at=? WHERE conversation_id=? AND user_id=? AND state='running' AND lease_until<?").bind(time(),id,uid,Date.now()).run();
 await db.prepare("UPDATE messages SET content=(SELECT partial FROM chat_generations WHERE conversation_id=? AND user_id=?),status='interrupted' WHERE conversation_id=? AND user_id=? AND id=(SELECT assistant_id FROM chat_generations WHERE conversation_id=? AND user_id=? AND state IN ('paused','failed'))").bind(id,uid,id,uid,id,uid).run();
 return db.prepare('SELECT * FROM chat_generations WHERE conversation_id=? AND user_id=?').bind(id,uid).first<GenerationRow>();
}
export function publicGeneration(g:GenerationRow|null){return g?{assistant_id:g.assistant_id,state:g.state,updated_at:g.updated_at,error:g.error}:null;}
// Each delta is committed before it is exposed. A retired token cannot write or release a new run.
export async function checkpoint(db:D1Database,g:GenerationRow,partial:string,state:GenerationRow['state']='running',error:string|null=null){
 const r=await db.batch([
 db.prepare("UPDATE chat_generations SET partial=?,state=?,lease_until=?,updated_at=?,error=? WHERE conversation_id=? AND user_id=? AND token=? AND state='running'").bind(partial,state,Date.now()+LEASE_MS,time(),error,g.conversation_id,g.user_id,g.token),
 db.prepare("UPDATE messages SET content=(SELECT partial FROM chat_generations WHERE conversation_id=? AND user_id=? AND token=?),status=(SELECT CASE state WHEN 'completed' THEN 'complete' WHEN 'running' THEN 'generating' ELSE 'interrupted' END FROM chat_generations WHERE conversation_id=? AND user_id=? AND token=?) WHERE id=? AND user_id=? AND EXISTS(SELECT 1 FROM chat_generations WHERE conversation_id=? AND user_id=? AND token=?)").bind(g.conversation_id,g.user_id,g.token,g.conversation_id,g.user_id,g.token,g.assistant_id,g.user_id,g.conversation_id,g.user_id,g.token),
 ]);return !!r[0].meta.changes;
}
export class SSELines{
 private pending='';
 feed(text:string,final=false){this.pending+=text;const lines=this.pending.split(/\r\n|\n|\r/);this.pending=lines.pop()||'';if(final&&this.pending){lines.push(this.pending);this.pending='';}return lines.filter(x=>x.startsWith('data:')).map(x=>x.slice(5).trim()).filter(Boolean);}
}
export function generationStream(db:D1Database,g:GenerationRow,config:any,env:{ENCRYPTION_KEY:string;PROVIDER_ALLOWED_HOSTS?:string},requestSignal:AbortSignal){
 const abort=new AbortController(),encoder=new TextEncoder(),decoder=new TextDecoder(),parser=new SSELines();
 let reader:ReadableStreamDefaultReader<Uint8Array>|undefined,answer=g.partial,stopped=false,finished=false,started=false,success=false;
 const queue:any[]=[];
 let writes:Promise<unknown>=Promise.resolve();
 function persist(partial:string,state:GenerationRow['state']='running',error:string|null=null){const write=writes.then(async()=>{const saved=await checkpoint(db,g,partial,state,error);if(saved)answer=partial;return saved;});writes=write.catch(()=>{});return write;}
 const pause=async()=>{stopped=true;abort.abort();void reader?.cancel().catch(()=>{});await writes;await persist(answer,'paused');};
 if(requestSignal.aborted)stopped=true;
 async function controlled<T>(operation:Promise<T>):Promise<T>{
  let active=true,timer:ReturnType<typeof setTimeout>|undefined;
  const watch=new Promise<never>((_,reject)=>{const poll=async()=>{try{const current=await db.prepare('SELECT token,state FROM chat_generations WHERE conversation_id=? AND user_id=?').bind(g.conversation_id,g.user_id).first<{token:string;state:string}>();if(stopped||!current||current.token!==g.token||current.state!=='running'){stopped=true;abort.abort();void reader?.cancel().catch(()=>{});reject(new Error('paused'));return;}await db.prepare("UPDATE chat_generations SET lease_until=? WHERE conversation_id=? AND user_id=? AND token=? AND state='running'").bind(Date.now()+LEASE_MS,g.conversation_id,g.user_id,g.token).run();if(active)timer=setTimeout(poll,200);}catch(e){reject(e);}};timer=setTimeout(poll,200);});
  try{return await Promise.race([operation,watch]);}finally{active=false;if(timer)clearTimeout(timer);}
 }
 async function finish(state:GenerationRow['state'],error:string|null=null){if(finished)return;finished=true;abort.abort();void reader?.cancel().catch(()=>{});await writes;await persist(answer,state,error);requestSignal.removeEventListener('abort',onAbort);}
 function onAbort(){void pause();}
 requestSignal.removeEventListener('abort',onAbort);requestSignal.addEventListener('abort',onAbort,{once:true});
 return new ReadableStream<Uint8Array>({
 async pull(controller){try{
  if(!started){started=true;controller.enqueue(encoder.encode('data: '+JSON.stringify({generation:publicGeneration(g)})+'\n\n'));return;}
  if(stopped){await finish('paused');controller.close();return;}
  if(!reader){const current=await db.prepare('SELECT token,state FROM chat_generations WHERE conversation_id=? AND user_id=?').bind(g.conversation_id,g.user_id).first<{token:string;state:string}>();if(!current||current.token!==g.token||current.state!=='running'){stopped=true;await finish('paused');controller.close();return;}const secret=await decrypt(config.key_cipher,env.ENCRYPTION_KEY);const context=JSON.parse(g.context_json);const messages=g.partial?[...context,{role:'assistant',content:g.partial},{role:'user',content:'请从上一条助手回复的末尾继续，不要重复已有内容。'}]:context;
   const response=await controlled(fetchProvider(providerURL(config.base_url,env.PROVIDER_ALLOWED_HOSTS)+'/chat/completions',{method:'POST',redirect:'manual',signal:AbortSignal.any([abort.signal,AbortSignal.timeout(120000)]),headers:{'Content-Type':'application/json',Authorization:'Bearer '+secret},body:JSON.stringify({model:config.model,stream:true,messages})},secret));if(!response.body)throw new Error('empty body');reader=response.body.getReader();
  }
  while(!queue.length){const {done,value}=await controlled(reader.read());const lines=parser.feed(done?decoder.decode():decoder.decode(value,{stream:true}),done);for(const raw of lines){if(raw==='[DONE]'){success=true;continue;}const data=JSON.parse(raw);if(data.error)throw new Error('provider error');const delta=data.choices?.[0]?.delta?.content;if(typeof delta==='string'&&delta)queue.push({delta});if(data.choices?.[0]?.finish_reason)success=true;}if(success||done){if(!success)throw new Error('incomplete stream');queue.push({done:true});break;}}
  const event=queue.shift();if(event.delta){const next=answer+event.delta;if(!await persist(next)){stopped=true;await finish('paused');controller.close();return;}answer=next;if(stopped){await finish('paused');try{controller.close();}catch{}return;}}else if(event.done){await finish('completed');}
  controller.enqueue(encoder.encode('data: '+JSON.stringify(event)+'\n\n'));if(event.done)controller.close();
 }catch(error){const detail=stopped?'已暂停，可基于已保存内容继续生成':providerResponse(error).error;await finish(stopped?'paused':'failed',detail);if(!stopped)controller.enqueue(encoder.encode('data: '+JSON.stringify({error:detail})+'\n\n'));controller.close();}},
 async cancel(){await pause();await finish('paused');}
 },{highWaterMark:0});
}
