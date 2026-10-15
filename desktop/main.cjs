const { app, BrowserWindow, Menu, dialog, shell } = require('electron');
const { spawn } = require('node:child_process');
const crypto = require('node:crypto');
const fs = require('node:fs');
const http = require('node:http');
const https = require('node:https');
const path = require('node:path');

const DEFAULT_PORT = 45713;
const ICON_PATH = path.join(projectRoot(), 'assets', 'icons', 'windows', 'aidesk-icon.ico');
let mainWindow = null;
let serverProcess = null;
let serverLogs = [];
let quitting = false;

function logServer(line) {
  const text = String(line || '').replace(/\s+$/, '');
  if (!text) return;
  serverLogs.push(text);
  if (serverLogs.length > 80) serverLogs.shift();
  console.log(`[aidesk-server] ${text}`);
}

function projectRoot() {
  return app.isPackaged ? app.getAppPath() : path.resolve(__dirname, '..');
}

function localPort() {
  const configured = Number(process.env.AIDESK_PORT || DEFAULT_PORT);
  return Number.isInteger(configured) && configured > 0 && configured < 65536 ? configured : DEFAULT_PORT;
}

/*
 * 服务器选择：云端控制台 / 本机独立运行。
 *
 * 之前只有环境变量 AIDESK_SERVER_URL 能切远端，界面上没有任何入口（应用菜单被设成 null），
 * 默认永远是"本机 + 空库"——这就是桌面端登录进不去的原因：账号、角色、模型、设备都在云端那套库里。
 * 现在：首次运行问一次（存 userData/server.json），菜单里随时可切；环境变量仍最高优先级（调试用）。
 */
const DEFAULT_REMOTE_URL = 'https://aidesk-console-312359-12-1485927190.sh.run.tcloudbase.com';
const SERVER_CONFIG_FILE = 'server.json';

function normalizeServerUrl(value) {
  const text = String(value || '').trim();
  if (!text) return '';
  const url = new URL(text);
  if (url.protocol !== 'https:' && url.protocol !== 'http:') {
    throw new Error('服务器地址只支持 http:// 或 https://');
  }
  return url.toString().replace(/\/$/, '');
}

function serverConfigPath() {
  return path.join(app.getPath('userData'), SERVER_CONFIG_FILE);
}

function readServerConfig() {
  try {
    const raw = JSON.parse(fs.readFileSync(serverConfigPath(), 'utf8'));
    if (raw && raw.mode === 'remote') return { mode: 'remote', url: normalizeServerUrl(raw.url || DEFAULT_REMOTE_URL) };
    return { mode: 'local', url: '' };
  } catch {
    return null; // 还没配置过（或文件读坏了）
  }
}

function writeServerConfig(config) {
  const dir = app.getPath('userData');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(serverConfigPath(), JSON.stringify(config, null, 2) + '\n', { encoding: 'utf8', mode: 0o600 });
}

function configuredRemoteUrl() {
  const fromEnv = String(process.env.AIDESK_SERVER_URL || '').trim();
  if (fromEnv) return normalizeServerUrl(fromEnv);
  const saved = readServerConfig();
  return saved && saved.mode === 'remote' ? saved.url || DEFAULT_REMOTE_URL : '';
}

/* 首次运行（且没有环境变量）时问一次：连云端还是本机独立跑。 */
async function chooseServerOnFirstRun() {
  if (String(process.env.AIDESK_SERVER_URL || '').trim()) return null;
  const saved = readServerConfig();
  if (saved) return saved;
  const answer = await dialog.showMessageBox({
    type: 'question',
    title: 'AIDesk 服务器',
    message: '这次要用哪个后端？',
    detail: `云端控制台（推荐）：${DEFAULT_REMOTE_URL}\n用你已有的账号、角色、模型，机器人也在这套数据里。\n\n本机独立运行：在这台电脑上另起一套（空库，需要重新注册）。`,
    buttons: ['连接云端控制台（推荐）', '本机独立运行'],
    defaultId: 0,
    cancelId: 0,
    noLink: true,
  });
  const config = answer.response === 1 ? { mode: 'local', url: '' } : { mode: 'remote', url: DEFAULT_REMOTE_URL };
  writeServerConfig(config);
  return config;
}

