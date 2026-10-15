# AIDesk 机器人控制台 DEPLOY

状态：已确认；最后更新：2026-10-13。用户已明确批准全方案并连续执行 T001–T006，每项验收及文档同步后进入下一项。禁止部署、推送、创建云资源；不得修改 Hardware、Android、MiniProgram、Design。

## 部署目标
Cloudflare Worker（`aidesk-console`，同一 Worker 同时提供 SPA 静态资源与 `/api`）+ D1（`aidesk`）。**2026-10-15 已实际部署**，细节见文末「实际执行记录（2026-10-15）」；源码托管已于同日推送到 GitHub（公开仓库，见下「源码托管」小节）。
## 本地初始化
npm install；npm run setup:local；npm run db:migrate；npm run dev:worker；另开 npm run dev。
## 产物
npm run build → dist；wrangler assets binding ASSETS，SPA fallback。
## 密钥
本地 `.dev.vars` gitignored；`.dev.vars.example` 是公开开发用示例，禁止用于生产。生产密钥：`ENCRYPTION_KEY` 已于 2026-10-15 用 `wrangler secret put` 写入线上（值取自本地 `.dev.vars`，**沿用不重建**，经 stdin/临时文件重定向写入，从不出现在命令行、文档或前端）。**该密钥丢失会导致库内模型密钥与语音凭据无法解密**，请另行离线备份；轮换需同时重新加密既有凭据。
## 发布流程（已执行，可重跑）
```
wrangler d1 create aidesk                    # 已执行 → database_id
# 把该 id 写入 wrangler.jsonc 的 d1_databases[0].database_id
wrangler d1 migrations apply DB --remote     # 已执行：0001–0005
wrangler secret put ENCRYPTION_KEY           # 已执行：值从 stdin 读，不要在命令行明文写
npm run build && wrangler deploy             # 已执行
```
再次发布只需最后两步（`npm run build` → `wrangler deploy`）；新增迁移后再跑一次 `d1 migrations apply DB --remote`。
## 源码托管（GitHub，2026-10-15 执行）
- 仓库：`https://github.com/litt-s/AIDeskRobot.git`（**公开**），分支 `main`，初始提交 `09a5800`（502 文件 / 8.51 MB）。本工作区此前**没有 git 仓库**，故本次从零 `git init`。
- 忽略规则（必须保留，否则仓库会从 8.5 MB 涨到 650 MB+）：`Hardware/esp32/build/`、`Hardware/esp32/managed_components/`（`idf.py` 可重新生成）、`Hardware/esp32/sdkconfig`／`sdkconfig.old`（**含本机真实设备码 `CONFIG_ROBOT_CLOUD_KEY`**）、`dependencies.lock`、`tmp-*`；根 `.gitignore` 另覆盖 `.dev.vars`／`node_modules/`／`dist/`／`.wrangler/`。
- **每次提交前的密钥扫描（强制步骤）**：`git grep --cached` 搜真实设备码与常见密钥模式（`ENCRYPTION_KEY=`、`appkey`／`appsecret`／`api_key` 的字面量、长 base64/hex）。本次扫到并处理：`aiui_sdk` 的注释示例里带有一份疑似真实 AIUI `appid/key/api_secret`，已替换为占位符。**扫描必须覆盖 vendored 第三方代码**，不能只看自己写的文件。预期命中一处：`.dev.vars.example` 里的 `ENCRYPTION_KEY=AAAA…A=` 是公开占位示例，不是真实密钥；除此以外的命中都要立刻停下核查。另有一条**固定误报**：用 `sk-` 这类短前缀正则会命中本项目自己的名字（`aide**sk-**console`、`aidesk-<日期>.sql`）——看到一片 `sk-` 命中先确认是不是这个，再决定是否入手核查。
- **提交节奏（项目规则）**：每过一个阶段提交并推送一次——一个编号任务完成、或一次验收与文档同步结束即提交；提交信息写明任务号与要点。这不只是留痕：`git diff` 是小阶段验收之外最便宜的一次"改动全貌复查"，也是回滚的粒度单位。
- **提交信息约定（便于按任务回溯）**：`T0xx <阶段>：<要点1>；<要点2>`，例如 `T023 验收：真机 capture report 无丢帧；长静音压缩生效`。要点写"改了什么／验证到什么"，空话（如"更新了文件"）不写；未验证项必须写明（如"未验证：真机"）。一个提交只归一个任务号；规则/流程类改动也给一个任务号，便于日后 `git log --grep T0xx` 直接回溯。
- 后续更新流程：`git add -A` → 密钥扫描 → `git commit -m …` → `git push`。本机 `github.com` 直连不通，git 通过 `http.https://github.com.proxy=http://127.0.0.1:7897` 走本地代理（代理需在运行）；凭据由系统级 `credential.helper=manager` 提供。
- **CI 一直在跑且是绿的（2026-10-15 核实，推翻此前「尚未实际运行」的说法）**：`GET /repos/litt-s/AIDeskRobot/actions/runs` → `total_count = 13`，自首个提交 `09a5800` 到 `012daeb` **全部 `conclusion=success`**；workflow `Verify console` 状态 `active`，每次推送约 30 秒（首次 `2026-10-14T16:29:56Z`）。手动查历史：`curl -sS --proxy http://127.0.0.1:7897 https://api.github.com/repos/litt-s/AIDeskRobot/actions/runs`。CI 步骤：`npm ci` / typecheck / build / 本地迁移 / 起 `dev:worker` 跑 `npm test` / `npm audit --audit-level=high`。
## 备份与回滚（2026-10-15 实测）
**纪律：每次 `wrangler d1 migrations apply DB --remote` 之前、以及每次上线前，先导出一份快照。** 线上库现在虽是空的，但机器人一旦开始上报数据，误改就没得救。
- **导出快照**（本机经代理；`wrangler` 必须能连上 `dash.cloudflare.com`，登录有效期见本节末）：
  ```
  $env:HTTPS_PROXY='http://127.0.0.1:7897'; npx wrangler d1 export aidesk --remote --output D:\AIDeskRobort\backups\aidesk-<日期>.sql
  ```
  导出期间 D1 **短暂不可用**（wrangler 会先提示），别在设备在线上报时跑。
