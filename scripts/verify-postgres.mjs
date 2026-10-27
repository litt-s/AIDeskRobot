import {readFileSync,writeFileSync} from 'node:fs';
import {createHash, randomBytes} from 'node:crypto';
import {DatabaseSync} from 'node:sqlite';
import {createPostgresD1} from '../server/node/d1-postgres.ts';
import {decrypt} from '../server/security.ts';

const origin = 'https://aidesk-console-312359-12-1485927190.sh.run.tcloudbase.com';
const db = createPostgresD1({envId: 'my-base01-d0gacogbi5e4deac3', apiKey: readFileSync('data/postgres-api-key.txt','utf8').trim()});
const mode = process.argv[2] || '--prepare';
const stateFile = 'data/postgres-smoke-session.json';
function assert(value, message) {if (!value) throw new Error(message);}
async function call(path, method='GET', body, cookie='') {
  const response = await fetch(origin+'/api'+path, {method, headers: {Origin: origin, 'Content-Type': 'application/json', ...(cookie ? {Cookie:cookie} : {})}, body: body === undefined ? undefined : JSON.stringify(body), signal:AbortSignal.timeout(60000)});
  const data = await response.json();
  return {response, data, cookie: response.headers.get('set-cookie')?.split(';')[0]};
}
if (mode === '--existing') {
  const source=new DatabaseSync('data/final-postgres-20261027.sqlite',{readOnly:true});
  const owner=source.prepare('SELECT id FROM users ORDER BY rowid LIMIT 1').get();
  assert(owner,'源快照无用户');
  const token=randomBytes(32).toString('base64url');
  const hash=createHash('sha256').update(token).digest('base64');
  const time=new Date().toISOString();
  await db.prepare('INSERT INTO sessions VALUES (?,?,?,?)').bind(hash,owner.id,new Date(Date.now()+300000).toISOString(),time).run();
  try {
    const counts={};
    for(const table of ['models','roles','devices','conversations']) {
      const expected=source.prepare('SELECT COUNT(*) AS n FROM '+table+' WHERE user_id=?').get(owner.id).n;
      const response=await call('/'+table,'GET',undefined,'session='+token);
      assert(response.response.status===200&&response.data[table].length===expected,table+' owner read failed');
      counts[table]=expected;
    }
    const before=JSON.parse(readFileSync('data/postgres-cloudrun-before.json','utf8')).data.ServerConfig;
    const encryptionKey=JSON.parse(before.EnvParams).ENCRYPTION_KEY;
    for(const model of (await db.prepare('SELECT key_cipher FROM models WHERE user_id=?').bind(owner.id).all()).results) assert((await decrypt(model.key_cipher,encryptionKey)).length>0,'model decryption failed');
    for(const voice of (await db.prepare('SELECT api_key_cipher FROM voice_settings WHERE user_id=?').bind(owner.id).all()).results) assert((await decrypt(voice.api_key_cipher,encryptionKey)).length>0,'voice decryption failed');
    console.log('原账号资源读取通过：'+JSON.stringify(counts)+'；模型及语音凭据可用原加密密钥解密');
  } finally {
    await db.prepare('DELETE FROM sessions WHERE token_hash=? AND user_id=?').bind(hash,owner.id).run();
    source.close();
  }
} else if (mode === '--prepare') {
  const health = await call('/health'); assert(health.response.status===200,'health failed');
  const guest = await call('/auth/me'); assert(guest.response.status===401,'guest auth failed');
  const username = 'pgcheck_'+randomBytes(6).toString('hex');
  const password = randomBytes(24).toString('hex');
  const registration = await call('/auth/register','POST',{username,password});
  assert(registration.response.status===201,'registration failed: '+registration.response.status);
  const state = {username,password,userId:registration.data.user.id,cookie:registration.cookie};
  writeFileSync(stateFile,JSON.stringify(state));
  const model = await call('/models','POST',{name:'PostgreSQL persistence verification',baseURL:'https://api.openai.com/v1',model:'fixture',apiKey:'fixture-not-real-key'},state.cookie);
  assert(model.response.status===201,'model creation failed: '+model.response.status);
  state.modelId=model.data.model.id; writeFileSync(stateFile,JSON.stringify(state));
  const remote = await db.prepare('SELECT id FROM models WHERE id=? AND user_id=?').bind(state.modelId,state.userId).first();
  assert(remote?.id===state.modelId,'HTTP write did not reach PostgreSQL');
  const duplicate=await call('/auth/register','POST',{username,password});
  assert(duplicate.response.status===409,'duplicate registration contract failed');
  console.log('线上 health=200、未登录=401、注册=201、重复用户名=409；HTTP 新建模型已在 PostgreSQL 读回');
} else {
  const state=JSON.parse(readFileSync(stateFile,'utf8'));
  assert(state.username.startsWith('pgcheck_'),'不是本次测试账号');
  if(mode==='--verify') {
    const login=await call('/auth/login','POST',{username:state.username,password:state.password});
    assert(login.response.status===200,'restart login failed: '+login.response.status);
    const models=await call('/models','GET',undefined,login.cookie);
    assert(models.response.status===200&&models.data.models.some(model=>model.id===state.modelId),'restart persistence failed');
    const me=await call('/auth/me','GET',undefined,state.cookie);
    assert(me.response.status===200,'session lost after restart');
    console.log('新实例登录=200；重启前的会话及模型标记均保留');
  } else if(mode==='--cleanup') {
    const user=await db.prepare('SELECT username FROM users WHERE id=?').bind(state.userId).first();
    assert(!user||user.username===state.username,'测试账号不匹配');
    await db.prepare('DELETE FROM users WHERE id=? AND username=?').bind(state.userId,state.username).run();
    console.log('仅本次隔离测试账号及级联记录已清理');
  } else throw new Error('未知验证模式');
}
