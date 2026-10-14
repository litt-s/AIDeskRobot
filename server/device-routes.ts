import {hash,generateDeviceKey,formatDeviceKey,maskDeviceKey} from './security';
import {deviceAuth,fail,limit} from './device-auth';
export {deviceAuth} from './device-auth';

const now=()=>new Date().toISOString();
const ONLINE_MS=60000;
const COMMAND_TYPES=['emotion','action','volume','speak','listen','reboot'];
const EMOTIONS=['neutral','happy','sad','angry','surprised','thinking','confused','embarrassed','crying','laughing','funny','loving','shocked','cool','relaxed','delicious','kissy','confident','silly','sleepy'];
const ACTIONS=['center','look_left','look_right','look_up','look_down','nod','shake','roll','dance','blink','wakeup','sleep'];

function token(v:unknown,label:string,max=200){if(typeof v!=='string'||!v.trim()||v.length>max)return fail(`${label}无效`);return v.trim();}
function payloadInput(type:string,payload:unknown){
 const input=(payload??{}) as Record<string,unknown>;
 if(type==='emotion'){const name=token(input.name,'表情',40);if(!EMOTIONS.includes(name))fail('表情不在支持列表内');return {name};}
 if(type==='action'){const name=token(input.name,'动作',40);if(!ACTIONS.includes(name))fail('动作不在支持列表内');return {name};}
 if(type==='volume'){const value=Number(input.value);if(!Number.isInteger(value)||value<0||value>100)fail('音量需为0到100的整数');return {value};}
 if(type==='speak')return {text:token(input.text,'文本',200)};
 /* 录音时长由设备端的静音检测决定，这里没有可调参数。 */
 if(type==='listen')return {};
 return {};
}

