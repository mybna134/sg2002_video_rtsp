#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <string>
#include <sys/select.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <cvi_rtsp/rtsp.h>
#include <sample_comm.h>

namespace {

constexpr VPSS_GRP kVpssGroup = 0;
constexpr VPSS_CHN kVpssChannel = 0;
constexpr VENC_CHN kVencChannel = 0;
constexpr CVI_U8 kOnlineVpssDevice = 1;
constexpr PIXEL_FORMAT_E kPipelinePixelFormat = VI_PIXEL_FORMAT;

struct Options {
    std::string codec = "h264";
    std::string stream_name = "live";
    std::string sensor_config;
    unsigned width = 1920;
    unsigned height = 1080;
    int fps = 30;
    int gop = 30;
    int bitrate_kbps = 4000;
    int port = 8554;
    int max_clients = 4;
    bool low_latency_wrap = true;
};

struct PipelineState {
    bool system_started = false;
    bool vi_started = false;
    bool vpss_created = false;
    bool vpss_started = false;
    bool venc_started = false;
    bool vpss_venc_bound = false;
};

Options g_options;
PipelineState g_pipeline;
SAMPLE_VI_CONFIG_S g_vi_config{};
SIZE_S g_sensor_size{};
chnInputCfg g_venc_input{};

std::atomic_bool g_running{true};
CVI_RTSP_CTX *g_rtsp_context = nullptr;
CVI_RTSP_SESSION *g_rtsp_session = nullptr;
bool g_rtsp_started = false;

void printUsage(const char *program)
{
    std::printf(
        "Usage: %s [options]\n"
        "\n"
        "Camera -> VI/ISP -> VPSS -> VENC -> RTSP for LicheeRV Nano.\n"
        "\n"
        "Options:\n"
        "  --sensor-config PATH  sensor_cfg.ini path (recommended)\n"
        "  --codec h264|h265     encoder codec (default: h264)\n"
        "  --width PIXELS        output width (default: 1920)\n"
        "  --height PIXELS       output height (default: 1080)\n"
        "  --fps FPS             output frame rate (default: 30)\n"
        "  --gop FRAMES          GOP length (default: 30)\n"
        "  --bitrate KBPS        CBR target bitrate (default: 4000)\n"
        "  --port PORT           RTSP port (default: 8554)\n"
        "  --stream-name NAME    RTSP URL path (default: live)\n"
        "  --max-clients COUNT   session/client limit (default: 4)\n"
        "  --no-wrap             disable VPSS low-latency wrap buffer\n"
        "  -h, --help            show this help\n",
        program);
}

bool parseInteger(const char *value, int minimum, int maximum, int *result)
{
    if (value == nullptr || *value == '\0') {
        return false;
    }

    errno = 0;
    char *end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < minimum || parsed > maximum) {
        return false;
    }

    *result = static_cast<int>(parsed);
    return true;
}

