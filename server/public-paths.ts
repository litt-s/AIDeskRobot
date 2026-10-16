/*
 * 不需要「用户会话」的接口白名单。
 *
 * 这些接口各自在路由内部鉴权：设备通道用设备密钥（`Authorization: Device …`），
 * agent 上报/轮询用上报凭据（`Authorization: Agent …`）。
 *
 * 单独放一个文件是为了能被单元测试覆盖 —— 2026-10-16 就是因为漏了
 * `GET /api/agent/events/:id/decision`，导致 PC 侧 hook 的轮询一直被会话中间件
 * 拦成 401，表现成"手机点了拒绝，PC 收不到"（查了半天才发现）。
 */

export function isSessionFreeRequest(path:string,method:string){
  if(path.startsWith('/api/device/'))return true;
  if(path==='/api/agent/events'&&method==='POST')return true;
  if(method==='GET'&&/^\/api\/agent\/events\/[^/]+\/decision$/.test(path))return true;
  return false;
}
