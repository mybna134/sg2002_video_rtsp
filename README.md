# LicheeRV Nano MMF RTSP Server

面向 LicheeRV Nano（SG2002）的摄像头硬件编码和 RTSP 串流工具：

```text
MIPI Sensor -> VI/ISP => VPSS Online (NV21, scale, wrap) -> VENC Bind -> cvi_rtsp -> RTSP Client
                         |     hardware path      |  | MMF Bind |
```

工程以 `sophgo/cvi_rtsp` 的 `sg200x-dev/example/rtsp_server_video.cpp` 为基础，但使用当前
LicheeRV Nano SDK 的 `SAMPLE_COMM_VI_IniToViCfg()` 初始化接口，并把官方示例的手动取帧/送帧改为
`VPSS -> VENC` 绑定模式。ISP 到 VPSS 使用官方 `cvi_rtsp` service 在 SG2002 上默认采用的
`VI_OFFLINE_VPSS_ONLINE` 路径，应用线程只执行
`QueryStatus -> GetStream -> WriteFrame -> ReleaseStream`。

传感器实际分辨率到 `--width/--height` 编码分辨率的缩放由 SG2002 的 VPSS 硬件通道执行。程序只给
`VPSS_CHN_ATTR_S` 设置目标宽高，不读取、映射或用 CPU 缩放原始帧。默认还启用 VPSS 的低延迟
wrap/SBM 缓冲（64 行、3 段），让 VPSS 和 VENC 以分段循环缓冲方式衔接。

## 已固定的官方依赖

| 依赖 | 版本 |
| --- | --- |
| LicheeRV-Nano-Build | `d4003f15b35d43ad4842f427050ab2bba0114fa5` |
| cvi_rtsp (`sg200x-dev`) | `58c825fa538731e7c230a1379afd2f33cf74592c` |
| live555 | SDK 自带的 musl 预编译包，`2020.07.21` |
| Xuantie RISC-V musl GCC | SDK host-tools，GCC `10.2.0` |
| xmake | 系统可用版本，缺失时安装固定的 `3.0.9` bundle |

`cvi_rtsp` 依赖旧版 live555 API，不能直接换用 Nano buildroot 中的 `2021.05.03` 源码接口。
构建脚本使用 SDK `oss_release_tarball/musl_riscv64/live555.tar.gz` 中与 SG200x 库匹配的版本。

## 一键构建

```bash
./scripts/build.sh
```

首次执行会自动：

1. 下载并校验约 841 MiB 的官方 `host-tools.tar.gz`；
2. 在项目的 `.tools/` 中安装 RISC-V musl 工具链和 xmake；
3. 稀疏检出 Nano SDK 到 `.sdk/LicheeRV-Nano-Build/`；
4. 获取固定版本的 `cvi_rtsp`，解出 SDK 自带的 live555；
5. 生成 SG2002 UAPI 头文件目录并构建 MMF middleware；
6. 使用 xmake 交叉编译最终程序。

输出文件：

```text
build/cross/riscv64/release/licheerv-nano-rtsp
```

首次安装后，各阶段也可以单独执行：

```bash
./scripts/install-tools.sh
./scripts/prepare-sdk.sh
./scripts/fetch-dependencies.sh
./scripts/build-middleware.sh
./scripts/build.sh
```

调试构建：

```bash
MODE=debug ./scripts/build.sh
```

已有完整 Nano SDK 时，可指定路径：

```bash
NANO_SDK_ROOT=/path/to/LicheeRV-Nano-Build ./scripts/build.sh
```

该 SDK 必须包含为 `sg2002_licheervnano_sd` 构建的 `middleware/v2/lib`，并与板端固件中的 MMF
用户态库兼容。

## 摄像头配置

传感器类型、I2C bus、MIPI device、lane 和 PN swap 必须使用实际摄像头配置。程序按 SDK 规则依次
读取：

```text
--sensor-config 指定的路径
/mnt/data/sensor_cfg.ini
/mnt/system/usr/bin/sensor_cfg.ini
```

建议先在开发板验证：

```bash
/mnt/system/usr/bin/sensor_test
# 或
/mnt/system/usr/bin/sample_vio
```

不要使用官方 RTSP 示例的 IMX327 默认值替代不匹配的传感器配置。本工程在配置解析失败时直接退出，
避免以错误 lane/I2C 参数继续初始化硬件。