- **恢复**：导出文件是「`CREATE TABLE IF NOT EXISTS` + `INSERT`」，只能灌进**空库或已清空的库**（主键冲突会中断）：
  ```
  npx wrangler d1 execute aidesk --remote --file D:\AIDeskRobort\backups\aidesk-<日期>.sql
  ```
  只恢复某张表，就从 dump 里摘出该表的 `CREATE` + `INSERT` 单独执行；schema 回滚仍要手工写反向 SQL。
- **Worker 版本回滚**：`npx wrangler rollback [version-id]`（或 Dashboard → Workers → Deployments → Rollback）；也可以直接 `npx wrangler deploy` 重新推一份旧代码。密钥不回滚（轮换后旧数据无法解密）。
- **快照是敏感文件**：内含 `users`（用户名／口令哈希／盐）、`devices`（设备密钥哈希与掩码）、`models`（供应商 API key 的密文）。`backups/` 已加入 `.gitignore`，**不得提交、不得外发**。
- **实测（2026-10-15）**：`backups/aidesk-20261015.sql` 4,541 字节；14 张表、5 条 `d1_migrations`（0001–0005）、业务表 0 行。用本机 Python `sqlite3.executescript()` 回灌临时库**成功**（14 表全部建起、行数与 dump 一致）——「导出文件可恢复」已实测；**「远程恢复」未实测**（不做破坏性试验）。另用 `d1 execute --remote` 的标量子查询交叉核对远端行数，与 dump 一致，排除「导出被截断」。
- **重要事实：线上 D1 目前是空库**——开发期间的数据（58 个用户等）全在本地 `.wrangler` 开发库里，线上只有表结构与迁移记录。因此**上线后第一个注册的账号就是线上管理员**，「是否放开注册」这件事必须在放机器人上线前定。
- **认证注意**：`npx wrangler login` 的 OAuth 回调只等 **120 秒**，浏览器弹出后要立刻点 Allow，超时报 `Timed out waiting for authorization code` 就重跑一次；OAuth token 过期后连 `d1 export` 都跑不动（报 `non-interactive environment … CLOUDFLARE_API_TOKEN`），重新 `login` 即可恢复。
## 当前状态
2026-10-15 **已部署但线上不可达**：Worker `aidesk-console`（版本 `d92ab0b5-520e-497b-9289-07c066e777ad`）、D1 `aidesk`（`19747cfe-3ef1-44ec-8ece-ea564ac01b40`，区域 WNAM，12 张业务表；**2026-10-15 复核：业务表全部 0 行**，只有表结构与迁移记录，详见「备份与回滚」）、secret `ENCRYPTION_KEY` 均就绪；但 `https://aidesk-console.1763584993.workers.dev` 在国内网络**不可访问**——`workers.dev` 的 DNS 被污染（解析到 Facebook 的 `31.13.85.34`），同一时刻 `https://www.cloudflare.com/` 返回 200；账号内 zone 数为 0，无法立即绑自定义域名。**因此不得记为「已上线可用」，也不要让机器人把服务器地址改成它**（会立刻失联）。GitHub 未推送（工作区无 git 仓库）。

## 本地测试
先运行 `npm run setup:local`、`npm run db:migrate`、`npm run build`，再启动 `npm run dev:worker`，另一个终端运行 `npm test`。集成测试需要 localhost:8787 的本地 Worker；使用随机测试账户和虚构key，不访问真实provider。`npm run dev` 使用5173代理8787；development只允许该本地Origin。

## 上线验收
生产设置随机32字节base64 ENCRYPTION_KEY，禁止开发示例key；替换D1占位ID，远程migration后验证注册/隔离/真实provider。ENCRYPTION_KEY变更会使旧模型key无法解密，需规划重加密。

## 迁移与暂停续写
远程 D1 需应用 migrations/0003_generations.sql（本地已应用）。该表保存生成状态、token、lease 与已生成片段，是暂停/继续的权威来源；缺少它时暂停与续写接口不可用。迁移前先备份远程库。

## 局域网与模型域名
局域网访问的开发来源校验：development 下本机地址（loopback 与 10/8、172.16/12、192.168/16、169.254/16 的地址字面量）**自动放行**，不需要逐个登记，因此换 Wi-Fi 或开热点后 IP 变了也能直接用。`DEV_ALLOWED_ORIGINS` 仍然保留，用于这条规则覆盖不到的情况（隧道域名、解析到内网地址的主机名），写法是完整 origin、逗号分隔、精确匹配。**不可使用通配**；production 不采用这一整段逻辑，只接受同源。保留原ENCRYPTION_KEY，禁止重建。PROVIDER_ALLOWED_HOSTS为管理员审批的精确HTTPS域名列表，逗号分隔；默认api.deepseek.com,api.openai.com。自定义供应商必须先审批域名后加入，不支持用户自行授权域名。白名单内域名仍需管理员确认所有权与解析可靠。

## 固件接入设备通道（T016）
1. 在控制台"设备"页绑定设备，复制一次性密钥。
2. 进入 `Hardware/esp32`，`idf.py menuconfig` → "DeskEmoji cloud device channel"（菜单名保持英文）：打开 `ROBOT_CLOUD_ENABLE` 即可。**`ROBOT_CLOUD_URL` 与 `ROBOT_CLOUD_KEY` 建议留空**，改在配网页填（见下），这样固件镜像里不含任何凭据。两项若填了，只作为配网页没提供时的兜底。
   注意：本工程的 Kconfig 菜单名与选项名必须保持 ASCII。esp-sr 的 movemodel.py 用系统默认编码读取 sdkconfig，中文 Windows 下为 GBK，菜单名一旦含中文会导致固件构建报 UnicodeDecodeError。
