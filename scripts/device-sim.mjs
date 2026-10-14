#!/usr/bin/env node
/*
 * Minimal device-side client for the console channel.
 *
 * It speaks exactly the same protocol as
 * Hardware/esp32/components/robot_cloud, so the binding, online status,
 * command delivery and acknowledgement paths can be validated before the
 * firmware is flashed.  It is a test tool: it does not touch real hardware.
 *
 * Usage:
 *   DEVICE_URL=http://127.0.0.1:8787 DEVICE_KEY=XXXX-XXXX-XXXX npm run device:sim
 * or:
 *   npm run device:sim -- http://127.0.0.1:8787 XXXX-XXXX-XXXX
 *
 * Never paste a real key into a shared shell history or a chat log.
 */

const baseUrl = process.env.DEVICE_URL || process.argv[2];
const deviceKey = process.env.DEVICE_KEY || process.argv[3];
const pollIntervalMs = Number(process.env.DEVICE_POLL_MS || 3000);
const statusIntervalMs = Number(process.env.DEVICE_STATUS_MS || 30000);

if (!baseUrl || !deviceKey) {
  console.error('缺少参数：DEVICE_URL 与 DEVICE_KEY（或位置参数 <url> <key>）');
  process.exit(2);
}
if (baseUrl.endsWith('/')) {
  console.error('DEVICE_URL 不要以斜杠结尾');
  process.exit(2);
}

let running = true;
let online = false;

async function call(method, path, body) {
  const response = await fetch(baseUrl + path, {
    method,
    headers: {
      Authorization: 'Device ' + deviceKey,
      'Content-Type': 'application/json',
    },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const text = await response.text();
  let parsed = null;
  try { parsed = text ? JSON.parse(text) : null; } catch { /* non-JSON error page */ }
  if (!response.ok) {
    const detail = parsed && parsed.error ? parsed.error : response.status;
    throw new Error(`${method} ${path} -> ${response.status} ${detail}`);
  }
  return parsed;
}

function describe(command) {
  let payload = {};
  try { payload = JSON.parse(command.payload || '{}'); } catch { /* ignore */ }
  switch (command.type) {
    case 'emotion': return `显示表情 ${payload.name}`;
    case 'action': return `执行动作 ${payload.name}`;
    case 'volume': return `音量设为 ${payload.value}`;
    case 'speak': return `朗读「${payload.text}」`;
    case 'reboot': return '重启（模拟器忽略）';
    default: return `未知指令 ${command.type}`;
  }
}

async function hello() {
  const result = await call('POST', '/api/device/hello', { firmware: 'simulator-1.0.0' });
  online = true;
  console.log(`已连接服务端，设备：${result && result.device ? result.device.name : '(未知)'}`);
}

async function pollOnce() {
  const result = await call('GET', '/api/device/commands');
  const commands = (result && result.commands) || [];
  for (const command of commands) {
    console.log(`收到指令：${describe(command)}`);
    await call('POST', `/api/device/commands/${command.id}/ack`, {
      status: 'done',
      result: '模拟设备已执行',
    });
  }
  if (!online) {
    online = true;
    console.log('设备通道恢复在线');
  }
}

async function reportStatus() {
  await call('POST', '/api/device/status', { firmware: 'simulator-1.0.0', battery: 88 });
}

process.on('SIGINT', () => {
  running = false;
  console.log('收到中断，正在退出…');
});

let failures = 0;
let lastStatusAt = 0;

while (running) {
  try {
    if (!online) {
      await hello();
    }
    await pollOnce();
    failures = 0;
  } catch (error) {
    failures += 1;
    online = false;
    console.error(`请求失败（第 ${failures} 次）：${error.message}`);
    if (failures >= 3) {
      console.error('连续失败，稍后重新握手；请检查设备密钥是否已被吊销');
    }
    const backoff = Math.min(5000 * failures, 30000);
    await new Promise(resolve => setTimeout(resolve, backoff));
    continue;
  }

  if (Date.now() - lastStatusAt >= statusIntervalMs) {
    lastStatusAt = Date.now();
    try { await reportStatus(); } catch { /* reported by the next poll */ }
  }

  await new Promise(resolve => setTimeout(resolve, pollIntervalMs));
}
