# AIDesk 机器人控制台 TECH_STACK

状态：已确认；最后更新：2026-10-13。用户已明确批准全方案并连续执行 T001–T006，每项验收及文档同步后进入下一项。禁止部署、推送、创建云资源；不得修改 Hardware、Android、MiniProgram、Design。

## 已批准技术
React TypeScript Vite；Hono Cloudflare Workers；D1 SQLite；单根 package.json。
## 环境
Node 24；npm lock；Worker WebCrypto。
## 环境变量
ENCRYPTION_KEY：AES-GCM 32 字节 base64 Worker secret；APP_ENV：development/production。真实密钥不写文档。
## 命令
npm install；npm run setup:local；npm run db:migrate；npm run dev:worker；npm run dev；npm run typecheck；npm test；npm run build；npm audit。2026-10-13 已验证 migrate/typecheck/test/build/audit 通过。Vitest 5.0.4；Wrangler 4.150.0；workers-types 5。
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