3. `idf.py build` 并烧录；启动日志出现 `device channel online` 即握手成功。
4. 默认关闭该开关时固件行为与原来完全一致；密钥会被编译进固件，含真实密钥的 `sdkconfig` 不得提交到公开仓库。
5. 已知限制：设备待机时"播报"仍可用（走云合成 `/api/device/say`，不需要本地会话）；表情、动作、音量不受此限制。
6. **2026-10-15：AIUI 已整体移除**（组件、SDK 源码、会话层、Opus 路径、配网页 AIUI 段与 `aiui_secret`、`ROBOT_ALLOW_VOICELESS`、媒体/音乐与桌面 demo 全部删除）。固件现在只需要控制台的服务器地址与设备码，语音一律走云端通道；本节以下凡提到「AIUI 凭据」「内置语音栈」的段落均为历史记录。

## 在配网页填服务器地址与设备码（T016 新增，T020 起为逐字段保存）
**怎么进入配网页**（三种，任选）：
1. **长按 BOOT 键约 1 秒**——设备**已经联网时也能用**，这是日常最方便的入口（机器人播报配网提示音、屏幕滚动显示热点名与 192.168.4.1）；在配网页里**再长按一次 BOOT＝重申配网**（屏幕与提示音再提示一遍，用于确认"我确实在配网模式里"）。**退出配网**：保存任何一项后设备会自动重启并联网；什么都没改就想退出，直接断电重启即可。
2. 开机后、还没连上网络的几秒内短按 BOOT（连接窗口内短按＝请求重新配网）。
3. 保存的 Wi-Fi 连不上时自动进入（等待 `ROBOT_STA_CONNECT_TIMEOUT_SECONDS`，默认 60 秒超时后进入）。

页面是**一个表单、一个保存按钮**（按钮文字「保存并重启」），按三段排版方便阅读：

- **Wi-Fi（可选，不想改就别填）**：从扫描列表选网络，或用折叠区手动输入名称；密码填在下面。
- **控制台设备通道**：服务器地址、设备码。

**规则一句话：填了就保存，没填就保持原值。**
- Wi-Fi 名**留空＝Wi-Fi 完全不动**（只改设备码时，整段 Wi-Fi 都不用碰）。
- Wi-Fi 名与当前相同且密码留空＝**沿用已保存的密码**（只改密码：选同一个网络、只填新密码）。
- 换到**别的网络**时请照实填名称与密码；新网络是**开放网络**则密码留空（此时按开放网络处理，不会沿用旧密码）。
- 服务器地址与设备码各自独立，只填其中一个就只改那一个。
- 所有输入框都空着点保存＝回「没有改动」，**不会重启**。
- 保存成功的回执会写明这次实际保存了什么（例如「已保存：设备码」），便于确认"没动的项确实没动"。

### 服务器地址该填什么（本地开发 vs 上线后）
- **补全规则（写入端＋使用端都做）**：保存时若没写 `://` 就补上；设备启动读地址时再补一次（IP 字面量或无点的主机名补 `http://`，其它名字补 `https://`，已经带 scheme 的原样保留）。**使用端这一层是给存量数据兜底的**：配网页只补写入端的版本上线前存进去的裸地址，不会因为重新烧录而变好，除非再保存一次；现在重烧后旧值会自愈。
- **本地开发**：填控制台 **Worker** 的地址，形如 `http://192.168.137.1:8787`。两个易错点：①**必须带 `http://`**——少了它 ESP-IDF 的 HTTP 客户端会直接报 `Error parse url ...`，设备看着在线却永远连不上控制台（现在保存时与设备读取时都会自动补上 `http://`）；②**端口是 8787，不是前端页面的 5173**，5173 只是 Vite 开发服务器（`npm run dev`），设备要直连 Worker（`npm run dev:worker`）。
- **上线之后不需要在配网页填**：域名是固定的，写进固件更省事——`idf.py menuconfig` → `ROBOT_CLOUD_URL` 填 `https://你的域名`，配网页「服务器地址」永远留空即可。优先级是"配网页填了就用配网页的，没填才用固件里编译好的"，所以两种做法都成立：
  - 推荐：域名写进固件（出厂即有地址，量产设备不必逐台填）；
  - 或者：固件留空、每台设备在配网页填一次域名（换域名时不必重新烧录）。
- **设备码与地址不同**：它是每台设备唯一的（控制台绑定设备时一次性签发），天然属于"逐台配置"，不能编进通用固件。
**推荐用它，而不是 menuconfig**，原因有二：控制台跑在电脑上，其局域网地址每换网络就变，地址编进固件的话一变就得重新编译烧录；而设备码写进 `sdkconfig` 会进固件镜像，是个需要时刻提防提交到仓库的凭据。填在 NVS 里则两者都不成问题，手机 30 秒就能改完。

行为要点：
- **逐字段生效**：只填服务器地址也能存下并生效，不必为了跟着 IP 变而重敲设备码；留空的字段不进 NVS。
- **回落次序**：设备启动时先读 NVS；某个字段在 NVS 里没有，才用固件里编译好的 `ROBOT_CLOUD_URL`／`ROBOT_CLOUD_KEY`。因此**从未配过网的设备行为与改动前完全一致**。
- **页面顶部显示当前配置**：Wi-Fi 名 **＋ 密码是否已保存**、服务器地址、设备码掩码（只显示后四位），便于确认"我现在改的是哪一段"。看到 `（未保存密码）` 就说明 NVS 里密码是空的——此时即使"留空沿用已存密码"也仍是空，必须手动填一次密码才能连上。
- **排查用的一条判据**：BOOT **短按能把设备带进配网，等于设备此刻没连上 Wi-Fi**。短按只在 `robot_network` 处于"正在连接"状态被接受（`robot_network_request_reprovision()` 的唯一条件），联网成功后短按只会唤醒/停止播放。所以"点一下也进配网"应先怀疑凭据或网络，而不是按键逻辑。日志上：连不上的设备在超时后会打印 `saved Wi-Fi did not connect within … seconds`，随后是 `provisioning: SSID=… URL=192.168.4.1`；连上则会出现 `device channel online`。
- **改完必须重启**（按钮会自己重启），因为三类配置都在启动时读取一次；若某段字段全空，页面会回"没有改动"且**不重启**。
- 设备码与服务器地址对大小写敏感。配网页对这两个输入框关掉了手机键盘的自动大写与自动纠错，但仍建议照抄控制台显示的值。

