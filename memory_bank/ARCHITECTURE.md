# AIDesk 机器人控制台 ARCHITECTURE

状态：已确认；最后更新：2026-10-15。用户已明确批准全方案并连续执行，每项验收及文档同步后进入下一项。部署、推送与 Cloudflare/CloudBase/PC 桌面等已获授权的范围见 PRD/TASKS/DEPLOY；不得修改 Hardware、MiniProgram、Design；Android 仅按 T036 的明确授权新增。

## 总体
web → /api → Hono 鉴权 → D1；聊天由 Worker 解密凭据并请求 HTTPS provider，前端不可访问凭据。
## 目录
server：鉴权、安全、API、provider 出站策略、设备通道；generation：持久生成状态与流；shared：公开类型；web：React；migrations：SQL；tests：安全/接口/生成/设备；scripts：本地初始化；memory_bank：项目权威记忆。
## 入口
Vite proxy /api → 8787；统一 Worker ASSETS SPA；后续多设备与小程序共享同一套 /api。
## 配置边界
production 未配置密钥时拒绝服务。开发密钥仅示例或 gitignored .dev.vars。
## 错误处理与权限
所有 API 资源按 user_id 授权；未登录401、跨租户404、来源403、频率429。模型密钥只在Worker解密；聊天通过 SSE 流传输，生成状态（running/paused/completed/failed）与片段以 D1 为权威逐条落库，暂停/继续可跨请求与跨页面恢复。server/generation.ts 负责流与状态机，server/chat-routes.ts 负责路由与并发准入，provider 出站仅允许配置域名且拒绝重定向。
来源校验（server/origins.ts）只对带 cookie 的 `/api/*` 生效：同源一律放行；production 除此之外一律拒绝；development 另放行 host 为 loopback 或私有地址字面量的 origin——因为开发机的局域网地址随网络变化，逐个登记会在换网后失效，且失败发生在登录之前、表现像"登录坏了"。`DEV_ALLOWED_ORIGINS` 仍用于该规则覆盖不到的情况（隧道域名等），精确匹配、不支持通配。
## 设备通道
设备以一次性签发的密钥鉴权（服务端只存哈希与掩码），通过 `/api/device/*` 握手、轮询待执行指令并回报结果；指令与状态存 D1，`/api/devices/*` 供控制台管理。该前缀在来源校验与会话鉴权中间件上按路径放行，改由设备密钥与独立限流保护，因此设备请求不需要 Origin 或 Cookie。当前采用轮询而非长连接：Worker 无法把一次 HTTP 请求的消息推给另一条 WebSocket 连接，推送需要 Durable Objects，属后续按需评估项。
设备侧由 Hardware/esp32/components/robot_cloud 实现同一协议：轮询指令、执行后回报、周期上报状态；它只依赖 robot_ui／robot_motion／robot_audio 的既有接口，播报通过 robot_app 注入的回调复用本地语音栈，因此不侵入既有语音链路。
配网是设备侧唯一的配置入口（`robot_network.c` 的 SoftAP + HTTP 服务）。进入方式有三种：保存的 Wi-Fi 连不上时自动进入（等 `ROBOT_STA_CONNECT_TIMEOUT_SECONDS` 超时）、开机后的连接窗口内短按 BOOT（`robot_network_request_reprovision()` 只在 CONNECTING 状态接受）、以及**长按 BOOT ≥800 ms**——最后这条由 `robot_app` 投递 `EventType::PROVISION_REQUEST` 给 worker 处理：会话中先 `StopSession` 再 `robot_network_enter_provisioning()`，已在配网时同一手势会**重申配网**（`robot_network_enter_provisioning()` 在已开页时重新调用 provisioning 回调，把屏幕与提示音再给一遍，不再重启）——早期版本在这里做重启，但"是否在配网中"在 app 与网络两侧可能不一致，重启看起来就是"长按没反应"，因此手势只保留一个含义。退出配网靠保存后自动重启或断电重启。按键语义上长按与短按互斥（`poll_button` 长按触发后不再补发短按），因此短按"唤醒/停止播放"的既有行为未变。
页面本身是**一个表单 + 一个「保存并重启」按钮**：`/save` 把**填了的字段逐个写进**对应的 NVS 命名空间（`wifi_cfg`／`cloud_cfg`），**留空的字段一律不写**，所以改设备码不必动 Wi-Fi、改 Wi-Fi 也不会动设备码；页面顶部显示当前配置，设备码只显示后四位。三条字段规则：①Wi-Fi 名留空＝Wi-Fi 完全不动；②Wi-Fi 名与已存相同且密码留空＝沿用已存密码，换到**别的**网络且密码留空＝按开放网络处理。设备启动时读 NVS，某个字段缺失才回落到固件编译值（`ROBOT_CLOUD_URL`／`ROBOT_CLOUD_KEY`）。
## 语音通道
机器人只有语音输入，所以语音不是网页端的附属功能，而是设备的主交互通道。`/api/device/voice` 用设备密钥鉴权，一次请求完成一轮对话：设备上传一段完整 PCM → Worker 调火山 ASR 转写 → 用设备绑定角色的模型流式生成回复 → 按句切分、逐句调 TTS → 把 PCM 边合成边下发给设备播放。**设备全程不需要 WebSocket**，也不需要理解模型协议，换语音供应商只改 server/voice 的适配层。
分层：`frames.ts` 是火山二进制帧的纯编解码，`stream.ts` 是设备侧 `[类型][长度][负载]` 分帧，两者都不碰网络，因此可离线单测；`pipeline.ts` 负责分句与流水线编排，依赖以接口注入，测试用假实现即可覆盖全部分支；`volc.ts` 是传输层；`routes.ts` 把三者接到 HTTP 与 D1 上。
两个必须记住的约束：①火山 ASR 走 Workers 的 `fetch()` + `Upgrade: websocket`，**URL 必须写 `https://`，写 `wss://` 会被 Fetch API 直接拒绝**；②`AbortSignal` 打不断卡住的握手，所以每一步都用 `withDeadline` 做墙钟兜底，保证设备总能收到明确的失败帧而不是无限等待。
凭据与角色：语音凭据按账号 AES-GCM 加密存储，未配置时回落平台环境变量；设备用哪个角色由控制台的 `devices.role_id` 决定，对话写入该设备专属会话，因此控制台能直接看到机器人聊了什么。
## 设备侧语音
`Hardware/esp32/components/robot_cloud/robot_voice.c` 实现机器人这一端：独立任务等待触发 → 等唤醒提示音播完并解除采集暂停（否则每句话前都会粘上提示音）→ 挂麦克风旁路录音，用能量 VAD 判断说完（见到说话后静音满 900 ms，或到 10 秒上限）→ 把整段 PCM POST 到 `/api/device/voice` → 增量解析同一套分帧，PCM 帧直接 `robot_audio_play_pcm` 边收边播。全程只有一次 HTTP 请求，没有 WebSocket。
两个接口上的取舍：①`robot_audio` 原本**只有一个**麦克风 consumer 槽位，而唤醒词引擎已经占用它，再注册就会把唤醒词顶掉；因此改为在既有 consumer 之后追加最多 2 个旁路（`robot_audio_add_pcm_tap`），主 consumer 的位置与行为完全不变。②robot_app 新增 `RobotAppState::CLOUD_VOICE_ACTIVE`——在这个状态下 PcmConsumer 既不喂唤醒词也不喂别的消费者（录音由旁路完成）。**2026-10-15 起 AIUI 已整体移除**，云端一轮失败时没有任何本地语音栈可回落，机器人保持待机并重新武装唤醒词。
触发方式有两种：控制台设备页的「和它说句话」（`listen` 指令，便于逐步排查音频链路）与唤醒词（`CONFIG_ROBOT_CLOUD_VOICE` 打开时接管）。 取消：BOOT 短按在云端一轮进行中（录音或播报）会走 `robot_voice_cancel()`——`robot_voice` 据此停止录音或截断回复流并丢弃已排队音频、恢复采集，控制台指令记录记为「已取消」；只丢弃本地队列会表现为"跳一段后继续说"，因为回复是边收边播的流。`speak` 指令走 `/api/device/say`（云合成，不录音、不走模型），失败时上报原因，没有本地语音栈可回落。已知限制：BOOT 键在云端一轮进行中不中断（需先用 BOOT 短按取消该轮）。
## 语音失败为什么必须落库
一轮语音对话的失败是**静默**的：设备拿不到音频就不会出声，而失败原因原本只写在设备串口日志里，控制台一个字都看不到。这在真机排查时直接把"能不能修"变成了"能不能看见"。因此 `runVoiceTurn` 会返回 `failure` 字段，路由层无论成败都把结果写进该设备的专属会话（回复或失败原因作为助手消息，失败时 status=failed）。诊断语音问题时应先看这条会话，而不是先怀疑固件。

