# OBS Blu-ray

Windows x64 原生 OBS 蓝光输入插件，当前版本 **0.1.0**，已在 OBS 32.2.2 验证。支持 BDMV/MPLS、音轨/章节、CPU/NVIDIA/Intel 解码、蓝光 LPCM、外挂 ASS/SSA/SRT 和盘内 PGS。

配套分发服务器：[StreamDesk Live Server](https://github.com/Iron237/streamdesk-live-server)。

## 架构

原生 libobs 插件通过有界共享内存接收独立解码进程输出的 BGRA 视频与 48kHz 双声道 float PCM。第三方 Qt/GStreamer/libbluray/libass 不载入 OBS 主进程，无外部播放器窗口。OBS 负责混音、场景、编码与推流。GPU 解码存在下载/上传，当前不是 GPU 零拷贝。

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

SDK 默认 `$env:LOCALAPPDATA/StreamDeskSDK`，可传 `--sdk` 或 `-SdkRoot`。构建脚本可传 `-ObsRoot`；运行测试前仍需设置 `OBS_ROOT`。匹配安装版本的 OBS 头文件与导入库由 setup 脚本生成。

## 安装与字幕

1. 关闭 OBS，在完整发布包内运行 `Install.cmd`；可使用 `Install.ps1 -ObsRoot` 指定安装目录。
2. 添加“蓝光原盘 / BDMV (StreamDesk)”源，选择自己的蓝光目录。选择节目、音轨和起始章节；默认最长节目、CPU 解码。
3. 在该画面源的“外挂字幕”导入 ASS/SSA/SRT，并按需设置字体目录、字幕延迟。
4. 默认启用独立透明字幕：再添加“蓝光关联字幕 / ASS (StreamDesk)”，关联画面源并放在其上方。libass 渲染字幕，OBS 可独立显隐、变换与加滤镜。整体缩放时将二者分组。
5. 正偏移表示字幕延后。取消独立字幕可合成到画面；PGS 当前只支持合成到画面。最终直播字幕会进入合成画面，观看端不能单独切换。
6. 使用 OBS 媒体控件暂停、继续、停止、重播、定位。音量由 OBS 混音器控制；编码与推流使用 OBS 自己的设置。

支持不超过 4096×2160 的 SDR 源。ISO 需先挂载；不含解密、菜单、BD-J 或 HDR 色调映射，音频暂下混双声道。卸载时关闭 OBS，仅移除本插件 DLL 和对应数据目录。

## 验证

```powershell
./build/bin/bluray-core-tests.exe
python -m pip install -r requirements-test.txt
python tests/integration.py
python scripts/privacy_check.py
```

原生 libobs 集成需 `OBS_ROOT` 和 `STREAMDESK_FFMPEG`。`RUN_HARDWARE_TESTS=1` 启用 Intel/NVIDIA 与 HEVC 测试；默认不把缺少 GPU 当作硬件验证通过。测试自动生成色块、音频和 ASS，不包含用户媒体。退出后检查解码进程回收，验证范围见 [VALIDATION.md](VALIDATION.md)。

插件按 GPL-2.0-or-later 提供，见 [LICENSE](LICENSE)。第三方依赖见 [THIRD_PARTY.md](THIRD_PARTY.md)。
