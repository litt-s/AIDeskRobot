# AIUI Verification 报告

验证日期：2026-07-23

## 总览

- Phase 1 Build：PASS。ESP-IDF 5.5.1 全量构建成功。
- Phase 2 Static：PASS（受限）。编译器未报告工程代码错误；本机未安装
  `cppcheck`，以 ESP-IDF 的 C/C++ 编译检查替代。AIUI 上游源码有两类已知
  warning，已限定在该组件内，不影响链接。
- Phase 3 Tests：SKIPPED `[HARDWARE_REQUIRED]`。嵌入式工程无主机单元测试；
  麦克风、扬声器、OLED、舵机和手势需要实板验证。
- Phase 4 Contract：PASS。配网、NTP 门禁、离线唤醒、BOOT、AIUI 连续会话、
  TTS 半双工、表情标签和动作队列的生产/消费路径一致。
- Phase 5 Security：PASS。用户提供的三项 AIUI 凭据未出现在源码、配置、
  构建脚本或资源中；AIUI HTTP/鉴权日志已脱敏；`.env`、`secrets/` 已忽略。
- Phase 6 Acceptance：静态可验证项目全部 PASS；联网与实板行为待烧录验收。
- Phase 7 Real SDK Smoke：SKIPPED `[NETWORK_REQUIRED] [HARDWARE_REQUIRED]`。
  真实 AIUI RTOS 源码、ESP-SR 模型和 Opus 资源已参与最终链接，不含
  stub/mock；云端鉴权和对话必须在实板联网后执行。

## 构建产物

- 应用：`build/deskemoji_aiui.bin`（约 1.53 MiB）
- 应用分区：0x800000 字节，剩余约 81%
- 离线唤醒模型：`build/srmodels/srmodels.bin`
- 配网音频资源：`build/assets.bin`

## 完整烧录布局

```text
0x000000  build/bootloader/bootloader.bin
0x008000  build/partition_table/partition-table.bin
0x010000  build/deskemoji_aiui.bin
0x810000  build/srmodels/srmodels.bin
0xB10000  build/assets.bin
```

## 实板终验清单

- [ ] 无 NVS 配置时自动进入 SoftAP，并播放“配网模式”。
- [ ] 手机能打开 `192.168.4.1`，保存 Wi-Fi 与 AIUI 凭据后重启。
- [ ] NTP 成功后才开始 AIUI 鉴权，串口日志不出现凭据。
- [ ] “小飞小飞”和 BOOT 短按均能开始会话，且分别播放 7911 对应提示音。
- [ ] 连续对话空闲 60 秒后停止上传、关闭 AIUI，播放 7911 `AiuiExit`，随后必须重新唤醒。
- [ ] 连续对话正常，TTS 时无明显自激，播完恢复收音。
- [ ] 60 秒无活动或断网后回到离线唤醒，网络恢复不自动开会话。
- [ ] 表情标签、NLP 回退、语音动作和九类手势均只通过动作队列执行。
- [ ] 舵机方向、限位和 PAJ7620U2 灵敏度符合实物机构。
