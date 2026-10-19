# OBS Blu-ray

Windows x64 原生 OBS 蓝光输入插件，当前版本 **0.1.1**，已在 OBS 32.2.2 验证。支持 BDMV/MPLS、音轨/章节、CPU/NVIDIA/Intel 解码、蓝光 LPCM、外挂 ASS/SSA/SRT 和盘内 PGS。

配套分发服务器：[StreamDesk Live Server](https://github.com/Iron237/streamdesk-live-server)。

## 架构

原生 libobs 插件通过有界共享内存，接收独立解码进程输出的 BGRA 视频和 48 kHz 双声道 float PCM。第三方 Qt/GStreamer/libbluray/libass 不会载入 OBS 主进程，也没有外部播放器窗口。OBS 负责混音、场景、编码和推流。GPU 解码涉及下载/上传，目前并非 GPU 零拷贝。

解码进程按共用 QPC 时钟播放；画面和关联字幕均关闭 OBS 内部异步视频缓存。这样，在短时无视频帧而音频仍继续的情况下，恢复后会直接呈现最新帧，避免将这段空窗积累成持续的画面延迟。没有固定音频补偿，也未改变音频时钟策略。

## 构建与打包

需要 Windows x64、Visual Studio 2022 C++、CMake 3.25+、Python 3.10+、已安装的 OBS。Qt 6.8.3、GStreamer 1.26.10、libbluray 1.4.0 均放入独立 SDK 目录。

```powershell
python -m pip install requests
python scripts/bootstrap.py
# 在当前终端设置 OBS_ROOT 为自己的 OBS 安装目录；不要将本地路径写入仓库。
./scripts/setup-obs-sdk.ps1
./scripts/build.ps1 -Deploy
./scripts/package.ps1
```

SDK 默认位于 `$env:LOCALAPPDATA/StreamDeskSDK`，可通过 `--sdk` 或 `-SdkRoot` 指定。构建脚本可传入 `-ObsRoot`；运行测试前仍需设置 `OBS_ROOT`。与所安装 OBS 版本匹配的头文件和导入库由 setup 脚本生成。

## 安装与字幕

1. 关闭 OBS 后，在完整发布包中运行 `Install.cmd`；也可使用 `Install.ps1 -ObsRoot` 指定安装目录。
2. 添加“蓝光原盘 / BDMV (StreamDesk)”源，并选择自己的蓝光目录。选择节目、音轨和起始章节；默认使用最长节目、CPU 解码。
3. 在该画面源的“外挂字幕”中导入 ASS/SSA/SRT，并按需设置字体目录和字幕延迟。
4. 默认启用独立透明字幕：再添加“蓝光关联字幕 / ASS (StreamDesk)”，将其关联到画面源并置于其上方。字幕由 libass 渲染，OBS 可独立控制显隐、变换和添加滤镜。整体缩放时请将二者分组。
5. 正偏移表示字幕延后。取消独立字幕后，字幕会合成到画面；PGS 目前仅支持合成到画面。最终直播字幕会进入合成画面，观看端无法单独切换。
6. 使用 OBS 媒体控件进行暂停、继续、停止、重播和定位。音量由 OBS 混音器控制；编码与推流使用 OBS 自身的设置。

支持不超过 4096×2160 的 SDR 源。ISO 需先挂载；不包含解密、菜单、BD-J 或 HDR 色调映射，音频暂下混为双声道。卸载时请关闭 OBS，仅移除本插件 DLL 及对应数据目录。

## 验证

```powershell
./build/bin/bluray-core-tests.exe
python -m pip install -r requirements-test.txt
python tests/integration.py
python scripts/privacy_check.py
```

原生 libobs 集成测试需要 `OBS_ROOT` 和 `STREAMDESK_FFMPEG`。设置 `RUN_HARDWARE_TESTS=1` 可启用 Intel/NVIDIA 与 HEVC 测试；默认不会将缺少 GPU 视为硬件验证通过。测试会自动生成色块、音频和 ASS，不包含用户媒体。退出后检查解码进程是否回收，验证范围见 [VALIDATION.md](VALIDATION.md)。

同步专项测试通过不等间隔白闪/短音，对比 OBS 实际合成视频与最终混音的时钟，并记录解码源时钟，以区分源时间戳与呈现缓冲。该测试会启动独立的 libobs 测试进程，不会操作正在运行的 OBS：

```powershell
python tests/synchronization.py --seconds 30 --second-source
python tests/synchronization.py --seconds 30 --decoder nvidia --fixture-format m2ts --second-source
python tests/synchronization.py --seconds 30 --decoder nvidia --fixture-format m2ts --second-source --encode-srt
```

第二条命令会生成 96 kHz/24-bit Blu-ray LPCM 的 M2TS，需要支持 NVIDIA 解码的硬件。默认检查合成输出偏移不超过 160 ms、首尾偏移变化不超过 80 ms；这些回归上限为不同解码器的调度差异及 23.976 fps 帧量化留出余量，实测范围见 VALIDATION.md。0.1.1 中主画面和字幕源已关闭 OBS 异步视频缓存。可使用 `--fixture-audio-delay-ms 400` 故意制造错位，确认测量会拒绝错误结果。

`--encode-srt` 会进一步使用真实的 OBS NVENC HEVC（VBR 6000/7000 kbps，其余为 OBS 默认值）、AAC 320 kbps 和 MPEG-TS 推流模块，强制 1920×1080 / 60 fps，记录复用前编码包的 PTS/DTS/CTS，并将随机本机端口收到的 SRT 负载原样保存，再按同一 MPEG-TS 时钟测量音画内容偏移。测试会将 OBS 自带的 NVENC 探测助手复制到被忽略的构建目录，不会改动 OBS 安装。此模式需要已安装的 OBS NVENC、FFmpeg、rtmp-services 模块及 `srt.dll`。该专项测试仍不包含 MediaMTX、远端播放器和小时级运行验证。

插件按 GPL-2.0-or-later 提供，详见 [LICENSE](LICENSE)。第三方依赖见 [THIRD_PARTY.md](THIRD_PARTY.md)。
