# DeskEmoji AIUI（ESP32-S3 N16R8）

DeskEmoji AIUI 是面向赛博太白 DeskEmoji 硬件的 ESP32-S3 固件。工程以讯飞
AIUI RTOS v3 极速超拟人示例为主链路，结合原 DeskEmoji/XiaoZhi 工程的板级驱动、
表情、动作、配网资源和 ESP-SR 模型，实现离线唤醒、连续语音对话、流式 TTS、
音乐播放、表情动作与手势控制。

> 当前目标芯片为 ESP32-S3 N16R8：16 MB Flash、8 MB Octal PSRAM。
> 工程要求 ESP-IDF 5.5.x，当前验证版本为 5.5.1。

## 1. 当前功能

- 首次启动或已保存 Wi-Fi 在 60 秒内连接失败时，自动进入 SoftAP 配网。
- 配网页面扫描附近 2.4 GHz Wi-Fi，并支持列表点选或手动输入 SSID。
- AIUI 的 APPID、APPKEY、APPSECRET 通过配网页面写入 NVS，不写入源码和日志。
- 联网后先执行 NTP 校时，再初始化 AIUI，避免鉴权时间偏差。
- ESP-SR MultiNet7 单麦离线唤醒，当前唤醒词为“小飞小飞”。
- BOOT 键可唤醒、打断 TTS/音乐；仅在 Wi-Fi 正在连接且尚未获得 IP 时用于重新配网。
- AIUI v3 `continuous_vad` 连续对话，`new_session=false` 保留同一次唤醒会话的上下文。
- 60 秒无有效活动后播放 7911 结束提示，关闭 AIUI 连接并重新等待离线唤醒。
- 支持“退下吧”“休息吧”“退出”“再见”等结束引导，进入可再次唤醒的休眠表情。
- AIUI TTS 使用 `opus-wb` 流式下发、设备端解码和预缓冲播放。
- 支持 AIUI `musicX` 结果、itemid 换取播放地址，以及已绑定网易云账号的直接搜索兜底。
- 沿用 `esp32-s3n16r8-emoji` 的 OLED 表情和匹配动作，并支持空闲随机表情动作。
- 支持 PAJ7620U2 九类手势；OLED 与手势模块共享 I2C 总线并串行访问。
- 音量按键支持 0～100 调节、NVS 持久化和约 +3 dB 的保护性数字增益。

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

OLED 在机构中倒装，初始化时同时启用 X/Y 镜像。舵机中心角均为 90°；当前安全范围为
水平 50°～130°、垂直 70°～110°。若机构安装方向不同，只调整
`components/robot_motion/robot_motion.c` 中的映射和限位，不要直接扩大角度范围。

## 3. 启动与交互流程

1. 初始化 OLED、动作队列、NVS/Wi-Fi、I2S 音频和按键。
2. 没有 Wi-Fi 配置时立即进入配网；有配置时尝试连接，默认超时 60 秒。
3. 获得 IP 后关闭 Wi-Fi 省电，完成 NTP 校时。
4. 从 NVS 读取 AIUI 凭据，初始化 AIUI、离线唤醒和手势传感器。
5. 播放“已联网，可以通过喊小飞小飞唤醒我”提示，进入离线待机。
6. 听到“小飞小飞”、短按 BOOT，或待机时识别到前推手势后，播放唤醒提示并连接 AIUI。
7. 单麦音频经本地 VAD 分轮次发送；收到 IAT、语义、NLP 和 TTS 后执行回复、表情和动作。
8. TTS 或音乐播放时暂停上传麦克风；播放队列排空后恢复连续收音。
9. 超时、明确退出指令、网络错误或不可恢复的 AIUI 错误会关闭在线会话并重新启用离线唤醒。

云端语义字段 `shouldEndSession=true` 只表示当前技能轮次结束，不会直接结束本地连续
对话；只有明确退出、空闲超时或错误恢复路径才真正关闭本地 AIUI 会话。

## 4. 按键操作

| 状态 | BOOT 短按 |
| --- | --- |
| 已保存 Wi-Fi 正在连接、尚未获得 IP | 立即停止连接并进入本地配网 |
| 离线待唤醒 | 唤醒并开启 AIUI 会话 |
| 正在播放 TTS 或音乐 | 立即打断播放；保持当前对话上下文并恢复收音 |
| 已在普通收音阶段 | 不重复创建新会话 |

