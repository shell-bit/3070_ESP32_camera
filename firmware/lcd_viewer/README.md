# LCD 按需预览客户端

目标板为 **Waveshare ESP32-S3-Touch-LCD-2.8 V2**（ST7789 + CST3530）。使用 Arduino-ESP32 **3.3.12**、LVGL **8.3.10**、16 MB Flash、OPI 8 MB PSRAM。此目录是独立 Arduino sketch，相机端位于 `../camera`。

当前 UI 采用已确认的 Catppuccin 配色，分为状态首页和全屏监控。Home 显示配置/Wi-Fi 状态并提供 Start；Status 显示现有连接、配置、观看和背光状态；Setup 提示 USB 配置步骤。三页均可通过 BOOT 开始观看。监控页保留原尺寸画面、倒计时、帧号、画面年龄、Continue 60s 和 Close；暂停/断线时明确标注旧画面，关闭或到期后返回 Home。没有新增天气和时钟。

## 目录职责

- `lcd_viewer.ino`：启动和主循环入口；Arduino 会递归编译 `src/`。
- `src/board/`：屏幕、触摸、背光和 JPEG 显示。
- `src/config/`：NVS 配置与 USB 配置接入。
- `src/preview/`：网络预览客户端、MJPEG 解析、会话时间和状态。
- `src/ui/`：页面视图、UI 控制和字体。
- `tests/`：协议/会话主机测试、UI 控制测试与离屏渲染入口；`tools/`：构建清单、离屏渲染等辅助脚本。

`build.ps1`、`prepare_dependencies.py` 保留在本目录；IDE 导出脚本为项目根目录的 `tools/export_lcd_arduino.py`。构建产物统一写入项目根目录 `build/lcd_viewer/`，LVGL 共用 `firmware/libraries/lvgl`。旧 `.build/arduino-data` 与 CLI 配置仍供相机共用，不迁移或删除。

## 构建

安装 Arduino CLI / ESP32 core 3.3.12 后，在 PowerShell 执行：

```powershell
python .\prepare_dependencies.py
.\build.ps1 -ArduinoCli 'path\to\arduino-cli.exe'
```

`build.ps1` 先用本机 `g++` 运行 LCD 协议/会话主机测试，再编译固件；只测试可传 `-HostTestsOnly`。可用 `-ConfigFile` 指向已有 Arduino CLI YAML，`-LibraryDirectory` 指向带有已固定 LVGL 库的目录。以下产物路径均相对项目根目录：固件输出位于 `build/lcd_viewer/arduino/`，日志为 `build/lcd_viewer/compile.log`。脚本不烧录。

上述主机测试覆盖 MJPEG 与会话协议；完整界面/控制验证在本目录另外运行 `python tools/render_ui.py`，需要 MinGW GCC/G++/ar、Python 和 Pillow。该脚本链接生产视图与固定 LVGL，生成 `build/lcd_viewer/ui-native/png/` 下 16 张离屏图及测试记录。图中的彩色画面是合成测试图，不是实机摄像头。固件编译成功后生成 `build/lcd_viewer/build-manifest.json`，记录源码与二进制哈希。

`lcd_viewer.ino.bin` 是应用镜像；`lcd_viewer.ino.merged.bin` 是从地址 `0x0` 使用的完整 16 MB 工厂镜像，会覆盖 NVS 配置。正常升级应使用 Arduino 常规上传流程，保留 NVS 时无需重新配对。构建时只把入口和 `src/` 源文件复制到干净的 `build/lcd_viewer/sketch/lcd_viewer/`，避免遍历本地 SDK 缓存。

Arduino IDE 请打开独立副本 `../../arduino/lcd_viewer/lcd_viewer.ino`。从仓库根目录运行 `python tools/export_lcd_arduino.py` 可导出与成功编译清单一致的源码；它不会覆盖已有的本地改动。不要直接在 IDE 中打开包含 `.build` 的开发目录，否则 IDE 会递归校验 SDK 内的 `extc++.h` 并报 Invalid sketch filename；不要重命名该编译器头文件。选择 **ESP32S3 Dev Module / Flash 16MB / OPI PSRAM / 16M Flash (3MB APP/9.9MB FATFS) / USB CDC On Boot Enabled**，安装上述脚本取得的 LVGL。源代码会拒绝 LVGL 9 或不匹配的 RGB565 配置。

## 配置与操作

首次启动时，通过 115200 波特率 USB 串口发送 `setup.example.json` 格式的一行 JSON，末尾换行。填入本地 Wi-Fi、相机局域网地址和相机端的 `PREVIEW_VIEW_TOKEN`；不使用云端上传地址。配置写入该显示板的 NVS 并重启，串口不回显密码/token。之后也可在停止观看时重新发送完整配置。

- 启动后短按 **BOOT（GPIO0）** 开始/结束。触摸界面提供 Start、Continue +60s、Close。
- 一次观看 60 秒；Continue 从点击时重新计算 60 秒。每 10 秒续租。视频读取使用另一任务/连接，不能代替续租。
- 停止后 3 秒关闭背光，停止收流和解码；Wi-Fi 保持连接并使用 modem sleep。没有启用深睡或触摸唤醒。
- 超过 1 秒没有新的成功解码帧，显示暂停/重连提示和画面年龄。PIR、登记、身份处理暂停分别显示原因。
- BOOT 在复位时仍是下载模式引脚；正常运行时短按使用。RESET 不作预览键，电池电源键也不作预览键。
- GPIO7 在程序运行时保持电池供电；本版未实现电池电源键的长按关机，关闭画面使用 BOOT/Close。

控制默认 HTTP 80，MJPEG 默认 81，均发送 `Authorization: Bearer ...`。会话 ID 只允许安全 URL 字符；`stream_url` 不被当作任意跳转目的地，流仍从配置的相机主机/端口 `/stream?session_id=...` 获取。协议是局域网明文 HTTP，请在受信任局域网使用。

## 内存与并发

只分配两份 128 KiB JPEG：一份解析中，一份最新完整帧。UI 解码占用第二份期间收流方丢弃新完整帧，不排队。解码使用两份 320×240 RGB565 缓冲，失败不破坏上一幅成功图。一个 UI 任务负责全部 LVGL、触摸、解码和 SPI 刷新；同步 SPI 完成后才 `lv_disp_flush_ready()`。

解析器限制 HTTP/part headers、chunk 扩展和 JPEG 长度，支持任意分片及 chunked multipart，验证 JPEG 头尾并交给真实解码器校验。断线和会话丢失重新连接；代次校验阻止旧 HTTP 结果/旧流污染新会话。相同序号重复出现不会刷新画面时间。

测试及实机待验收清单见 `../../docs/development/lcd-implementation.md`，官方驱动证据见 `THIRD_PARTY.md`。

本次界面改版、最终构建数据与本地待烧录交付包见 [UI 验证记录](../../docs/development/lcd-ui-implementation.md)。本次没有烧录设备。