## 无硬件验证设备通道（模拟器）
固件未编译或未烧录时，可用 device-sim.mjs 走同一套协议验证服务端：先在控制台绑定设备并复制短码，再启动服务端与前端，然后运行

```bash
DEVICE_URL=http://127.0.0.1:8787 DEVICE_KEY=你的短码 npm run device:sim
```

（Windows PowerShell 用 `$env:DEVICE_URL='...'; $env:DEVICE_KEY='...'; npm run device:sim`，或写成 `npm run device:sim -- http://127.0.0.1:8787 短码`。）

控制台设备页此时应显示在线；在页面上点表情／动作／音量／播报，模拟器终端会打印收到的指令并回报"模拟设备已执行"，设备页的指令记录变为"已完成"。这验证的是绑定、鉴权、在线判定、指令下发与回报；真机的表情、舵机与语音播报仍需烧录后验证。

## 开启语音对话闭环（T018，固件侧）
前置两项，缺一会被服务端以 409 拒绝（设备侧只回报原因，不会有音频）：
1. 控制台「语音设置」页填入火山引擎语音 API Key（音色与两个资源号可用默认值）。填好后页面上方卡片会显示"正在使用你的语音凭据"。
2. 控制台「设备」页，在设备卡片下方的「语音角色」下拉里选一个角色。换角色会清空该设备已有的对话，让新角色从干净上下文开始。

（可选）平台级回落：若希望没配凭据的账号也能用，可在服务端 `.dev.vars`／Worker 变量里提供 `VOLC_API_KEY`、`VOLC_ASR_RESOURCE`、`VOLC_TTS_RESOURCE`、`VOLC_TTS_SPEAKER`。账号自己的凭据优先；未配置时页面会显示"正在使用平台默认语音凭据"。**这四个变量只放在服务端，不要写进前端或提交到仓库。**

固件侧：`idf.py menuconfig` → "DeskEmoji cloud device channel" → 打开 `ROBOT_CLOUD_VOICE`（默认关闭；它依赖 `ROBOT_CLOUD_ENABLE`，所以设备通道也要一起打开）。有四项可调：
- `ROBOT_CLOUD_VOICE_MAX_RECORD_MS`（默认 10000）一句话的硬上限，也决定录音缓冲区大小（10 秒约 320 KB，取自 PSRAM）。
- `ROBOT_CLOUD_VOICE_SILENCE_MS`（默认 900）说完后静音多久就送出去。调小更跟手，但说话慢的人会被截断。
- `ROBOT_CLOUD_VOICE_THRESHOLD`（默认 **700**）低于此电平视为静音。环境吵就调高；如果机器人总在你没说完时就停下，调低。**这个数是绝对电平，只有和采集增益放在一起才有意义**——`ROBOT_MIC_SHIFT` 一动，它必须按同一倍数跟着动。它原先的默认值 320 是在增益还错误地放大 16 倍时选的，增益修正后正常说话均方根只有 1500–2300，320 太容易越过，导致每轮都录满 10 秒上限；700 大约在说完话一秒后收尾。**注意现有 `sdkconfig` 里若已有这一项，改默认值不会自动生效，需在 menuconfig 里显式改。**

`idf.py build` 后烧录。两种触发方式：
- **先验证音频链路**：控制台设备页点「和它说句话」，或用串口手动触发。设备开始收音，说完停一会儿就会回答。这条路径与唤醒词无关，适合先排除音频问题。
- **唤醒词**：打开 `ROBOT_CLOUD_VOICE` 后，唤醒词会开启一轮云端对话（用的是控制台里选的角色），而不是内置技能；如果语音通道没准备好（例如还没填凭据），机器人保持待机并重新武装唤醒词——AIUI 已移除，没有本地语音栈可回落。

串口日志依次出现 `heard: …`（识别文本）、`answer: …`（回复文本）即链路正常；`cloud voice turn finished: …` 会带上失败原因。**排查语音问题时先看设备专属对话**：每次尝试无论成败都会在那里留下一条记录（听到的话 + 回复，或失败原因 + 收到音频的时长/峰值/有声占比/削顶/过零），比串口日志好找得多，也不必远程连串口。已知限制：AIUI 会话进行中再按「和它说句话」会让两边同时占麦；BOOT 键不会中断云端一轮。

**2026-10-14 真机验证通过**：连续三轮对话（「测试。」「你好。」「麦克风。采样。率测试。」）均被正确识别并正常回复、正常播报，T014–T018 阶段收尾。定位过程中修掉的两个真实缺陷值得记住：**`ROBOT_MIC_SHIFT` 必须为 16**（12 会把采集增益放大 16 倍，满量程削顶，识别必然为空），以及**客户端的 WebSocket 消息类型处理**（见下）。