音量按键：

- 音量 `+` 短按：增加 10；长按约 0.8 秒：设为 100。
- 音量 `-` 短按：减少 10；长按约 0.8 秒：静音。
- 默认音量为 80，修改后保存在 NVS 的 `robot_audio/volume`，重启后仍然有效。

## 5. 配网与 AIUI 凭据

### 5.1 进入配网

以下任一情况会进入配网：

- NVS 中没有保存 Wi-Fi SSID。
- 保存的 Wi-Fi 在 `CONFIG_ROBOT_STA_CONNECT_TIMEOUT_SECONDS` 内未连接成功。
- Wi-Fi 正在连接但还没有获得 IP 时短按 BOOT。
- AIUI 凭据缺失或格式无效。

设备创建开放热点 `DeskEmoji-XXXX`，OLED 滚动显示连接方式，并播放预先合成的
“小飞”配网提示。该音频经过 16 kHz、单声道、16-bit PCM 和响度匹配处理后保存在
`assets/provisioning.wav`，无需联网即可播放，并与联网提示保持一致的音色和音量。
手机连接该热点后访问：

```text
http://192.168.4.1
```

页面会自动扫描最多 20 个附近 Wi-Fi。仅支持 ESP32-S3 可连接的 2.4 GHz 网络。
选择网络、填写密码，并在“AIUI 凭据”中填写已经发布应用的三项凭据，保存后设备重启。

若只修改 Wi-Fi，可以把 AIUI 三项留空，原凭据会保留；若要更换 AIUI 应用，必须同时
填写完整的 APPID、APPKEY、APPSECRET。凭据保存位置：

| NVS 命名空间 | 键 |
| --- | --- |
| `wifi_cfg` | `ssid`、`password` |
| `aiui_secret` | `appid`、`appkey`、`appsecret` |

设备已经联网后，BOOT 保持唤醒/打断功能，不会切换到配网。需要强制重新配置时，可先
关闭已保存的路由器并重启，在连接阶段按 BOOT；最后手段是执行 `idf.py erase-flash`
后重新完整烧录，此操作会清除固件、模型、资源和全部 NVS 数据。

### 5.2 安全要求

- 不要把真实 APPID、APPKEY、APPSECRET 写入源码、README、`sdkconfig`、构建命令或 Git。
- 配网页面仅在设备本地 SoftAP 中提供，热点为开放网络，配网时应避免无关人员接入。
- 日志只允许打印“credentials redacted”，不得输出凭据明文或完整鉴权 URL。

## 6. AIUI 平台与固件配置

当前默认配置：

| 参数 | 值 | 说明 |
| --- | --- | --- |
| `aiui_ver` | `3` | RTOS 极速超拟人链路 |
| `scene` | `main` | 已审核发布应用的正式场景 |
| `voice_name` / VCN | `x6_wuyediantai_mini` | 当前发音人 |
| TTS 编码 | `opus-wb` | 16 kHz 单声道 Opus 流 |
| 交互模式 | `continuous_vad` | 连续语音轮次 |
| `new_session` | `false` | 同一次唤醒内保留上下文 |
| `uid` / SN | Wi-Fi STA MAC 派生 | 稳定设备标识 |

可通过 `idf.py menuconfig` 的 `DeskEmoji AIUI` 菜单修改：

- `ROBOT_AIUI_SCENE`：发布应用通常使用 `main`；`main_box` 是开发测试场景，不能作为
  已发布应用报错时的通用修复方式。
- `ROBOT_AIUI_VCN`：TTS 发音人，当前为 `x6_wuyediantai_mini`。
- `ROBOT_SESSION_IDLE_TIMEOUT_SECONDS`：在线会话空闲超时，默认 60 秒。
- `ROBOT_STA_CONNECT_TIMEOUT_SECONDS`：已保存 Wi-Fi 的连接等待时间，默认 60 秒。
- ESP-SR、表情标签、手势、随机动画和双舵机功能开关。

修改 VCN 或 scene 后需要重新编译并烧录应用。平台侧也必须为当前 APPID 开通对应
发音人和能力；否则即使 WebSocket 连接成功，TTS 仍可能返回授权错误。正常启动日志应包含：

