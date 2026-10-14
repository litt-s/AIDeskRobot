export const bytes=(s:string)=>new TextEncoder().encode(s);
export const b64=(a:Uint8Array)=>btoa(String.fromCharCode(...a));
export const unb64=(s:string)=>Uint8Array.from(atob(s),c=>c.charCodeAt(0));
export async function hash(s:string){return b64(new Uint8Array(await crypto.subtle.digest('SHA-256',bytes(s))));}
export async function passwordHash(password:string,salt:string){const key=await crypto.subtle.importKey('raw',bytes(password),'PBKDF2',false,['deriveBits']);return b64(new Uint8Array(await crypto.subtle.deriveBits({name:'PBKDF2',salt:unb64(salt),iterations:100000,hash:'SHA-256'},key,256)));}
export function randomToken(){return b64(crypto.getRandomValues(new Uint8Array(32))).replaceAll('+','-').replaceAll('/','_').replaceAll('=','');}
export function equal(a:string,b:string){let diff=a.length^b.length;for(let i=0;i<Math.max(a.length,b.length);i++)diff|=(a.charCodeAt(i)||0)^(b.charCodeAt(i)||0);return diff===0;}
export async function aesKey(secret:string){let raw:Uint8Array<ArrayBuffer>;try{raw=unb64(secret);}catch{throw new Error('服务端加密配置无效');}if(raw.length!==32)throw new Error('服务端加密配置无效');return crypto.subtle.importKey('raw',raw,'AES-GCM',false,['encrypt','decrypt']);}
export async function encrypt(value:string,secret:string){const iv=crypto.getRandomValues(new Uint8Array(12));const out=await crypto.subtle.encrypt({name:'AES-GCM',iv},await aesKey(secret),bytes(value));return b64(iv)+'.'+b64(new Uint8Array(out));}
export async function decrypt(value:string,secret:string){const [iv,data]=value.split('.');return new TextDecoder().decode(await crypto.subtle.decrypt({name:'AES-GCM',iv:unb64(iv)},await aesKey(secret),unb64(data)));}
export function validateBaseURL(input:string){let url:URL;try{url=new URL(input);}catch{throw new Error('请输入有效的 HTTPS 服务地址');}const host=url.hostname.toLowerCase();if(url.protocol!=='https:'||url.username||url.password||url.search||url.hash||(url.port&&url.port!=='443'))throw new Error('服务地址必须为 HTTPS，且不能包含用户信息、参数或非标准端口');if(!host.includes('.')||host.endsWith('.')||host.includes(':')||/^[\d.]+$/.test(host)||['localhost','.localhost','.local','.internal','.lan','.home','.test','.invalid','.example'].some(s=>host===s||host.endsWith(s)))throw new Error('不允许本地、私网或 IP 服务地址');return url.href.replace(/\/+$/,'');}
export function maskKey(key:string){return '••••••••'+key.slice(-4);}
/* Crockford base32: no I, L, O or U, so the code survives being read aloud or
 * retyped by hand.  12 symbols carry 60 bits, and the device endpoint is
 * rate-limited per source address. */
const DEVICE_ALPHABET='0123456789ABCDEFGHJKMNPQRSTVWXYZ';
export function generateDeviceKey(){const bytes=crypto.getRandomValues(new Uint8Array(12));let key='';for(const byte of bytes)key+=DEVICE_ALPHABET[byte%32];return key;}
export function normalizeDeviceKey(input:string){return input.toUpperCase().replace(/[^0-9A-Z]/g,'').replace(/[IL]/g,'1').replace(/O/g,'0');}
export function formatDeviceKey(key:string){return key.replace(/(.{4})(?=.)/g,'$1-');}
export function maskDeviceKey(key:string){return '••••-••••-'+key.slice(-4);}
