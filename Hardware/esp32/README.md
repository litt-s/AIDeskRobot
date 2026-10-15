# DeskEmoji robot（ESP32-S3 N16R8）

面向赛博太白 DeskEmoji 硬件的 ESP32-S3 固件。语音链路只有一条：**本地离线唤醒 → 板载麦克风录一句 → 自建控制台（Cloudflare Worker）做识别、调模型、合成语音 → PCM 流回设备播放**；设备同时通过同一条控制台通道接收**表情、动作、音量、播报**指令。

> 目标芯片：ESP32-S3 N16R8（16 MB Flash、8 MB Octal PSRAM）。
> 工程要求 ESP-IDF 5.5.x，本机验证版本 5.5.5。

**2026-10-15 起本固件不再包含讯飞 AIUI**：SDK、协议适配、凭据配置、音乐技能、Opus 解码路径与桌面 demo 均已删除（见 §15）。`VERIFICATION.md` 是 AIUI 时期的历史报告，仅供追溯。

## 1. 当前功能

- 首次启动，或已保存的 Wi-Fi 在 60 秒内连不上，自动进入 SoftAP 配网。
- 配网页扫描附近 2.4 GHz Wi-Fi，也可手动输入 SSID；**Wi-Fi、服务器地址、设备码三项互相独立，填哪项写哪项，留空保持原值**。
- ESP-SR MultiNet7 单麦离线唤醒，当前唤醒词“小飞小飞”。
- 唤醒词、BOOT 短按、待机时前推手势都能开一轮云端语音。
- **唤醒一次就能连着聊**：一轮回答播完自动继续听下一句（`ROBOT_CLOUD_VOICE_FOLLOW_UP`，默认开）；窗口内没人说话就结束这段对话、回到离线待机。
- **唤醒词可自定义**：控制台下发 `wake` 指令即可换词（MultiNet7 支持运行时增删命令），新词写入 NVS、重启后保留；模型不认识的词会被拒绝并**保留原词**，原因回报控制台。默认词是固件里的 `WAKE_PHRASE_DEFAULT`（拼音 `xiao fei xiao fei`）。
- **打断用 BOOT 短按**：取消这一轮并结束这段对话。唤醒词打断（`ROBOT_CLOUD_VOICE_BARGE_IN`）**默认关闭**——2026-10-15 实测单麦无 AEC 时喇叭漏音把用户语音压住（麦克风 9k–26k），唤醒短语检不出；要做真打断必须先加 AEC（见 §13 与 Kconfig help）。
- 云端语音：设备录一句 16 kHz／16 bit／单声道 PCM 上传，服务端做 ASR、模型对话与 TTS，把 PCM 流回来边收边播；说完约 900 ms 自动成句。
- 一轮进行中再按一次 BOOT 可**立即取消**：正在录就停止录音，正在播就截断回复流，不会“跳一段接着说”。
- 控制台设备通道：轮询并执行**表情、动作、音量、播报**指令，回报 ack 与状态；控制台的“播报”走 `/api/device/say`，不经唤醒词。
- 沿用 `esp32-s3n16r8-emoji` 的 OLED 表情与匹配动作，空闲时约每 10 秒随机做一个行为。
- PAJ7620U2 九类手势；OLED 与手势模块共享 I2C 总线并串行访问。
- 音量按键 0～100 调节、NVS 持久化、约 +3 dB 保护性数字增益。

## 2. 硬件与引脚

| 模块 | 接口/引脚 | 当前配置 |
| --- | --- | --- |
| INMP441 麦克风 | I2S1 BCLK / WS / DATA | GPIO5 / GPIO4 / GPIO6 |
| MAX98357A 功放 | I2S0 BCLK / WS / DATA | GPIO15 / GPIO16 / GPIO7 |
| SSD1306 OLED | I2C0 SDA / SCL / 地址 | GPIO41 / GPIO42 / `0x3C` |
| PAJ7620U2 手势 | I2C0 SDA / SCL / 地址 | GPIO41 / GPIO42 / `0x73` |
| 水平舵机 | LEDC 通道 0 | GPIO11 |
| 垂直舵机 | LEDC 通道 1 | GPIO12 |
| BOOT | 低电平按键 | GPIO0 |
| 音量加 | 低电平按键 | GPIO40 |
| 音量减 | 低电平按键 | GPIO39 |

