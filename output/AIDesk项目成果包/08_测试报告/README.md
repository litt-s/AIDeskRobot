# AIDesk 测试报告

## 本次结论

2026-10-23，Node 24 环境类型检查、Web 构建、Node 构建通过。完整 Vitest 集共 99 项测试，99 项通过，失败 0 项。原始结果在 test-results.json，按文件明细见下文。此次集成测试访问独立 Node 服务 localhost:8787 与新建 SQLite，不访问线上服务或真实用户数据。

## 分文件结果

- agent.test.ts：15/15 通过。
- device.test.ts：8/8 通过。
- generation.test.ts：7/7 通过。
- integration.test.ts：1/1 通过。
- meeting.test.ts：5/5 通过。
- provider.test.ts：4/4 通过。
- rewind.test.ts：6/6 通过。
- security.test.ts：3/3 通过。
- trust.test.ts：7/7 通过。
- voice-frames.test.ts：11/11 通过。
- voice-turn.test.ts：32/32 通过。

## 覆盖范围

测试包含密码与密钥处理、供应商错误与出站约束、来源策略、生成状态和撤销、设备凭据与指令、语音帧和流水线、会议文本与权限、Agent 通知与批准等。集成用例验证注册、Cookie、账号间模型/角色/会话隔离、掩码、CSRF 拒绝、非法地址拒绝与登出失效。多数上游服务通过 mock 控制分支，不等于真实服务验收。

## 本次界面检查

用后台 Chrome 访问本地真实应用，采集登录、对话、模型、角色、设备、会议、语音和 Agent 界面。桌面 1440×960，另采集手机 390×844。数据由临时演示账号创建，设备经过 HTTP 模拟握手，聊天文本为带说明的示例。截图只能证明页面在指定环境渲染，不证明实物动作或模型质量。

## 历史记录与本次执行的区别

2026-10-14 的项目记录有 ESP32 表情、动作与音量指令真机回报；T043 有实际允许/拒绝的命令级结果。这些作为历史证据引用，本次没有复验。旧文档中 31、61、94 等测试数属于不同阶段，本文以附带 JSON 为准。

## 待验证与已知风险

1. 真机语音、唤醒打断、连续对话、T044 整段上传的最终表现。
2. Android 通知权限、后台省电、开机恢复和整条交互链路。
3. 微信开发者工具与真机 SSE、暂停续写、合法域名配置。
4. 会议真实录音、外部 ASR 和纪要模型链路，自动声纹未实现。
5. Worker 云端新版本、网络可达性与线上迁移，本次未重新部署。
6. CloudBase 容器持久化问题未解决，快照/种子只是过渡方案。
7. 本次未执行 npm audit、负载测试、渗透测试和完整无障碍审计。

## 复现

源码依赖已安装时运行 npm run typecheck、npm run build、npm run build:node。新环境先 npm ci。用新的 DB_PATH、APP_ENV=development、随机 ENCRYPTION_KEY 在 8787 启动 Node 服务，再运行 npm test。不要用真实云数据库作为测试库。启动包用 45715，若复跑 integration.test.ts 需要另以 8787 启动专用实例。