## 双运行形态（2026-10-15 T034：Cloudflare Worker 与 Node 容器）
`server/` 里所有路由、SQL、鉴权、语音编排都是**平台无关**的——它们只依赖三个能力：`env.DB`（D1 形状）、`crypto`（WebCrypto）、`fetch`。因此"换个托管"不需要重写业务层，只需要在**适配层**补齐这三件事：

1. **数据库**：`server/node/d1-sqlite.ts` 用 `node:sqlite` 的 `DatabaseSync` 实现 D1 里我们真正用到的那一小块——`prepare(sql).bind(...)`、`first()/all()/run()`（返回 `{meta:{changes,last_row_id}}`）、以及 `batch()`（包在 `BEGIN/COMMIT/ROLLBACK` 里）。**故意只实现用到的部分**：接口面越窄，"D1 与 SQLite 行为不一致"能藏的地方就越少；写超范围的方法会立刻编译/运行失败，而不是静默返回怪结果。
2. **语音识别**：Worker 版靠 `fetch(..., {headers:{Upgrade:'websocket'}})` 拿 `response.webSocket`——这是 Workers 专有语义，Node 没有。`server/node/asr-ws.ts` 用 `ws` 直连同一端点，**帧协议复用 `server/voice/frames.ts`**（与 Worker 版同一份编解码实现，对照已真机验证过 `scripts/volc-voice-smoke.mjs`）。`volc.ts` 里加了 `setTranscriber()` 注入点：Worker 不注入（保持原路径零改动），Node 启动时注入。**TTS 不需要适配**——那是普通 `fetch` 流式响应，两边一样。
3. **静态资源**：Worker 由 `wrangler.jsonc` 的 `assets`（`not_found_handling: single-page-application`）兜底；Node 版在 `server/node/index.ts` 里读 `dist/`：**没有扩展名的路径**回落 `index.html`（前端路由），**有扩展名却找不到**则返回 404（否则拼错的文件名会拿到一段 HTML，浏览器报 `Unexpected token '<'`，把真正的原因藏起来）。