OLED 在机构中倒装，初始化时同时启用 X/Y 镜像。舵机中心角均为 90°；当前安全范围为水平 50°～130°、垂直 70°～110°。若机构安装方向不同，只调整 `components/robot_motion/robot_motion.c` 中的映射和限位，不要直接扩大角度范围。

**舵机供电**：只用烧录口（USB）供电带不动舵机，必须接独立电源口。排查“舵机不动”时先确认供电，再查引脚与固件。

## 3. 启动与交互流程

1. 初始化 OLED、动作队列、NVS/Wi-Fi、I2S 音频与按键。
2. 没有 Wi-Fi 配置立即进入配网；有配置则尝试连接，默认超时 60 秒。
3. 获得 IP 后关闭 Wi-Fi 省电、完成 NTP 校时，随后启动控制台设备通道。
4. 初始化 ESP-SR 离线唤醒与手势传感器，播放“已联网”提示音，进入离线待机（`offline wake ready`）。
5. 唤醒词、BOOT 短按或待机时前推手势 → 开一轮云端语音：暂停唤醒词、独占麦克风录音。
6. 说完（连续静音约 900 ms）后上传整段音频；服务端返回识别文本、回复文本与 PCM，设备边收边播。
7. 播完或取消后恢复采集、重新武装唤醒词，回到待机。
8. 音量键、配网页与控制台指令在任何状态下都可用；**BOOT 长按随时可进配网**。

## 4. 按键操作

| 按键 | 短按 | 长按（约 800 ms） |
| --- | --- | --- |
| BOOT | ① 已保存 Wi-Fi 正在连接、尚无 IP → 停止连接并进配网；② 一轮云端语音进行中 → **取消这一轮**；③ 否则唤醒并开一轮 | **进入配网页面**（已联网也生效）；在配网状态下长按 = 重启离开配网 |
| 音量 + | +10 | 直接到 100 |
| 音量 − | −10 | 静音 |

默认音量 80，改动保存在 NVS 的 `robot_audio/volume`，重启后仍然有效。

## 5. 配网与控制台凭据

### 5.1 进入配网

- NVS 中没有保存 Wi-Fi SSID；
- 保存的 Wi-Fi 在 `CONFIG_ROBOT_STA_CONNECT_TIMEOUT_SECONDS`（默认 60 秒）内未连接成功；
- Wi-Fi 正在连接但还没获得 IP 时短按 BOOT；
- **任何时候长按 BOOT 约 800 ms**（给“已经配过网、想改配置”用，这是 2026-10-15 补上的入口）。

设备创建开放热点 `DeskEmoji-XXXX`，OLED 滚动显示连接方式，并播放预置的配网提示音 `assets/provisioning.wav`（16 kHz 单声道，离线可播）。手机连接该热点后访问：

```text
http://192.168.4.1
```

页面自动扫描最多 20 个附近 Wi-Fi（仅 2.4 GHz），也可以手动输入 SSID。

### 5.2 三个字段互相独立

**页面上只有一个保存按钮**：三项一起提交，但服务端只写“这次真的填了”的那一项，留空的一律不写——所以不会出现“改设备码顺手把密码清空”的情况。

| 想做的事 | 怎么填 | 结果 |
| --- | --- | --- |
| 只改 Wi-Fi | 填 Wi-Fi（和密码） | 服务器地址与设备码原样保留 |
| 只改设备码 | 只填设备码 | Wi-Fi 与已保存密码不受影响 |
| 只改服务器地址 | 只填服务器地址 | 其余不动 |