```text
cfg loaded: ver=3, scene=main, uid=..., vcn=x6_wuyediantai_mini
offline wake ready: 小飞小飞 or BOOT short press
```

## 7. 音频与连续对话

- 麦克风输入：16 kHz、单声道、S16 PCM；INMP441 以 32-bit I2S 左声道采集后转为 S16。
- I2S 采集回调：640 samples / 40 ms。
- AIUI 上传：拆成 320 samples / 640 bytes / 20 ms，满足 RTOS WebSocket 单包限制。
- 本地 VAD：参考 7911 的起止策略，检测到语音结束后发送本轮音频 EOS。
- TTS：WebSocket 流式接收 `opus-wb`，在独立任务中解码，按实际 PCM 约 1 秒预缓冲后播放。
- 短回复收到流结束标记后可以提前开播；异常缺失终帧时由超时恢复逻辑回收连接。
- 播放为半双工：TTS/音乐期间暂停麦克风上传，避免单麦、无 AEC 设备自激。
- AIUI 意外断开时最多重试 3 次，退避约 1/2/3 秒；重试失败后回到离线唤醒。

## 8. 音乐技能

音乐链路按以下顺序处理：

1. 解析 AIUI `musicX`、`media` 或 `drama` 语义结果。
2. 若结果带可播放 URL，直接加入播放列表。
3. 若结果只有 `itemid`，通过讯飞音乐服务换取签名播放地址。
4. 若 `musicX` 返回空曲目或 `rc=3`，提取用户查询词，通过已绑定网易云账号直接搜索。
5. 兜底搜索成功后抑制“歌曲走丢了”等失败 TTS，改为播放本地正常播报并启动歌曲。
6. 搜索失败也保持连续对话，不因技能 `shouldEndSession=true` 强制休眠。

音乐服务请求会使用当前 APPID、APPKEY/APPSECRET 生成的 HMAC-SHA256 鉴权、Wi-Fi
STA MAC 派生序列号和局域网 IP。使用前必须在讯飞平台完成音乐技能开通、账号登录与
设备绑定，并确保设备时间已经同步。不要在日志中输出完整签名参数。

歌曲通常是 44.1 kHz、双声道 MP3；ESP-GMF 会解码并转换为 16 kHz、单声道、16 bit
后送入统一播放队列。出现下面警告但随后进入 `ESP_AUD_SIMPLE_PLAYER_RUNNING` 时，
只是解码器把输出缓冲区从 1024 扩到 4608 字节，不影响播放：

```text
ESP_GMF_ASMP_DEC: Not enough memory for out, need:4608, old: 1024, new: 4608
```

播放音乐或 TTS 时短按 BOOT 可以打断，并继续当前对话，不主动清除 AIUI 上下文。

## 9. 表情、动作与手势

OLED 表情沿用 `esp32-s3n16r8-emoji/emoji_controller.cc` 的风格，包括：中性、开心、
难过、生气、惊讶、思考、困惑、尴尬、哭、大笑、搞怪、爱心、震惊、酷、放松、美味、
亲吻、自信、调皮和睡眠。

动作支持：眨眼、左看、右看、抬头、低头、回中、点头、摇头、转圈、跳舞、唤醒和
休眠。动作、舵机与对应表情统一进入一个队列执行，避免屏幕动作与机械动作错位。
空闲时每约 10 秒随机执行一次行为，当前概率为：60% 眨眼、18% 左看、18% 右看、
1% 转圈、3% 困惑。

语音文本和 AI 回复中的表情/动作标签均可触发动作；没有结构化标签时使用中文关键词
回退，例如“点头”“摇头”“向左看”“跳舞”“开心”“难过”“思考”等。

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

方向手势按设备上 PAJ7620U2 的实际安装角度进行了坐标换算。为避免手停留在传感器前
造成连续误触，单次动作后有 900 ms 冷却，并且传感器连续安静约 200 ms 后才会重新
接收下一次手势；同时会丢弃一次上报多个方向的模糊结果。连续点头/摇头手势须在 4 秒
内完成。传感器使用完整官方寄存器表初始化，连续通信失败时会自动停用，避免与其共用
I2C 总线的 OLED 被故障传感器拖住。

## 10. 语音退出与节能

