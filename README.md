# DRAW / RTC

Windows x64 的 OpenCASCADE DRAW Harness 插件。DRAW 在已登录的桌面会话中创建隐藏的 OpenGL 视图，浏览器通过 WebRTC 接收 H.264 视频，并可旋转、平移、缩放、点选及执行任意 DRAW / Tcl 脚本。

目标 SDK：`C:\Programs\OpenCASCADE-7.7.0-vc14-64\opencascade-7.7.0`。

## 构建

需要 VS2022 的“使用 C++ 的桌面开发”、Windows SDK、Git 和网络访问。使用 Release x64，与指定 SDK 的 DLL / CRT 配套。

```powershell
cd C:\Projects\occ-rtc
.\scripts\build.ps1
```

脚本优先使用 PATH 中的 CMake，否则使用 VS2022 附带的 CMake。第一次会获取固定版本的依赖：libdatachannel 0.24.5、Mbed TLS 3.6.5 和 cpp-httplib 0.18.3。WebRTC 依赖静态链接进插件；视频编码使用 Windows Media Foundation。

产物：

- `build/bin/Release/TKDrawRTC.dll`
- `build/bin/Release/web/index.html`
- `build/resources/DrawPlugin`（保留 SDK 的插件注册项并添加 DRAWRTC）

指定其他 SDK 路径：

```powershell
.\scripts\build.ps1 -OcctRoot 'D:\SDK\opencascade-7.7.0'
```

## 启动与使用

```powershell
.\scripts\run.ps1
```

启动脚本会输出 DRAW 进程 PID、日志路径和带有随机访问令牌的浏览器地址。打开该地址即可看到示例模型。DRAWEXE 及其 3D 视图均隐藏；日志保存在 `build/draw-8080.log` 和 `build/draw-8080.err.log`。

```powershell
# 局域网监听；其他电脑将输出地址中的 127.0.0.1 换成此机器的局域网 IP。
.\scripts\run.ps1 -BindAddress 0.0.0.0 -Port 8080

# 空场景，指定帧率、码率及固定令牌。
.\scripts\run.ps1 -NoDemo -Fps 30 -Bitrate 4000000 -Token 'your_own_long_random_token'
```

局域网需要允许 TCP `Port`（页面）、TCP `Port+1`（信令）及 UDP `50000–50100`（ICE / WebRTC）。默认仅监听 `127.0.0.1`，无需 STUN / TURN。首版为一个 DRAW 会话、一幅视图、一个浏览器连接。

访问令牌同时授予远程 Tcl 执行权限，命令拥有 DRAWEXE 进程的完整权限。默认 HTTP / WS 面向本机及可信局域网；WebRTC 视频与 DataChannel 使用 DTLS / SRTP。请将带令牌的地址视作会话凭据。

浏览器操作：

三维画面占满浏览器视口，状态、工具条和选择提示叠加在画面上。Tcl 控制台默认收起，可通过右上角“控制台”或左下角“展开”打开；拖动标题栏可移动面板，拖动右下角可调整大小。面板展开、收起和移动均不改变视频的分辨率。

| 操作 | 输入 |
|---|---|
| 旋转 | 左键拖动 |
| 平移 | 中键 / 右键拖动，或 Shift + 左键拖动 |
| 缩放 | 滚轮 |
| 点选 | 单击，Ctrl + 单击切换选择 |
| 适合窗口 | 顶部按钮或双击 |
| 执行 Tcl | 控制台“执行”按钮或 Ctrl + Enter |

点选后显示 DRAW 对象名称。Tcl 支持多行脚本、持久变量、UTF-8 输出、stderr 和错误堆栈。示例：

```tcl
vclear
box part 80 60 40
pcylinder cutter 12 60
bcut result part cutter
vdisplay -dispMode 1 result
vfit
```

浏览器视图区或设备像素比变化时自动调整服务端视图与编码尺寸，保持比例，采用偶数像素，上限为 1920 × 1080。H.264 参数集随关键帧更新，无需重建 PeerConnection。

停止串流并退出启动脚本创建的 DRAW 进程，可在浏览器控制台执行：

```tcl
after 100 {rtcstop; set ::drawrtc_shutdown 1}
```

## DRAW 命令

启动脚本已配置好插件搜索路径。在相同环境的 DRAWEXE 中可使用：

```tcl
pload MODELING VISUALIZATION DRAWRTC
rtcinit -hidden 1 -width 1280 -height 720
box demo 80 60 40
vdisplay -dispMode 1 demo
vfit
rtcstart -bind 127.0.0.1 -port 8080 -fps 30 -bitrate 4000000
rtcstatus
rtcstop
```