**若「说话」一直报"没有听清"**：先看记录里括号中的六个数字。它们区分的几种情况从外部看一模一样，修法却相反。
- `峰值 0` / `有声占比 0%` → 麦克风根本没采到声音。注意唤醒词引擎是麦克风唯一的本地消费者（AIUI 已移除），没有唤醒词初始化时麦克风不会被任何功能使用，静音也不会有人发现。串口里会每秒打印一行 `microphone peak=…`，对着麦克风说话看它是否跳动。
- `峰值 32768`（或 32767）且 `均方根` 上万 → **不是没声音，是信号大到削顶**。麦克风在工作，问题在增益或数据本身。此时再看后两个数：
  - **`过零` 个位数（1% 上下）而 `削顶` 一两成** → 真实波形被放大过头。这是**实测确认过的那一种**：24000 Hz 下量到 `峰值 32768、均方根 18552、有声占比 99%、削顶 16%、过零 1%`。把 `ROBOT_MIC_SHIFT` 从 12 调到 **16** 即可。
  - `过零` 约 50% 而 `削顶` 接近 0% → 送进来的**根本不是语音，是噪声**。相邻采样互不相关才会有一半时间在变号。这指向输入脚浮空／接错，或送进缓冲区的根本不是麦克风数据。
  - `过零` 低但 `削顶` 极高（几成以上）且均方根接近满量程 → 过驱得比 16 倍还狠，或真实信号叠了低频干扰。
- `峰值` 有几千但 `有声占比` 很低 → 采到了环境声但没抓到说话人，多半是录音起止时刻不对（VAD 阈值或提示音时长需要调）。
- 若电平恒为 0，先量麦克风第 6 脚（L/R 声道选择）对地是否导通。该脚接 GND 表示数据在左声道，与固件默认一致；若它接的是 3.3V，数据在右声道而固件读的是左声道，结果就是**全静音且不报任何错**。这种情况在 menuconfig 里打开 `ROBOT_MIC_RIGHT_CHANNEL` 即可。

**判读这两个数的原理**（为什么它们能分开）：`削顶` 是样本落在 ±满量程的比例，`过零` 是相邻样本变号的比例。噪声互不相关 → 过零必然约 50%；真实语音即使被削成方波，仍保留 100–200 Hz 的基频周期性，过零会塌到基频速率（个位数百分比）。另外**纯正弦／工频干扰也会被排除**：它的均方根/峰值比恒为 0.707 且不会削顶，而实测是 0.566 且削顶 16%。

**若电平已经正常（削顶 0%）却仍报"没有听清"**：不要再调增益，问题不在音频。依次看：
- 记录里出现 **「语音识别没有收到任何结果帧」** → 客户端没读懂服务端响应（而不是服务端说没听到人说话）。这两件事修法相反，代码里已把它们分开报。历史原因：`volc.ts` 的消息处理原先只认 `ArrayBuffer` 和字符串，其余形态被静默变成空数组，而 WebSocket 规范里 `binaryType` 默认是 `"blob"`。现已由 `frames.ts` 的 `messageBytes()` 显式处理 `ArrayBuffer`／视图／字符串／`Blob`。
- 记录里仍是普通的"没有听清"（**没有**那句"没有收到结果帧"）→ 响应帧收到了，是服务端在我们的音频里没找到人声。此时查**声明的采样率**：设备通道把设备上报的率原样声明给火山（`volc.ts` 的 `audio.rate`），而**只有 16000 对火山验证过**，官方要求也是 16 kHz。把 `ROBOT_MIC_SAMPLE_RATE` 改回 16000 再试。
- 两条都排除了再回头查录音内容本身（是否只是环境噪声、是否 10 秒里绝大部分没有人在说话）。

**麦克风引脚**（板载 I2S MEMS 麦克风，与固件一致）：SCK=GPIO5、WS=GPIO4、SD=GPIO6，VDD=3.3V，GND，第 6 脚 L/R 为声道选择。采样 16 kHz / 16 位 / 单声道。

**麦克风采样率可配**：menuconfig → "DeskEmoji robot" → `ROBOT_MIC_SAMPLE_RATE`（默认 16000，范围 8000–48000）。它只影响**采集**通道；回放固定 16 kHz，因为服务端按 16 kHz 合成语音，两端必须一致（采集与回放是两条独立 I2S 控制器，可各自独立）。改这个值不需要动服务端：固件会在识别请求里带上 `X-Sample-Rate`，服务端据此声明，两侧不会漂移。

本工程板级源码来自的原始固件（同板型）用的是 **24000**，其余 I2S 字段（端口、MONO、LEFT、32BIT、`bit_shift`、`left_align`、`>> 12`）与当前固件**逐字相同**。2026-10-14 用户实测跑了一轮：采样率**不是**原因——收到的音频不是静音而是满量程削顶（峰值 32768、均方根 18252、有声占比 99%），判读见上表。

**采集位移（= 采集增益）可配**：menuconfig → "DeskEmoji robot" → `ROBOT_MIC_SHIFT`（默认 12，范围 8–16）。I2S 每个采样给一个 32 位槽，而麦克风只驱动其中高位，所以要先右移再 clamp 成 16 位；这个位移就是采集增益，也是决定音频能不能用的那一个数。**收到语音报 `峰值 32768` 就把它调大 4**（12 → 16）；报 `峰值` 只有几百就调小。启动日志会打印一行 `microphone capture: <采样率> Hz, <声道> channel, raw>><位移>`，把当前生效的三项配置一起写出来——电平读数只有连同产生它的固件配置一起看才有意义。

**参考固件为什么"能用"**：原固件把麦克风数据送进 AFE 音频前端（AEC／降噪／VAD，语音通信模式还含 AGC），本项目云端通道则**直接上传原始 PCM**，没有任何前端处理。所以即使音频过驱，AGC 会把原固件的电平拉回来、唤醒词对中等削顶也较宽容；本通道会把削顶原样送到识别服务。这解释了两种固件在同一块板上的表现差异，但**不足以据此断定增益一定过大**。

「播报」在有 `ROBOT_CLOUD_VOICE` 的固件上走 `/api/device/say`（只做云合成，不录音、不走模型），在任何固件上都能出声；通道不可用时上报失败原因（AIUI 已移除，没有本地语音栈可回落）。

## 语音链路烟测（火山豆包，实现 T018 之前）
前置：账号必须在火山控制台**开启 Agent Plan 抵扣**（为账号或席位启用 deduction），否则调用会被拒绝，错误为 `403 Forbidden.AgentPlanDeductNotEnabled`。未开启前无需排查代码。
在实现语音闭环前，先用真实 Key 验证火山 ASR 与 TTS 是否可用、延迟多少、音频格式是否匹配：