## 部署和运行

```bash
./scripts/deploy.sh root@192.168.1.100
```

同时部署一个传感器配置：

```bash
SENSOR_CONFIG=/path/to/sensor_cfg.ini \
  ./scripts/deploy.sh root@192.168.1.100
```

在板端运行：

```bash
/root/licheerv-nano-rtsp \
  --sensor-config /mnt/data/sensor_cfg.ini \
  --codec h264 \
  --width 1920 \
  --height 1080 \
  --fps 30 \
  --gop 30 \
  --bitrate 4000 \
  --port 8554 \
  --stream-name live
```

查看全部参数：

```bash
/root/licheerv-nano-rtsp --help
```

如果当前板端 middleware 不支持 wrap，启动会在 `SAMPLE_COMM_VPSS_WRAP_Start` 处报告错误，可用普通
整帧 VPSS 缓冲回退：

```bash
/root/licheerv-nano-rtsp --no-wrap --sensor-config /mnt/data/sensor_cfg.ini
```

播放：

```bash
ffplay \
  -rtsp_transport tcp \
  -fflags nobuffer \
  -flags low_delay \
  rtsp://192.168.1.100:8554/live
```

也可以选择 H.265：

```bash
/root/licheerv-nano-rtsp --codec h265 --port 8554 --stream-name live
```

新客户端进入 RTSP `PLAY` 状态时程序只请求一次 IDR，以缩短首画面等待时间。

### VLC 低延迟播放

可靠的有线网络上，最低延迟配置使用 RTP/UDP、关闭网络缓存和时钟同步。先关闭硬件解码以排除
客户端解码器问题：

```bash
vlc \
  --no-rtsp-tcp \
  --network-caching=0 \
  --live-caching=0 \
  --clock-jitter=0 \
  --clock-synchro=0 \
  --avcodec-hw=none \
  rtsp://192.168.1.100:8554/live
```

程序的 H.264 默认使用 Baseline + CAVLC，并将 CBR initial delay 从 SDK 默认的 1000 ms 降为
SDK 允许的最低值 10 ms。IDR 在客户端实际进入 RTSP `PLAY` 状态后请求，保证 RTSP media source
已开始接收数据。
确认软件解码显示正常后，可以删除 `--avcodec-hw=none` 再测试客户端硬件解码。

VLC 仍有明显缓冲时，使用 MPV 的低延迟 profile 做对照：

```bash
mpv \
  --profile=low-latency \
  --cache=no \
  --untimed \
  --video-sync=desync \
  --demuxer-lavf-o=rtsp_transport=udp \
  rtsp://192.168.1.100:8554/live
```

或使用 FFplay：

```bash
ffplay \
  -rtsp_transport udp \
  -fflags nobuffer \
  -flags low_delay \
  -framedrop \
  -probesize 32 \
  -analyzeduration 0 \
  -sync ext \
  rtsp://192.168.1.100:8554/live
```

如果 UDP 环境存在丢包，再切回 TCP，并只保留 50 ms 缓存：

```bash
vlc \
  --rtsp-tcp \
  --network-caching=50 \
  --live-caching=0 \
  --clock-jitter=0 \
  --clock-synchro=0 \
  --avcodec-hw=none \
  rtsp://192.168.1.100:8554/live
```

绿色滤镜的服务端原因是 SG2002 的 VI 输出为 NV21，而旧代码错误地把 VPSS Group 输入声明成
YUV420 Planar。当前版本让 VI、VPSS 输入和 VPSS 输出保持 NV21，避免错误解释 VU 色度平面及
不必要的像素格式转换。

VPSS、VENC 和 GOP 应与 ISP 的实际运行帧率一致。GC4653 30 FPS 模式可使用：

```bash
/root/licheerv-nano-rtsp \
  --sensor-config /mnt/data/sensor_cfg.ini \
  --fps 30 \
  --gop 30 \
  --bitrate 3000
```

## 运行时依赖

`cvi_rtsp` 和 live555 已静态链接到程序。MMF、ISP 和 sensor 库仍由板端固件提供，ELF 的 RPATH 为：

```text
/mnt/system/lib:/mnt/system/lib/3rd:/usr/lib
```

