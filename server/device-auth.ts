/* 设备凭据校验与设备侧限流。设备通道与语音通道共用同一套，避免两处实现漂移。 */
import {HTTPException} from 'hono/http-exception';
import {hash,normalizeDeviceKey} from './security';

export const DEVICE_KEY_LENGTH = 12;

/* 用函数声明而不是箭头函数常量：调用方依赖它的 never 返回类型做收窄。 */
export function fail(message:string,status:400|401|403|404|409|413|429=400):never{throw new HTTPException(status,{message});}

export async function limit(db:D1Database,key:string,max:number){const t=Date.now(),expires=t+60000;const r=await db.prepare('INSERT INTO rate_limits(key,count,expires_at) VALUES (?,1,?) ON CONFLICT(key) DO UPDATE SET count=CASE WHEN expires_at<? THEN 1 ELSE count+1 END,expires_at=CASE WHEN expires_at<? THEN ? ELSE expires_at END RETURNING count').bind(key,expires,t,t,expires).first<{count:number}>();if((r?.count||0)>max)fail('操作频繁，请稍后重试',429);}

/**
 * 设备密钥鉴权。限流排在密钥校验**之前**：密钥只有 60 位熵，
 * 先限流才能压住在线枚举；反过来则每次猜测都能直达数据库查询。
 */
export async function deviceAuth(c:any){await limit(c.env.DB,'device-auth:'+(c.req.header('CF-Connecting-IP')||'local'),120);const header=c.req.header('Authorization')||'';const key=header.startsWith('Device ')?header.slice(7).trim():'';if(!key)fail('缺少设备凭据',401);const normalized=normalizeDeviceKey(key);if(normalized.length!==DEVICE_KEY_LENGTH)fail('设备凭据无效或已被吊销',401);const row=await c.env.DB.prepare('SELECT * FROM devices WHERE key_hash=?').bind(await hash(normalized)).first() as any;if(!row)fail('设备凭据无效或已被吊销',401);return row;}