以下识别文本会直接结束在线对话、关闭 WebSocket、显示休眠表情，并重新启用离线
唤醒：“你退下吧”“退下吧”“退下”“你休息吧”“去休息吧”“休息吧”“退出吧”
“退出”“闭嘴”“再见”“拜拜”“结束”。

在线会话默认 60 秒无有效活动后也会自动结束，并播放 `assets/aiui_exit.wav`。待机时
AIUI WebSocket 不连接，只保留本地麦克风采集和离线唤醒，降低网络与云端功耗。

## 11. 构建

在已导出 ESP-IDF 5.5.1 环境的终端进入本目录：

```powershell
cd esp32
idf.py build
```

首次构建、修改 Kconfig 后配置未刷新时执行：

```powershell
idf.py reconfigure
idf.py build
```

一般不需要 `fullclean`。只有构建目录跨 IDF 版本、工具链或目标芯片后出现无法解释的
CMake 缓存冲突时才使用：

```powershell
idf.py fullclean
idf.py build
```

主要构建产物：

| 文件 | 用途 |
| --- | --- |
| `build/deskemoji_robot.bin` | 应用固件 |
| `build/bootloader/bootloader.bin` | 二级 Bootloader |
| `build/partition_table/partition-table.bin` | 分区表 |
| `build/srmodels/srmodels.bin` | ESP-SR/MultiNet 模型 |
| `build/assets.bin` | SPIFFS 提示音资源 |

当前应用约 1.91 MiB，应用分区为 8 MiB，空间充足；实际大小以每次构建末尾输出为准。

## 12. 烧录、监视和合并固件

将 `COMx` 替换为设备实际串口：

```powershell
idf.py -p COMx flash monitor
```

`flash` 会按项目清单同时烧录应用、分区表、模型和资源。新设备不要只单独写入
`deskemoji_robot.bin`，否则会缺少离线唤醒模型或提示音。

生成量产合并固件：

```powershell
idf.py merge-bin
```

`build/merged-binary.bin` 为完整 16 MB 是正常现象，与 N16R8 的 Flash 容量一致，并非
应用超出分区。它包含各分区在固定偏移处的数据和空白填充。烧录合并固件时从地址 0
写入；开发调试仍优先使用 `idf.py flash`。

当前分区布局：

| 分区 | 偏移 | 大小 | 内容 |
| --- | ---: | ---: | --- |
| `nvs` | `0x009000` | `0x006000` | Wi-Fi、AIUI 凭据和音量 |
| `phy_init` | `0x00F000` | `0x001000` | RF 数据 |
| `factory` | `0x010000` | `0x800000` | 应用 |
| `model` | `0x810000` | `0x300000` | ESP-SR 模型 |
| `assets` | `0xB10000` | `0x4F0000` | SPIFFS 提示音 |

## 13. 关键启动日志

正常冷启动应依次看到：

```text
[init 1/5] OLED
[init 2/5] motion queue
[init 3/5] NVS and Wi-Fi
[init 4/5] I2S audio
[init 5/5] buttons
sta ip: ...
[online 1/3] AIUI credentials and runtime
cfg loaded: ver=3, scene=main, uid=..., vcn=x6_wuyediantai_mini
[online 2/3] ESP-SR MultiNet custom wake
custom wake phrase ready: 小飞小飞
[online 3/3] gesture sensor
offline wake ready: 小飞小飞 or BOOT short press
```

唤醒和一轮正常对话通常包含：

```text
offline wake word detected
AIUI connected
local VAD: speech started
local VAD: speech ended; AIUI audio end sent
AIUI recognized: ...
result sub=nlp ...
result sub=tts ...
AIUI Opus stream complete
```

## 14. 常见问题

