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
