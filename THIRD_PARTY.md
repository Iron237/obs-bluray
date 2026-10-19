# Third-party notices

- OBS Studio 32.2.2, GPL-2.0-or-later: https://github.com/obsproject/obs-studio .
- Qt 6.8.3 Core/Network/Concurrent/Gui/Widgets/Svg/Test, dynamically linked; notices in `licenses/qt`.
- GStreamer 1.26.10 MSVC x64: https://gstreamer.freedesktop.org/ . Plugins include LGPL/GPL components and their own dependencies; packaging retains the SDK's component license notices.
- libbluray 1.4.0: https://www.videolan.org/developers/libbluray.html . Packaging includes its COPYING file. BD-J, FreeType, libxml2 and fontconfig are disabled in this build.
- libass and codec libraries are supplied by the GStreamer SDK and retain their upstream licenses.
- MediaInfo 26.10 Windows x64 DLL, BSD-2-Clause: https://mediaarea.net/MediaInfo . Loaded only by the isolated helper for bounded audio metadata inspection; never loaded into OBS. Packaging retains its license notice.

bootstrap applies two MSVC compatibility typedefs for ssize_t in bundled libudfread; media-reading logic is unchanged. SDKs/binaries and user media are excluded from Git. FFmpeg/FFprobe are test tools, not the application's decoding engine.