```bash
VOLC_API_KEY=你的key npm run volc:smoke
```

（Windows PowerShell：`$env:VOLC_API_KEY='你的key'; npm run volc:smoke`。可选 `VOLC_ASR_WAV=D:\some\16k-mono.wav` 指定识别输入；不指定时脚本会先用 TTS 合成一句 PCM、包成 WAV 再送去识别，一次运行验证两条链路。）

脚本会打印 TTS 首块与总耗时、ASR 首个结果与最终结果耗时、以及识别文本。判读方式：TTS 首块时间 ≈ 机器人开口延迟；ASR 最终结果时间 ≈ 每轮识别耗时；若识别文本明显偏离合成内容，说明音频格式或采样率需要调整。**Key 只放环境变量，不要写进命令行历史、文档或仓库。**
## 实际执行记录（2026-10-15）
- 账号：`1763584993@qq.com`（Account ID `50c97dd7965ce5c1abc69ad20c3ce5dd`），wrangler 4.150.0，OAuth 登录（凭据在 `%APPDATA%\xdg.config\.wrangler\config\default.toml`）。
- `wrangler d1 list` → 账号内仅有旧项目的 `ownerweb`（0 表），故新建本项目库。
- `wrangler d1 create aidesk` → `19747cfe-3ef1-44ec-8ece-ea564ac01b40`（WNAM）；真 ID 已写入 `wrangler.jsonc`（原为全零占位）。
- `wrangler d1 migrations apply DB --remote` → 0001–0005 全部 ✅；`d1 execute ... --json` 复核到 12 张业务表：users、sessions、models、roles、conversations、messages、chat_generations、chat_locks、devices、device_commands、voice_settings、rate_limits。
- `wrangler secret put ENCRYPTION_KEY`（值源自本地 `.dev.vars`，经临时文件重定向写入，事后删除；`wrangler secret list` 确认存在）。
- `npm run build`（1578 模块；`dist/index.html` + `index-*.css` + `index-*.js`）→ `wrangler deploy`：上传 3 个资源、总上传 149.67 KiB（gzip 37.03 KiB），Worker 启动 4 ms；绑定 `env.DB (aidesk)`、`env.ASSETS`、`env.APP_ENV ("production")`；地址 `https://aidesk-console.1763584993.workers.dev`，版本 `d92ab0b5-520e-497b-9289-07c066e777ad`。
- **可达性验证（负面结论，重要）**：`curl -sS --max-time 25` → `http=000`、`Could not connect ... after 21036 ms`；`Resolve-DnsName` 把该域名解析到 `31.13.85.34` 与 `2a03:2880:f10c:283:face:b00c:0:25de`（均为 Facebook 的地址，DNS 污染特征）；同一时刻 `https://www.cloudflare.com/` 返回 **200**。`GET /client/v4/zones` → 账号 zone 数 **0**，无自定义域名可用。**结论：部署成功，但产品在目标网络不可用。**
- 后续可选路径：①把域名接入 Cloudflare 并给 Worker 绑 custom domain（推荐，手机与机器人都能直连）；②用代理/VPN 访问（仅人能访问，机器人不行）；③换国内可达的托管（改动最大，需重新确认技术栈）。
## 唤醒词拼音表与自定义唤醒词（2026-10-15）
- **为什么需要转换**：设备侧的讯飞无关——是 ESP-SR 的 MultiNet7 中文模型**只接受拼音音节串**（空格分隔、不带声调），汉字会被模型自己拒绝（真机日志 `MN_COMMAND: invalid command, please check format, 你好小智`）。所以控制台让用户填中文，服务端转拼音再下发。详见 LEARNINGS「文档说收 grapheme」。
- **`server/pinyin-data.ts` 是生成物**：音节表直接取自设备模型词表（`Hardware/esp32/build/srmodels/mn7_cn/vocab`，405 个音节 ⇒ 服务端校验过的音节设备一定认识）；汉字读音取自 **mozillazg/pinyin-data v0.15.0（MIT 许可）**，规则是"每个字取第一个模型认识的读音、去声调、ü→v"，只覆盖基本区 U+4E00–U+9FFF（20,919 字，45 KB 文本）。**注意 ü→v 必须在对读音做 NFD 去附加符号之前替换**，否则"女"会变成 `nu`（= 努）而不是 `nv`。
- **重新生成的方式**：`curl` 下载 `https://raw.githubusercontent.com/mozillazg/pinyin-data/master/pinyin.txt` → 按上述规则重跑生成脚本 → 覆盖 `server/pinyin-data.ts`（生成脚本随本轮 commit 记录在 PROGRESS 里，需要时按同样步骤重来即可）。
- **接口**：`POST /api/devices/:id/wake`，body `{"text":"你好小智"}`（也接受直接写拼音）；服务端转换+校验后写入 `devices.wake_label`/`wake_phrase`，并入队一条 `wake` 指令——payload 同时带 **`text`（拼音，喂模型）与 `label`（中文，设备存 NVS 供开机提示音使用）**；设备 ack `done` 时服务端**把这台设备的 `wake_applied` 更新为该拼音**（设备的 `hello` 只在开机／重连时发，光靠它会让人以为「设备当前」没刷新）；设备在 `/api/device/hello` 里回报当前生效的词，存到 `devices.wake_applied`。**若设备回报值与目标不一致**（换固件、清 NVS），服务端会在 hello 时补发一次（有在途 `wake` 指令时不重复补）。
- **迁移**：`migrations/0006_wake.sql`（devices 加 `wake_label`／`wake_phrase`／`wake_applied`）**本地已应用**；**线上（Cloudflare D1 `aidesk`）尚未应用**——部署前必须跑 `npx wrangler d1 migrations apply DB --remote`，否则线上设备页的唤醒词接口会 500。
- **未验证（真机/浏览器）**：控制台点「下发」→ 设备 3 秒内执行 → 「设备当前」显示回报值；以及喊新词能唤醒、旧词失效、断电重启后保留。
- **预录提示音的生成与复核（2026-10-15 用过一次，可复用）**：`assets/provisioning.wav` 是用账号自己的火山 TTS 合成的——脚本从 `.dev.vars` 取 `ENCRYPTION_KEY`、从本地 D1 的 `voice_settings` 取（AES-GCM 加密的）api key 与 speaker，调 `…/tts/unidirectional`（`format: "pcm"`，`sample_rate: 16000`），把返回的 base64 分片拼成裸 PCM 再手工加 44 字节 WAV 头（16 kHz／单声道／16 bit，固件只收这个格式）。**合成完必须回读验证**：`VOLC_ASR_WAV=<文件> npm run volc:smoke` 把音频转回文字——这一次就是靠它发现品牌名被念成了 "desky em"。密钥只放环境变量／临时文件，用完即删，不进仓库、不打印。
- **2026-10-15 晚（域名到手后）**：①`npx wrangler d1 migrations apply DB --remote` → `0006_wake.sql` ✅（今天新加的唤醒词三列，上线前必须做的最后一步）；②`npx wrangler deploy` → Worker `aidesk-console` 重新部署，版本 **`3fab4406-bcd9-49a5-9802-2065a32fb0c1`**，上传 198.57 KiB（gzip 65.62 KiB），地址仍是 `https://aidesk-console.1763584993.workers.dev`（**国内仍不可达**，路由还没绑自定义域名）。③查询 Cloudflare 账号 zone 列表：**仍为 0 个** ⇒ 域名尚未接入 Cloudflare（或已加但 NS 未改），因此 `custom_domain` 还不能绑。
- **下一步（等用户）**：在 Cloudflare「Add a site」加入域名 → 到阿里云把域名 NS 改成 Cloudflare 给的两台 → 等 zone 变为 Active → 在 `wrangler.jsonc` 加 `routes: [{pattern: "desk.<域名>", custom_domain: true}]` → `npx wrangler deploy` → 从国内实测（`Resolve-DnsName` + `curl https://<域名>/api/health` 连打多次）→ 再把机器人的服务器地址指到该域名。
## 接入自定义域名（操作步骤，2026-10-15 起）
`workers.dev` 在国内被 DNS 污染（解析到 Facebook 的 IP），必须把域名接进 Cloudflare 才能给 Worker 绑 custom domain。**这一步只能由域名持有人做**（涉及注册商账户），我这边负责后面的路由、部署与实测。
1. **Cloudflare 添加站点**：dash.cloudflare.com → Add a site → 输入域名（**不带 `www.`、不带路径**）→ 选 **Free** 计划 → 它会扫描现有 DNS 记录（新域名一般没有，直接继续）→ 给出**两台 nameserver**（形如 `xxx.ns.cloudflare.com`），复制下来。
2. **注册商改 NS**：阿里云控制台 → 域名列表 → 该域名 → **DNS 修改 / DNS 服务器** → 改成 Cloudflare 给的那两台。⚠️ 域名需已完成**实名认证**（审核中可能不允许改 NS）；改 NS 与实名不冲突，也不受"注册后 60 天禁止转出"限制。
3. **等 zone 变 Active**：回 Cloudflare 域名页面，状态从 `Pending Nameserver Update` → **`Active`**（一般 10 分钟~1 小时，最长 24 小时；可点 "Check nameservers now" 手动触发）。**不要手建 DNS 记录**——绑 custom domain 时 Cloudflare 会自动建，证书（Universal SSL）也会自动签发，Free 计划够用。
4. **我这边收尾**：在 `wrangler.jsonc` 加 `routes: [{pattern: "desk.<域名>", custom_domain: true}]` → `npx wrangler deploy` → 从国内实测（`Resolve-DnsName` + `curl https://desk.<域名>/api/health` 连打 10 次）→ 再把机器人的服务器地址指过去（Kconfig `ROBOT_CLOUD_URL` 或配网页）。
5. 校验手段：`npx wrangler whoami`（刷新 OAuth）之后用 API 查 zone 列表即可确认是否已接入（直接拿配置文件里的 token 调 API 会报 `Invalid access token`，因为它不负责刷新）。
## 上线状态（2026-10-15 晚，域名接入完成）
- **已接入**：`aideskrobot.xin`（注册局委派已发布 `dell`/`rob.ns.cloudflare.com` ✓、Cloudflare zone `active` ✓）；`wrangler.jsonc` 加 `routes: [{pattern: "aideskrobot.xin", custom_domain: true}]` → 部署版本 `78ec188f-4fc9-441d-932c-4501b7668e43`，线上入口变为 **`https://aideskrobot.xin`**（顺带：配置里没有 `workers_dev`，该入口已被 wrangler 自动关闭）。线上库迁移到 `0006` ✓。
- **但国内直连质量不合格（实测）**：不走代理连打 `https://aideskrobot.xin/api/health` → **7/10**（成功耗时 1.1s~13.2s，失败为 15s 超时）；同一时刻**隧道那条路 6/6 稳定**（1.2~1.5s）。
- **根因定位（对照实验，同一台 IP `104.16.230.132`、同刻各 6 次）**：SNI=`www.cloudflare.com` → **6/6**；SNI=`aideskrobot.xin` → **3/6**；SNI=`*.trycloudflare.com` → **6/6**。⇒ 既不是 Cloudflare、也不是 IP、也不是配置问题，而是**这个主机名（很可能 `.xin` 后缀）在国内被针对性干扰**。另外测了 12 个候选 Cloudflare IP（`--resolve` 指定 IP + 正确 SNI），最好的也只有 **4/6**，所以"IP 优选"救不回来。
- **子域名实验（2026-10-15 晚，用户提议）**：另加 `robot.aideskrobot.xin`（custom domain，版本 `ce4b0f6f-aca5-4238-a62a-2fdb26c31b82`）后同刻对照（各 4 次、不走代理）：裸域名 **0/4**、子域名 **0/4**、隧道 **4/4**。两者解析到同一批 IP，一起挂 ⇒ **干扰作用在 `.xin` 后缀这一层，换子域名无效**；且同一天里 0/4 → 3/6 → 7/10 波动，说明**干扰强度随时间变化**（所以判定可达性必须"同刻对照"，跨时刻的数据不可比）。**结论：Cloudflare 免费版没问题（隧道一直稳），是 `.xin` 这条路不行。**
- **三方同刻对照（2026-10-15 晚，含用户旧项目对照，各 6 次、不走代理）**：隧道 `*.trycloudflare.com` **6/6**（1.4~1.8 s）；用户旧项目 `ownerweb.pages.dev` **4/6**；我们的 `aideskrobot.xin` **0/6**。再结合"同 IP 换 SNI：`www.cloudflare.com` 6/6、我们域名 3/6"⇒ **被拦的是"被点名的域名模式"（`*.workers.dev`／`*.pages.dev`／`.xin` 这类），而不是 Cloudflare、IP 或我们的配置**；普通主机名（如 `www.cloudflare.com`）在同一台 IP 上很稳。据此判断：**换一个"没被点名"的后缀确实有较大概率解决**（但非保证），而"接了自己的域名"本身只解决 DNS 污染这一层。
- **因此的使用建议**：机器人 + 控制台演示走**局域网**（`192.168.137.1` 系列，实测稳）；对外/远程用 **Cloudflare 快速隧道**（`trycloudflare.com`，实测 6/6）；`aideskrobot.xin` 作为"已上线"的正式入口保留（能开，但可能偶发超时，不要作为唯一依赖）。彻底改善公网直连需要换后缀（如 `.com`/`.top`）或换国内托管（后者要备案 + 改代码）。