/* 切服务器：写完配置就重启应用（本机服务是子进程，重启最干净）。 */
function switchServer(config) {
  writeServerConfig(config);
  app.relaunch();
  app.exit(0);
}

/* 应用菜单：默认隐藏，按 Alt 调出。 */
function buildMenu(active) {
  Menu.setApplicationMenu(Menu.buildFromTemplate([
    {
      label: '服务器',
      submenu: [
        { label: '连接云端控制台', type: 'radio', checked: active.mode === 'remote', click: () => switchServer({ mode: 'remote', url: DEFAULT_REMOTE_URL }) },
        { label: '本机独立运行（空库）', type: 'radio', checked: active.mode === 'local', click: () => switchServer({ mode: 'local', url: '' }) },
        { type: 'separator' },
        { label: `当前：${active.mode === 'remote' ? active.url : '本机 127.0.0.1'}`, enabled: false },
        { label: '切换后会自动重启', enabled: false },
      ],
    },
    { label: '编辑', submenu: [{ role: 'cut' }, { role: 'copy' }, { role: 'paste' }, { role: 'selectAll' }] },
    { label: '视图', submenu: [{ role: 'reload' }, { role: 'forceReload' }, { type: 'separator' }, { role: 'resetZoom' }, { role: 'zoomIn' }, { role: 'zoomOut' }] },
    { label: '窗口', submenu: [{ role: 'minimize' }, { role: 'close' }] },
  ]));
}
function ensureEncryptionKey() {
  const dir = app.getPath('userData');
  const file = path.join(dir, 'encryption.key');
  fs.mkdirSync(dir, { recursive: true });
  let key = '';
  try {
    key = fs.readFileSync(file, 'utf8').trim();
  } catch {}

  if (key) return key;
  key = crypto.randomBytes(32).toString('base64');
  fs.writeFileSync(file, `${key}\n`, { encoding: 'utf8', mode: 0o600 });
  try {
    fs.chmodSync(file, 0o600);
  } catch {}
  return key;
}

function startLocalServer() {
  const root = projectRoot();
  const entry = path.join(root, 'dist-server', 'index.mjs');
  if (!fs.existsSync(entry)) {
    throw new Error(`本地服务入口不存在：${entry}\n请先运行 npm run build:node`);
  }
  const staticDir = path.join(root, 'dist');
  const migrationsDir = path.join(root, 'migrations');
  if (!fs.existsSync(path.join(staticDir, 'index.html'))) {
    throw new Error(`前端产物不存在：${staticDir}\\index.html\n请先运行 npm run build`);
  }
  if (!fs.existsSync(path.join(migrationsDir, '0001_initial.sql'))) {
    throw new Error(`数据库迁移不存在：${migrationsDir}`);
  }

  const dbPath = process.env.DB_PATH || path.join(app.getPath('userData'), 'aidesk.sqlite');
  fs.mkdirSync(path.dirname(dbPath), { recursive: true });
  const encryptionKey = ensureEncryptionKey();
  const port = localPort();

  console.log(`starting local server: ${entry}`);
  console.log(`database: ${dbPath}`);
  console.log(`listening on 127.0.0.1:${port} (local UI); device endpoint uses 0.0.0.0`);

  serverProcess = spawn(process.execPath, [entry], {
    cwd: root,
    env: {
      ...process.env,
      ELECTRON_RUN_AS_NODE: '1',
      APP_ENV: 'production',
      PORT: String(port),
      DB_PATH: dbPath,
      ENCRYPTION_KEY: encryptionKey,
      STATIC_DIR: staticDir,
      MIGRATIONS_DIR: migrationsDir,
      NODE_NO_WARNINGS: '1',
    },
    stdio: ['ignore', 'pipe', 'pipe'],
    windowsHide: true,
  });

  serverProcess.stdout.setEncoding('utf8');
  serverProcess.stderr.setEncoding('utf8');
  serverProcess.stdout.on('data', logServer);
  serverProcess.stderr.on('data', logServer);
  serverProcess.on('exit', (code, signal) => {
    serverProcess = null;
    if (!quitting) {
      const message = `本地服务已退出（code=${code ?? 'null'}, signal=${signal ?? 'null'}）。`;
      logServer(message);
      showError('AIDesk 服务已退出', `${message}\n\n最近日志：\n${serverLogs.join('\n')}`);
      app.quit();
    }
  });

  return `http://127.0.0.1:${port}`;
}

