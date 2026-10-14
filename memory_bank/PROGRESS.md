# AIDesk 机器人控制台 PROGRESS

状态：已确认；更新2026-10-14。用户批准连续执行；未部署、未推送、未创建云资源。

## 当前摘要
T001–T017 与 T009 已完成（依据见各任务条目）。**T018 语音对话闭环的服务端与网页端已完成并验证**，固件侧音频通道尚未开始。2026-10-14 另完成 T020（配网页改为分区表单，Wi-Fi 与设备码互不影响）与 T021（BOOT 长按进入配网、配网中长按重启），两者均通过编译验证、真机未验证。最近一次 `npm test`（2026-10-14）为 8 文件 61 项全部通过，`npm run typecheck` 与 `vite build` 通过；本地 D1 已应用迁移 0005。Cloudflare 部署、GitHub 推送与云资源创建均未执行。tests/integration.test.ts 需要本地 Worker 在 8787 运行，未运行时会失败，属既有环境依赖。

## 工作记录
### 2026-10-13 — T001
- 完成记忆九文档、AGENTS/CLAUDE、React/Vite/Hono/Wrangler/D1骨架和迁移。
- npm install成功；升级Vitest至5.0.4后npm audit 0漏洞。
- `npm run db:migrate`本地两次迁移成功。
### 2026-10-13 — T002
- 完成注册、登录、退出、`HttpOnly` same-site cookie、hash session token、PBKDF2 salted password、Origin CSRF检查、用户限流。
### 2026-10-13 — T003/T004
- 完成模型CRUD、AES-GCM密钥、mask、URL安全约束和连接测试；角色CRUD及模型租户授权关联。
### 2026-10-13 — T005
- 完成真实 OpenAI-compatible chat completions SSE、消息持久化、会话锁和错误处理；完成中文浅色响应式工作台（360/390/768/1024/1440媒体规则）。
### 2026-10-13 — provider诊断修复
- 新增 `server/provider.ts`：只抽取上游 JSON 的 message/code/type，掩码 key/header，返回 HTTP status，并区分 HTTP、网络、超时和协议错误；测试和聊天统一使用。
- 模型连接按钮新增 per-model testing 状态，finally 清除测试中与失败残留 notice；表单提示 DeepSeek `https://api.deepseek.com`、`deepseek-chat`/`deepseek-reasoner`，不限制其他 provider。
- 最终 `npm run check`：typecheck、8项测试、build 全部通过。- `npm run typecheck`通过；`npm test` 2文件4测试通过；`npm run build`通过；`npm audit` 0漏洞。
- Browser preview Worker/Vite启动成功；真实注册201，移动375px snapshot无明显溢出或遮挡。
- 集成测试覆盖账户隔离、key masking、CSRF、URL、角色/会话授权、登出及密码错误。
## 未验证
真实provider链路已获得用户“跑通了”的反馈；全部供应商、完整流式断线恢复及线上环境仍未验证。Cloudflare远程部署、GitHub推送和云资源均按批准范围未执行。npm 仅提示 esbuild/workerd install scripts 待审核，实际 Wrangler 4.150.0可运行，audit为0。

## 下一阶段进度
用户批准继续T007–T010。新增DEV_ALLOWED_ORIGINS精确开发来源配置；production忽略开发来源。聊天上下文改为最近100条再正序；新增停止生成并取消上游读取，部分回复标记interrupted。provider出站仅允许管理员指定主机（默认api.deepseek.com/api.openai.com），拒绝重定向；不宣称DNS查询防重绑定。原有真实配置未改。npm run check初次11测试通过；最终复跑见本次交付。Browser attach到用户现有服务被工具拒绝（本安装未启用URL附加预览），T009视觉验证未执行。不部署不推送。

### 2026-10-14 — T011 持久暂停与继续生成
- 用户报告：暂停后再点继续出现"此会话正在生成回复"，且刷新后没有生成记录。
- 只读定位：旧实现仅在客户端断开流，无服务端可寻址停止；旧锁 finally 无 token 条件删除；用户消息在 provider 连接成功后才写入，连接前中断会丢历史；历史查询排除 interrupted。
- 实现：migrations/0003_generations.sql 新增 chat_generations；server/generation.ts 提供 token/lease 状态机、逐 delta checkpoint、reconcile 失联收敛；server/chat-routes.ts 提供 /:id/generation、/:id/chat/pause、/:id/chat/resume 与并发准入；新请求以 D1 batch 原子写入 generation+user+assistant。前端接入 generation 状态、暂停/继续按钮、pagehide 尽力暂停、打开会话自动恢复、recent 上下文。
- 已执行验证：`npm run db:migrate` 本地应用 0003 成功；`npm run check` 类型检查通过、5 文件 18 测试通过、Vite build 通过。测试覆盖暂停幂等与跨租户404、即时 resume 换 token 且不追加用户消息、completed 拒绝 resume、cancel 与 checkpoint 竞态不丢已提交片段、旧 token 不能覆盖新生成、失联 reconcile、CRLF/DONE 解析、provider 未连接即取消不调用上游。
- 未验证：真实浏览器下的"暂停→继续→刷新→关闭页面后恢复"整链路（本安装未启用附加预览，且预览端口与用户正在运行的服务冲突）；真实 provider 的暂停续写内容连贯性；Cloudflare 远程 migration 与部署。以上不得记为已通过。

### 2026-10-14 — T012 聊天交互体验
- 用户提出四项体验问题：习惯回车发送、发送与生成时消息区未跟随滚动、续写缺少进行中提示、需要类似 rewind 的撤销上一轮对话。
- 实现：输入框回车发送、Shift+回车换行，并跳过中文输入法组合态；消息区滚动跟随，用户上滑离开底部 80px 内即暂停跟随，滑回底部自动恢复；续写期间在中断处显示"继续生成中"动效；新增服务端 `POST /api/conversations/:id/rewind`，删除最后一条提问及其回复并清空生成状态，把问题文本回填输入框，最后一条提问下方显示"撤销这条对话"按钮。
- 待执行验证：`npm run check`（typecheck／测试／build）在本轮改动后尚未复跑成功——Bash 与终端工具因分类器临时不可用被拦截，非权限拒绝；新增 tests/rewind.test.ts 覆盖撤销、生成中拒绝撤销、空历史与跨租户。浏览器与真实 provider 验收同样未完成。
- 2026-10-14 用户实测反馈 T011、T012 功能可行，两者状态更新为已完成。

### 2026-10-14 — T013 对话布局与重新生成
- 用户要求：提问靠右、AI 回复靠左；在撤销旁新增"重新生成"，按该提问重新生成。
- 实现：样式将用户消息反转为右侧并右对齐文本，AI 回复保持左侧；服务端新增 `POST /api/conversations/:id/regenerate`，确认后删除该轮旧回复、保留原提问、以 rowid 之前的完整消息与系统提示重建上下文并复用 generationStream，不新增用户消息；生成中与跨租户分别返回 409／404。
- 待执行验证：`npm run check` 复跑（工具分类器仍不可用）与用户浏览器验收；tests/rewind.test.ts 已扩展重新生成的三条用例（替换旧回复且保留提问、生成中拒绝、无提问与跨租户）。
- 2026-10-14 用户实测确认提问右置、回复左置与重新生成可用，T013 标记完成。用户并确认下一阶段顺序为：①设备管理 → ②语音对话闭环 → ③会议纪要。