NVS 落点：

| 命名空间 | 键 | 内容 |
| --- | --- | --- |
| `wifi_cfg` | `ssid`、`password` | Wi-Fi |
| `cloud_cfg` | 服务器地址、设备码 | 控制台通道 |

服务器地址**不含 `://` 时自动补 `http://`**；写入端与读取端各做一次规范化，所以 NVS 里的旧裸 host 也不会继续报 `Error parse url`。

### 5.3 安全要求

- 设备码只在控制台签发一次，库内只存哈希与掩码；填进 `sdkconfig` 会被编译进二进制，**带真实设备码的 `sdkconfig` 不得入库**。
- 配网页只在设备本地 SoftAP 提供，热点是开放网络，配网时避免无关人员接入。
- 普通日志不打印设备码明文，配网页只显示掩码。

## 6. 固件配置（`idf.py menuconfig`）

主菜单 `DeskEmoji robot`：离线唤醒、手势、空闲随机动画、双舵机、舵机引脚、舵机自检、麦克风声道／采样率／增益（`ROBOT_MIC_SHIFT`，默认 16）、已保存 Wi-Fi 连接超时。

另一个菜单 `DeskEmoji cloud device channel`：

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `ROBOT_CLOUD_ENABLE` | n | 打开控制台设备通道 |
| `ROBOT_CLOUD_URL` | 空 | 服务器地址的**编译期兜底**；配网页填了就用配网页的 |
| `ROBOT_CLOUD_KEY` | 空 | 设备码的编译期兜底；同上。**建议留空**，让固件镜像不带任何凭据 |
| `ROBOT_CLOUD_POLL_INTERVAL_S` | 3 | 指令轮询间隔 |
| 唤醒词 | `xiao fei xiao fei` | 不是 Kconfig，而是代码里的 `WAKE_PHRASE_DEFAULT`（`robot_wakeup.c`）；运行时由 NVS（`wake_cfg`/`phrase`）覆盖，控制台或设置页下发即可换词 |
| `ROBOT_CLOUD_STATUS_INTERVAL_S` | 60 | 状态上报间隔 |
| `ROBOT_CLOUD_VOICE` | n | 打开云端语音对话 |
| `ROBOT_CLOUD_VOICE_MAX_RECORD_MS` | 10000 | 单句录音上限 |
| `ROBOT_CLOUD_VOICE_SILENCE_MS` | 900 | 说完多久算一句结束 |
| `ROBOT_CLOUD_VOICE_THRESHOLD` | 700 | 静音判定阈值，**与 `ROBOT_MIC_SHIFT` 联动**：改增益要同步改它 |
| `ROBOT_CLOUD_VOICE_FOLLOW_UP` | y | 回答播完继续听下一句（连续对话）；关掉即恢复"一次唤醒一轮" |
| `ROBOT_CLOUD_VOICE_FOLLOW_UP_MS` | 4000 | 等不到人说话多久就结束这一轮／这段对话 |
| `ROBOT_CLOUD_VOICE_BARGE_IN` | **n** | 说话期间保持麦克风与唤醒引擎工作，让唤醒词能打断回答。**默认关闭**：单麦无 AEC 时实测打断不可靠（见 §13），要开的前提是先给唤醒引擎加 AEC |

优先级是「**配网页填了用配网页，没填才用固件里的 `ROBOT_CLOUD_URL` / `ROBOT_CLOUD_KEY`**」。上线后域名固定，写进 Kconfig 更省事（量产不必逐台填）；开发期建议留空、在配网页填。

## 7. 音频与云端对话

