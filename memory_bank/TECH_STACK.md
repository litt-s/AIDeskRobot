# AIDesk 机器人控制台 TECH_STACK

状态：已确认；最后更新：2026-10-15。用户已明确批准全方案并连续执行 T001–T006，每项验收及文档同步后进入下一项。用户 2026-10-15 已授权 T036 新增 Android 工程、T037 新增 PC 桌面工程；Hardware/MiniProgram/Design 仍不改。

## 已批准技术
React TypeScript Vite；Hono Cloudflare Workers；D1 SQLite；单根 package.json。
## 环境
Node 24；npm lock；Worker WebCrypto。
## 环境变量
ENCRYPTION_KEY：AES-GCM 32 字节 base64 Worker secret；APP_ENV：development/production。真实密钥不写文档。
## 命令
npm install；npm run setup:local；npm run db:migrate；npm run dev:worker；npm run dev；npm run typecheck；npm test；npm run build；npm audit。2026-10-13 已验证 migrate/typecheck/test/build/audit 通过。Vitest 5.0.4；Wrangler 4.150.0；workers-types 5。

PC 端命令（T037）：`npm run desktop`（build + build:node + electron）、`npm run desktop:start`（只启动 Electron）、`npm run desktop:pack`（build + build:node + `electron-builder --win --x64 --dir`，产物 `desktop/release/win-unpacked/AIDesk.exe`）。
## 平台约束
仅本地验证，不创建云资源。

新增配置DEV_ALLOWED_ORIGINS（仅开发完整来源列表），PROVIDER_ALLOWED_HOSTS（出站精确域名审批列表）。
## 迁移与依赖变化
migrations 现为 0001_initial、0002_limits、0003_generations、0004_devices；暂停/续写、撤销与设备通道均为应用层实现，未引入新的运行时依赖。0003 与 0004 需在本地与远程 D1 分别应用。
## 验证记录
2026-10-14：本地应用 0003 迁移成功；当时 `npm run check`（typecheck、5 文件 18 测试、build）通过。其后新增四项交互改动与 tests/rewind.test.ts，该轮 `npm run check` 因工具分类器临时不可用未复跑，不计为已通过。

## 运行形态（2026-10-15 T034 新增：同一份代码、两种运行时）
现状是**双形态**——不是两套代码，也不是两个仓库：
- **形态 A（现有线上）**：Cloudflare Worker + D1 + ASSETS 绑定。入口 `server/index.ts`，部署见 DEPLOY「上线状态」。国内可达性实测不合格（被点名的主机名模式）。
- **形态 B（T034 新增）**：**Node 容器**（腾讯云 CloudBase 云托管）。入口 `server/node/index.ts`，`Dockerfile` 云端构建。业务代码（`server/**.ts`、`web/**`、`migrations/**`）两边**完全共用**，差异只在三处平台能力：

| 能力 | Worker（形态 A） | Node 容器（形态 B） |
| --- | --- | --- |
| 数据库 | D1（`env.DB`） | `node:sqlite` 经 `server/node/d1-sqlite.ts` 包装成同一个 D1 接口 |
| 语音识别 | `fetch` + `Upgrade` 头拿 WebSocket | `ws` 库（`server/node/asr-ws.ts`），`setTranscriber()` 注入 |
| 静态资源 | ASSETS 绑定（`wrangler.jsonc` 的 `assets`） | 直接读 `dist/`，SPA 回落 `index.html` |

依赖变化：新增 `@hono/node-server`（HTTP 服务）与把 `ws` 从 devDependencies 提到 **dependencies**（运行时需要）；`types/ws.d.ts` 是仓库内的最小类型声明（见 LEARNINGS：ws 的 exports 没有 types 条件，`@types/ws` 在 `moduleResolution: bundler` 下解析不到）。新增命令 `npm run build:node`（esbuild 打包 `server/node/index.ts` → `dist-server/index.mjs`，`--packages=external` 保留 node_modules 依赖）与 `npm run start:node`；`npm run check` 已把 `build:node` 纳入。

约束不变：仍**不得**修改 Android／MiniProgram／Design；机器人既有语音链路（`robot_audio`、`robot_wakeup`）行为不动。形态 B 是**新增**运行形态，形态 A 的线上入口保持可用（回滚路径）。
形态 B 另有三个可选环境变量：`PORT`（默认 80）、`DB_PATH`（默认 `./data/aidesk.sqlite`，**必须挂持久卷**）、`SEED_FILE`（默认 `/app/seed/seed.sql`，只在库为空时执行一次，用于把已有账号/设备/凭据搬进新库）。


## Android 客户端（2026-10-15 T036 新增）
选择**原生 WebView 控制台壳**而不是复制一套业务 UI：网页端 React/Vite 已经是响应式单页应用，同一 origin 的 HttpOnly Cookie、SSE 流、轮询、暂停续写和 SPA 路由都能被 Android WebView 完整承载。Android 只保留三件原生职责：服务器地址持久化（SharedPreferences）、WebView Cookie/LocalStorage 持久化、加载/更换服务器原生入口。

工程基线：Java 17、Android Gradle Plugin 8.7.2、Gradle 8.9、compileSdk 35、minSdk 26、targetSdk 35；无 AndroidX/第三方运行时依赖。包名 `cn.aidesk.console`。为支持局域网 Node 容器，应用声明允许 HTTP 明文；所有密钥仍只存在服务端，Android 只保存用户选择的服务器 URL 和平台 Cookie。

## PC 桌面端（2026-10-15 T037 新增）
选择 **Electron 壳 + 既有 React UI + 既有 Node 后端**，不复制业务代码。devDependencies 新增 `electron@^39.8.10` 与 `electron-builder@^26.15.3`；`package.json.main` 指向 `desktop/main.cjs`。主进程仅做单实例锁、窗口、本机 Node 子进程生命周期、远端/本机双模和最小安全配置（`contextIsolation:true` / `sandbox:true` / `nodeIntegration:false`），业务与 UI 仍由 `web/`、`server/`、`shared/` 提供。
- **本机形态**：`desktop/main.cjs` 用 `ELECTRON_RUN_AS_NODE=1` 启动 `dist-server/index.mjs`，同源加载 `http://127.0.0.1:45713`；数据库 `%APPDATA%\aidesk-console\aidesk.sqlite`，AES-GCM 密钥 `%APPDATA%\aidesk-console\encryption.key`。**业务代码零改动**，迁移/静态资源/D1包装/语音识别路径完全复用 T034。
- **远端形态**：置 `AIDESK_SERVER_URL` 即让窗口加载已有线上服务，与浏览器打开同一地址完全等价；适合把 PC 客户端当浏览器壳用。
- **打包**：`asar:false`，files 只带 `desktop/main.cjs`／`dist`／`dist-server`／`migrations`／`package.json`／`assets/icons/windows/aidesk-icon.ico`，产物 `desktop/release/win-unpacked/AIDesk.exe`。`portable` 单文件目标在当前网络会卡在下载 `winCodeSign`，暂不作为默认产物。
- **约束**：不动 `Android`／`MiniProgram`／`Design`，不改 `robot_audio`／`robot_wakeup`。
同源判定（`server/origins.ts`）在形态 B 下要额外容忍"同主机不同协议"：容器平台把 TLS 终止在网关，应用只看到 `http://`，而浏览器 `Origin` 是 `https://`（详因与实测见 LEARNINGS）。
