const SERVER_KEY = 'aidesk_server';
const TOKEN_KEY = 'aidesk_token';
const DEFAULT_SERVER = 'https://aideskrobot.xin';

function normalizeServer(value) {
  const text = String(value || '').trim().replace(/\/+$/, '');
  if (!/^https?:\/\//.test(text)) throw new Error('服务地址必须以 http:// 或 https:// 开头');
  return text;
}

function getServer() {
  return wx.getStorageSync(SERVER_KEY) || DEFAULT_SERVER;
}

function setServer(value) {
  const normalized = normalizeServer(value);
  wx.setStorageSync(SERVER_KEY, normalized);
  return normalized;
}

function getToken() {
  return wx.getStorageSync(TOKEN_KEY) || '';
}

function setToken(token) {
  wx.setStorageSync(TOKEN_KEY, String(token || ''));
}

function clearToken() {
  wx.removeStorageSync(TOKEN_KEY);
}

function headers(withJson = false) {
  const token = getToken();
  const result = {
    'X-Aidesk-Client': 'miniprogram'
  };
  if (token) result.Authorization = 'Bearer ' + token;
  if (withJson) result['Content-Type'] = 'application/json';
  return result;
}

function request(path, method = 'GET', data = undefined) {
  const server = getServer().replace(/\/+$/, '');
  return new Promise((resolve, reject) => {
    wx.request({
      url: server + '/api' + path,
      method,
      data,
      header: headers(method !== 'GET'),
      success(res) {
        if (res.statusCode >= 200 && res.statusCode < 300) {
          resolve(res.data);
          return;
        }
        if (res.statusCode === 401) clearToken();
        reject(new Error((res.data && res.data.error) || '请求失败'));
      },
      fail(error) {
        reject(new Error(error.errMsg || '网络请求失败'));
      }
    });
  });
}

function makeUtf8Decoder() {
  if (typeof TextDecoder === 'function') return new TextDecoder('utf-8');
  let pending = [];
  return {
    decode(chunk) {
      const bytes = new Uint8Array(chunk);
      pending = pending.concat(Array.from(bytes));
      let out = '';
      let i = 0;
      while (i < pending.length) {
        const b = pending[i];
        let codePoint = 0;
        let extra = 0;
        if (b < 0x80) { codePoint = b; }
        else if (b >= 0xc0 && b < 0xe0) { codePoint = b & 0x1f; extra = 1; }
        else if (b >= 0xe0 && b < 0xf0) { codePoint = b & 0x0f; extra = 2; }
        else if (b >= 0xf0 && b < 0xf8) { codePoint = b & 0x07; extra = 3; }
        else { i += 1; continue; }
        if (i + extra >= pending.length) break;
        let valid = true;
        for (let n = 1; n <= extra; n += 1) {
          if ((pending[i + n] & 0xc0) !== 0x80) valid = false;
        }
        if (!valid) { i += 1; continue; }
        for (let n = 1; n <= extra; n += 1) codePoint = (codePoint << 6) | (pending[i + n] & 0x3f);
        out += String.fromCodePoint(codePoint);
        i += extra + 1;
      }
      pending = pending.slice(i);
      return out;
    }
  };
}

function stream(path, data, handlers = {}) {
  const server = getServer().replace(/\/+$/, '');
  const decoder = makeUtf8Decoder();
  let buffer = '';
  const task = wx.request({
    url: server + '/api' + path,
    method: 'POST',
    data,
    enableChunked: true,
    header: Object.assign(headers(true), { Accept: 'text/event-stream' }),
    success(res) {
      if (res.statusCode >= 200 && res.statusCode < 300) {
        if (handlers.done) handlers.done();
        return;
      }
      /* 带上服务端给的原因：只显示状态码时，400/409 分不清是'格式不对'还是'有一轮回复正在生成'。 */
      const reason = res.data && res.data.error;
      const error = new Error(reason || ('请求失败：' + res.statusCode));
      if (handlers.error) handlers.error(error);
    },
    fail(error) {
      if (handlers.error) handlers.error(new Error(error.errMsg || '网络请求失败'));
    }
  });

  if (task && task.onChunkReceived) {
    task.onChunkReceived(res => {
      buffer += decoder.decode(res.arrayBuffer || '');
      const lines = buffer.split(/\r\n|\n/);
      buffer = lines.pop() || '';
      lines.forEach(line => {
        if (!line.startsWith('data:')) return;
        try {
          const event = JSON.parse(line.slice(5).trim());
          if (handlers.event) handlers.event(event);
        } catch (_) {
          // 忽略半包和心跳行
        }
      });
    });
  }

  return task;
}

module.exports = {
  request,
  stream,
  getServer,
  setServer,
  getToken,
  setToken,
  clearToken,
  DEFAULT_SERVER
};