- 采集：INMP441 → I2S，16 kHz、单声道、S16 PCM；32-bit 槽右移 `ROBOT_MIC_SHIFT`（默认 16 就是把该麦克风的 24 位正确对齐）后钳到 16 bit。
- 采集回调 640 samples／40 ms；日志约每秒打印一次 `microphone peak=… samples=640`。
- 成句判据：RMS 低于 `ROBOT_CLOUD_VOICE_THRESHOLD` 的帧算静音，连续 `SILENCE_MS`（默认 900 ms）后结束这一句；开头一小段忽略静音检测，避免把第一个字当停顿。
- 成句前会把**长内部静音压缩**（≥400 ms 的内部停顿压到约 300 ms，短停顿不动），这是“识别断断续续”的修法。
- 每轮结束打印 `capture report`：音频时长、占用墙钟、有声时长、最长停顿、≥400 ms 停顿个数。**先看这份报告再猜**：`audio ≈ wall` 说明没有丢帧，剩下的停顿是音频里真实存在的。
- 播放是半双工：播放期间暂停采集，避免单麦、无 AEC 的自激。
- 取消：`robot_voice_cancel()` 同时置停止与取消标志，读取循环据此截断流、丢弃队列、恢复采集——这是“长回答按一次停不下来”的修法。
- **连续对话**：`follow_up_ms > 0` 时，一轮正常结束后自动开下一轮聆听（不回唤醒词、不播提示音）；没人说话、被取消或出错就结束整段对话。上限 30 轮兜底（`VOICE_MAX_TURNS_PER_CONVERSATION`）。
- **“播完”要怎么看**：下一轮必须等上一轮**真的播完**（含"已入队但还在预缓冲"那段），判据是 `robot_audio_is_playing() || robot_audio_playback_buffered_samples() > 0`；只看前者会踩空——预缓冲窗口里它是 false，结果录音抢在回复开播之前开始，"没人说话"的超时还会在回复仍在播放时结束整段对话。
- **没人说话的退出有两处**：采集回调里按帧统计之外，录音主循环也按时间兜底（回调不跑时不会傻等满 `MAX_RECORD_MS`）。
- **起始静音超时**：一轮里还没听到人声时，`follow_up_ms` 到点即结束且**不上传**（旧行为是录满 `MAX_RECORD_MS` 再把 10 秒静音传上去）。
- **打断**：默认走 **BOOT 短按**（取消这一轮并结束这段对话）。`ROBOT_CLOUD_VOICE_BARGE_IN` 打开时会额外做"唤醒词打断"——播放期间麦克风与唤醒引擎保持工作，唤醒词被听到就切断回复流并进入下一轮聆听（`robot_voice_interrupt_listen()`）；但它**默认关闭**，因为单麦无 AEC 时喇叭漏音会盖住用户语音（实测 9k–26k，见 §13），开着只会偶尔生效、还带自激风险。真要"喊一声就打断"，前置工作是给唤醒引擎加 AEC（ESP-SR AFE，用播放 PCM 当参考）。

## 8. 表情、动作与手势

OLED 表情沿用 `esp32-s3n16r8-emoji/emoji_controller.cc` 的风格：中性、开心、难过、生气、惊讶、思考、困惑、尴尬、哭、大笑、搞怪、爱心、震惊、酷、放松、美味、亲吻、自信、调皮、睡眠。

动作支持：眨眼、左看、右看、抬头、低头、回中、点头、摇头、转圈、跳舞、唤醒、休眠。动作、舵机与对应表情统一进入一个队列执行，避免屏幕动作与机械动作错位。

动作的来源有三处：**空闲随机行为**（约每 10 秒一次；约 60% 眨眼、18% 左看、18% 右看、1% 转圈、3% 困惑）、**手势映射**、**控制台下发的表情／动作指令**。
（旧版“从 AI 回复文本里解析表情／动作标签”的能力已随 AIUI 一起删除，代码中不再有文本到动作的解析。）

PAJ7620U2 手势映射：

| 手势 | 动作 |
| --- | --- |
| 左 / 右 | 左看 / 右看 |
| 上 / 下 | 抬头 / 低头 |
| 连续左、右、左（或相反） | 摇头 |
| 连续上、下、上（或相反） | 点头 |
| 顺时针 | 转圈 |
| 逆时针 | 摇头 |
| 挥手 | 跳舞 |
| 前推 | 惊讶；离线待机时同时唤醒 |
| 后拉 | 眨眼 |