export function deviceRoutes(app:any){
 const view=(row:any)=>({id:row.id,name:row.name,key_mask:row.key_mask,status:row.status,firmware:row.firmware,battery:row.battery,last_seen_at:row.last_seen_at,role_id:row.role_id??null,conversation_id:row.conversation_id??null,online:!!row.last_seen_at&&Date.now()-Date.parse(row.last_seen_at)<ONLINE_MS});
 async function owned(c:any,id:string){const row=await c.env.DB.prepare('SELECT * FROM devices WHERE id=? AND user_id=?').bind(id,c.get('user').id).first() as any;if(!row)fail('设备不存在',404);return row;}
 async function touch(db:D1Database,id:string,status='online'){await db.prepare('UPDATE devices SET status=?,last_seen_at=?,updated_at=? WHERE id=?').bind(status,now(),now(),id).run();}

 app.get('/api/devices',async(c:any)=>{const rows=(await c.env.DB.prepare('SELECT * FROM devices WHERE user_id=? ORDER BY created_at DESC').bind(c.get('user').id).all()).results as any[];return c.json({devices:rows.map(view)});});
 app.post('/api/devices',async(c:any)=>{const b=await c.req.json(),uid=c.get('user').id,id=crypto.randomUUID(),key=generateDeviceKey(),mask=maskDeviceKey(key),t=now();const name=token(b.name,'设备名称',60);await limit(c.env.DB,'device-create:'+uid,10);
  /* 显式列出列名而不是裸 VALUES：0005 给 devices 加了两列，
   * 裸写法会让插入静默错位，只有跑到运行时才炸。 */
  await c.env.DB.prepare('INSERT INTO devices (id,user_id,name,key_hash,key_mask,firmware,battery,status,last_seen_at,created_at,updated_at) VALUES (?,?,?,?,?,?,?,?,?,?,?)').bind(id,uid,name,await hash(key),mask,null,null,'offline',null,t,t).run();return c.json({device:{id,name,key_mask:mask,status:'offline',online:false,last_seen_at:null,firmware:null,battery:null,role_id:null,conversation_id:null},key:formatDeviceKey(key)},201);});
 app.patch('/api/devices/:id',async(c:any)=>{const row=await owned(c,c.req.param('id')),b=await c.req.json(),uid=row.user_id,name=token(b.name,'设备名称',60);
  /* role_id 决定这台设备用哪个角色说话。换角色时清空会话归属，
   * 否则新角色会续着上一个角色的对话继续聊。 */
  const roleId=b.role_id===null||b.role_id===''?null:(b.role_id===undefined?(row.role_id??null):token(b.role_id,'语音角色',100));
  if(roleId&&!await c.env.DB.prepare('SELECT id FROM roles WHERE id=? AND user_id=?').bind(roleId,uid).first())fail('语音角色不存在',404);
  const conversationId=(row.role_id??null)===roleId?(row.conversation_id??null):null;
  await c.env.DB.prepare('UPDATE devices SET name=?,role_id=?,conversation_id=?,updated_at=? WHERE id=? AND user_id=?').bind(name,roleId,conversationId,now(),row.id,uid).run();
  return c.json({device:view({...row,name,role_id:roleId,conversation_id:conversationId})});});
 app.delete('/api/devices/:id',async(c:any)=>{const r=await c.env.DB.prepare('DELETE FROM devices WHERE id=? AND user_id=?').bind(c.req.param('id'),c.get('user').id).run();if(!r.meta.changes)fail('设备不存在',404);return c.json({ok:true});});
 app.get('/api/devices/:id/commands',async(c:any)=>{const row=await owned(c,c.req.param('id'));const rows=(await c.env.DB.prepare('SELECT id,type,payload,status,result,created_at,acked_at FROM device_commands WHERE device_id=? AND user_id=? ORDER BY created_at DESC,rowid DESC LIMIT 30').bind(row.id,row.user_id).all()).results;return c.json({device:view(row),commands:rows});});
 app.post('/api/devices/:id/commands',async(c:any)=>{const row=await owned(c,c.req.param('id')),b=await c.req.json(),uid=c.get('user').id;await limit(c.env.DB,'device-command:'+uid,60);const type=token(b.type,'指令类型',20);if(!COMMAND_TYPES.includes(type))fail('不支持的指令类型');const payload=payloadInput(type,b.payload);const id=crypto.randomUUID(),t=now();await c.env.DB.prepare('INSERT INTO device_commands VALUES (?,?,?,?,?,?,?,?,?,?)').bind(id,row.id,uid,type,JSON.stringify(payload),'pending',null,t,t,null).run();return c.json({command:{id,type,payload,status:'pending',created_at:t}},201);});

 app.post('/api/device/hello',async(c:any)=>{const d=await deviceAuth(c);await limit(c.env.DB,'device-hello:'+d.id,60);const b=await c.req.json().catch(()=>({}));await c.env.DB.prepare('UPDATE devices SET firmware=?,status=?,last_seen_at=?,updated_at=? WHERE id=?').bind(typeof b.firmware==='string'?b.firmware.slice(0,60):null,'online',now(),now(),d.id).run();return c.json({device:{id:d.id,name:d.name},server_time:now()});});
 app.post('/api/device/status',async(c:any)=>{const d=await deviceAuth(c);await limit(c.env.DB,'device-status:'+d.id,120);const b=await c.req.json().catch(()=>({}));const battery=Number.isInteger(b.battery)&&b.battery>=0&&b.battery<=100?b.battery:null;await c.env.DB.prepare('UPDATE devices SET battery=COALESCE(?,battery),firmware=COALESCE(?,firmware),status=?,last_seen_at=?,updated_at=? WHERE id=?').bind(battery,typeof b.firmware==='string'?b.firmware.slice(0,60):null,'online',now(),now(),d.id).run();return c.json({ok:true});});
 app.get('/api/device/commands',async(c:any)=>{const d=await deviceAuth(c);await limit(c.env.DB,'device-poll:'+d.id,120);await touch(c.env.DB,d.id);const pending=(await c.env.DB.prepare("SELECT id,type,payload,created_at FROM device_commands WHERE device_id=? AND status='pending' ORDER BY created_at,rowid LIMIT 10").bind(d.id).all()).results as any[];if(pending.length)await c.env.DB.batch(pending.map(row=>c.env.DB.prepare("UPDATE device_commands SET status='sent',updated_at=? WHERE id=? AND status='pending'").bind(now(),row.id)));return c.json({commands:pending});});
 app.post('/api/device/commands/:id/ack',async(c:any)=>{const d=await deviceAuth(c);await limit(c.env.DB,'device-ack:'+d.id,120);const b=await c.req.json().catch(()=>({}));const status=b.status==='failed'?'failed':'done';const result=typeof b.result==='string'?b.result.slice(0,300):null;const r=await c.env.DB.prepare("UPDATE device_commands SET status=?,result=?,acked_at=?,updated_at=? WHERE id=? AND device_id=? AND status IN ('pending','sent')").bind(status,result,now(),now(),c.req.param('id'),d.id).run();if(!r.meta.changes)fail('指令不存在或已处理',404);return c.json({ok:true});});
}