启动顺序（Node 版）：建/打开 SQLite → `PRAGMA foreign_keys=ON`／`busy_timeout` → 跑 `migrations/*.sql`（记在 `_migrations` 表里，**跑过就不跑**——`ALTER TABLE` 不幂等，不能靠 SQL 自己保证）→ **可选播种**（`seed.sql` 存在且 `users` 表为空时执行一次，见 DEPLOY「从已有数据播种」）→ `setTranscriber()` → `serve()`。数据文件默认 `./data/aidesk.sqlite`（可用 `DB_PATH` 覆盖），**该目录必须挂持久卷**，否则容器发布即丢库。

限流按来源 IP 分桶，Worker 读的是 `CF-Connecting-IP`；容器里由入口把 `X-Forwarded-For` 的第一个地址补成这个头（容器只经平台网关暴露），业务代码因此一行不用改。


## Android 客户端架构（T036）
`Android/app/src/main/java/cn/aidesk/console/MainActivity.java` 是单 Activity 壳：无地址时渲染原生服务器设置页，有地址后创建 `WebView` 并加载用户保存的控制台 URL。WebView 启用 JavaScript/DOM Storage/Database，CookieManager 接受并 flush Cookie，因此登录态、SPA 状态与网页端完全同源。`shouldOverrideUrlLoading` 只允许当前控制台 origin 内部导航，外部 origin 交给系统浏览器，避免服务器地址被页面任意替换。Activity 生命周期里暂停/恢复 WebView、持久化 Cookie；返回键优先执行 WebView 历史。业务 API、聊天 SSE、设备轮询与语音设置没有任何 Android 副本，仍由 `server/**` 提供。

## PC 桌面端架构（T037）
`desktop/main.cjs` 是 Windows Electron 壳：`contextIsolation:true`、`sandbox:true`、`nodeIntegration:false`，不做业务计算，只负责单实例、窗口、安全导航、本机 Node 子进程生命周期和远端/本机双模。本机模式用 `ELECTRON_RUN_AS_NODE=1` 启动 `dist-server/index.mjs`，因此复用 T034 的 Node 容器代码路径（同一套 `node:sqlite` 包装、`ws` ASR、静态资源 SPA 回落、迁移与播种逻辑）；窗口同源加载 `http://127.0.0.1:<PORT>`。远端模式置 `AIDESK_SERVER_URL` 后直接加载既有 URL，与浏览器打开同一地址等价。

PC 端把数据库与 AES-GCM 密钥放在 `%APPDATA%\aidesk-console\`：SQLite 文件是本机数据卷，`encryption.key` 首次启动生成 32 字节 base64 后存盘并 `chmod 600`。设备协议仍走 `server/device-routes.ts`，机器人可指向 `http://<PC-IP>:<PORT>`；`server/` 没有任何业务副本。窗口只允许同源内部导航，外部链接交给系统浏览器。

## T047 PostgreSQL 持久化准备（2026-10-27）

用户授权并确认立即切换。现有 CloudBase 上海环境已开通共享 PostgreSQL；已新增 aidesk 独立 schema 和仅供 service_role 调用的 HTTPS 事务函数，15 张原有业务表逐字段校验一致。保留原账户、设备码哈希和凭据密文，沿用原 ENCRYPTION_KEY。npm run check：105/105 测试、类型检查、Web/Node 构建通过；发布与重启持久化验收尚待完成。详见 docs/数据库持久化迁移.md。自动拉库任务保持禁用。