### 2026-10-14 — 设备接入阶段（T014 编码完成，T015 编码完成）
- 背景：用户说明机器人只能语音输入，因此设备侧不做文本对话；同时提出后续要做带声纹的会议纪要，明确会议纪要需要多人转写（说话人分离），声纹用于把说话人编号替换为姓名，属可选第二步。
- T014：新增 migrations/0004_devices.sql（devices、device_commands）与 server/device-routes.ts；控制台走 /api/devices 的会话鉴权，设备走 /api/device/* 的设备密钥鉴权，密钥仅签发时返回一次、库内只存哈希与掩码；指令类型限定 emotion／action／volume／speak／reboot 并校验取值；`/api/device/*` 在来源校验与会话鉴权中间件上按路径放行，改由设备密钥与独立限流保护。新增 tests/device.test.ts 覆盖密钥一次性与掩码、跨账号隔离、解绑后密钥失效、缺省与错误凭据 401、握手置在线、指令下发→轮询→回报闭环、参数校验。
- T015：导航新增"设备"，实现绑定（弹窗内一次性展示密钥）、重命名、解绑、在线状态与固件电量展示，以及表情／动作／音量／播报指令与最近指令记录。
- 未验证：本地迁移 0004 未执行；`npm run check`（typecheck／测试／build）因沙箱分类器故障持续无法运行，T014、T015 均不得记为已验证。T016 固件设备通道、T017 联调、T018 语音闭环、T019 会议纪要均未开始。
- 用户代跑结果（2026-10-14）：`npm test` 实际执行，7 个文件 31 项中 29 项通过；device.test.ts 两项失败，原因为 device_commands 的 INSERT 只写了 9 个占位符而表为 10 列，已修正为 10 个。同轮 rewind(6)、generation(7)、integration(1)、trust(3)、provider(4)、security(3) 全部通过。
- 用户代跑结果（2026-10-14 02:31）：`npm run check` 全绿——typecheck 通过、7 文件 31 项测试全部通过（device 7、rewind 6、generation 7、integration 1、trust 3、provider 4、security 3）、Vite 构建通过。据此 T014 标记完成；T015 编码与技术验证完成，仅剩浏览器视觉验收。
- 待办：T015 浏览器视觉验收（用户确认）；T016 固件设备通道、T017 联调、T018 语音闭环（依赖 ASR／TTS 选型，mosi 文档仍未能读取）、T019 会议纪要。

### 2026-10-14 — T016 固件设备通道（编码完成，待编译与真机验证）
- 新增独立组件 Hardware/esp32/components/robot_cloud：FreeRTOS 任务先 POST /api/device/hello 握手，之后按间隔 GET /api/device/commands、执行后逐条 POST ack，并周期上报固件版本与音量；失败按 5s 步进退避到 30s，连续 3 次失败重新握手。表情与动作各自维护名称映射表（服务端用的 crying／sleepy 等写法在固件侧做了别名兼容）。
- 接入方式保持最小：main/CMakeLists.txt 增加 robot_cloud 依赖；robot_app.h 增加静态回调 CloudSpeak 声明；robot_app.cpp 增加 include、CloudSpeak 定义，并在 SetupOnlineServices 末尾在 CONFIG_ROBOT_CLOUD_ENABLE 打开时初始化并启动通道（默认 n，关闭时既有行为完全不变）。
- 已知限制：播报复用 AIUI 会话，设备待机（会话未连接）时返回"请先唤醒"，不自行建立会话；设备密钥通过 menuconfig 编入固件，尚未接入配网页面；HTTPS 依赖证书 bundle，本地联调可用 http 地址。
- 未验证：未编译（本会话无法运行 idf.py，缺少 ESP-IDF 环境），未烧录真机，未与本地服务端联调；T017 仍待开始。
- 2026-10-14 用户反馈设备密钥 43 位不便手输：改为 12 位 Crockford base32（约 60 位熵，排除易混字符），展示为四位一组，校验前归一化（去短横线、转大写、I/L→1、O→0），控制台增加"复制密钥"按钮。同时补上缺口：设备鉴权此前排在限流之后，短码会放大在线枚举风险，现改为按来源地址先限流再校验。该变更使已绑定的旧设备（43 位密钥）失效，需重新绑定。
- 2026-10-14 短码改造经用户代跑 `npm run check` 通过（typecheck、测试与构建全绿）。
- 2026-10-14 用户实测：手机访问 `http://<电脑IP>:8787/api/health` 返回 ok；机器人首次烧录后进入配网（NVS 无 Wi-Fi 信息，属预期）。据此发现接入位置缺陷：通道原先挂在 SetupOnlineServices 末尾，而该函数在 AIUI／ESP-SR 初始化失败时整体返回 FAILED，等于让"设备管理"依赖语音栈。已改为在 NETWORK_GOT_IP 时启动，仅需 IP 连通；并区分播报失败原因（未配置语音服务 / 待机需唤醒）。仍未编译验证。
- 2026-10-14 新增 scripts/device-sim.mjs 与 `npm run device:sim`：无硬件时以同一协议验证设备通道。尚未实际运行；使用步骤见 DEPLOY。
- 2026-10-14 用户提供硬件原理图，据此核对引脚：舵机信号 H1=GPIO11（水平）、H2=GPIO12（垂直），OLED SCL/SDA=GPIO41/42，麦克风 SCK/WS/SD=GPIO5/4/6，功放 MAX98357 BCLK/LRC/DIN=GPIO15/16/7，按键 GPIO0 —— **与固件现有配置完全一致**，因此排除"固件属于其他板型"的假设。代码路径亦无跳过舵机的分支（NOD 直接调用 head_nod 驱动两个通道，OLED 动画失败不影响）。舵机不动的排查转入硬件侧：供电轨（H1/H2 的 V+ 取自板上 3.3V/5V）、舵机三线方向与共地。
- 2026-10-14 用户曾怀疑固件版本不符（指向 xiaozhi-yuntai）。核查结果：该目录是小智（xiaozhi-esp32）版本，其 main/boards 同样只有 README；build.ninja 显示它引用的板级源码来自另一台机器的 D:/oldxiaozhi/xiaozhi-deskemoji-taibai-master/.../boards/esp32-s3n16r8-emoji/（含 servo_controller.cc），本机不存在，无法据此比对引脚。两个工程选中的板型标识相同（CONFIG_BOARD_TYPE_ESP32_S3N16R8_EMOJI），且用户设备上 OLED 与音频工作正常，说明现有固件与板子在显示/音频/按键上本就匹配。另确认 CONFIG_ROBOT_ENABLE_SERVO=y，舵机代码已编译在内，引脚原为硬编码 11/12。据此把舵机引脚改为可配置（ROBOT_SERVO_X_PIN／ROBOT_SERVO_Y_PIN，默认 11/12）并在启动日志打印，便于用 menuconfig 试引脚。尚未编译验证。
- 2026-10-14 编译烧录成功并在真机验证：日志出现 `device channel online`；表情（happy／sad／surprised）、动作（dance／nod／shake／look_left）与音量指令均被设备执行并正确上报 ack；`speak` 按设计回报"设备未配置语音服务"。据此 T016、T017 标记为已完成（播报待 T018 接入语音后复验）。
- 2026-10-14 舵机不动的根因确认为**供电不足**：仅用烧录口（USB）供电带不动舵机，插上独立电源口后舵机立即正常动作。此前"烧录新固件后舵机动不了"并非版本差异或新增代码所致，而是两次测试的供电条件不同。
- 期间修复的固件问题：①Kconfig 中文菜单名被写入 sdkconfig，触发 esp-sr movemodel.py 以 GBK 解码失败，已全部改为英文；②新增 Kconfig 选项后未重新生成配置，导致 `CONFIG_ROBOT_SERVO_X_PIN` 未声明，已给引脚宏加 `#ifndef` 兜底，并提示执行 `idf.py reconfigure` 让新选项生效。
- 2026-10-14 用户实测设备页在手机／平板／桌面上均正常（绑定、在线状态、指令下发与记录），T015 标记完成。用户同时确认登录、模型与角色表单、弹窗、聊天各状态在各宽度下无遮挡或横向溢出，T009 一并标记完成（未保留逐宽度截图记录）。至此**设备接入阶段 T014–T017 全部完成**，网页端与固件的设备通道已在真机联调通过。
- 下一步：T018 语音对话闭环，前置为确定 ASR／TTS 供应商（mosi 文档因联网工具不可用始终未能读取）；其后为 T019 会议纪要。
- 2026-10-14 语音供应商评估：Moss（ASR 文件转写 + TTS 流式 + 60 分钟说话人分离）、GLM-ASR（文件转写，仅支持 30 秒、支持内联 base64）均**不支持真正的实时流式识别**；火山引擎豆包提供**双向流式 ASR + 双向流式 TTS**，且音频要求为 16 kHz/16 bit/单声道，与机器人采集格式一致。据此选定火山豆包用于 T018，Moss 保留给 T019 会议纪要（说话人分离）。
- 2026-10-14 架构核实（据 Cloudflare 官方文档）：**Workers 支持主动发起出站 WebSocket**，我此前的否定判断有误；Durable Objects 免费版可用（仅 SQLite 后端）。因此实时语音**无需离开 Cloudflare**，也无需购买国内服务器或迁移到 Node；先前提出的 Node 迁移方案已取消。
- 2026-10-14 产出 scripts/volc-voice-smoke.mjs（`npm run volc:smoke`）：实现火山 ASR 二进制帧协议（编码与解析）、WAV 读写、TTS HTTP 分块接收，并支持"TTS 合成 → 包 WAV → ASR 回环"的自检；仅通过 `node --check` 语法检查，**未联网实测**（缺 API Key）。
- 2026-10-14 用户首跑烟测（有 Key）：**ASR 帧协议复刻正确**——服务端返回的结构化错误被正确解析出来（帧头、消息类型、gzip 解码、错误分支均验证）。当前阻塞点是账号设置，错误原文为 `403 Forbidden.AgentPlanDeductNotEnabled: Agent Plan deduction is not enabled for this account or seat. Please enable the deduction setting before calling this endpoint.`，即需在火山控制台为账号/席位开启 Agent Plan 抵扣后才能调用。TTS 侧本次返回 0 字节音频，已修脚本：响应未必是行分隔 JSON（可能是单个 JSON 对象或直接二进制），新增兜底解析，并输出 Content-Type 与响应诊断信息，便于下次定位。
- 待办：用户在火山控制台开启 Agent Plan 抵扣后重跑 `npm run volc:smoke`；跑通再实现 T018 的服务端适配与固件音频通道。
- 2026-10-14 用户二跑烟测：**TTS 已验证可用**——HTTP 分块返回 `text/plain`，成功解析出音频（首块 1920 ms、全部 2624 ms、约 20 字一句共 40 KB，已存 volc-tts-smoke.mp3）。ASR 的 403 已消失（说明抵扣已开启），但返回 `The audio could not be decoded using the declared format`，即送入的音频与声明格式不符；最可能是回环阶段请求 `format=pcm` 时服务端仍返回非 PCM 容器。
- 据此改进烟测脚本（均自动尝试，一次运行定论）：①回环阶段依次请求 pcm／wav／mp3，探测真实返回容器并打印前 8 字节，取第一个可用的裸 PCM 或 WAV；②ASR 声明在 `format=wav` 与 `format=pcm` 之间依次重试；③保留 `VOLC_ASR_FORMAT`／`VOLC_ASR_CODEC`／`VOLC_ASR_STRIP_HEADER`／`VOLC_ASR_WAV` 覆盖项。仍只过语法检查，未再实测。
- 实测延迟记录（用户家网络直连火山，非生产路径）：TTS 首块 1920 ms、整句 2624 ms。这将是"模型出完文本→机器人开口"的量级，加上识别与模型推理，整轮预计 3–5 秒；生产路径（Worker→火山）需另测。

### 2026-10-14 — T018 语音对话闭环（服务端与网页端完成，固件待做）
- 交付内容：migrations/0005_voice.sql（voice_settings 表；devices 增加 role_id 与 conversation_id）；server/voice/frames.ts（火山二进制帧纯编解码）、stream.ts（设备侧 `[类型][长度 4 字节小端][负载]` 分帧）、pipeline.ts（一轮对话的编排 + 增量分句 + `withDeadline` 墙钟兜底）、volc.ts（ASR/TTS 客户端）、routes.ts（语音设置 CRUD 与 `/api/device/voice`）；server/device-auth.ts（设备鉴权与限流从 device-routes 抽出，两条设备通道共用一份实现）；控制台新增「语音设置」页、设备页的语音角色选择与「查看对话」入口。
- 设计要点：机器人只上传一段 PCM、只下载一段带分帧的 PCM，**不需要 WebSocket**，也不需要理解模型协议。模型输出按句切分，一句合成完立刻下发，首字延迟≈识别 + 模型首句 + TTS 首块。凭据按账号加密存储，未配置时回落平台环境变量，表单里标明来源；设备用哪个角色对话由控制台决定，对话落在该设备专属会话，可在工作台直接查看。
- 本轮修的问题：①`POST /api/devices` 的裸 `INSERT ... VALUES` 与 0005 新增的两列对不上（11 对 13），已改为显式列名；②`wss://` 被 Workers 的 Fetch API 拒绝，改用 `https://` + `Upgrade` 头；③ASR 建连阶段缺独立超时，已补 `AbortSignal.timeout`；④`AbortSignal` 打不断卡住的握手，另加 `withDeadline` 保证设备总能收到明确失败帧；⑤控制台语音分支漏写 `tab==='voice'?`，造成三臂三元表达式。
- 已执行的验证（2026-10-14）：`npm run typecheck` 通过；`npm test` 8 文件 61 项通过（新增 tests/voice-frames.test.ts 6 项、tests/voice-turn.test.ts 25 项，其中覆盖分句与流水线顺序、四种失败分支、超时兜底、分帧拆包、设置接口不回传密钥、跨账号隔离、换角色清空会话）；`vite build` 通过；本地 D1 应用 0005 成功；浏览器在 390/768/1440 三档宽度实测语音设置页与设备页无横向溢出（并以 `documentElement.scrollWidth` 复核）；`/api/voice/settings` 的保存、省略密钥只改音色、清除三条路径在页面上实走通过。
- 端到端探针（真机前，用占位密钥）：向 `/api/device/voice` 实发 16 kHz 单声道 PCM，611 ms 收到分帧应答 `{"type":"error","message":"语音识别失败：语音识别建连失败：HTTP 401"}`。这证明设备鉴权 → 角色解析 → 会话建立 → **到火山的 WebSocket 握手** → 分帧下发 → 错误回传整条链路连通；401 由占位密钥导致，符合预期。
- 未验证（不得记为通过）：用真实密钥跑完整一轮（识别→模型→合成→播放）；固件侧录音上传与播放回传 PCM 的编译与真机；生产路径（Worker→火山）的延迟与稳定性。验证用的本地测试账号及其模型/角色/设备已在验证后从本地 D1 删除，未触及用户自有数据。
- 控制台本轮的可见变化：「语音设置」页（凭据、音色、两个资源号，保存/清除，标明凭据来源）、设备页的「语音角色」下拉与「查看对话」按钮、设备指令面板新增「说话」行（触发一次收音）。浏览器实测 375/390/768/1440 宽度均无横向溢出，「和它说句话」按钮点击后生成 `listen` 指令并出现在指令记录中。

### 2026-10-14 — T018 固件音频通道（编码完成，未编译未烧录）
- 新增 Hardware/esp32/components/robot_cloud/robot_voice.c 与 include/robot_voice.h：独立任务等待触发信号量 → 等唤醒提示音播完并解除采集暂停 → 挂麦克风旁路录音（能量 VAD：见到说话后静音满 900 ms 或到 10 s 上限即停）→ POST `/api/device/voice` 上传整段 PCM → 增量解析 `[类型][长度 4 字节小端][负载]` 分帧 → PCM 帧直接 `robot_audio_play_pcm` 边收边播，JSON 事件打印并识别 done/error。
- robot_audio 新增麦克风旁路（tap）槽位：`robot_audio_add_pcm_tap`/`remove_pcm_tap`。此前只有**一个** consumer 槽位，而唤醒词引擎已占用它；若直接注册会顶掉唤醒词，所以改为在既有 consumer 之后追加最多 2 个旁路，主 consumer 的位置与行为不变。
- robot_app 增加 `RobotAppState::CLOUD_VOICE_ACTIVE` 与两个事件：该状态下 PcmConsumer 既不喂唤醒词也不喂 AIUI（录音由旁路完成），因此不必改动既有两条分支。唤醒路径在 `CONFIG_ROBOT_CLOUD_VOICE` 打开时改为触发云端一轮；`robot_voice_trigger()` 失败仍回落到原有 AIUI 路径，保证没配好凭据时机器人照常可用。另有 `robot_voice_busy()` 守卫，避免在控制台已触发的一轮录音期间叠加一次唤醒。
- robot_cloud 新增 `listen` 指令（触发一次收音）与 `robot_cloud_base_url()`／`robot_cloud_device_key()` 访问器，让语音通道复用同一套服务地址与设备密钥，不必配置两遍。服务端 `COMMAND_TYPES` 同步加入 `listen`。
- 新增 Kconfig：`ROBOT_CLOUD_VOICE`（默认 n）、`ROBOT_CLOUD_VOICE_MAX_RECORD_MS`、`ROBOT_CLOUD_VOICE_SILENCE_MS`、`ROBOT_CLOUD_VOICE_THRESHOLD`。菜单与提示名保持 ASCII（见 LEARNINGS 中 GBK 解码那条）。
- 未验证：**未编译、未烧录**。本会话没有 ESP-IDF 环境，无法执行 `idf.py build`；录音缓冲区、VAD 阈值、与唤醒词/AIUI 的状态交替都需要真机确认。已知限制：`listen` 在 AIUI 会话进行中若被触发，两边会同时占麦（需要用户先唤醒再按按钮才会遇到）；BOOT 键在云端一轮进行中不做中断。
- 2026-10-14 首次真机构建暴露三处编译错误，已全部修复：①`robot_voice.c` 用了 `robot_cloud_base_url()`／`robot_cloud_device_key()` 但漏包含 `robot_cloud.h`；②`CLOUD_VOICE_FINISHED` 在 `case` 标签下声明了 `robot_action_t` 而未加花括号，报 `jump to case label crosses initialization`；③同处的 `robot_action_t cloud_neutral` 声明使编译器连带报 `enumeration value 'STOP' not handled in switch`——**这是次生错误**，不是真的漏了分支，加花括号后两条一起消失。
- 修复后改用 `build/compile_commands.json` 里 CMake 生成的原始编译命令做编译校验（同一个交叉编译器、同一套 `-Wall -Werror=all`），`robot_voice.c`、`robot_cloud.c`、`main/robot_app.cpp`、`main/app_main.cpp`、`robot_audio.c` 五个文件均无报错。这仍**不等于构建通过**：链接、内存占用与真机行为都未验证。
- 2026-10-14 第二次构建又暴露一处：`-Werror=format-truncation` 报 `robot_voice.c` 把 256 字节的 HTTP 错误响应体拼进 160 字节的 `last_result`（"output between 23 and 291 bytes into a destination of size 160"）。已改为按是否读到响应体分两条格式串，并用 `%.100s` 限定宽度；同一文件里把上游错误消息写进 `last_result` 的地方也一并改为 `%.120s`。
- 重要教训：首次做校验时我用了 `-fsyntax-only`，而 `-Wformat-truncation` 属于中端优化阶段的警告，只在真正编译时才产生，因此**被漏掉了一整类错误**。改为真实编译到临时对象文件后，五个文件一次通过。校验脚本与临时产物已删除。

### 2026-10-14 — 开发来源校验改为按地址类型判断
- 用户反馈：一直用局域网地址访问控制台，本次是 `http://192.168.137.1:5173/`，登录报"请求来源不受信任"。核查 `.dev.vars` 里 `DEV_ALLOWED_ORIGINS` 登记的是更早的 `http://10.34.20.209:5173`，对不上；这已是第三次因换网络改 IP 而撞上（10.34.20.209 → 10.243.104.162 → 192.168.137.1）。
- 这是一处易误判的体验问题：报错发生在**登录之前**，界面上看不出是来源校验而不是账号问题。
- 改动（server/origins.ts）：development 下不再依赖逐个登记的 IP，改为放行 host 为 loopback 或私有地址字面量（10/8、172.16/12、192.168/16、169.254/16）的 origin；origin 必须是裸的 `scheme://host[:port]`（带路径、用户信息、查询或冗余默认端口一律拒绝）。production 仍在到达该分支前返回，只接受同源。`DEV_ALLOWED_ORIGINS` 保留给隧道域名等覆盖不到的情况。
- 安全性论证：公网页面无法把私有地址作为自己的 origin；DNS 重绑定也不行，因为 Origin 头带的是页面加载时所用的名字而非解析到的地址；cookie 为 `sameSite: Lax`，跨站 POST 本就不带凭证。
- 验证：`npm run typecheck` 通过；`npm test` **9 文件 65 项**全部通过（含集成测试，测试期内 Worker 在 8787 运行）；`vite build` 通过。tests/trust.test.ts 按新规则重写，除正向用例外补了 10 条反例（11/8、193.168、172.32、`127.0.0.1.evil.org`、带路径/用户信息/查询的 origin、非 http(s)、>255 八位组等）。另以真实 HTTP 请求复核：`Origin: http://192.168.137.1:5173` 返回 401（通过来源校验、只是账号不存在），`Origin: https://evil.example.org` 返回 403。
- 用户侧无需改 `.dev.vars`；`DEV_ALLOWED_ORIGINS` 里那条过期 IP 无害，可留可删。

### 2026-10-14 — 语音链路真机排查（根因未定位；修掉三处真实缺陷）
- 用户反馈"播报和说话都不好使"。从数据库取到的证据：`listen` 两次回报"已开始收音"（语音模块就绪），服务器确实建了设备对话 `设备对话 · DeepSeek`（10:52:29，说明音频上传、凭据与角色校验都通过），但该对话**零消息**；整轮耗时约 40 秒 ≈ 10 秒录音 + 25 秒识别超时，据此判定卡在识别。`speak` 每次都报"设备未配置语音服务"，因为该走的是 `CloudSpeak` → AIUI 会话，而用户固件是无 AIUI 模式。
- **用探针逐条排除，而不是靠推理**：①本地 workerd 出站 WebSocket —— 本地回声服务 + 临时 Worker，`status=101`、`echo:hello`/`echo:ping` 双向正常；②经 TLS 连远程 WebSocket —— 对 postman-echo 与 echo.websocket.org 均得 101 且回显成功；③`new Blob([subarray])` 是否尊重 byteOffset —— `blobHonoursOffset: true`、gzip 往返正常；④采样率声明 —— 固件强制 16 kHz 单声道，与请求一致。四条全部排除，**传输层不是原因，根因仍未定位**。探针目录已删除。
- 修掉三处真实缺陷：①`transcribe()` 的 `AbortSignal.timeout` 在握手成功后仍 armed，会按 20 秒砍断已建立的连接（改为 `AbortController` + 拿到 socket 后立刻 `clearTimeout`）；②识别请求漏了烟测里验证过的 `enable_ddc: true`，已补齐；③`speak` 改为优先走新增的 `/api/device/say`（云合成，不需要 AIUI 会话），失败才回落内置语音栈，无 AIUI 凭据的固件因此也能播报。
- 补上可见性：`runVoiceTurn` 返回 `failure`，路由层无论成败都把结果写进设备专属会话（回复或失败原因，失败时 status=failed）。此前失败是**完全静默**的——机器人不出声，控制台也没有任何可查的东西，这是这次排查最大的障碍。
- 验证：`npm run typecheck` 通过；`npm test` **8 文件 68 项**通过（voice-turn 增至 28 项，新增失败落库、云播报、空/超长文本拒绝）；`vite build` 通过；固件五个改动文件（robot_voice.c、robot_cloud.c、robot_app.cpp、app_main.cpp、robot_audio.c）用 CMake 原始编译命令真实编译通过。
- 未验证：**识别卡住的原因仍未找到**；固件未编译烧录；`speak` 改走云端后能否出声未验证。下一步：烧录新固件后重试「和它说句话」，失败原因会出现在设备对话里，据此继续定位。
- 2026-10-14 用户实测反馈：**播报已通**——这一条同时证明了凭据有效、云合成可用、设备能收能放流式 PCM，整条 HTTP 链路没问题。说话仍报"没有听清"（即 `EMPTY_TRANSCRIPT`），且三次都是。
- 由此把结论从"识别超时"修正为"**识别调用成功但判定音频里没有人声**"。三次的时间间隔均为 11–12 秒 ≈ 10 秒录音跑满上限 + 2 秒识别；**录音跑满上限意味着固件端 VAD 也从头到尾没检出说话**。两个互相独立的检测器（本地能量 VAD 与火山识别）得出同一结论：**送进去的音频基本无声**。
- 重要背景：该固件是无 AIUI 模式，`SetupOnlineServices()` 在凭据缺失时提前 `return`，**唤醒词引擎从未初始化**；而麦克风电平日志原先只在 `SESSION_ACTIVE` 状态打印——因此这台设备的麦克风**在此之前从未被任何东西真正使用或验证过**，静音也不会有人发现。
- 本轮补的可见性：①`PcmConsumer` 的麦克风电平日志改为任何状态下都打印（约每秒一行，静音时标注 `(silent)`）；②服务端 `describeAudio()` 把收到的音频量出来（时长/峰值/均方根/有声占比），写在失败原因后面。**"麦克风完全没采到"和"采到了但没听到说话人"从外部看一模一样，却需要完全相反的修法**，这两个数字就是把它们区分开的依据。
- 验证：`npm run typecheck` 通过；`npm test` 8 文件 69 项通过（voice-turn 29 项，新增音频电平断言：静音全零必须报 `峰值 0 / 有声占比 0%`，1 kHz 满幅音必须报出真实峰值）；固件五个文件真实编译通过。
- 仍未验证：麦克风是否真的不工作（需要串口日志或对话记录里的电平数字）；上述两处可见性改动未烧录。

### 2026-10-14 — 与已验证固件对比，采样率改为可配置
- 用户提供关键信息："之前的源码麦克风是好使的我试过"。这**直接排除了硬件方向**（麦克风本体、供电、I2S 三个引脚、声道接法都是对的），问题在当前固件代码里。
- 找到那份可用固件的板级源码 `xiaozhi-deskemoji-gesture-music-taibai-main/main/boards/esp32-s3n16r8-emoji`（同板型），并逐项对比 I2S 配置：
  - **完全一致**：I2S 端口（麦克风 `I2S_NUM_1`、扬声器 `I2S_NUM_0`）、麦克风 `slot_mode=MONO`、`slot_mask=LEFT`、`data_bit_width=32BIT`、`ws_width=32BIT`、`bit_shift`、`left_align`、读数右移量 `>>12`。其 `board_config.h` 注释亦写明 `L/R: GND (左声道)` —— 我先前怀疑的声道接法**是错的，已排除**。
  - **两处不同**：①**麦克风采样率 24000 vs 16000**；②DMA `frame_num` 240 vs 640。
- 改动（按用户要求"把采样率做成可配置"）：
  - 新增 Kconfig `ROBOT_MIC_SAMPLE_RATE`（int，默认 **16000**、范围 8000–48000）。默认值保持现状，便于对照实验。
  - `robot_app.cpp` 用它配置采集通道；`robot_audio_init` 的硬性 `!= 16000` 校验改为范围校验；`rx_config` 使用配置值。
  - **扬声器（tx）保持固定 16000 不动**：TTS 是 16 kHz 合成的，改了会变速；采集与回放本就是两条独立 I2S 控制器，可各自独立。
  - `robot_voice.c` 中两处硬编码的"16 采样/毫秒"（`sample_count / 16`、`recorded_samples / 16`）改为按实际采样率推导——否则采样率一改，静音判定与录音时长全错。
  - 固件在识别请求上新增 `X-Sample-Rate` 头，把实际采集率告诉服务端；服务端 `captureSampleRate()` 校验范围（8000–48000，非法值回落默认）后用于识别声明与 `describeAudio` 的秒数换算。**只有识别用设备上报的率，合成仍用固定 16000**，两侧因此不会各自漂移。
- 验证：`npm run typecheck` 通过；`npm test` 9 文件 **72 项**通过（新增：按 24000 上报时识别收到 24000、且 1 秒音频算作 1.0 秒；非法上报值一律回落 16000）；固件在**三种配置组合**（16000/左声道、24000/左声道、16000/右声道）下均真实编译通过。
- 未验证：麦克风在这两种采样率下是否真能采到声音——这正是用户接下来要跑的实验。DMA `frame_num` 240 vs 640 的差异**未跟进**（通常只影响延迟），若采样率不是原因再回头查。

### 2026-10-14 — 电平数字拿到：不是静音，是满量程削顶
- 用户实测结果（「和它说句话」一轮）：`收到 10.0 秒音频，峰值 32768，均方根 18252，有声占比 99%`。**"麦克风没采到声音"这条线被彻底推翻**——峰值 32768 是 int16 绝对值上限，说明信号触顶饱和；均方根 18252 是满量程的 56%，正常说话录音在几百到两三千；有声占比 99% 说明固件自己的能量 VAD 也全程判为有声，所以录音跑满 10 秒上限（而不是"说完静音 900 ms 就停"）。
- 采样率**不是**原因（这是本轮要验的假设）：10 秒上限跑满与采样率无关，而满量程削顶更与采样率无关。
- 本轮排除的两条：
  - **位移量 `>> 12` 不是笔误**——参考固件 `main/audio/codecs/no_audio_codec.cc:401` 用的是同一个 `>> 12`，且 clamp 到 `±INT16_MAX`。既然那份固件的麦克风好用，说明 `>> 12` 本身是对的。
  - **I2S 配置与参考固件逐字段相同**。ESP32-S3 走 `i2s_std.h` 的 `#else` 分支，`I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(32BIT, MONO)` 展开为 `slot_bit_width=AUTO`、`slot_mask=I2S_STD_SLOT_BOTH`（随后被覆盖为 LEFT/RIGHT）、`ws_width=32`、`bit_shift=true`、`left_align=true`、`big_endian=false`、`bit_order_lsb=false`；参考固件的 `.slot_mask = I2S_STD_SLOT_LEFT` 与其余字段完全一致。**I2S 配置这条线也关闭了**。
- 上传路径复核：`robot_audio.c:177-185` 把 int32 `>>12` + clamp 成 int16 存入 `capture_pcm`；`robot_voice.c:155` 用 `memcpy` 拷进 `record_buffer` 再发送。转换与拷贝都不产生额外增益或错位，因此**服务端收到的就是固件里那份饱和的 PCM**，服务端没有误读。
- 剩下的两种可能，峰值/均方根**分不开**，修法却相反：
  | 假设 | 观测 | 理论均方根 |
  |---|---|---|
  | 真实语音被放大 16 倍后削顶 | 32768 / 18252 / 99% | ~16000 |
  | 数据不是语音（满量程噪声／读错字节） | 32768 / 18252 / 99% | **18919**（均匀分布于 int16 全域） |
  后者与实测只差 4%，而"有声占比 99%"正是满量程随机信号 `|x|>300` 的概率（99.1%）。这两种都解释得通，**未下结论**。
- 为此给服务端 `describeAudio()` 补了两个能一次分开它们的数（**纯服务端改动，不需要重新烧录**）：
  - `削顶`：样本落在 ±满量程的比例。真实信号被放大过头时会有很大一部分样本被 clamp 到轨上；噪声只会偶尔碰到轨。
  - `过零`：相邻样本变号的比例。即便削顶，语音仍有波形相关性，远低于 20%；噪声互不相关，稳定在 50% 上下。
  另外修正了原实现对"两轨"的不对称统计（`abs()` 使 -32768 也计入）。
- 验证：`npm run typecheck` 通过；`npm test` **9 文件 73 项**通过（voice-turn 增至 32 项，新增用例把两个方向都钉住：过驱正弦必须报 `削顶 8x-9x%` + `过零 个位数`，LCG 噪声必须报 `削顶 0%` + `过零 4x-5x%`；静音用例补上 `削顶 0%` / `过零 0%`）；`vite build` 通过。
- 用户补充：这轮是**按 24000 跑的**。这同时验证了采样率可配置确实打通了——服务端算出「10.0 秒音频」只有在固件真以 24 kHz 采满 10 秒（240000 采样）时才成立，若固件仍按 16 kHz 采、只改了上报值，这里会显示 6.7 秒。
- 参考固件的另一处差异（解释了"它能用、我们不能用"）：它把麦克风数据送进 **AFE 音频前端**（`AfeAudioProcessor`，含 AEC／降噪／VAD，语音通信模式还含 AGC），而本项目的云端通道是**把原始 PCM 直接上传**，没有任何前端处理。若音频确实被过驱，AGC 会把参考固件的电平拉回来、唤醒词对中等削顶也比较宽容，所以它"能用"；我们则把削顶原样送给了云端识别。这条推理自洽但**仍不足以据此改代码**。
- 据此新增 Kconfig `ROBOT_MIC_SHIFT`（int，默认 **12**、范围 8–16）：把 `robot_audio.c` 里硬编码的 `>> 12` 变成可调项，help 文本写明"这个位移就是采集增益，太小会让正常说话削顶、太大则音量过低"。默认值保持现状，便于对照实验。同时在 RX 初始化后新增一行启动日志，把**采样率／声道／位移**一起打印，因为这三个都能从 menuconfig 改且都会改变音频，电平读数必须连同产生它的固件配置一起才有意义。
- 验证：`robot_audio.c` 用 CMake 生成的原始编译命令真实编译通过（位移取 12／16／8 三种值各编译一次）。**`idf.py` 在本会话仍不可用**：`export.sh` 在 Git Bash 下被 ESP-IDF 明确拒绝（`ERROR: MSys/Mingw is not supported`），工具链实际在 `C:\Espressif\tools` 而非默认的 `~/.espressif`。校验脚本与临时产物用完即删。
- 未决：用户报告「采样率配成 24000 时 robot_audio 会有麦克风的报错」，**具体报错原文尚未提供**，未据此改动任何代码。已排除的可能性包括 I2S 读超时（`I2S_CAPTURE_TIMEOUT_MS` 100 ms，24 kHz 下一帧 26.7 ms，余量充足）与 `AUDIO_FRAME_SAMPLES` 取值。
- 未验证：麦克风是否真的过驱（等 `削顶`／`过零` 的实测值）；`ROBOT_MIC_SHIFT` 尚未在真机上改过值。

### 2026-10-14 — 根因确认：`>> 12` 把采集增益放大了 16 倍
- 第二轮实测（24000 Hz）拿到判读所需的两个数：`峰值 32768，均方根 18552，有声占比 99%，削顶 16%，过零 1%`（另一次 `18547 / 14% / 1%`）。**一次定案。**
- 判读过程：
  - `过零 1%`（而非 50%）**排除噪声**——噪声相邻采样互不相关，过零必然约一半。
  - `过零 1%` 同时**排除纯正弦／工频干扰**：正弦的均方根/峰值比恒为 0.707 且**根本不会削顶**，实测是 0.566 且削顶 16%。**这一步很关键**，否则"低频、幅度大、识别不出来"很容易被误判成电源干扰而跑去查硬件。
  - 对剩下的假设做**定量检验**：设语音近似拉普拉斯分布（比高斯更贴近语音），放大 16 倍后削顶，用削顶比例 16% 反解尺度参数 b=17878，再算削顶后的均方根得 **18690**，实测 18552，**只差 0.7%**。反推原始语音均方根约 **1580**，是正常说话电平。
- 结论与**独立的位对齐推理**一致：INMP441 是 24 位麦克风，其 24 位位于 32 位槽的高位，因此 `>> 16` 正好把麦克风满量程映射到 int16 满量程，而 `>> 12` 恰好放大 16 倍（+24 dB）。**测量与原理两条独立路径给出同一个答案**，据此才敢改默认值。
- 改动：`ROBOT_MIC_SHIFT` 默认值 **12 → 16**，Kconfig help 重写为记录实测数据与判读依据。`robot_audio.c` 的 C 代码未再改动（上一轮已改为读该宏），三种位移值 12／16／8 均已真实编译通过。
- 参考固件为何用 12 却能用（已查明并记入 LEARNINGS）：它把麦克风送进带 **AGC 的 AFE 前端**，AGC 会拉回过驱电平；本项目云端通道直接上传原始 PCM，没有前端处理。
- 同时确认的一条**未验证假设**：设备通道把设备上报的采样率原样声明给火山（`volc.ts:165` 的 `audio.rate`，并按该率切 200 ms 帧），而 **24000 从未对火山验证过**——烟测用的是 16000，官方要求也是 16 kHz。**若削顶归零后仍报"没有听清"，这就是下一个排查点**，不是增益问题。
- 未验证：改到 16 后的真机电平与识别结果（用户下一步）；用户提到的「24000 时 robot_audio 的麦克风报错」始终未提供原文，未据此改动任何代码。

### 2026-10-14 — 增益修复生效，但识别仍空；查出客户端把服务端响应静默丢掉
- 用户实测（`ROBOT_MIC_SHIFT=16`）：`峰值 6244，均方根 1515，有声占比 77%，削顶 0%，过零 2%`（另一次 `4184 / 937 / 69% / 0% / 3%`）。**三个预测全部命中**（削顶 16%→0%、均方根 18552→1515、峰值 32768→6244），波峰因数 4.1／4.5，是正常语音量级。**增益这条线确认解决，音频本身已是健康录音。**
- 但识别**仍然返回空**（"没有听清"），且每一次都是。按上一轮的约定，问题不在增益，转入客户端。
- 逐项排除（都读代码，不靠推理）：
  - **协议实现一致**：`scripts/volc-voice-smoke.mjs` 的 `frameHeader`／`fullRequestFrame`／`audioFrame` 与 `server/voice/frames.ts` **逐字节相同**（int32BE 序号、uint32BE 长度、末帧取负且 `NEG_WITH_SEQUENCE`）。
  - **解析路径一致**：两边都读 `payload.result.text`，帧解析字段顺序（headerSize、flags 位、SERVER_FULL 跳 4、SERVER_ERROR 取 code+size 跳 8）也一致。
  - **`.buffer` 无隐患**：`concat()` 返回的是新建的精确长度 `Uint8Array`，`.buffer` 不含多余字节。
  - 烟测用的是 `ws` 库（Node），得到 `Buffer`；生产走 `fetch` + `Upgrade`，`event.data` 的类型由**运行时**决定——**这条路径从未被端到端验证过**。
- **找到的缺陷**：`volc.ts` 的消息处理只认 `ArrayBuffer` 和字符串，其余一切被 `new TextEncoder().encode(typeof data === 'string' ? data : '')` **静默编码成空数组**。空数组喂给 `parseServerMessage` 会走到 `if (!payload.length) return result`——不报错、`isLast` 为假、`code` 为 0；于是整轮一直等到 socket 关闭，拿到空文本，报"没有听清"。而按 WebSocket 规范 `binaryType` 默认为 `"blob"`，`binaryType` 在本仓库**从未设置过**。此前那个探针验证的是**文本帧**（`echo:hello`），二进制帧从未验证。
- 修法（两处）：
  - `frames.ts` 新增 `messageBytes(data)`，把 `ArrayBuffer`／任意 `ArrayBufferView`（按 byteOffset/byteLength 取，不能直接用 `.buffer`）／字符串／`Blob`／未知形态全部显式处理，未知形态返回空。
  - `volc.ts` 统计真正收到的响应帧数（长度 ≥ 4 才计），**一帧都没收到时直接抛错**「语音识别没有收到任何结果帧」，而不是把"自己没读懂"伪装成"识别说没听到人说话"。这两者修法完全相反，此前正是被混为一谈。
- 验证：`npm run typecheck` 通过；`npm test` **9 文件 78 项**通过（voice-frames 6→11 项，新增 ArrayBuffer／带 byteOffset 的视图／Blob／字符串／未知形态五条，其中"视图按自身 offset 取"一条专门锁死 `.buffer` 误用）；`vite build` 通过。
- 未验证（不得记为通过）：**workerd 下 `event.data` 究竟是 ArrayBuffer 还是 Blob 尚未实测确认**——本次两处改动对两者都能正确处理，且帧计数会把结论暴露出来，因此不需要先做探针。真机复验后即可定论：识别出文本 → 就是解码问题；仍报"没有听清"但**不再报"没有收到结果帧"** → 帧收到了，问题在音频内容或声明的采样率。

### 2026-10-14 — T018 语音对话闭环真机验证通过（服务端 + 网页端 + 固件全链路）
- 用户实测：连续三轮「测试。」「你好。」「麦克风。采样。率测试。」**全部被正确识别**，模型回复正常，TTS 播报正常。**根因确认就是上一轮修的解码缺陷**——改完立即恢复，说明 workerd 交给 `event.data` 的确实不是 `ArrayBuffer`（即按 WebSocket 规范默认的 `binaryType = "blob"` 给了 Blob），而原实现把它静默变成了空数组。
- **T018 至此全部完成并经真机验证**：服务端（火山 ASR／TTS 适配 + 一轮编排）、网页端（语音设置页、设备语音角色、设备对话记录）、固件（录音 VAD、上传、分帧流式播放）三段都跑通。T014–T018 设备接入与语音闭环阶段收尾。
- 四轮录到的问题修复累计（按发现顺序）：①origin 校验按地址类型放行；②`speak` 改走云合成以支持无 AIUI 固件；③`AbortSignal` 在手握握手后仍 armed 会砍断活连接；④ASR 漏 `enable_ddc`；⑤`>> 12` 采集增益放大 16 倍导致满量程削顶；⑥WebSocket 消息类型未处理导致响应被静默丢弃。**其中 ⑤ 和 ⑥ 是本次排查最终定位到的两个真实缺陷。**
- 遗留（非阻塞）：四轮里有一次「没有听清」（`峰值 7302，均方根 2257，有声占比 85%，削顶 0%，过零 1%`），音频参数本身健康，属偶发漏识别。**可调项**：四轮录音都跑满 10 秒上限，说明固件 VAD 从未检出 900 ms 静音——`ROBOT_CLOUD_VOICE_THRESHOLD` 默认 320 是在增益还错的时候选的，如今电平已降 16 倍，320 相对均方根（约 1500–2300）明显偏低，环境噪声都能越过它。适当调高（如 600–800）可让录音在说完后及时收尾，缩短上传并减少"整段里只有一小段是人声"的情况。
- 用户固件这两轮实际使用的采样率未确认；不过识别已经跑通，说明该取值可被火山接受。

### 2026-10-14 — 采集链路优化：VAD 阈值随增益失配，已重设
- 用户提问「这个声音采样可以优化吗」。逐处看过采集链路后，按证据排序给出结论，并只改了**有直接实测依据**的那一处。
- **证据**：四轮录音**全部跑满 10 秒上限**（`收到 10.0 秒音频`）。录音跑满上限意味着固件 VAD 从未累计出 900 ms 静音。
- **根因**：`ROBOT_CLOUD_VOICE_THRESHOLD` 原为 **320**，是在增益还错 16 倍时选的绝对电平；增益修正后正常说话均方根为 1500–2300，320 只相当于它的六分之一，**环境噪声的起伏就能不断越过它**，于是 `trailing_silence_ms` 每被一帧"响"清零，永远攒不满 900 ms。**绝对电平阈值与采集增益是绑死的：`ROBOT_MIC_SHIFT` 一动，它必须按同一倍数跟着动。** 这条已写进 Kconfig help。
- 改动：`components/robot_cloud/Kconfig` 的默认值 **320 → 700**，help 重写（说明"这个数只有和采集增益放在一起才有意义"、实测均方根区间、以及顺带说明 700 大约在说完话一秒后收尾）；`robot_voice.c` 的同名 C 兜底常量一并改为 700 保持一致（仅在选项为 0 时生效，`robot_app.cpp` 传的是 CONFIG 值）。**纯 Kconfig 默认值与数值常量改动，无 C 逻辑变化，故未重跑编译校验。**
- 明确**不建议**动的几处，以及理由：①DMA 帧长 `AUDIO_FRAME_SAMPLES = 640`（改小能降几十毫秒延迟，但该宏被 AIUI 的 Opus 路径共用，收益小、波及面大）；②位深改 32 位（ASR 只吃 16 位，只会让上传体积翻倍）；③再加软件增益或 AGC（增益刚刚对准，再加会把刚修好的弄坏）。
- 建议用户自行在 menuconfig 调整的两项：①`ROBOT_MIC_SAMPLE_RATE` 24000 → **16000**（火山只验证过 16000、官方要求 16 kHz；上传体积减半；与服务端 16 kHz 合成、以及同样是 16 kHz 的离线唤醒引擎保持一致）；②可选把 `ROBOT_CLOUD_VOICE_MAX_RECORD_MS` 从 10000 降到 6000，把最坏情况兜底压下来。
- 另记一处**体验**问题（未改）：控制台点「和它说句话」后，设备要轮询到指令才开始录音，**没有任何声音提示**，用户不知道何时开口，前几个字可能被吞。唤醒路径有提示音（且会先暂停采集、等提示音播完再录），控制台路径没有。

### 2026-10-14 — 设备码与服务器地址改由配网页填写（叠加在固件编译值之上）
- 用户提议「配网界面改一下，配网 + 填入设备码」。判断为**值得做**，理由不止方便：
  - **服务器地址编进固件是个反复发作的痛点**：控制台跑在电脑上，其局域网地址每换网络就变（LEARNINGS 记着三次：10.34.20.209 → 10.243.104.162 → 192.168.137.1）。**IP 一变设备通道就失联，而唯一的办法是 menuconfig + 编译 + 烧录。** 配网页能改之后手机 30 秒即可。
  - **顺手解决安全问题**：设备码原本写在 `sdkconfig` 里，DEPLOY 不得不常挂一句"含真实密钥的 sdkconfig 不得提交到公开仓库"。挪进 NVS 后固件镜像不再带凭据。
  - **配网页本来就有现成模式**：`robot_network.c` 里已有一组 AIUI 凭据输入框（`appid`/`appkey`/`appsecret`）+ `save_handler` 存 NVS + `robot_network_load_aiui_credentials()` 读回，三件事都有先例可循。
- 实现（四个文件）：
  - `robot_network.h`：新增 `robot_cloud_credentials_t`（`base_url[128]`、`device_key[64]`）与 `robot_network_load_cloud_credentials()`。
  - `robot_network.c`：配网页新增「控制台（设备通道）」折叠区（服务器地址 + 设备码）；`save_handler` 存 NVS 命名空间 `cloud_cfg`（键 `url`／`key`）；新增读取函数。`FORM_LIMIT` 由 1024 提到 **2048**——新增两个字段后 URL 编码的表单可能超过原上限，超了会被 400 拒掉。
  - `robot_app.cpp`：`NETWORK_GOT_IP` 里先读 NVS，**逐字段回落**到 `CONFIG_ROBOT_CLOUD_URL`／`CONFIG_ROBOT_CLOUD_KEY`。
  - `components/robot_cloud/Kconfig`：两项的 help 改写为"建议留空、在配网页填"，并说明此处只是兜底。
- 两个关键设计决定：
  - **逐字段回落而非整体回落**：只填服务器地址也能生效，不必为了跟着 IP 变而重敲设备码。
  - **存储必须 `static`**：`robot_cloud_init` 是**保存指针**（`s_cloud.config = *config`）而不是拷贝字符串，用块内局部数组会让指针在函数返回后悬空。已加注释说明这个约束。
  - 与 AIUI 凭据的**有意不同**：AIUI 三项是"要么全存要么不存"，这里改为"填了就存、空着不动"，正是为了能只改地址。
- 验证：三个包含该头文件的源文件（`robot_network.c`、`robot_music.c`、`robot_app.cpp`）用 CMake 生成的原始编译命令**真实编译全部通过**。
- **期间查出并修掉的是我自己的校验脚本缺陷**：CMake 会把 `-DMBEDTLS_CONFIG_FILE=\"mbedtls/esp_config.h\"` 写进命令，`\"` 打断了脚本里 `"..."` 的分词正则，宏定义被撕成两半而变成空值，于是 `robot_music.c` 报出一堆与改动毫不相干的 mbedtls 错误。已确证两个文件的这条定义**逐字相同**（未改动的那个文件也含同样的定义），所以问题在脚本。修法是分词前把 `\"` 换成哨兵、写响应文件时再还原。**这类"校验工具本身出错、却伪装成被测代码出错"的情况，判断依据是"报错位置与被改动的部分有无因果关系"**——mbedtls 的报错和"新增一个 typedef"之间没有因果。
- 未验证：**未烧录真机**，配网页新字段的实际填写与保存、以及回落逻辑在真机上的表现都还没验过。若配网页填了值，设备重启后应连到该地址而不是固件里的值。
### 2026-10-14 — T020 配网页面：Wi-Fi 与设备码互不影响
- 用户要求：机器人的配网界面里 Wi-Fi 与设备码互相不影响——已配网的情况下，改设备码不动 Wi-Fi，改 Wi-Fi 不动设备码。
- 只读定位根因（`robot_network.c`）：配网页原先只有一个 `<form>`，Wi-Fi 名是必填（缺 SSID 直接 400），因此"改设备码"在 HTTP 层必然携带 Wi-Fi 字段；而密码框留空会被当作开放网络写进 NVS，把已保存的密码覆盖成空 → 设备连不上 → 退回配网。用户看到的因果是"改了设备码，Wi-Fi 坏了"，真实因果是"提交了半个 Wi-Fi 表单"。设备码一侧原本已是逐字段写入（留空不动），所以问题在耦合，不在存储。
- 改动（仅 `Hardware/esp32/components/robot_network/robot_network.c`）：页面改为「静态头 + 当前配置 + 静态尾」分块下发；三张卡片（Wi-Fi、控制台设备通道、AIUI 凭据）各带一个隐藏 `section` 与一个提交按钮；`save_handler` 解析后按 `section` 分派到 `save_wifi_section`／`save_cloud_section`／`save_aiui_section`，各自只写自己的 NVS 命名空间；Wi-Fi 名不变且密码留空时沿用已存密码；提交的字段全空则回"没有改动"且**不重启**；`section` 缺失直接 400；页面顶部显示当前配置（设备码只显示后四位）。
- 验证（本机可做的部分）：按 `Hardware/esp32/build/compile_commands.json` 记录的真实编译命令、用项目自带 `xtensa-esp32s3-elf-gcc` 编译该文件，退出码 0 且无警告（对象文件内可检索到新增分支与字符串）；从源码抽出页面 HTML，再抽出其中唯一的 `<script>` 后 `node --check` 通过；核对页面含 3 个表单、3 个提交按钮、3 个 `section` 值（wifi／cloud／aiui）。页面预览（含模拟 Wi-Fi 列表）生成在 C:\Users\15163\.codex\visualizations\2026\10\14\01a13ab6-a76a-71e1-b2c7-213ead72fdbb\provision-page-mock.html，供人工在浏览器中查看。
- 未验证：**真机未烧录**。配网页的实际填写与保存、「改设备码不动 Wi-Fi／改 Wi-Fi 不动设备码」的真机表现、以及页面在手机浏览器里的渲染（本机无可用渲染/截图工具，`idf.py` 也不在 PATH 中，只能用编译命令单文件编译）都还没有验证过，不得记为已通过。
### 2026-10-14 — T021 BOOT 长按进入配网（已配网也能进）
- 用户确认要求：设备已配网时也要能改配置——"长按按钮进入配网"。
- 只读定位：入口只有一个且很窄——`robot_network_request_reprovision()` 仅在 `CONNECTION_CONNECTING` 时接受（`robot_network.c`），而 `robot_app.cpp` 的 BOOT 短按在连上之后会退化成唤醒/停止播放；因此设备一旦在线，只能靠"让保存的 Wi-Fi 连不上、等 60 秒超时自动进配网"这条路，对"设备在线想改设备码"基本等于没有入口。
- 改动（`Hardware/esp32/main/robot_app.cpp`、`robot_app.h`）：BOOT 轮询由 `poll_button(boot, now, 0)` 改为 `poll_button(boot, now, kProvisionLongPressMs)`（800 ms）；长按投递新事件 `EventType::PROVISION_REQUEST`，由 worker 任务处理——处于 `SESSION_ACTIVE` 时先 `StopSession("provisioning requested from BOOT long press")`（AIUI 运行时与播放栈只有该任务能碰），再调用 `robot_network_enter_provisioning()`；**已经处于 `PROVISIONING` 时，同一手势改为 `esp_restart()`**，理由是配网页只写配置、没有"返回"按钮，重启即用已存配置重新联网，比让用户手动断电更合理。短按语义与既有行为完全不变——`poll_button` 在长按触发后把 `long_press_sent` 置位、释放时不再补发短按，所以两种手势互不干扰。
- 验证：从 `build/compile_commands.json` 取真实编译命令，编译 `main/robot_app.cpp`、`main/app_main.cpp`（后者用于确认改动后的 `robot_app.h` 不影响其他包含者）与 `robot_network.c`，三个文件退出码均为 0（`-Wall -Wextra -Werror=all`）。
- 未验证：**真机未烧录**。长按进配网、配网中长按重启、以及"会话或播放中长按"的实际表现都还没验过；`robot_cloud` / `robot_voice` 在 Wi-Fi 被停掉后的报错行为与开机配网时一致（既有行为），也未在真机上复核。
### 2026-10-14 — T020 修订：配网页改回单表单一个保存按钮
- 用户反馈："配网页面不用每一个都留一个按钮，就一个按钮保存信息，如果输入框中填了信息就将这个信息保存，如果没填就默认是原来的信息就行。"
- 判断：这是**修正而不是回退**。独立性来自"没有必填项 + 留空＝不修改"，与表单拆不拆无关；拆成三段只是我自己选的实现形状，用户看到的是三个按钮 + 三套提示，反而更繁琐。据此改回单表单，规则不变。
- 改动（仍只动 `Hardware/esp32/components/robot_network/robot_network.c`）：删除 `section` 分派与 `save_wifi_section`／`save_cloud_section`／`save_aiui_section`，页面由三段表单合并为**一个表单 + 一个「保存并重启」按钮**（不再有必填的 Wi-Fi 名）；`save_handler` 改为逐字段写入：`ssid` 非空才写 Wi-Fi（同名且密码空＝沿用已存密码，换名且密码空＝开放网络）、`server`／`devicekey` 各自非空才写、AIUI 三项必须同时填（唯一例外，缺一项在写入前就拒掉，避免半套配置）、全部留空则回「没有改动」且不重启；回执页改为列出**这次实际保存了哪几项**（如「已保存：设备码」）。页面 JS 的提交校验只在"真的在设 Wi-Fi"时才拦（选中加密网络+密码空+与当前网络不同）。
- 验证：`robot_network.c` 用真实编译命令编译通过（退出码 0，`-Wall -Wextra -Werror=all`）；从源码抽出页面 → **1 个表单、1 个提交按钮、0 个 `section` 字段**；页面唯一 `<script>` 经 `node --check` 通过；预览页（含模拟 Wi-Fi 列表）已更新至 C:\Users\15163\.codex\visualizations\2026\10\14\01a13ab6-a76a-71e1-b2c7-213ead72fdbb\provision-page-mock.html。
- 未验证：仍**未烧录真机**——"只填设备码保存后 Wi-Fi 不动""只改 Wi-Fi 保存后设备码不动""全部留空不重启"这三条都要在真机上复核。
### 2026-10-14 — T021 修订：长按＝总是进入/重申配网（不再重启）
- 用户反馈："这回长按按钮咋不进入配网功能了"。
- 只读复查：三处可疑点各查了一遍——①按键层：`poll_button(boot, now, kProvisionLongPressMs)` 的长按判断（长按触发后置 `long_press_sent`，释放不再补发短按）与短按阈值（50–1500 ms）互不冲突；②事件层：`PostEvent` 是 `xQueueSend(..., 0)`，队列深度 20，`HandleEvent` 的 `PROVISION_REQUEST` 分支位于 switch 顶部，`CLOUD_VOICE_FINISHED`／`AIUI_DISCONNECTED` 等迟到事件都有各自的状态守卫，不会把刚设好的 `PROVISIONING` 顶掉；③网络层：`robot_network_enter_provisioning()` 从"已连接"进入与从"连不上"进入走的是同一段代码（先 `set_connection_state(CONNECTION_PROVISIONING)` 再 `esp_wifi_stop()`，因此不会再触发重连或 `ROBOT_NETWORK_DISCONNECTED`）。
- 找到并修掉的**设计缺陷**：原实现里"配网中再长按＝`esp_restart()`"，这是一个手势承担两种语义。只要 app 的 `state_` 与网络侧的实际状态不一致（app 因迟到事件离开 `PROVISIONING`、而 AP 仍在服务），长按就会表现为"重启"或"什么都没发生"——正好就是用户描述的现象。改为：长按**总是**调用 `robot_network_enter_provisioning()`；`robot_network.c` 里"已开页"的早退分支不再静默返回，而是重新调用 provisioning 回调，把屏幕与提示音重放一遍，等于顺带把 app 状态同步回配网。退出配网改为：保存任一项后自动重启联网，或断电重启。
- 可诊断性：手势力加分层日志——`BOOT long press detected`（按键任务，含 `PostEvent` 失败告警）→ `provisioning requested from BOOT long press`（worker 处理）→ `provisioning: SSID=… URL=192.168.4.1`（`robot_network` 真正起来）。下次"什么都没发生"时，日志停在哪一层就说明问题在哪一层。
- 验证：`main/robot_app.cpp`、`robot_network.c` 用真实编译命令编译均通过（退出码 0，`-Wall -Wextra -Werror=all`）。
- 未验证：**真机未烧录**。以上是代码层复查 + 行为修正，仍需真机确认"长按进配网"与三条日志按预期出现；若用户当前固件是改动前的版本，需要重新 `idf.py flash` 才会包含本次改动。
### 2026-10-14 — "点按也能进配网"= 设备未联网的告警（并补显示密码是否已保存）
- 用户反馈："主要现在变成了点按进入配网模式"。
- 定位（读代码即可确定，不需要真机）：`robot_network_request_reprovision()` 只在 `CONNECTION_CONNECTING` 时返回 true，而 `robot_app` 的短按分支是"能重配就重配、否则唤醒"，**短按分支本次一个字未改**。因此"点按就进配网"等价于"按下那一刻设备不在网"。与上一条反馈（长按像没反应）合起来，三个现象指向同一个状态：**设备的 Wi-Fi 一直连不上**；最可能是最早那个 bug 的遗留——旧固件在保存设备码时把 NVS 里的 Wi-Fi 密码覆盖成了空串。
- 代码改动（仍只动 `robot_network.c`）：页面顶部状态卡新增"密码是否已保存"提示——有 SSID 时显示 `（已保存密码）`／`（未保存密码）`；`root_handler` 因此也读取 `wifi_cfg/password`（读完立即 `memset`）。编译通过（退出码 0）。
- 给用户的操作路径：进配网页 → 选 Wi-Fi → **手动填一次密码** → 保存（"留空＝沿用已存密码"在"已存的就是空"时仍然沿用空）；只改设备码时不要点 Wi-Fi 列表里的网络行（单表单下，点了会被一起提交）。日志判据：连不上时会出现 `saved Wi-Fi did not connect within … seconds` 紧接 `provisioning: SSID=… URL=192.168.4.1`；连上则出现 `device channel online`。
- 未验证：真机（等用户按上面路径试一次）。
### 2026-10-14 — 配网页服务器地址规范化（裸 host 自动补 http://）
- 用户真机日志：`E (38077) HTTP_CLIENT: Error parse url 192.168.137.1:5173/api/device/hello` / `Failed to set URL`。这条日志同时说明两件事：**设备已经联网并在轮询设备通道**（能主动发起请求），故障在地址字符串本身——缺 `http://`，且端口填了前端 Vite 的 5173 而不是 Worker 的 8787。
- 代码改动（仍只动 `robot_network.c`）：新增 `normalize_server()`，保存服务器地址时若不含 `://` 就补 `http://`（已带 scheme 的原样保留，https 同理）；补不下时返回 `ESP_ERR_INVALID_SIZE`，`save_handler` 映射为中文提示页「服务器地址太长」；页面该字段下方的提示改为"填控制台 Worker 的地址，本地开发形如 http://192.168.1.10:8787（前端页面的 5173 不是它）；漏写 http:// 会自动补上"。编译通过（退出码 0）。
- 用户提问与回答（同步写入 DEPLOY）：**为什么要在配网页填服务器地址**——因为开发期控制台跑在电脑上，地址随电脑换网变化（本项目已变过 10.34.20.209 → 10.243.104.162 → 192.168.137.1），编进固件就得每次重烧；**上线后不必在配网页填**：域名固定，把它写进 `ROBOT_CLOUD_URL`（Kconfig）即可，配网页那栏留空。优先级是"配网页填了用配网页、没填用固件值"，两种做法都成立。设备码与地址不同：每台设备唯一、一次性签发，天然逐台配置。
- 未验证：真机把地址改成 `http://192.168.137.1:8787` 后设备通道恢复在线（用户下一步）。
### 2026-10-14 — T022 设备页与对话页自动刷新（不用再手动刷新整页）
- 用户要求："把网页写成热加载，我每次还要刷新网页才能看到新数据。"
- 只读定位（`web/src/main.tsx`，单文件 React 应用）：设备列表来自 `refresh()`，只在加载与增删改后调用；展开的设备详情来自 `refreshDetail()`，只在点开设备／下发指令／换语音角色时调用 —— 所以设备把指令执行完（轮询间隔 3 秒）后，"待执行→已下发→已完成"的进度要手动刷新整页才看得到；在线圆点、最后在线时间同理。对话页则只在**生成中**才有 1.5 秒轮询，机器人从云端自主说话时不会自己出现。
- 改动（只动 `web/src/main.tsx`，新增两个 effect）：
  - 设备页：停留在「设备」标签时每 3 秒 `GET /api/devices` 刷新列表；若展开了某台设备，再 `GET /api/devices/:id/commands` 刷新指令进度。
  - 对话页：停留在「对话」标签、存在当前会话、且不在生成中时，每 3 秒 `open(active)` 刷新当前会话（`open` 只读会话+生成状态，且不设置 `busy`、不动 `follow`/滚动，因此不会打断用户的滚动与输入）。
  - 两处都以 `document.visibilityState==='visible'` 为前提：切到别的标签页或把浏览器切后台就跳过本次轮询，离开该页时清除定时器。
  - 生成中的轮询保持原样（1.5 秒），与新加的 3 秒轮询互斥，避免叠加请求。
- 限流核对（因为轮询会持续打请求）：`/api/devices`、`/api/devices/:id/commands`、`/api/conversations/:id`、`/api/conversations/:id/generation` 四个 GET 端点在服务端**都没有 `limit()`**，所以 3 秒一次不会吃掉写操作的限流额度（写接口仍有各自的 limit）。
- 验证：`npm run typecheck` 通过；`npm test` 9 文件 78 项全部通过；`npm run build` 通过（1578 模块，产物写入根 `dist/`，Worker 直接服务该目录）。沙箱下 `vite build` 会因 `realpath` 被拒而 EPERM，已按规则改用沙箱外执行，非代码问题。
- 未验证：**真实浏览器中的自动更新**（本会话没有可用的浏览器/预览工具）；另外侧栏「最近的对话」列表有意不轮询，机器人新建会话后仍需刷新一次。
### 2026-10-14 — 服务器地址"使用时兜底"：让旧值自愈（补 T020 范围）
- 用户再次贴出同样的真机错误：`E HTTP_CLIENT: Error parse url 192.168.137.1:8787/api/device/hello`。地址里仍然没有 `http://`。
- 只读定位：上一轮把规范化放在了**保存时**（配网页写入端），而这条错误来自设备**读取 NVS 里那条旧值**——旧值不会因为换固件而改变，除非再保存一次；用户重烧后并没有重新保存地址，于是症状一模一样。**这是"只补写入端"的典型漏项：修复必须覆盖存量数据。**
- 改动（`Hardware/esp32/components/robot_cloud/robot_cloud.c`）：新增 `looks_like_lan_host()` 与 `normalize_base_url()`，在 `robot_cloud_init()` 里把 `config->base_url` 规范化后放进静态缓冲 `s_base_url` 再使用（robot_cloud 存的是指针而非字符串，规范化结果必须有静态存储）。规则与写入端一致：已带 `://` 原样保留；IP 字面量或无点主机名补 `http://`；其它名字补 `https://`。两处注释互相指向对方，避免规则漂移。`robot_voice` 通过 `robot_cloud_base_url()` 拿同一份规范化结果，无需改动。
- 验证：`robot_cloud.c`、`robot_voice.c` 用 CMake 记录的真实命令编译通过；并用同样方式重编了 `robot_network.c`、`main/robot_app.cpp`、`main/app_main.cpp`，使 `build/` 里的目标文件都与真实构建一致。**过程中暴露了校验脚本缺陷并记录进 LEARNINGS**：把 `compile_commands.json` 的命令交给 PowerShell 会二次转义 `-DXXX=\"...\"`，导致 `MBEDTLS_CONFIG_FILE` 变空、报 `#include expects "FILENAME"`；改用"原始命令写进 .bat + `cmd /c`"后全部通过。
- 期间事故与修复：写文档时一个 here-string 漏了结束标记，把后面整段脚本写进了 DEPLOY.md（并截断了原句）；已按锚点替换回正确内容，并用标题清单与关键片段计数核对无残留，教训记入 LEARNINGS。
- 未验证：真机重烧后设备通道恢复在线（旧值自愈）。用户侧的即时解法：在配网页把地址写成 `http://192.168.137.1:8787` 后保存一次，不需要等新固件。
### 2026-10-14 — T023 语音"断断续续"：先加可区分证据 + 修正过时的阈值
- 背景：用户真机跑通云端语音闭环（日志可见 `robot_voice: answer: …`），但转写结果为"你。能听见。我说话。吗？"，反馈"语音识别断断续续"。
- 只读定位：转写里的句号可能来自①音频中真实的停顿、②识别服务自己的分句、③采集丢帧造成的空洞；三者修法不同，不能凭转写内容判断。同时核对设备配置发现一个过时值：`Hardware/esp32/sdkconfig` 里 `CONFIG_ROBOT_CLOUD_VOICE_THRESHOLD=320`，而 Kconfig 默认值与实测结论是 700（320 是增益高 16 倍时代的取值；**Kconfig 改 default 不会覆盖已存在的 sdkconfig**）。阈值偏低会让房间噪声也算"有声"，录音起止因此更不准。
- 改动一（`components/robot_cloud/robot_voice.c`）：每轮录音结束打印 `capture report`，包含 `audio/wall`（缓冲区音频时长 vs 墙钟时长，接近即无丢帧）、`voiced ms`、`longest mid-speech gap`、`gaps >= 400 ms` 个数（只统计其后仍有语音的静音段，尾部静音不计）。这样下一次反馈可直接定位到"采集／说话节奏／识别分句"三者之一。编译通过（`robot_voice.c`，退出码 0）。
- 改动二（`Hardware/esp32/sdkconfig`）：`ROBOT_CLOUD_VOICE_THRESHOLD` 由 320 改为 700，与 Kconfig 默认值及实测结论一致（仍可在 menuconfig 调整：环境噪声大就调高，轻声被截断就调低）。
- 未验证：真机重烧后的 `capture report` 读数，以及阈值修正后录音起止是否更准（等用户下一轮实测）。
- 安全提醒（未改动）：`sdkconfig` 内含真实 `CONFIG_ROBOT_CLOUD_KEY`，即设备码被编译进固件。建议确认配网页 NVS 里已有设备码后把该项留空，并确保 `sdkconfig` 不进仓库（DEPLOY 已有相同约定）。
### 2026-10-14 — T023 追加：提示音不进录音 + 首尾静音裁剪（含日志取证）
- 用户贴回真机日志：`capture report: 5840 ms of audio over 5870 ms wall, voiced 3140 ms, longest mid-speech gap 860 ms, gaps >= 400 ms: 2`，随后 `heard: 介绍。1下。你自己。`。
- 判断：**采集没有丢帧**（5840 vs 5870，差 0.5%），而音频里确实有两段 ≥400 ms 静音（最长 860 ms）——火山的断句正确，不是误判。再按时间戳定位静音来源：`playback stream start`(90518) 与录音起点（≈90528）只差 10 ms，300 ms 后麦克风峰值 20271（提示音），随后 860 ms 静音才是用户说话 → **唤醒提示音被录进了这一段，且"等提示音播完"的逻辑遇上竞态（判断时提示音还没开始播）**。
- 改动（`components/robot_cloud/robot_voice.c`）：①麦克风旁路新增"提示音还在播就丢弃该帧"的守卫；②录音结束后统一收尾——统计（`audio/wall`、`voiced ms`、最长句内静音、`gaps >= 400 ms` 计数）**并裁掉首尾静音**（头部保留 150 ms、尾部保留 250 ms 余量），日志新增 `trimmed %u ms`。编译通过（`robot_voice.c`，退出码 0）。
- 回答问题并写入 LEARNINGS：设备侧**不做断句**，只判断"说完了"；标点与分段由服务端请求里的火山 ASR 参数决定（`enable_punc`／`enable_ddc`／`show_utterances`，见 `server/voice/volc.ts`）。
- 未验证：真机复测（预期 `gaps >= 400 ms` 降到 0–1，`heard:` 变成一句完整的话）。
### 2026-10-14 — T023 复测：提示音已排除，剩余断句来自真实静音 → 压缩长静音
- 用户复测（贴回日志，两轮）：`capture report: 4800 ms of audio over 4799 ms wall … longest mid-speech gap 760 ms, gaps >= 400 ms: 1, trimmed 690 ms` 与 `4840 ms of audio over 4839 ms wall … 580 ms … trimmed 1080 ms`；转写 `你。能帮我。做哪些工作？` / `那你。给我介绍。一下哈尔滨。`。麦克风峰值不再出现提示音的 20271 爆点，用户反馈"好一点点"。
- 判断：`robot_audio` 的 `capture_paused` 是"不喂帧"而非"喂静音"，暂停必然让 `audio/wall` 变小；两轮两者相等 → 无丢帧、无暂停，**那段 760/580 ms 静音是房间里的真实安静**，识别端据此断句正确。
- 改动（`components/robot_cloud/robot_voice.c`）：新增 `voice_frame_is_voiced()` 与 `voice_compact_silence()`——把 ≥400 ms 的内部静音压缩到 300 ms（短于 400 ms 的原样保留、尾部余量不动），`capture report` 新增 `squeezed %u ms`。目的：一轮对话合成一句话，既减少识别端在长静音处断句，也避免模型据此吐槽用户"说话一顿一顿"。编译通过（退出码 0）。
- 未验证：真机复测（预期 `heard:` 变成一整句，`squeezed` 显示压缩量）。
- 待评估（本轮未改）：`enable_itn: true` 会把"一下"写成"1下"；关闭与否需真实服务验证后再定。
### 2026-10-15 — T024 一键取消云端回答（按一次就停）
- 用户反馈：机器人说长回答时按一次 BOOT 不好使——"会跳过一段文字继续说剩下的"，要按好几遍才停。
- 定位（读代码）：云端回复是**边收边播**的流；按键路径执行的是 `robot_audio_abort_playback()`，只丢掉**已缓冲**的音频，而 HTTP 流仍在到达并被继续播放 → 观感即"跳一段、接着说"，按几次跳几段。AIUI 路径不受影响（它有自己的 TTS 中断处理与 `discard_tts_until_final` 收敛逻辑）。
- 改动：①`robot_voice.c` 新增 `robot_voice_cancel()` 与 `cancel_requested` 状态；读取循环每次读前检查取消标志，取消时 `robot_audio_abort_playback()`、结束播放、恢复采集，并返回 `ESP_ERR_INVALID_STATE`，`last_result` 记为"已取消"；录音阶段被取消则直接不上传；每轮开始清除残留标志。②`robot_voice.h` 导出该接口。③`robot_app.cpp` 新增 `cloud_voice_turn_busy()`／`cancel_cloud_voice_turn()`（无云端语音的构建下恒为 false，调用点不必写条件编译）；按键判定把"云端一轮进行中"也算忙，`MEDIA_STOP_REQUEST` 分支优先取消云端一轮并**立即**丢弃已排队音频。
- 验证：`robot_voice.c`、`main/robot_app.cpp`、`main/app_main.cpp`（含改动后的 `robot_voice.h`）用 CMake 记录的原始命令编译，全部退出码 0。
- 未验证：真机按键体验（预期：按一次立刻安静且不再继续；控制台指令记录显示"已取消"）。
### 2026-10-15 — T019 会议纪要：进入需求确认（本轮不写业务代码）
- 用户指示"继续文档中的任务"。核对九文档后：T020／T021／T023／T024 均在等用户烧录验证，T022 需浏览器实测；**唯一未开始的开发任务是 T019 会议纪要**。
- 但它**在 PRD 中没有任何 REQ/AC**（需求止于 REQ9 设备绑定），TASKS 里只有方向性描述；且四项决策会直接改变实现形状（录音触发方式、最长时长与音频留存、转写供应商与密钥、声纹换姓名是否第一步做）。按项目规则，本轮**不生成业务代码**，改为给出确认稿并把待确认项写入 TASKS。
- 同时明确两条硬约束：①转写供应商（Moss）的接口文档此前从未读取成功、也没有可用密钥，未拿到前只能做"适配层 + 假实现 + 测试"；②长时间录音需要对象存储（R2），而项目规则禁止创建云资源，因此只写绑定与代码、本地用 Wrangler 本地模拟验证，建桶待用户许可。
- 待用户确认后按序施工：①数据模型与迁移 → ②服务端接口与转写适配层 → ③控制台「会议」页 → ④固件侧长录音。
### 2026-10-15 — T025 部署控制台到 Cloudflare（Worker + D1），线上地址在国内网络不可达
- 用户改变方向："先不做会议纪要，我们先把网站部署一下吧"——这解除了 AGENTS/PRD 中"不得部署、创建云资源"的原有约束（已同步改 AGENTS.md、PRD.md 记录该授权与范围）。
- 只读侦察：wrangler 4.150.0 已登录（OAuth token 存于 `%APPDATA%\xdg.config\.wrangler\config\default.toml`，含 `workers:write`／`d1:write`）；`wrangler.jsonc` 里 D1 `database_id` 仍是全零占位；`dist/` 为最近一次构建产物；**工作区没有 `.git`**，因此本次不存在推送动作。
- 执行（全部在沙箱外联网运行，逐条经用户批准）：
  1. `wrangler whoami` → 账号 `1763584993@qq.com`（Account ID `50c97dd7965ce5c1abc69ad20c3ce5dd`）。
  2. `wrangler d1 list` → 账号内仅有旧项目 `ownerweb`（0 张表），本项目需自建。
  3. `wrangler d1 create aidesk` → `19747cfe-3ef1-44ec-8ece-ea564ac01b40`（WNAM）；把真 ID 替换掉 `wrangler.jsonc` 的占位值。
  4. `wrangler d1 migrations apply DB --remote` → 0001–0005 全部 ✅；随后 `wrangler d1 execute ... --json` 核对到 12 张业务表。
  5. 生产密钥：把本地 `.dev.vars` 的 `ENCRYPTION_KEY`（44 字符 base64，遵守"保留原密钥、禁止重建"）经临时文件重定向写入 `wrangler secret put`（**值不打印、临时文件用完即删**）；`wrangler secret list` 确认存在。
  6. `npm run build`（1578 模块，产物 257.9 KB JS）→ `wrangler deploy` → Worker `aidesk-console`，版本 `d92ab0b5-520e-497b-9289-07c066e777ad`，`https://aidesk-console.1763584993.workers.dev`。
- **验证结果（关键，且是负面结论）**：从本机 `curl` 该地址拿不到任何响应（`http=000`，`Could not connect ... after 21s`）；`Resolve-DnsName` 显示 `aidesk-console.1763584993.workers.dev` 被解析成 **Facebook 的 IP**（`31.13.85.34`、`2a03:2880:f10c:283:face:b00c:0:25de`），而同一时刻 `https://www.cloudflare.com/` 返回 **200** ——这是典型的 **`*.workers.dev` DNS 污染**，不是部署失败。又查了账号 zone 列表：**0 个域名**，所以也无法立刻绑自定义域名绕过。
- 结论与待决：部署本身已完成（Worker/版本/D1/迁移/secret 全部就绪），但**"线上可用"不成立**：本机浏览器与机器人都访问不到该地址。三条出路已写入 TASKS T025（接域名到 Cloudflare 绑 custom domain／用代理仅人可访问／换国内可达托管）。**在此之前不得把控制台记为"已上线"**，也不要让机器人切到该地址（会立刻失联）。
- 文档同步：AGENTS.md（解除部署禁令并记录已创建资源）、PRD.md（状态/变更/不做三处）、TASKS T025、PROGRESS 本条、DEPLOY 实际执行记录与回滚、LEARNINGS 新增"部署成功 ≠ 可访问"。
### 2026-10-15 — T026 源码托管到 GitHub（公开仓库，提交 09a5800）
- 用户要求："这些代码还没有提交到 github，帮我推送到 GitHub"，并给出已建好的空仓库 `https://github.com/litt-s/AIDeskRobot.git`。这解除了 AGENTS/PRD 里"不得推送"的原约束（已同步改 AGENTS.md、PRD.md）。
- 侦察（关键前提）：**工作区没有 `.git`**——不是"没提交"，而是从来没有仓库，因此必须从零 init；`Hardware` 目录 653 MB（其中 `build/` 236 MB、`managed_components/` 409 MB）若不排除会直接毁掉仓库；`Hardware/esp32/sdkconfig` 里有**真实设备码** `CONFIG_ROBOT_CLOUD_KEY`，只应存在于 NVS/配网页。
- 执行：
  1. 补 `.gitignore`：`Hardware/esp32/build/`、`managed_components/`、`sdkconfig`、`sdkconfig.old`、`dependencies.lock`、`tmp-*`（根 `.gitignore` 原本已覆盖 `.dev.vars`／`node_modules/`／`dist/`／`.wrangler/`）；新增 `.gitattributes`（`* text=auto`，仓库内存 LF）。
  2. `git init -b main`；本地身份设为 `litt-s <litt-s@users.noreply.github.com>`（仓库级，用户可自行改）。
  3. `git add -A` → **502 文件 / 8.51 MB**（体积从 653 MB 压到 8.5 MB）。
  4. **提交前密钥扫描**：搜真实设备码与常见密钥模式。真实设备码未出现 ✓；但发现 `Hardware/esp32/components/aiui_sdk/aiui_api_esp.c:823` 与 `Hardware/src/aiui/aiui_api.c:654` 的注释示例里带着一份**疑似真实的 AIUI `appid/key/api_secret`**（随 SDK 附带的示例文本）→ 替换为 `<你的appid>`／`<你的appkey>`／`<你的apisecret>` 占位符（纯注释，功能无影响）。复查索引：不含 `sdkconfig`／`.dev.vars`／`build`／`managed_components`／真实设备码。
  5. 提交 `09a5800 初始提交：AIDesk 机器人控制台（控制台 + ESP32 固件）`；`git remote add origin https://github.com/litt-s/AIDeskRobot.git`；`git push -u origin main` → `* [new branch] main -> main`，跟踪已建立。
- 验证：`git status -sb` = `## main...origin/main`（无分歧）；`git ls-remote --heads origin` = `09a58005c8619f69d992a12c3fa12c0ae9bc098d refs/heads/main`；经本机代理 `curl -x http://127.0.0.1:7897` 探测仓库返回 **200（公开）**。
- 环境事实（值得记住）：本机 git 走 `http.https://github.com.proxy=http://127.0.0.1:7897`（代理必须开着，直连 github.com 返回 000）；推送凭据由系统级 `credential.helper=manager` 提供，无需输入口令；**沙箱把工作区 `.git` 设为只读**，因此 `git add`／`commit`／`push` 必须在沙箱外执行（`index.lock: Permission denied` 就是它的表现）。
- 未验证/待办：`.github/workflows/verify.yml` 尚未在 GitHub 上实际跑过；仓库若需转私有需用户在 GitHub 设置里操作；`ENCRYPTION_KEY` 仍只存在于本地 `.dev.vars` 与 Cloudflare secret（建议用户离线备份）。
### 2026-10-15 — 规则新增：每过一个阶段提交（并推送）一次仓库
- 用户要求："在项目规则上添加，项目每过一个小阶段就提交一次仓库"。
- 落实：`AGENTS.md` 新增提交节奏条款——"一个阶段"＝一个编号任务完成，或一次验收与文档同步结束；提交信息写明任务号与要点；**提交前必做密钥扫描**（见 DEPLOY「源码托管」小节，真实密钥一律不入库）。`DEPLOY.md` 的源码托管小节同步写入该节奏与理由（小阶段提交既是留痕，也是成本最低的一次"改动全貌复查"，同时是回滚的粒度单位）。
- 本条规则从 2026-10-15 起生效；仓库此前只有两个提交（`09a5800` 初始入库、`a242af9` 文档同步），历史提交粒度不再追改。
### 2026-10-15 — T027 提交信息约定（带任务号与要点，便于按任务回溯）
- 用户在原规则基础上进一步要求："一次验收结束，提交信息带任务号与要点，方便日后按任务回溯。"
- 落实：`AGENTS.md` 把提交节奏与**提交信息格式**一并写死——`T0xx <阶段>：<要点1>；<要点2>`；"一个阶段"明确包含**一次验收结束**（含文档同步）；要点写"改了什么／验证到什么"，未验证项必须写明；规则/流程类改动同样给任务号，便于 `git log --grep T0xx` 回溯。`DEPLOY.md` 补上示例与约定。本条改动以 `T027` 为任务号、按新格式提交，作为该约定的第一个样例。