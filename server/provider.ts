export type ProviderFailureKind = 'http' | 'timeout' | 'network' | 'protocol';
export class ProviderError extends Error {
  constructor(public kind: ProviderFailureKind, public status?: number, message = '模型请求失败') {
    super(message);
    this.name = 'ProviderError';
  }
}

function safeText(value: unknown, secret: string) {
  if (typeof value !== 'string') return '';
  const redacted = secret ? value.replaceAll(secret, '[已隐藏]') : value;
  return redacted.replace(/Bearer\s+[^\s,;]+/gi, 'Bearer [已隐藏]').replace(/[\r\n]+/g, ' ').slice(0, 240).trim();
}

function providerMessage(status: number, detail: string) {
  const prefix = status === 401 || status === 403 ? '认证失败，请检查 API key' :
    status === 402 ? '账户余额或额度不足' :
    status === 404 ? '服务地址或模型不存在，请检查基础地址和模型 ID' :
    status === 408 ? '模型服务请求超时' :
    status === 409 ? '模型服务拒绝了当前请求' :
    status === 429 ? '模型服务请求过于频繁或额度受限' :
    status >= 500 ? '模型服务暂时不可用' : `模型服务返回 HTTP ${status}`;
  return detail ? `${prefix}（HTTP ${status}）：${detail}` : `${prefix}（HTTP ${status}）`;
}

export async function providerError(response: Response, secret: string) {
  let detail = '';
  try {
    const body: unknown = await response.clone().json();
    if (body && typeof body === 'object') {
      const record = body as Record<string, unknown>;
      const error = record.error;
      if (typeof error === 'string') detail = error;
      else if (error && typeof error === 'object') {
        const item = error as Record<string, unknown>;
        detail = [item.message, item.code, item.type].filter(v => typeof v === 'string').join(' · ');
      }
      if (!detail) detail = [record.message, record.code].filter(v => typeof v === 'string').join(' · ');
    }
  } catch { /* Some providers return HTML or an empty body. */ }
  return new ProviderError('http', response.status, providerMessage(response.status, safeText(detail, secret)));
}

function runtimeDetail(error: unknown, secret: string) {
  if (!error || typeof error !== 'object') return '';
  const value = error as {name?: unknown; message?: unknown; cause?: {code?: unknown; message?: unknown}};
  const name = typeof value.name === 'string' ? value.name : '';
  const code = value.cause && typeof value.cause.code === 'string' ? value.cause.code : '';
  const message = typeof value.message === 'string' ? value.message : '';
  const detail = [name, code, message].filter(Boolean).join(': ');
  return safeText(detail, secret).slice(0, 160);
}

export async function fetchProvider(input: RequestInfo | URL, init: RequestInit, secret: string) {
  let response: Response;
  try {
    response = await fetch(input, {...init, redirect: 'manual'});
  } catch (error) {
    const detail = runtimeDetail(error, secret);
    const runtimeHint = error instanceof Error && error.message.includes('Invalid redirect value') ? 'Worker 请求选项不兼容' : '';
    const safeDetail = runtimeHint || detail;
    if (error instanceof DOMException && error.name === 'TimeoutError') {
      throw new ProviderError('timeout', undefined, `连接模型服务超时，请检查地址或稍后重试${safeDetail ? `（${safeDetail}）` : ''}`);
    }
    throw new ProviderError('network', undefined, `无法连接模型服务，请检查基础地址、网络或 TLS 配置${safeDetail ? `（${safeDetail}）` : ''}`);
  }
  if (response.status >= 300 && response.status < 400) {
    throw new ProviderError('http', response.status, '模型接口发生重定向，请填写最终 API 基础地址；未向重定向地址发送密钥');
  }
  if (!response.ok) throw await providerError(response, secret);
  return response;
}

export function providerResponse(error: unknown, fallback = '模型请求失败，请检查连接配置') {
  if (error instanceof ProviderError) return { error: error.message, kind: error.kind, ...(error.status ? { status: error.status } : {}) };
  return { error: fallback, kind: 'network' as const };
}