| 日志/现象 | 判断与处理 |
| --- | --- |
| `err(20002) ... app config not exists` | 当前 APPID/凭据/scene 找不到应用配置。已发布应用使用 `scene=main`，确认三项凭据来自同一应用。 |
| `err(20019)`、`LiccCheck failed`、`11200` | 发音人未授权、额度受限或 VCN 不属于当前应用。确认平台为当前 APPID 开通 `x6_wuyediantai_mini`。 |
| WebSocket `101 Switching Protocols` 后无结果 | 只代表连接成功。继续检查麦克风峰值、VAD、音频 EOS、IAT/NLP/TTS 日志。 |
| `getaddrinfo() returns 202` | DNS/网络不可用，不是 APPKEY 错误；检查网关、DNS、信号和网络时间。 |
| TLS connect/read/write timeout | 网络抖动或服务端连接中断。会话内会自动重试 3 次；持续发生时检查 Wi-Fi RSSI、路由器和时间同步。 |
| `playback buffer underrun` | TTS 分片到达速度低于播放消耗，会产生可听断音；当前已有约 1 秒预缓冲，若连续出现需保留完整 TTS chunk 时间日志排查网络。 |
| `TTS final frame missing` / `TTS stream stalled` | 云端未及时下发最终状态帧；固件会回收残留音频并重连。频繁出现时记录完整 SID 和分片序列。 |
| `I2C transaction failed` / `gesture sensor unavailable` | PAJ7620U2 NACK、供电/接线或初始化表不兼容。OLED 可继续运行，但手势功能降级。检查 GPIO41/42、地址 `0x73` 和模组电压。 |
| `ESP_GMF_ASMP_DEC: Not enough memory for out` | 解码器动态扩容警告。随后若出现播放器 RUNNING 且音乐正常，可忽略。 |
| 音乐技能说“歌曲走丢了” | 检查 AIUI 音乐技能、网易云绑定、同一 APPID/设备 SN、NTP 和讯飞音乐接口响应；固件会尝试直接搜索兜底。 |
| 音乐搜索失败后无法对话 | 当前代码会强制保持本地连续会话；若仍复现，请从 IAT 到播放结束/断连保留完整日志。 |
| 休眠后无法唤醒 | 正常应看到 `offline wake re-armed; microphone capture resumed`。检查是否仍在播放、麦克风是否暂停以及 ESP-SR 是否已启动。 |
| `merged-binary.bin` 恰好 16 MB | 正常，是整片 Flash 镜像，不代表应用大小为 16 MB。 |

## 15. 代码结构

```text
esp32/
├─ main/                         应用状态机、AIUI 结果和媒体编排
├─ components/aiui_sdk/          AIUI RTOS v3 协议与结果解析
├─ components/aiui_port/         FreeRTOS、Socket、TLS 平台适配
├─ components/robot_network/     Wi-Fi、配网页面、NTP、音乐鉴权
├─ components/robot_audio/       单麦采集、I2S 播放、Opus 解码和缓冲
├─ components/robot_wakeup/      ESP-SR MultiNet 离线唤醒
├─ components/robot_ui/          SSD1306 表情、动画和配网滚动显示
├─ components/robot_motion/      舵机、动作队列和随机行为
├─ components/robot_gesture/     PAJ7620U2 手势
├─ assets/                       本地提示音
├─ partitions.csv                16 MB 分区表
├─ sdkconfig.defaults            默认构建配置
```

## 16. 实板验收清单

- [ ] 无 NVS 时自动进入配网，OLED 和语音提示正确。
- [ ] 网页能扫描并选择附近 2.4 GHz Wi-Fi，保存后自动重启。
- [ ] 凭据不出现在源码和普通日志中。
- [ ] 联网后先校时，再初始化 AIUI；启动日志中的 scene 和 VCN 正确。
- [ ] “小飞小飞”、BOOT 和前推手势均可从离线待机唤醒。
- [ ] 单轮识别、连续上下文、流式 TTS 和播完恢复收音正常。
- [ ] 播放期间 BOOT 能打断，不丢失本地连续对话状态。
- [ ] 60 秒空闲后播放结束提示、断开 AIUI，并能再次唤醒。
- [ ] 退出类语音进入睡眠表情后仍可通过唤醒词或 BOOT 唤醒。
- [ ] 音量加减、长按最大/静音和重启持久化正常。
- [ ] AIUI 音乐 URL、itemid 解析和网易云搜索兜底均可播放。
- [ ] 表情与舵机动作同步，随机行为与原 DeskEmoji 风格一致。
- [ ] PAJ7620U2 九类手势与动作映射正确，无持续 I2C NACK。
- [ ] 长时间运行无任务栈溢出、播放队列卡死或无法重新唤醒。

更详细的历史静态验证记录见 `VERIFICATION.md`；实板日志始终以当前构建版本为准。