function requestOk(url, timeoutMs = 1000) {
  return new Promise(resolve => {
    const target = new URL(url);
    const request = (target.protocol === 'https:' ? https : http).request(target, { method: 'GET' }, response => {
      response.resume();
      resolve(response.statusCode !== undefined && response.statusCode < 500);
    });
    request.setTimeout(timeoutMs, () => {
      request.destroy(new Error('timeout'));
    });
    request.on('error', () => resolve(false));
    request.end();
  });
}

async function waitUntilReady(url) {
  for (let attempt = 0; attempt < 120; attempt += 1) {
    if (await requestOk(url)) return;
    await new Promise(resolve => setTimeout(resolve, 250));
  }
  throw new Error(`等待本地服务超时：${url}\n\n最近日志：\n${serverLogs.join('\n')}`);
}

function showError(title, detail) {
  dialog.showErrorBox(title, detail);
}

function createWindow(url) {
  const origin = new URL(url).origin;
  mainWindow = new BrowserWindow({
    width: 1380,
    height: 880,
    minWidth: 1024,
    minHeight: 640,
    show: false,
    backgroundColor: '#f3f8fa',
    title: 'AIDesk',
    autoHideMenuBar: true,
    icon: fs.existsSync(ICON_PATH) ? ICON_PATH : undefined,
    webPreferences: {
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
      spellcheck: true,
    },
  });

  mainWindow.setMenuBarVisibility(false);
  const reveal = () => {
    if (mainWindow && !mainWindow.isVisible()) mainWindow.show();
    if (mainWindow) mainWindow.focus();
  };
  mainWindow.once('ready-to-show', reveal);
  mainWindow.webContents.once('did-finish-load', () => setTimeout(reveal, 80));
  mainWindow.webContents.setWindowOpenHandler(({ url: target }) => {
    if (target.startsWith('http://') || target.startsWith('https://')) shell.openExternal(target);
    return { action: 'deny' };
  });
  mainWindow.webContents.on('will-navigate', (event, target) => {
    if (target !== origin && !target.startsWith(`${origin}/`)) event.preventDefault();
  });
  mainWindow.on('closed', () => {
    mainWindow = null;
  });
  mainWindow.loadURL(url);
}

function stopServer() {
  if (!serverProcess) return;
  const child = serverProcess;
  serverProcess = null;
  try {
    child.kill();
  } catch (error) {
    console.error('failed to stop server', error);
  }
}

async function bootstrap() {
  await chooseServerOnFirstRun();
  let remoteUrl = configuredRemoteUrl();
  /* 云端连不上时别只留个白屏：给一次"改用本机"的机会。 */
  if (remoteUrl && !(await requestOk(remoteUrl, 5000))) {
    const answer = dialog.showMessageBoxSync({
      type: 'warning',
      title: 'AIDesk 服务器',
      message: '连不上云端控制台',
      detail: `${remoteUrl}\n\n可以改用本机独立运行（空库），或者检查网络后重试。`,
      buttons: ['重试', '改用本机服务', '退出'],
      defaultId: 0,
      cancelId: 2,
      noLink: true,
    });
    if (answer === 1) {
      writeServerConfig({ mode: 'local', url: '' });
      remoteUrl = '';
    } else if (answer === 2) {
      app.quit();
      return;
    }
  }
  buildMenu(remoteUrl ? { mode: 'remote', url: remoteUrl } : { mode: 'local', url: '' });
  const url = remoteUrl || startLocalServer();
  if (!remoteUrl) await waitUntilReady(url);
  createWindow(url);
}
const gotLock = app.requestSingleInstanceLock();
if (!gotLock) {
  app.quit();
} else {
  app.on('second-instance', () => {
    if (!mainWindow) return;
    if (mainWindow.isMinimized()) mainWindow.restore();
    mainWindow.focus();
  });
  app.whenReady().then(bootstrap).catch(error => {
    console.error(error);
    showError('AIDesk 启动失败', error && error.stack ? error.stack : String(error));
    app.quit();
  });

  app.on('window-all-closed', () => {
    app.quit();
  });

  app.on('before-quit', () => {
    quitting = true;
    stopServer();
  });
}
