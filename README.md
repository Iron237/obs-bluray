# OBS Blu-ray

<img src="assets/obs-bluray.svg" width="80" height="80" alt="原创蓝光节目单图标">

Windows x64 原生 OBS 蓝光输入插件，当前版本 **0.2.0**，面向 OBS 32.2.2。支持 BDMV/MPLS 选集队列、时间范围、音轨/章节、CPU/NVIDIA/Intel 解码、蓝光 LPCM、外挂 ASS/SSA/SRT 和盘内 PGS。

配套分发服务器：[StreamDesk Live Server](https://github.com/Iron237/streamdesk-live-server)。

## 架构

原生 libobs 插件通过有界共享内存接收独立解码进程输出的 BGRA 视频与 48kHz 双声道 float PCM。第三方 Qt/GStreamer/libbluray/libass 不载入 OBS 主进程，无外部播放器窗口。OBS 负责混音、场景、编码与推流。GPU 解码存在下载/上传，当前不是 GPU 零拷贝。

解码进程负责按共用 QPC 时钟播放，画面与关联字幕均关闭 OBS 内部异步视频缓存。这样短时没有视频帧、音频仍继续时，恢复后直接呈现最新帧，避免把这次空窗积累成持续的画面延迟。没有固定音频补偿，也没有改变音频时钟策略。

节目单编辑器使用独立 Qt Widgets 进程，只在配置时打开；Qt Widgets 和媒体依赖不载入 OBS 模块。播放本身不需要外部播放器窗口。

窗口内置原创 SVG 图标，EXE 资源内置多尺寸 ICO；图形为光盘、播放按钮与节目列表，不使用 OBS 标志。源文件位于 `assets/obs-bluray.svg`；安装 Pillow 后可用 `python scripts/generate-icon.py` 重生成 ICO 与 PNG，图标无需网络、系统字体或外部文件才能显示。

## 选集、顺播与每 MPLS 字幕

1. 在蓝光源属性选择光盘目录，点击“编辑选集与每 MPLS 字幕…”。
2. 左侧查看扫描到的 MPLS、时长、章节和 PGS 数；可添加整条 MPLS，或按当前 MPLS 的章节批量拆成候选条目。章节只表示跳转点，不自动认定为动漫集或歌曲名。
3. 右侧为真正播放顺序：启用/停用、重命名、复制、上移/下移或删除。双击起止时间编辑，支持 `HH:mm:ss.zzz` 或秒；一条 MPLS 可以编排多个范围，队列也可跨多个 MPLS。
4. 选择“仅播放起播集”或“按节目单顺序播放”，再选择起播集与是否循环。OBS 媒体控制支持上一集/下一集、暂停、继续、重播及本集内定位。进度和时长按当前集显示。
5. 下方按 MPLS 保存一套字幕设置：默认选择首条盘内 PGS（无轨时关闭），也可指定 PGS 轨、外挂 ASS/SSA/SRT、字体目录和偏移。同一 MPLS 的所有分段共用该设置，外挂字幕始终按完整 MPLS 时间轴对齐，不会从分段起点重新计时。
6. 保存并关闭编辑器，回到 OBS 点击“读取已保存节目单”，最后点击“应用/确定”。编辑与导入不会自动重启正在播放的源；取消 OBS 属性不会应用这次编辑。节目单嵌入 OBS 源设置，不依赖临时文件。

启用节目单后，属性中的起播集、播放方式和循环可以直接调整；停用节目单可恢复原有单 MPLS 参数。切集会重新准备解码器，不承诺无缝切换。终点为排除边界，精度受视频帧及音频采样边界限制。

可选盘内标题名和导航类型与 MPLS 分开显示：菜单标题编号不等于 MPLS 编号。可读取 `META/DL` 的导航标题名和 `META/TN/tnmt_<语言>_<MPLS>.xml` 的章节名称；有名称时导入，无名称时保留章节编号。名称合并不创建时间点，数量不一致时保留来源和匹配数。当前不执行 HDMV/BD-J 菜单程序，也不自动猜测“本体/幕后”分类。

音轨列表及完整说明显示原盘语言、编码、采样率、声道、位深、码率和 PID/PES。LPCM 参数来自有界 PES 头解析；压缩音频由独立 helper 中的 MediaInfo 读取最多 4 MiB，不持续解码、不扫描全片。Atmos 只在有明确扩展标记时显示；TrueHD 本身不等于 Atmos。CBR 使用头部码率，VBR 片段估算会明确标注，未知字段不按经验补值。这里只显示源参数，送入 OBS 的音频仍为 48 kHz 双声道。

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
./build/bin/bluray-program-tests.exe
./build/bin/bluray-program-editor-tests.exe -platform offscreen
./build/bin/bluray-audio-info-tests.exe
./build/bin/bluray-audio-probe-tests.exe
./build/bin/bluray-disc-metadata-tests.exe
python tests/program_integration.py
python scripts/privacy_check.py
```

原生 libobs 集成需 `OBS_ROOT` 和 `STREAMDESK_FFMPEG`。`RUN_HARDWARE_TESTS=1` 启用 Intel/NVIDIA 与 HEVC 测试；默认不把缺少 GPU 当作硬件验证通过。测试自动生成色块、音频和 ASS，不包含用户媒体。退出后检查解码进程回收，验证范围见 [VALIDATION.md](VALIDATION.md)。

`python tests/audio_probe_fixtures.py` 可生成合成 AC-3/E-AC-3 头部探测素材。将 `OBS_BLURAY_AUDIO_PROBE_FIXTURES` 指向生成的 `test-artifacts/audio-probe-fixtures`，再运行 audio-probe 测试，可验证真实 MediaInfo DLL；未提供时只跳过该可选素材用例，规则用例仍运行。GUI 测试可指定 `PROGRAM_EDITOR_SCREENSHOT` 输出仅含合成数据的界面预览。

同步专项通过不等间隔白闪/短音，对比 OBS 实际合成视频与最终混音的时钟，另记录解码源时钟，区分源时间戳与呈现缓冲。它启动独立 libobs 测试进程，不操作正在运行的 OBS：

```powershell
python tests/synchronization.py --seconds 30 --second-source
python tests/synchronization.py --seconds 30 --decoder nvidia --fixture-format m2ts --second-source
python tests/synchronization.py --seconds 30 --decoder nvidia --fixture-format m2ts --second-source --encode-srt
```

第二条生成 96 kHz/24-bit Blu-ray LPCM 的 M2TS，需支持 NVIDIA 解码的硬件。默认检查合成输出偏移不超过 160 ms、首尾偏移变化不超过 80 ms；这些回归上限为不同解码器的调度及 23.976 fps 帧量化留出余量，实测范围见 VALIDATION.md。0.1.1 的主画面和字幕源已关闭 OBS 异步视频缓存。可用 `--fixture-audio-delay-ms 400` 故意制造错位，确认测量会拒绝错误结果。

`--encode-srt` 进一步使用真实 OBS NVENC HEVC（VBR 6000/7000 kbps、其余为 OBS 默认值）、AAC 320 kbps 和 MPEG-TS 推流模块，强制 1920×1080 / 60 fps，记录复用前编码包的 PTS/DTS/CTS，并把随机本机端口收到的 SRT 负载原样保存，再按同一 MPEG-TS 时钟测量音画内容偏移。测试会将 OBS 自带的 NVENC 探测助手复制到忽略的构建目录，不改动 OBS 安装。此模式需要已安装的 OBS NVENC、FFmpeg、rtmp-services 模块及 `srt.dll`。专项仍不包含 MediaMTX、远端播放器和小时级运行验证。

插件按 GPL-2.0-or-later 提供，见 [LICENSE](LICENSE)。第三方依赖见 [THIRD_PARTY.md](THIRD_PARTY.md)。
