/*
 * Agent 事件通道（T042 第一期：只通知，不批准）。
 *
 *   POST   /api/agent/events         PC 上的 hook 上报（Authorization: Agent <key>）
 *                                    → 事件落库 + 给该用户每台设备排一条 speak 播报
 *   GET    /api/agent/events         控制台 / 手机看板读（会话鉴权）
 *   POST   /api/agent/events/:id/ack 标记已处理
 *   POST   /api/agent/keys           生成上报凭据（明文只在创建时返回一次）
 *   GET    /api/agent/keys           列出凭据（只有掩码）
 *   DELETE /api/agent/keys/:id       吊销
 *
 * 只支持三种事件：需要确认 / 失败 / 结束 —— 正常进度不打扰。
 */
import {hash,maskKey,randomToken} from './security';
import {fail} from './device-auth';

const now=()=>new Date().toISOString();
const KINDS=['needs_confirmation','failed','finished'];

/** 播报文案：必须带上"哪个软件"，不然不在电脑前根本听不懂。 */
export function announcementText(source:string,kind:string,title:string){
  const suffix=title?`：${title}`:'';
  const text=kind==='needs_confirmation'?`${source} 需要你确认${suffix}`
    :kind==='failed'?`${source} 执行失败${suffix}`
    :`${source} 的任务完成了${suffix}`;
  /* 语音念长句会拖时间：机器人只报"一件事"，细节去手机上看。 */
  return text.length>60?`${text.slice(0,59)}…`:text;
}

export function agentRoutes(app:any){
  const text=(value:unknown,label:string,max:number,required=true)=>{
    if(typeof value!=='string'||value.length>max||(required&&!value.trim()))fail(`${label}无效`);
    return value.trim();
  };
  async function agentAuth(c:any){
    const header=c.req.header('Authorization')||'';
    const key=header.startsWith('Agent ')?header.slice(6).trim():'';
    if(!key)fail('缺少上报凭据',401);
    const row=await c.env.DB.prepare('SELECT * FROM agent_keys WHERE key_hash=?').bind(await hash(key)).first() as any;
    if(!row)fail('上报凭据无效或已被吊销',401);
    await c.env.DB.prepare('UPDATE agent_keys SET last_used_at=? WHERE id=?').bind(now(),row.id).run();
    return row;
  }

  app.post('/api/agent/events',async(c:any)=>{
    const key=await agentAuth(c);
    const b=await c.req.json().catch(()=>({}));
    const source=text(b.source,'来源',40);
    const kind=text(b.kind,'事件类型',40);
    if(!KINDS.includes(kind))fail('事件类型不支持');
    const title=text(b.title??'','标题',200,false);
    const detail=text(b.detail??'','细节',1000,false);
    const id=crypto.randomUUID(),t=now();
    /* 先落库：机器人可能离线，播报只是尽力而为，事件本身不能丢。 */
    await c.env.DB.prepare('INSERT INTO agent_events (id,user_id,key_id,source,kind,title,detail,status,announced,created_at) VALUES (?,?,?,?,?,?,?,?,?,?)')
      .bind(id,key.user_id,key.id,source,kind,title||null,detail||null,'open',0,t).run();
    /* 走和控制台"播报"按钮同一条通道：device_commands 里排一条 speak。 */
    const devices=(await c.env.DB.prepare('SELECT id FROM devices WHERE user_id=?').bind(key.user_id).all()).results as any[];
    const say=announcementText(source,kind,title);
    let announced=0;
    for(const device of devices){
      await c.env.DB.prepare('INSERT INTO device_commands (id,device_id,user_id,type,payload,status,result,created_at,updated_at,acked_at) VALUES (?,?,?,?,?,?,?,?,?,?)')
        .bind(crypto.randomUUID(),device.id,key.user_id,'speak',JSON.stringify({text:say}),'pending',null,t,t,null).run();
      announced+=1;
    }
    if(announced)await c.env.DB.prepare('UPDATE agent_events SET announced=1 WHERE id=?').bind(id).run();
    return c.json({event:{id,source,kind,title:title||null,status:'open',created_at:t},announced,say},201);
  });

  app.get('/api/agent/events',async(c:any)=>{
    const onlyOpen=c.req.query('status')==='open';
    const rows=(await c.env.DB.prepare(`SELECT id,source,kind,title,detail,status,announced,created_at,acked_at FROM agent_events WHERE user_id=?${onlyOpen?" AND status='open'":''} ORDER BY created_at DESC LIMIT 50`).bind(c.get('user').id).all()).results;
    return c.json({events:rows});
  });

  app.post('/api/agent/events/:id/ack',async(c:any)=>{
    const r=await c.env.DB.prepare("UPDATE agent_events SET status='acked',acked_at=? WHERE id=? AND user_id=? AND status='open'").bind(now(),c.req.param('id'),c.get('user').id).run();
    if(!r.meta.changes)fail('事件不存在或已处理',404);
    return c.json({ok:true});
  });

  app.get('/api/agent/keys',async(c:any)=>{
    const rows=(await c.env.DB.prepare('SELECT id,name,key_mask,created_at,last_used_at FROM agent_keys WHERE user_id=? ORDER BY created_at DESC').bind(c.get('user').id).all()).results;
    return c.json({keys:rows});
  });

  app.post('/api/agent/keys',async(c:any)=>{
    const b=await c.req.json().catch(()=>({}));
    const name=text(b.name??'','名称',40,false)||'PC 上的 Codex';
    const key=randomToken(),id=crypto.randomUUID(),t=now();
    await c.env.DB.prepare('INSERT INTO agent_keys (id,user_id,name,key_hash,key_mask,created_at,last_used_at) VALUES (?,?,?,?,?,?,?)')
      .bind(id,c.get('user').id,name,await hash(key),maskKey(key),t,null).run();
    /* 明文只在这里返回一次；库里只有哈希与掩码。 */
    return c.json({key:{id,name,key_mask:maskKey(key),created_at:t},secret:key},201);
  });

  app.delete('/api/agent/keys/:id',async(c:any)=>{
    const r=await c.env.DB.prepare('DELETE FROM agent_keys WHERE id=? AND user_id=?').bind(c.req.param('id'),c.get('user').id).run();
    if(!r.meta.changes)fail('凭据不存在',404);
    return c.json({ok:true});
  });
}