方向手势按设备上 PAJ7620U2 的实际安装角度做了坐标换算。为避免手停在传感器前连续误触，单次动作后有 900 ms 冷却，传感器安静约 200 ms 后才接收下一次；一次上报多个方向的模糊结果会被丢弃；连续点头／摇头须在 4 秒内完成。传感器连续通信失败会自动停用，避免把共用 I2C 的 OLED 一起拖住。

## 9. 控制台设备通道（T016/T018）

- 周期性 `POST /api/device/hello` 与 `GET /api/device/commands`，用设备码鉴权（库内只存哈希）。
- 支持指令：表情、动作、音量、播报、**唤醒词（`wake`，payload `{"text":"你好小智"}` 或拼音）**、重启；每条回报 ack。
- 在线判定与状态上报（固件版本、音量）默认每 60 秒一次。
- 播报与对话共用同一条云端语音通道，所以“正在对话中”时下发播报会被拒绝，并把原因如实回报给控制台。
- 语音能力需要控制台侧配置（语音设置里的供应商凭据 + 该设备选定语音角色），否则服务端返回 409，设备在**不录音**的情况下回报原因。

## 10. 构建

在已导出 ESP-IDF 5.5.x 环境的终端进入本目录：

```powershell
cd esp32
idf.py build
```

首次构建、或改过 Kconfig 后：

```powershell
idf.py reconfigure
idf.py build
```

一般不需要 `fullclean`；只有构建目录跨 IDF 版本、工具链或目标芯片后出现无法解释的 CMake 缓存冲突时才用。

主要构建产物：

| 文件 | 用途 |
| --- | --- |
| `build/deskemoji_robot.bin` | 应用固件 |
| `build/bootloader/bootloader.bin` | 二级 Bootloader |
| `build/partition_table/partition-table.bin` | 分区表 |
| `build/srmodels/srmodels.bin` | ESP-SR/MultiNet 模型 |
| `build/assets.bin` | SPIFFS 提示音资源 |

当前应用约 1.28 MiB（`0x148640` 字节），应用分区 8 MiB，空闲约 84%；实际大小以每次构建末尾输出为准。

## 11. 烧录、监视和合并固件

将 `COMx` 替换为设备实际串口：

```powershell
idf.py -p COMx flash monitor
```

`flash` 会按项目清单同时写入应用、分区表、模型与资源。**新设备不要只单独烧 `deskemoji_robot.bin`**，否则会缺离线唤醒模型或提示音。

## 12. 关键启动日志

以真机记录为准，下面这些行在 2026-10-14/15 的实机日志里出现过：

```text
sta ip: ...
offline wake ready: 小飞小飞 or BOOT short press
robot_cloud: device channel online
robot_app: microphone peak=... samples=640
robot_voice: capture report: ... ms of audio over ... ms wall, voiced ... ms, longest mid-speech gap ... ms, gaps >= 400 ms: N
robot_voice: sending ... bytes of audio
robot_voice: heard: ...
robot_audio: playback stream start: buffered=... ms waited=... ms
robot_voice: answer: ...
robot_voice: cloud voice turn finished: 对话完成
robot_wakeup: wake phrase set to: 你好小智
robot_wakeup: the model cannot use this phrase: ...
robot_wakeup: stored wake phrase rejected (…); using the default
robot_voice: no speech within 4000 ms; nothing uploaded
robot_voice: listening again: conversation turn 2
robot_voice: interrupted by the wake word: listening again
robot_voice: conversation ended after 3 turns: 对话完成
```

开机提示音排空后应出现这一行（**它是"麦克风真的开始工作"的证据**；没有这行而唤醒词又不灵，就是采集还停在暂停状态）：