如果启动时报某个 `lib*.so` 不存在，说明板端固件没有安装完整 middleware，或固件与构建 SDK
不一致。优先使用匹配固件的 SDK 重编译，不要随意覆盖板端 ISP/sensor 库。

## 清理顺序

程序退出时按以下顺序释放资源：

```text
停止 VENC 取流线程
-> 停止并销毁 RTSP session/context
-> UnBind VPSS -> VENC
-> 停止 VENC
-> 停止 VPSS
-> 销毁 ISP/VI
-> SYS/VB Exit
```

`CVI_VENC_GetStream()` 成功后，无论 RTSP 是否有客户端，都会调用
`CVI_VENC_ReleaseStream()`。超过 `CVI_RTSP_DATA_MAX_BLOCK`（8）的 pack 会先合并为一个连续的
Annex-B access unit。

程序每 5 秒输出一次 `VENC queue age`，它是编码包 PTS 到应用取流时刻的差值。如果该值只有
几十毫秒，而肉眼端到端延迟接近 1 秒，主要缓冲位于 RTSP 客户端、解码器或显示同步阶段。
`CVI_RTSP_WriteFrame` 单次阻塞超过 50 ms 时程序也会打印告警，表示客户端已经向服务端施加背压。

最低延迟应先用上面的 MPV 或 FFplay UDP 命令测量。VLC 即使网络缓存设为 0，也可能在 RTSP、解码
和显示时钟层继续排队；有线网络只能降低传输抖动，不能消除这些客户端缓冲。若 `VENC queue age`
持续低于约一帧到两帧，服务端继续调码率或网络参数通常不会消除剩余的近 1 秒延迟。

## `CVI_VENC_GetStream` 返回 `0xc0078012`

`0xc0078012` 是 `CVI_ERR_VENC_BUSY`。SDK 自带的 `sample_common_venc.c` 也会在该返回值下重试，
它不是 RTSP 错误，也不是共享库缺失。程序通过 `CVI_VENC_GetFd()` 和 `select()` 等待编码器可读。
默认 wrap/SBM 路径按官方 service 的顺序先调用 `CVI_VENC_StartRecvFrame()`，再绑定 VPSS 到 VENC；
`--no-wrap` 则使用 SDK 的普通 `VENC_BIND_VPSS` 路径。

如果只偶尔出现一次 `CVI_ERR_VENC_BUSY`，程序会自动恢复。如果随后出现：

```text
No encoded frame available for 5 seconds
```

则说明 VI、VPSS 或 VENC 没有继续产帧。先停止可能占用摄像头或 VENC 的其他程序，再单独运行本程序：

```bash
killall sample_venc sample_vi licheerv-nano-rtsp 2>/dev/null || true
/root/licheerv-nano-rtsp --sensor-config /mnt/data/sensor_cfg.ini
```

程序启动时还会用 `CVI_SYS_GetBindbyDest()` 校验 VENC 的输入绑定；绑定失败会在 RTSP Server 启动前
退出并打印具体 MMF 错误码。

## 官方资料

- [LicheeRV Nano MMF 开发指南](https://wiki.sipeed.com/hardware/zh/lichee/RV_Nano/8_mmf_development_guide.html)
- [LicheeRV-Nano-Build](https://github.com/sipeed/LicheeRV-Nano-Build)
- [cvi_rtsp SG200x 示例](https://github.com/sophgo/cvi_rtsp/blob/sg200x-dev/example/rtsp_server_video.cpp)
- [CVITEK/SOPHGO VPSS 设计说明](https://doc.sophgo.com/cvitek-develop-docs/master/docs_latest_release/CV180x_CV181x/en/01.software/MPI/Media_Processing_Software_Development_Reference/build/html/6_Video_Processing_Subsystem/Design_Overview.html)
- [CVITEK/SOPHGO VPSS wrap API](https://doc.sophgo.com/cvitek-develop-docs/master/docs_latest_release/CV180x_CV181x/en/01.software/MPI/Media_Processing_Software_Development_Reference/build/html/6_Video_Processing_Subsystem/API_Reference.html#cvi-vpss-setchnbufwrapattr)
- [CVITEK/SOPHGO VENC API](https://doc.sophgo.com/cvitek-develop-docs/master/docs_latest_release/CV180x_CV181x/en/01.software/MPI/Media_Processing_Software_Development_Reference/build/html/7_Video_Encoding/API_Reference.html)
