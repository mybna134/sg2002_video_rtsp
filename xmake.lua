set_project("licheerv-nano-mmfrtsp")
set_version("0.1.0")

add_rules("mode.debug", "mode.release")
set_defaultmode("release")
set_plat("cross")
set_arch("riscv64")
set_warnings("all")

option("nano_sdk")
    set_default(path.join(os.projectdir(), ".sdk", "LicheeRV-Nano-Build"))
    set_showmenu(true)
    set_description("LicheeRV-Nano-Build root")
option_end()

option("toolchain_root")
    set_default(path.join(os.projectdir(), ".tools", "host-tools", "gcc", "riscv64-linux-musl-x86_64"))
    set_showmenu(true)
    set_description("SG2002 riscv64 musl toolchain root")
option_end()

local cpu_flags = {
    "-mcpu=c906fdv",
    "-march=rv64imafdcv0p7xthead",
    "-mcmodel=medany",
    "-mabi=lp64d",
    "-mno-ldd"
}

target("cvi_rtsp")
    set_kind("static")
    set_languages("cxx11")
    set_targetdir(path.join("build", "$(plat)", "$(arch)", "$(mode)"))
    add_files("third_party/cvi_rtsp/src/api.cpp")
    add_includedirs(
        "third_party/cvi_rtsp/include",
        "third_party/cvi_rtsp/src",
        "third_party/live555/include/BasicUsageEnvironment",
        "third_party/live555/include/groupsock",
        "third_party/live555/include/liveMedia",
        "third_party/live555/include/UsageEnvironment",
        {public = true})
    add_linkdirs("third_party/live555/lib", {public = true})
    add_links(
        "liveMedia", "BasicUsageEnvironment", "UsageEnvironment", "groupsock",
        {public = true})
    add_defines("NO_OPENSSL=1")
    add_cxflags(cpu_flags, {force = true})
    add_cxflags("-fPIC")

target("licheerv-nano-rtsp")
    set_kind("binary")
    set_languages("cxx17")
    set_targetdir(path.join("build", "$(plat)", "$(arch)", "$(mode)"))
    add_files("src/*.cpp")
    add_deps("cvi_rtsp", {inherit = true})
    add_defines("ARCH_SG200X", "__SOC_MARS__")
    add_cxflags(cpu_flags, {force = true})
    add_ldflags(cpu_flags, {force = true})

    on_load(function (target)
        local sdk = get_config("nano_sdk")
        local middleware = path.join(sdk, "middleware", "v2")
        local kernel_uapi = path.join(
            sdk, "linux_5.10", "build", "sg2002_licheervnano_sd", "riscv", "usr", "include")

        target:add("includedirs",
            path.join(middleware, "include"),
            path.join(middleware, "include", "isp", "sg200x"),
            path.join(middleware, "sample", "common"),
            path.join(middleware, "3rdparty", "inih"),
            kernel_uapi,
            path.join(sdk, "osdrv", "interdrv", "v2", "include", "common", "uapi"),
            path.join(sdk, "osdrv", "interdrv", "v2", "include", "chip", "mars", "uapi"))
        target:add("linkdirs", path.join(middleware, "lib"), path.join(middleware, "lib", "3rd"))
    end)

    before_build(function ()
        local sdk = get_config("nano_sdk")
        local middleware = path.join(sdk, "middleware", "v2")
        local required = {
            path.join(middleware, "lib", "libsample.so"),
            path.join(middleware, "lib", "libsys.so"),
            path.join(middleware, "lib", "libvenc.so"),
            path.join(middleware, "lib", "libisp.so")
        }
        for _, filename in ipairs(required) do
            if not os.isfile(filename) then
                raise("missing MMF library %s; run scripts/build-middleware.sh", filename)
            end
        end
    end)

    -- This is the same library set exposed by cvi_common/cvi_sample pkg-config.
    add_links(
        "sample",
        "sys", "vpu", "venc", "cvi_bin", "cvi_bin_isp",
        "isp", "isp_algo", "ae", "af", "awb", "sns_full",
        "cvi_audio", "cvi_vqe", "cvi_VoiceEngine", "cvi_RES1", "cvi_ssp",
        "vdec", "misc", "ini", "tinyalsa", "cli",
        "atomic", "dl", "rt", "pthread", "m")
    add_rpathdirs("/mnt/system/lib", "/mnt/system/lib/3rd", "/usr/lib")

    after_build(function (target)
        os.execv(path.join(get_config("toolchain_root"), "bin", "riscv64-unknown-linux-musl-readelf"),
            {"-h", target:targetfile()})
    end)
