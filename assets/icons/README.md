# 通用应用图标

这套图标使用项目配色（青蓝 + 珊瑚信号眼）绘制，中心图形保持极简，便于 16px 到 512px 都保持可识别。

## 文件说明

- `aidesk-icon.svg`：矢量源文件，适合继续改色、改形。
- `aidesk-icon-*.png`：通用 PNG，常用 512/192/180/144/128/96/64/48/32/16。
- `android/aidesk-adaptive-background-*.png`：Android 自适应图标背景层（全幅渐变）。
- `android/aidesk-adaptive-foreground-*.png`：Android 自适应图标前景层（透明背景，已留安全边距）。
- `windows/aidesk-icon.ico`：Windows/Electron/PC 桌面图标，内含 16–256px 多尺寸。
- `macos/aidesk-icon.icns`：macOS 应用图标。
- `scripts/generate-icons.py`：生成脚本，依赖 Pillow，重跑命令为 `python scripts/generate-icons.py`。

## 使用建议

- 网页端：已把 `favicon.svg`、`favicon.ico`、`apple-touch-icon.png` 放入 `web/public/`，并在 `web/index.html` 引用。
- 微信小程序：上传 `aidesk-icon-512.png` 或 `aidesk-icon-192.png` 即可。
- Android：把 `android/` 下对应尺寸的 background / foreground PNG 放入 `mipmap-*` 资源目录，再配置 adaptive icon。
- Windows / Electron：使用 `windows/aidesk-icon.ico`。
- macOS：使用 `macos/aidesk-icon.icns`。