## 腾讯云 CloudBase 云托管（T034，2026-10-15 起）
**为什么走这条**：Cloudflare 免费版本身没问题（隧道实测 6/6），但 `workers.dev`／`pages.dev`／`.xin` 这类**被点名的主机名模式**在境内被针对性干扰（见「上线状态」的对照实验）。国内托管 + **CloudBase 默认域名** 既不出境、又**免备案**（自有域名绑上去才需要 ICP 备案）。目标形态是**单容器**：一个 Node 进程同时给前端静态资源和 `/api`，与现在"同源"的形态一致（不改前端、不加跨域）。

**产物**：`Dockerfile`（两段构建：`npm ci` + `npm run build` + `npm run build:node` → 运行段只留生产依赖）、`.dockerignore`（排除 `Hardware/`、`node_modules/`、`.dev.vars` 等）、`server/node/**`。代码包 zip（`data/cloudbase-source.zip`）由 `git archive` 从已提交的版本生成，**不含** `Hardware/`、`.dev.vars`、`sdkconfig`。

### 步骤（控制台，只能由账号持有人操作）
1. **建环境**：腾讯云 → 云开发 CloudBase 控制台 → 新建环境（按量计费，新用户有免费额度）→ 记下**环境 ID**。
2. **开通云托管**：环境内左侧「云托管」→ 开通。
3. **新建服务**：名称 `aidesk-console`；部署方式选**代码包上传／本地代码（用 Dockerfile 云端构建）**；上传代码包 zip；**服务端口填 80**（与镜像里的 `EXPOSE 80` 一致）；实例规格取最小档；最小实例数 1（避免冷启动把第一次唤醒拖成十几秒）。
4. **环境变量**：`ENCRYPTION_KEY` = **与 Cloudflare 上那个完全相同的 44 字符 base64**（换新值不会报错，但 Console 里已保存的模型密钥会解不开）；`APP_ENV=production`；`DB_PATH=/app/data/aidesk.sqlite`；账号里已经存过语音凭据的话不必再加，要用平台级回落再加 `VOLC_API_KEY`。
5. **数据持久化（重要）**：容器文件系统**随发布/重启重置**，必须在「存储 → 挂载卷」把一块 CFS 目录挂到 `/app/data`。不挂的话每次发版都是空库：要重新注册账号、重新建设备，机器人还得重新配网。挂载卷内的 SQLite**不要开 WAL**（跨网络文件系统的共享内存语义不可靠），当前实现已按此保守设置。
6. **拿默认域名**：服务详情里的默认域名形如 `https://<env-id>.service.tcloudbase.com`，**免备案**、境内直连。
7. **首次使用**：用默认域名打开控制台 → 注册账号 → 加模型/角色 → 建设备（**设备码只显示这一次**）。
8. **机器人**：配网页「服务器地址」填 `https://<默认域名>`（**必须带 `https://`**，只填 `192.168.137.1:8787` 这类裸地址会报 `Error parse url`；固件已挂 CA 证书包，公网 HTTPS 可直接用）、「设备码」填新生成的设备码 → 保存 → 重启机器人。
9. **验收**：`/api/health` 200；登录；建设备后 3 秒内状态变 online；喊唤醒词能回答（这一条同时验证 Node 版语音识别）。

### 若控制台只允许"镜像部署"
本机**没有装 Docker**，无法本地构建镜像。届时有两条路：①装 Docker Desktop（数百 MB + 需要 WSL2）后本地构建并推到腾讯云 TCR；②用 GitHub Actions 在云端构建镜像再推到 TCR，控制台填镜像地址。

### 与 Cloudflare 版的关系
两个形态可以并存：Cloudflare 版是**回滚路径**（`aideskrobot.xin` 仍然在线），CloudBase 版是**主路径**。两边数据库**不互通**——CloudBase 上是独立的 SQLite，需要重新注册与建设备（现有账号/设备数据在本地 D1 与 Cloudflare D1 里，都不在 CloudBase）。