bool parseArguments(int argc, char **argv)
{
    enum LongOption {
        kSensorConfig = 1000,
        kCodec,
        kWidth,
        kHeight,
        kFps,
        kGop,
        kBitrate,
        kPort,
        kStreamName,
        kMaxClients,
        kNoWrap,
    };

    static const option long_options[] = {
        {"sensor-config", required_argument, nullptr, kSensorConfig},
        {"codec", required_argument, nullptr, kCodec},
        {"width", required_argument, nullptr, kWidth},
        {"height", required_argument, nullptr, kHeight},
        {"fps", required_argument, nullptr, kFps},
        {"gop", required_argument, nullptr, kGop},
        {"bitrate", required_argument, nullptr, kBitrate},
        {"port", required_argument, nullptr, kPort},
        {"stream-name", required_argument, nullptr, kStreamName},
        {"max-clients", required_argument, nullptr, kMaxClients},
        {"no-wrap", no_argument, nullptr, kNoWrap},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    for (;;) {
        const int value = getopt_long(argc, argv, "h", long_options, nullptr);
        if (value == -1) {
            break;
        }

        int parsed = 0;
        switch (value) {
        case kSensorConfig:
            g_options.sensor_config = optarg;
            break;
        case kCodec:
            g_options.codec = optarg;
            if (g_options.codec != "h264" && g_options.codec != "h265") {
                std::fprintf(stderr, "Invalid codec: %s\n", optarg);
                return false;
            }
            break;
        case kWidth:
            if (!parseInteger(optarg, 64, 4096, &parsed)) {
                std::fprintf(stderr, "Invalid width: %s\n", optarg);
                return false;
            }
            g_options.width = static_cast<unsigned>(parsed);
            break;
        case kHeight:
            if (!parseInteger(optarg, 64, 4096, &parsed)) {
                std::fprintf(stderr, "Invalid height: %s\n", optarg);
                return false;
            }
            g_options.height = static_cast<unsigned>(parsed);
            break;
        case kFps:
            if (!parseInteger(optarg, 1, 120, &g_options.fps)) {
                std::fprintf(stderr, "Invalid fps: %s\n", optarg);
                return false;
            }
            break;
        case kGop:
            if (!parseInteger(optarg, 1, 1000, &g_options.gop)) {
                std::fprintf(stderr, "Invalid GOP: %s\n", optarg);
                return false;
            }
            break;
        case kBitrate:
            if (!parseInteger(optarg, 64, 100000, &g_options.bitrate_kbps)) {
                std::fprintf(stderr, "Invalid bitrate: %s\n", optarg);
                return false;
            }
            break;
        case kPort:
            if (!parseInteger(optarg, 1, 65535, &g_options.port)) {
                std::fprintf(stderr, "Invalid port: %s\n", optarg);
                return false;
            }
            break;
        case kStreamName:
            g_options.stream_name = optarg;
            if (g_options.stream_name.empty() || g_options.stream_name.front() == '/' ||
                g_options.stream_name.size() >= sizeof(CVI_RTSP_SESSION_ATTR::name)) {
                std::fprintf(stderr, "Invalid stream name: %s\n", optarg);
                return false;
            }
            break;
        case kMaxClients:
            if (!parseInteger(optarg, 1, 32, &g_options.max_clients)) {
                std::fprintf(stderr, "Invalid max-clients: %s\n", optarg);
                return false;
            }
            break;
        case kNoWrap:
            g_options.low_latency_wrap = false;
            break;
        case 'h':
            printUsage(argv[0]);
            std::exit(EXIT_SUCCESS);
        default:
            return false;
        }
    }

    if (optind != argc) {
        std::fprintf(stderr, "Unexpected positional argument: %s\n", argv[optind]);
        return false;
    }
    if ((g_options.width & 1U) != 0U || (g_options.height & 1U) != 0U) {
        std::fprintf(stderr, "Width and height must be even for YUV420.\n");
        return false;
    }
    return true;
}

void printMmfError(const char *operation, CVI_S32 result)
{
    std::fprintf(stderr, "%s failed: %#x\n", operation, static_cast<unsigned>(result));
}

void handleSignal(int)
{
    g_running.store(false, std::memory_order_relaxed);
}

void installSignalHandlers()
{
    // SAMPLE_PLAT_SYS_INIT installs SDK handlers, so install ours after pipeline setup.
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::signal(SIGPIPE, SIG_IGN);
}

CVI_S32 initializeVideoInput()
{
    SAMPLE_INI_CFG_S ini_config{};

    if (!g_options.sensor_config.empty()) {
        CVI_S32 result = SAMPLE_COMM_VI_SetIniPath(g_options.sensor_config.c_str());
        if (result != CVI_SUCCESS) {
            printMmfError("SAMPLE_COMM_VI_SetIniPath", result);
            return result;
        }
    }

    CVI_S32 result = SAMPLE_COMM_VI_ParseIni(&ini_config);
    if (result != CVI_SUCCESS) {
        std::fprintf(
            stderr,
            "Unable to parse sensor configuration. Pass --sensor-config or install "
            "/mnt/data/sensor_cfg.ini.\n");
        return result;
    }

    result = CVI_VI_SetDevNum(ini_config.devNum);
    if (result != CVI_SUCCESS) {
        printMmfError("CVI_VI_SetDevNum", result);
        return result;
    }

    result = SAMPLE_COMM_VI_IniToViCfg(&ini_config, &g_vi_config);
    if (result != CVI_SUCCESS) {
        printMmfError("SAMPLE_COMM_VI_IniToViCfg", result);
        return result;
    }

    PIC_SIZE_E sensor_picture_size = PIC_BUTT;
    result = SAMPLE_COMM_VI_GetSizeBySensor(
        g_vi_config.astViInfo[0].stSnsInfo.enSnsType, &sensor_picture_size);
    if (result != CVI_SUCCESS) {
        printMmfError("SAMPLE_COMM_VI_GetSizeBySensor", result);
        return result;
    }

    result = SAMPLE_COMM_SYS_GetPicSize(sensor_picture_size, &g_sensor_size);
    if (result != CVI_SUCCESS) {
        printMmfError("SAMPLE_COMM_SYS_GetPicSize", result);
        return result;
    }

    if (g_options.width > g_sensor_size.u32Width || g_options.height > g_sensor_size.u32Height) {
        std::fprintf(
            stderr,
            "Requested output %ux%u exceeds sensor mode %ux%u.\n",
            g_options.width,
            g_options.height,
            g_sensor_size.u32Width,
            g_sensor_size.u32Height);
        return CVI_FAILURE;
    }

    result = SAMPLE_PLAT_SYS_INIT(g_sensor_size);
    if (result != CVI_SUCCESS) {
        printMmfError("SAMPLE_PLAT_SYS_INIT", result);
        return result;
    }
    g_pipeline.system_started = true;

    VI_VPSS_MODE_S vi_vpss_mode{};
    vi_vpss_mode.aenMode[0] = VI_OFFLINE_VPSS_ONLINE;
    result = CVI_SYS_SetVIVPSSMode(&vi_vpss_mode);
    if (result != CVI_SUCCESS) {
        printMmfError("CVI_SYS_SetVIVPSSMode(VI_OFFLINE_VPSS_ONLINE)", result);
        return result;
    }

    VPSS_MODE_S vpss_mode{};
    vpss_mode.enMode = VPSS_MODE_DUAL;
    vpss_mode.aenInput[0] = VPSS_INPUT_MEM;
    vpss_mode.ViPipe[0] = 0;
    vpss_mode.aenInput[1] = VPSS_INPUT_ISP;
    vpss_mode.ViPipe[1] = 0;
    result = CVI_SYS_SetVPSSModeEx(&vpss_mode);
    if (result != CVI_SUCCESS) {
        printMmfError("CVI_SYS_SetVPSSModeEx(VPSS_INPUT_ISP)", result);
        return result;
    }

    result = SAMPLE_PLAT_VI_INIT(&g_vi_config);
    if (result != CVI_SUCCESS) {
        // SAMPLE_PLAT_VI_INIT performs its own rollback on failure.
        g_pipeline.system_started = false;
        printMmfError("SAMPLE_PLAT_VI_INIT", result);
        return result;
    }
    g_pipeline.vi_started = true;

    ISP_PUB_ATTR_S isp_attributes{};
    result = CVI_ISP_GetPubAttr(0, &isp_attributes);
    if (result == CVI_SUCCESS) {
        const int sensor_fps = static_cast<int>(isp_attributes.f32FrameRate + 0.5F);
        std::printf("ISP output: %ux%u @ %.2f FPS\n",
            isp_attributes.stWndRect.u32Width,
            isp_attributes.stWndRect.u32Height,
            isp_attributes.f32FrameRate);
        if (sensor_fps > 0 && sensor_fps != g_options.fps) {
            std::fprintf(
                stderr,
                "Warning: requested %d FPS but ISP runs at %.2f FPS; use --fps %d --gop %d for minimum latency.\n",
                g_options.fps,
                isp_attributes.f32FrameRate,
                sensor_fps,
                sensor_fps);
        }
    } else {
        printMmfError("CVI_ISP_GetPubAttr", result);
    }
    return CVI_SUCCESS;
}

CVI_S32 initializeVpss()
{
    CVI_BOOL channel_enabled[VPSS_MAX_PHY_CHN_NUM] = {};
    VPSS_CHN_ATTR_S channel_attributes[VPSS_MAX_PHY_CHN_NUM] = {};
    VPSS_GRP_ATTR_S group_attributes{};

    channel_enabled[kVpssChannel] = CVI_TRUE;
    VPSS_CHN_ATTR_S &channel = channel_attributes[kVpssChannel];
    channel.u32Width = g_options.width;
    channel.u32Height = g_options.height;
    channel.enVideoFormat = VIDEO_FORMAT_LINEAR;
    channel.enPixelFormat = kPipelinePixelFormat;
    channel.stFrameRate.s32SrcFrameRate = g_options.fps;
    channel.stFrameRate.s32DstFrameRate = g_options.fps;
    channel.u32Depth = 0;
    channel.bMirror = CVI_FALSE;
    channel.bFlip = CVI_FALSE;
    channel.stAspectRatio.enMode = ASPECT_RATIO_NONE;
    channel.stAspectRatio.bEnableBgColor = CVI_FALSE;
    channel.stNormalize.bEnable = CVI_FALSE;

    group_attributes.stFrameRate.s32SrcFrameRate = -1;
    group_attributes.stFrameRate.s32DstFrameRate = -1;
    group_attributes.enPixelFormat = kPipelinePixelFormat;
    group_attributes.u32MaxW = std::max(g_sensor_size.u32Width, g_options.width);
    group_attributes.u32MaxH = std::max(g_sensor_size.u32Height, g_options.height);
    group_attributes.u8VpssDev = kOnlineVpssDevice;

    CVI_S32 result = CVI_SUCCESS;
    if (g_options.low_latency_wrap) {
        VPSS_CHN_BUF_WRAP_S wrap_attributes{};
        wrap_attributes.bEnable = CVI_TRUE;
        wrap_attributes.u32BufLine = 64;
        wrap_attributes.u32WrapBufferSize = 3;
        result = SAMPLE_COMM_VPSS_WRAP_Start(
            kVpssGroup,
            channel_enabled,
            &group_attributes,
            channel_attributes,
            &wrap_attributes);
        if (result != CVI_SUCCESS) {
            printMmfError("SAMPLE_COMM_VPSS_WRAP_Start", result);
            return result;
        }
    } else {
        result = SAMPLE_COMM_VPSS_Init(
            kVpssGroup, channel_enabled, &group_attributes, channel_attributes);
        if (result != CVI_SUCCESS) {
            printMmfError("SAMPLE_COMM_VPSS_Init", result);
            return result;
        }
        g_pipeline.vpss_created = true;

        result = SAMPLE_COMM_VPSS_Start(
            kVpssGroup, channel_enabled, &group_attributes, channel_attributes);
        if (result != CVI_SUCCESS) {
            printMmfError("SAMPLE_COMM_VPSS_Start", result);
            return result;
        }
    }
    g_pipeline.vpss_created = true;
    g_pipeline.vpss_started = true;

    // VPSS device 1 receives frames directly from ISP in online mode.
    return CVI_SUCCESS;
}

CVI_S32 bindVpssToVenc()
{
    MMF_CHN_S source{};
    source.enModId = CVI_ID_VPSS;
    source.s32DevId = kVpssGroup;
    source.s32ChnId = kVpssChannel;

    MMF_CHN_S destination{};
    destination.enModId = CVI_ID_VENC;
    destination.s32DevId = 0;
    destination.s32ChnId = kVencChannel;

    const CVI_S32 result = CVI_SYS_Bind(&source, &destination);
    if (result != CVI_SUCCESS) {
        printMmfError("CVI_SYS_Bind(VPSS -> VENC)", result);
    }
    return result;
}

CVI_S32 verifyVpssToVencBinding()
{
    MMF_CHN_S destination{};
    destination.enModId = CVI_ID_VENC;
    destination.s32DevId = 0;
    destination.s32ChnId = kVencChannel;

    MMF_CHN_S source{};
    const CVI_S32 result = CVI_SYS_GetBindbyDest(&destination, &source);
    if (result != CVI_SUCCESS) {
        printMmfError("CVI_SYS_GetBindbyDest(VENC)", result);
        return result;
    }

    if (source.enModId != CVI_ID_VPSS || source.s32DevId != kVpssGroup ||
        source.s32ChnId != kVpssChannel) {
        std::fprintf(
            stderr,
            "Unexpected VENC source: module=%d device=%d channel=%d\n",
            source.enModId,
            source.s32DevId,
            source.s32ChnId);
        return CVI_FAILURE;
    }
    return CVI_SUCCESS;
}

void unbindVpssFromVenc()
{
    MMF_CHN_S source{};
    source.enModId = CVI_ID_VPSS;
    source.s32DevId = kVpssGroup;
    source.s32ChnId = kVpssChannel;

    MMF_CHN_S destination{};
    destination.enModId = CVI_ID_VENC;
    destination.s32DevId = 0;
    destination.s32ChnId = kVencChannel;

    const CVI_S32 result = CVI_SYS_UnBind(&source, &destination);
    if (result != CVI_SUCCESS) {
        printMmfError("CVI_SYS_UnBind(VPSS -> VENC)", result);
    }
}

PIC_SIZE_E pictureSizeFor(unsigned width, unsigned height)
{
    if (width == 1280 && height == 720) {
        return PIC_720P;
    }
    if (width == 1920 && height == 1080) {
        return PIC_1080P;
    }
    if (width == 2560 && height == 1440) {
        return PIC_1440P;
    }
    return PIC_CUSTOMIZE;
}

CVI_S32 initializeEncoder()
{
    SAMPLE_COMM_VENC_InitChnInputCfg(&g_venc_input);
    std::snprintf(
        g_venc_input.codec,
        sizeof(g_venc_input.codec),
        "%s",
        g_options.codec == "h264" ? "264" : "265");
    g_venc_input.width = g_options.width;
    g_venc_input.height = g_options.height;
    g_venc_input.rcMode = SAMPLE_RC_CBR;
    g_venc_input.gop = g_options.gop;
    g_venc_input.bitrate = g_options.bitrate_kbps;
    g_venc_input.framerate = g_options.fps;
    g_venc_input.srcFramerate = g_options.fps;
    g_venc_input.num_frames = -1;
    // Wrap/SBM requires StartRecvFrame before the VPSS -> VENC binding.
    g_venc_input.bind_mode =
        g_options.low_latency_wrap ? VENC_BIND_DISABLE : VENC_BIND_VPSS;
    g_venc_input.vpssGrp = kVpssGroup;
    g_venc_input.vpssChn = kVpssChannel;
    g_venc_input.pixel_format = 3; // SDK sample mapping: 3 = NV21.
    g_venc_input.initialDelay = CVI_INITIAL_DELAY_MIN;
    g_venc_input.firstFrmstartQp = 34;
    g_venc_input.bEsBufQueueEn = CVI_TRUE;
    g_venc_input.bIsoSendFrmEn = CVI_TRUE;
    g_venc_input.maxQp = 42;
    g_venc_input.minQp = 24;
    g_venc_input.maxIqp = 42;
    g_venc_input.minIqp = 24;
    g_venc_input.u32Profile =
        g_options.codec == "h264" ? H264E_PROFILE_BASELINE : 0;
    if (g_options.codec == "h264") {
        g_venc_input.h264EntropyMode = H264E_ENTROPY_CAVLC;
    }

    VENC_GOP_ATTR_S gop_attributes{};
    CVI_S32 result = SAMPLE_COMM_VENC_GetGopAttr(VENC_GOPMODE_NORMALP, &gop_attributes);
    if (result != CVI_SUCCESS) {
        printMmfError("SAMPLE_COMM_VENC_GetGopAttr", result);
        return result;
    }

    const PAYLOAD_TYPE_E payload = g_options.codec == "h264" ? PT_H264 : PT_H265;
    result = SAMPLE_COMM_VENC_Start(
        &g_venc_input,
        kVencChannel,
        payload,
        pictureSizeFor(g_options.width, g_options.height),
        SAMPLE_RC_CBR,
        g_venc_input.u32Profile,
        CVI_FALSE,
        &gop_attributes);
    if (result != CVI_SUCCESS) {
        printMmfError("SAMPLE_COMM_VENC_Start", result);
        return result;
    }
    g_pipeline.venc_started = true;

    if (g_options.low_latency_wrap) {
        result = bindVpssToVenc();
        if (result != CVI_SUCCESS) {
            return result;
        }
    }
    g_pipeline.vpss_venc_bound = true;

    result = verifyVpssToVencBinding();
    if (result != CVI_SUCCESS) {
        return result;
    }
    return CVI_SUCCESS;
}

CVI_S32 initializePipeline()
{
    CVI_S32 result = initializeVideoInput();
    if (result == CVI_SUCCESS) {
        result = initializeVpss();
    }
    if (result == CVI_SUCCESS) {
        result = initializeEncoder();
    }
    return result;
}

void stopPipeline()
{
    if (g_pipeline.vpss_venc_bound) {
        unbindVpssFromVenc();
        g_pipeline.vpss_venc_bound = false;
    }

    if (g_pipeline.venc_started) {
        const CVI_S32 result = SAMPLE_COMM_VENC_Stop(kVencChannel);
        if (result != CVI_SUCCESS) {
            printMmfError("SAMPLE_COMM_VENC_Stop", result);
        }
        g_pipeline.venc_started = false;
    }

    if (g_pipeline.vpss_created) {
        CVI_BOOL channel_enabled[VPSS_MAX_PHY_CHN_NUM] = {};
        channel_enabled[kVpssChannel] = CVI_TRUE;
        const CVI_S32 result = SAMPLE_COMM_VPSS_Stop(kVpssGroup, channel_enabled);
        if (result != CVI_SUCCESS) {
            printMmfError("SAMPLE_COMM_VPSS_Stop", result);
        }
        g_pipeline.vpss_created = false;
        g_pipeline.vpss_started = false;
    }

    if (g_pipeline.vi_started) {
        SAMPLE_COMM_VI_DestroyIsp(&g_vi_config);
        SAMPLE_COMM_VI_DestroyVi(&g_vi_config);
        g_pipeline.vi_started = false;
    }

    if (g_pipeline.system_started) {
        SAMPLE_COMM_SYS_Exit();
        g_pipeline.system_started = false;
    }
}

void onClientConnected(const char *ip, void *)
{
    std::printf("RTSP client connected: %s\n", ip != nullptr ? ip : "unknown");
}

void requestIdrForClient()
{
    const CVI_S32 result = CVI_VENC_RequestIDR(kVencChannel, CVI_TRUE);
    if (result != CVI_SUCCESS) {
        printMmfError("CVI_VENC_RequestIDR", result);
    }
}

void onClientPlay(int references, void *)
{
    std::printf("RTSP PLAY started: %d existing client(s)\n", references);
    // The media source is active here; an IDR requested during TCP accept can be dropped.
    requestIdrForClient();
}

void onClientDisconnected(const char *ip, void *)
{
    std::printf("RTSP client disconnected: %s\n", ip != nullptr ? ip : "unknown");
}

int startRtspServer()
{
    CVI_RTSP_CONFIG configuration{};
    configuration.port = g_options.port;
    configuration.timeout = 60;
    configuration.maxConnNum = g_options.max_clients;
    configuration.tcpBufSize = 64U * 1024U;

    if (CVI_RTSP_Create(&g_rtsp_context, &configuration) != 0) {
        std::fprintf(stderr, "CVI_RTSP_Create failed.\n");
        return -1;
    }

    CVI_RTSP_SESSION_ATTR attributes{};
    std::snprintf(attributes.name, sizeof(attributes.name), "%s", g_options.stream_name.c_str());
    attributes.reuseFirstSource = 1;
    attributes.maxConnNum = g_options.max_clients;
    attributes.video.codec = g_options.codec == "h264" ? RTSP_VIDEO_H264 : RTSP_VIDEO_H265;
    attributes.video.bitrate = static_cast<unsigned>(g_options.bitrate_kbps);
    attributes.video.play = onClientPlay;

    if (CVI_RTSP_CreateSession(g_rtsp_context, &attributes, &g_rtsp_session) != 0) {
        std::fprintf(stderr, "CVI_RTSP_CreateSession failed.\n");
        CVI_RTSP_Destroy(&g_rtsp_context);
        return -1;
    }

    CVI_RTSP_STATE_LISTENER listener{};
    listener.onConnect = onClientConnected;
    listener.onDisconnect = onClientDisconnected;
    if (CVI_RTSP_SetListener(g_rtsp_context, &listener) != 0) {
        std::fprintf(stderr, "CVI_RTSP_SetListener failed.\n");
        CVI_RTSP_DestroySession(g_rtsp_context, g_rtsp_session);
        g_rtsp_session = nullptr;
        CVI_RTSP_Destroy(&g_rtsp_context);
        return -1;
    }

    if (CVI_RTSP_Start(g_rtsp_context) != 0) {
        std::fprintf(stderr, "CVI_RTSP_Start failed.\n");
        CVI_RTSP_DestroySession(g_rtsp_context, g_rtsp_session);
        g_rtsp_session = nullptr;
        CVI_RTSP_Destroy(&g_rtsp_context);
        return -1;
    }
    g_rtsp_started = true;
    return 0;
}

void stopRtspServer()
{
    if (g_rtsp_context == nullptr) {
        return;
    }
    if (g_rtsp_started) {
        CVI_RTSP_Stop(g_rtsp_context);
        g_rtsp_started = false;
    }
    if (g_rtsp_session != nullptr) {
        CVI_RTSP_DestroySession(g_rtsp_context, g_rtsp_session);
        g_rtsp_session = nullptr;
    }
    CVI_RTSP_Destroy(&g_rtsp_context);
}

void streamVencToRtsp()
{
    std::vector<VENC_PACK_S> packs;
    std::vector<std::uint8_t> merged;
    const CVI_S32 venc_fd = CVI_VENC_GetFd(kVencChannel);
    if (venc_fd < 0) {
        printMmfError("CVI_VENC_GetFd", venc_fd);
        g_running.store(false, std::memory_order_relaxed);
        return;
    }

    unsigned consecutive_select_timeouts = 0;
    unsigned consecutive_busy_results = 0;
    CVI_U64 encoded_frame_count = 0;

    while (g_running.load(std::memory_order_relaxed)) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(venc_fd, &read_fds);

        timeval timeout{};
        timeout.tv_sec = 1;
        const int ready = select(venc_fd + 1, &read_fds, nullptr, nullptr, &timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::fprintf(stderr, "select(VENC) failed: %s\n", std::strerror(errno));
            g_running.store(false, std::memory_order_relaxed);
            break;
        }
        if (ready == 0) {
            ++consecutive_select_timeouts;
            if (consecutive_select_timeouts == 5 ||
                (consecutive_select_timeouts > 5 && consecutive_select_timeouts % 30 == 0)) {
                std::fprintf(
                    stderr,
                    "No encoded frame available for %u seconds; check the VI -> VPSS -> VENC pipeline.\n",
                    consecutive_select_timeouts);
            }
            continue;
        }
        consecutive_select_timeouts = 0;

        VENC_CHN_STATUS_S status{};
        CVI_S32 result = CVI_VENC_QueryStatus(kVencChannel, &status);
        if (result != CVI_SUCCESS) {
            if (g_running.load(std::memory_order_relaxed)) {
                printMmfError("CVI_VENC_QueryStatus", result);
            }
            usleep(10U * 1000U);
            continue;
        }
        if (status.u32CurPacks == 0) {
            usleep(1000U);
            continue;
        }

        packs.resize(status.u32CurPacks);
        VENC_STREAM_S stream{};
        stream.pstPack = packs.data();
        result = CVI_VENC_GetStream(kVencChannel, &stream, 1000);
        if (result != CVI_SUCCESS) {
            if (result == CVI_ERR_VENC_BUSY) {
                if (consecutive_busy_results++ == 0) {
                    std::fprintf(
                        stderr,
                        "CVI_VENC_GetStream returned CVI_ERR_VENC_BUSY; retrying as required by the SDK sample.\n");
                }
                usleep(10U * 1000U);
            } else if (g_running.load(std::memory_order_relaxed)) {
                printMmfError("CVI_VENC_GetStream", result);
            }
            continue;
        }
        consecutive_busy_results = 0;
        ++encoded_frame_count;

        const CVI_U64 latency_log_interval =
            static_cast<CVI_U64>(std::max(g_options.fps, 1)) * 5U;
        if (stream.u32PackCount != 0 &&
            (encoded_frame_count == 1 || encoded_frame_count % latency_log_interval == 0)) {
            CVI_U64 current_pts = 0;
            const CVI_U64 stream_pts = stream.pstPack[0].u64PTS;
            if (stream_pts != 0 && CVI_SYS_GetCurPTS(&current_pts) == CVI_SUCCESS &&
                current_pts >= stream_pts && current_pts - stream_pts < 60U * 1000U * 1000U) {
                std::printf(
                    "VENC queue age: %.2f ms (frame %llu)\n",
                    static_cast<double>(current_pts - stream_pts) / 1000.0,
                    static_cast<unsigned long long>(encoded_frame_count));
            }
        }

        CVI_RTSP_DATA data{};
        if (stream.u32PackCount <= CVI_RTSP_DATA_MAX_BLOCK) {
            for (CVI_U32 index = 0; index < stream.u32PackCount; ++index) {
                VENC_PACK_S &pack = stream.pstPack[index];
                if (pack.pu8Addr == nullptr || pack.u32Len <= pack.u32Offset) {
                    continue;
                }
                const CVI_U32 block = data.blockCnt++;
                data.dataPtr[block] = pack.pu8Addr + pack.u32Offset;
                data.dataLen[block] = pack.u32Len - pack.u32Offset;
            }
        } else {
            std::size_t total_size = 0;
            for (CVI_U32 index = 0; index < stream.u32PackCount; ++index) {
                const VENC_PACK_S &pack = stream.pstPack[index];
                if (pack.pu8Addr != nullptr && pack.u32Len > pack.u32Offset) {
                    total_size += pack.u32Len - pack.u32Offset;
                }
            }

            merged.resize(total_size);
            std::size_t write_offset = 0;
            for (CVI_U32 index = 0; index < stream.u32PackCount; ++index) {
                const VENC_PACK_S &pack = stream.pstPack[index];
                if (pack.pu8Addr == nullptr || pack.u32Len <= pack.u32Offset) {
                    continue;
                }
                const std::size_t size = pack.u32Len - pack.u32Offset;
                std::memcpy(merged.data() + write_offset, pack.pu8Addr + pack.u32Offset, size);
                write_offset += size;
            }
            if (!merged.empty()) {
                data.blockCnt = 1;
                data.dataPtr[0] = merged.data();
                data.dataLen[0] = static_cast<std::uint32_t>(merged.size());
            }
        }

        if (data.blockCnt != 0) {
            CVI_U64 write_start_pts = 0;
            CVI_U64 write_end_pts = 0;
            CVI_SYS_GetCurPTS(&write_start_pts);
            const int write_result =
                CVI_RTSP_WriteFrame(g_rtsp_context, g_rtsp_session->video, &data);
            CVI_SYS_GetCurPTS(&write_end_pts);

            if (write_result != 0) {
                std::fprintf(stderr, "CVI_RTSP_WriteFrame failed.\n");
            }
            if (write_end_pts > write_start_pts && write_end_pts - write_start_pts > 50U * 1000U) {
                std::fprintf(
                    stderr,
                    "CVI_RTSP_WriteFrame blocked for %.2f ms; the RTSP client is applying backpressure.\n",
                    static_cast<double>(write_end_pts - write_start_pts) / 1000.0);
            }
        }

        result = CVI_VENC_ReleaseStream(kVencChannel, &stream);
        if (result != CVI_SUCCESS) {
            printMmfError("CVI_VENC_ReleaseStream", result);
        }
    }
}

} // namespace

int main(int argc, char **argv)
{
    if (!parseArguments(argc, argv)) {
        printUsage(argv[0]);
        return EXIT_FAILURE;
    }

    const CVI_S32 pipeline_result = initializePipeline();
    if (pipeline_result != CVI_SUCCESS) {
        stopPipeline();
        return EXIT_FAILURE;
    }

    // The SDK installs its own handlers during system initialization.
    installSignalHandlers();

    if (startRtspServer() != 0) {
        stopPipeline();
        return EXIT_FAILURE;
    }

    std::printf(
        "Streaming %s %ux%u@%d, %d Kbit/s\n"
        "MMF path: VI offline -> ISP/VPSS online (NV21%s) -> VENC bind\n"
        "RTSP URL: rtsp://<licheerv-nano-ip>:%d/%s\n",
        g_options.codec.c_str(),
        g_options.width,
        g_options.height,
        g_options.fps,
        g_options.bitrate_kbps,
        g_options.low_latency_wrap ? ", wrap 64 lines x 3" : "",
        g_options.port,
        g_options.stream_name.c_str());

    std::thread stream_thread(streamVencToRtsp);
    stream_thread.join();

    stopRtspServer();
    stopPipeline();
    return EXIT_SUCCESS;
}