```text
robot_app: ready prompt drained; microphone resumed for offline wake
```

唤醒引擎起不来时会明确报错，不再照打"就绪"：

```text
robot_wakeup: no wake model in the "model" partition - was build/srmodels/srmodels.bin flashed?
robot_app: offline wake UNAVAILABLE: only BOOT can start a turn (wake engine failed to initialise)
```

两条按键路径会额外打印：

```text
BOOT long press detected
provisioning requested from BOOT long press
```

## 13. 常见问题

| 日志／现象 | 判断与处理 |
| --- | --- |
| `Error parse url 192.168.x.x:8787/api/device/hello` | 服务器地址缺 scheme。配网页会自动补 `http://`，但**旧 NVS 里的值不会自己变**——重开配网页保存一次即可。同时确认填的是控制台地址（本地 `:8787`），**不是**前端页面的 `:5173` |
| `Failed to set URL`、设备通道一直不在线 | 地址不可达或设备码不对。先在同一 Wi-Fi 下用手机／电脑打开该地址，再确认设备已绑定 |
| 唤醒没反应 | 先看 `offline wake ready`（引擎已就绪）与 `ready prompt drained; microphone resumed for offline wake`（麦克风真的在采集）。**若"只有重启后第一次不灵、按一次 BOOT 就好"，那是 T031 已修的老 bug**；若一直不灵，看有没有 `offline wake UNAVAILABLE`／`no wake model`（模型分区没烧全） |
| 待机时看不到 `microphone peak=` | 说明采集被暂停（`robot_audio_set_capture_paused(true)` 之后没人恢复），唤醒引擎同样拿不到数据 |
| `capture report: 0 ms of audio over … ms wall` | 这段录音一帧都没进来：通常是"以为没在播、其实在播"（播放期间 tap 丢帧）或采集被暂停。**"回复播完再聆听"的判据已修**（见 §7）；仍复现就把日志贴出来 |
| 播放期间喊唤醒词打断不了 | **已知上限，且已按 2026-10-15 决策（选 B）关闭该功能**：单麦无 AEC 时喇叭漏音在麦克风上是 9k–26k，用户语音被压在里面，MultiNet 整段播放零检出；调低阈值只会误触发。要真打断需先加 AEC（ESP-SR AFE，用播放 PCM 做参考）；当前用 **BOOT 短按**打断 |
| 识别断断续续 | 先看 `capture report`：`audio ≈ wall` 且有 `gaps >= 400 ms` → 是音频里真实的停顿；`audio` 明显小于 `wall` → 采集丢帧 |
| 识别不到／听不清 | 看 `microphone peak`：长期只有几十 → 麦克风没接或 `ROBOT_MIC_SHIFT` 不对；长期钉在 32768、RMS 过半 → 增益过大，语音被钳成方波 |
| 长回答按一次停不下来 | 已修（`robot_voice_cancel`）。若复现，请保留 `robot_voice:` 前后的完整日志 |
| 播报回报“设备未配置语音服务” | 控制台「语音设置」没配供应商凭据，或该设备没选语音角色（服务端 409） |
| 舵机不动 | 先查供电（必须用独立电源口），再用 `ROBOT_SERVO_SELFTEST` 自检：自检也不动就是硬件侧（供电／接线／舵机） |
| `I2C transaction failed`／手势失效 | PAJ7620U2 NACK、供电或接线问题。OLED 仍可用，手势降级；查 GPIO41/42 与地址 `0x73` |
| `merged-binary.bin` 恰好 16 MB | 正常，是整片 Flash 镜像，不代表应用大小为 16 MB |

## 14. 代码结构

