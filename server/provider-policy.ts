import {validateBaseURL} from './security';

const defaultHosts = ['api.deepseek.com', 'api.openai.com'];
export function providerURL(input: string, configured = '') {
  const base = validateBaseURL(input);
  const host = new URL(base).hostname;
  const allowed = configured ? configured.split(',').map(value => value.trim().toLowerCase()).filter(Boolean) : defaultHosts;
  if (!allowed.includes(host)) throw new Error('该模型服务域名尚未获平台授权，请管理员配置 PROVIDER_ALLOWED_HOSTS');
  return base;
}