| 命令 | 说明 |
|---|---|
| `rtcinit [-hidden 1] [-width W] [-height H]` | 创建 / 激活隐藏视图，或调整尺寸 |
| `rtcstart [-bind IP] [-port P] [-fps F] [-bitrate B] [-token T]` | 启动页面、信令、编码和 WebRTC，返回带令牌的地址 |
| `rtcstatus` | JSON：分辨率、相机、帧数、丢帧、字节数、命令数和最近错误 |
| `rtcstop` | 停止串流，释放网络和编码器；保留模型与视图，可再次启动 |

端口范围为 1024–65534，帧率 1–60，码率 200000–50000000 bit/s。自定义令牌须为 16–128 个 ASCII 字母、数字、下划线或连字符。

使用 `DRAWEXE -v -f script.tcl` 启动长期服务时，脚本末尾需要 `vwait` 保持 Tcl 事件循环；`scripts/run.ps1` 已包含该逻辑。初始化后，标准 `vdisplay` / `vfit` / `vsetcolor` 等命令继续操作同一 ViewerTest 视图。

## 架构与边界

```text
DRAW / Tcl 所属线程：
  Tcl 定时器 → 控制请求队列 → OCCT 场景 / 相机 / 选择
                            → 隐藏 WNT_Window + V3d_View
                            → ToPixMap → BGRA 内存帧

编码线程：最新帧队列（容量 1）→ NV12 → Media Foundation H.264
                            → libdatachannel RTP / SRTP → 浏览器 video

网络线程：HTTP 页面 + WS 信令 + WebRTC DataChannel
          控制请求排队交给 Tcl 线程；不直接调用 OCCT / Tcl
```

支持 RTCP Sender Report、NACK 重传、PLI / FIR 关键帧请求，以及 REMB 码率反馈。浏览器协商为 DTLS passive，避免 Mbed TLS 3.6 对新版浏览器分片 ClientHello 的兼容性问题。

当前使用 OpenGL 像素回读和软件 H.264 编码，实际帧率取决于模型、分辨率和 CPU；尚未接入 GPU 零拷贝 / NVENC。编码尺寸变化时重新创建编码器，RTP 时间戳保持连续。长时间运行的 Tcl / 建模命令会暂停画面及交互，直到所属线程返回事件循环。控制台拦截 Tcl stdout / stderr；原生库直接写入操作系统 stdout / stderr 的内容保留在 DRAW 日志。

默认目标帧率为 30，`run.ps1 -Fps 60` / `rtcstart -fps 60` 可设为 60；1–60 是当前代码的参数校验范围。Tcl 定时器按目标帧率安排采集，实际采集仍包含 `ToPixMap()` 重新绘制和同步像素回读、内存分配与复制，编码线程再执行 CPU BGRA→NV12 转换及软件 H.264 编码。

本机示例方块的独立测量（预热 2 秒，每组采样约 5 秒，Chromium 本机接收）：

| 分辨率 | 目标 FPS | 采集 FPS | 接收 FPS | 队列丢帧 |
|---|---:|---:|---:|---:|
| 1280 × 720 | 30 | 22.2 | 22.0 | 0 |
| 1280 × 720 | 60 | 40.3 | 40.1 | 0 |
| 1920 × 1080 | 30 | 21.7 | 22.1 | 0 |
| 1920 × 1080 | 60 | 41.5 | 39.7 | 8 |

720p 的采集与发送基本同步且无队列丢帧，瓶颈更指向采集 / 调度；1080p、目标 60 时编码队列开始丢弃旧帧。浏览器平均单帧解码约 1–2.3 ms。这些计数不能进一步区分渲染、回读和调度各自的耗时，需要阶段计时才能确定其占比。

这是已登录桌面的隐藏渲染方案。Windows 服务 / Session 0、公网 TURN、多浏览器协作和多视图不在当前实现范围。

## 验证

```powershell
npm install
npx playwright install chromium
npm test
```

端到端测试使用真实 DRAWEXE、插件、Windows 编码器和 Chromium，检查 H.264 解码、隐藏窗口、远程 Tcl、建模更新、点选、旋转 / 平移 / 缩放、连续调整分辨率、连接恢复及服务停止后端口重用。报告及截图写入 `test-results/`。

主要文件：`src/Plugin.cxx` 注册命令，`src/Session.cxx` 实现会话 / 渲染 / 交互 / 网络，`src/Encoder.cxx` 封装编码器，`web/index.html` 提供无构建步骤的浏览器客户端。

上游接口参考：[OCCT DRAW 插件](https://github.com/Open-Cascade-SAS/OCCT/blob/V7_7_0/src/Draw/Draw_PluginMacro.hxx)、[libdatachannel](https://github.com/paullouisageneau/libdatachannel)、[Windows H.264 编码器](https://learn.microsoft.com/en-us/windows/win32/medfound/h-264-video-encoder)。依赖源代码及其许可证随各自仓库保存在 `third_party/`。