```text
esp32/
├─ main/                          应用状态机、事件与按键（robot_app）
├─ components/robot_cloud/        控制台设备通道 + 云端语音（robot_voice.c）
├─ components/robot_network/      Wi-Fi、配网页、NTP、凭据 NVS
├─ components/robot_audio/        单麦采集、I2S 播放与缓冲
├─ components/robot_wakeup/       ESP-SR MultiNet 离线唤醒
├─ components/robot_ui/           SSD1306 表情、动画与配网滚动显示
├─ components/robot_motion/       舵机、动作队列与随机行为
├─ components/robot_gesture/      PAJ7620U2 手势
├─ assets/                        本地提示音（SPIFFS，整目录打包）
├─ partitions.csv                 16 MB 分区表
```

`assets/` 是用 `spiffs_create_partition_image` **整目录打包**成 SPIFFS 镜像的：放进去的文件都会被烧进 `assets` 分区，且镜像**按分区大小（`partitions.csv` 里的 0x4F0000 = 4.93 MiB）填满生成**——所以删文件**不会减少烧录字节**，只会腾出分区内部的空闲页（2026-10-15 实测：删掉 166 KB 无用文件后 `assets.bin` 仍是 5,177,344 字节）。想让删资源真正省 Flash，得连分区大小一起改。

当前 4 个文件全部被代码引用：`key_wakeup.wav`、`network_ready.wav`、`provisioning.wav`、`wakeup_ack.wav`。（2026-10-15 已删除 4 个无引用的 AIUI／旧工程遗留：`aiui_exit.wav`、`kws_enable.wav`、`ready.ogg`、`wificonfig.ogg`，合计 166,091 字节。）

## 15. 与旧版 AIUI 固件的差异（T028，2026-10-15）

删除的内容：`aiui_sdk`／`aiui_port` 组件、`Hardware/include/aiui` 与 `Hardware/src/aiui` 源码、`robot_app` 的 AIUI 会话层、`robot_audio` 的 Opus 解码路径、配网页「AIUI 凭据」段与 `aiui_secret` NVS、音乐／媒体子系统（`robot_music`）、桌面 AIUI demo，以及 Kconfig 的 `ROBOT_AIUI_*`／`ROBOT_ALLOW_VOICELESS`／`ROBOT_SESSION_IDLE_TIMEOUT_SECONDS`／`ROBOT_ENABLE_EXPRESSION_TAG`。

保留并成为**唯一**语音链路的是：控制台云端语音（`robot_voice`）+ 设备通道（`robot_cloud`）。工程名由 `aiui_demo` 改为 `deskemoji_robot`；`robot_app.cpp` 从 4045 行降到 995 行。

## 16. 实板验收清单

- [ ] 无 NVS 时自动进配网；配网页能扫到并连上 2.4 GHz Wi-Fi。
- [ ] **只填设备码保存：Wi-Fi 仍然连得上**（密码没被清空）；**只填 Wi-Fi：设备码不变**。
- [ ] 已联网状态下**长按 BOOT 约 800 ms 进入配网**；配网中长按可重启离开。
- [ ] “小飞小飞”、BOOT 短按、待机时前推手势都能开一轮对话。
- [ ] 说完一句、回答播完后**不喊唤醒词**直接说下一句，仍能继续对话；安静约 4 秒后结束回到待机。
- [ ] 回答播到一半时**按一下 BOOT**，回答被切断并结束这段对话（唤醒词打断已按 2026-10-15 决策关闭；开启它需先加 AEC）。
- [ ] 下发一个新唤醒词后：**喊新词能唤醒、喊旧词不再唤醒**，且断电重启后仍是新词。
- [ ] 一轮识别—回复—播放完整；播完自动回待机并重新武装唤醒词。
- [ ] 长回答播到一半按一次 BOOT 就能停（不“跳一段接着说”）。
- [ ] 音量加减、长按最大／静音与重启持久化正常。
- [ ] 控制台下发表情／动作／音量／播报四条指令都能执行并回报 ack。
- [ ] 表情与舵机动作同步，空闲随机行为正常。
- [ ] PAJ7620U2 九类手势映射正确，无持续 I2C NACK。
- [ ] 长时间运行无任务栈溢出、播放队列卡死或无法重新唤醒。
