# AIDesk 架构设计

项目名称：AIDesk 基于 ESP32 的办公陪伴机器人及多端控制台

编制日期：2026-10-23。个人信息以选题申报表、开题报告和答辩 PPT 中的填写内容为准。

## 1 分层结构

表现层包含 React Web、Android WebView、Electron 与原生微信小程序。业务层由 Hono 提供统一 /api，管理身份、模型、角色、会话、设备、语音、会议与 Agent 动态。数据和基础设施层包含 D1 或 SQLite、迁移脚本、加密、限流和外部模型及火山语音适配。ESP32 是独立设备终端，通过 robot_cloud 调用设备 API。系统功能图与架构图见本目录 SVG 和 PNG。

## 2 两种运行形态

Worker 入口为 server/index.ts，依赖 D1 和 ASSETS。Node 入口为 server/node/index.ts，使用 node:sqlite 包装业务需要的 D1 接口，用 ws 适配 ASR 连接并读取 dist 静态文件。业务路由不复制。Node 启动时按文件名顺序应用 migrations，以 _migrations 记录执行历史。CloudBase 是该 Node 容器的历史部署目标，不表示当前数据库已经迁入 CloudBase SQL 或 PostgreSQL。

## 3 身份与信任边界

浏览器以 HttpOnly Cookie 会话访问控制台，小程序在特定客户端标识下取得并携带 Bearer token。设备采用 Authorization: Device，PC 上报脚本采用 Authorization: Agent。放行路径只意味着交给专属鉴权，不意味着匿名资源访问。资源查询带 user_id，跨租户资源隐藏为 404。供应商凭据只能在服务端解密，不随公开响应返回。

## 4 对话状态机

会话建立后，服务端原子写入用户消息、助手占位及生成记录。每个 delta 先 checkpoint 再传送。running 可转为 paused、completed 或 failed，resume 对 paused/failed 通过 CAS 换新 token 后追加到原助手消息。lease 失效由 reconcile 收敛，旧 token 的写入无权覆盖新生成。撤销删除最后一轮并回填输入，重新生成保留用户问题并替换旧助手回复。

## 5 设备与语音链路

绑定签发一次性短码，库中保存哈希与掩码。设备 hello 握手、周期上报状态、轮询 commands、执行后 ack。语音上传 16 kHz、16 bit、单声道 PCM，服务端执行 ASR、角色模型生成、逐句 TTS，把 JSON 事件与 PCM 按类型和长度封装返回。整段上传是 T044 后的默认路径，连续对话和唤醒打断存在固件实现与历史记录，本次没有重烧录验证。

## 6 会议与 Agent 链路

server/meeting-routes.ts 与 web/src/MeetingPage.tsx 已有本地代码。会议支持文本导入和短 PCM 分块转写，当前音频请求结束后仅保留转写文本；说话人手动命名不能宣称为自动声纹识别。纪要由获准模型生成，需要真实服务验收。Agent 事件落库后可排队机器人短播报，Android 提醒服务复用网页登录态；批准事件遵循明确决定、一次生效、超时和过期约束。

## 7 持久化与部署风险

本机启动包的 data 目录可持久保存。现有 CloudBase 容器未配置可靠持久盘，快照与 seed 是过渡方案，不能保证发布窗口零丢失。数据持久化迁移尚未完成，旧文档里“方案 B”的评估不能当作已上线架构。本包只包含空库迁移，不包含真实快照或种子。MCP 接入仍是后续规划。

## 8 目录与证据

server 为服务端，web 为 UI，shared 为公开类型，migrations 为结构迁移，tests 为自动验证，scripts 为开发运维，Android、desktop、MiniProgram 为终端入口，Hardware/esp32 为既有硬件工程。08 目录记录本次测试；05 目录给出历史开发摘要与来源索引。
