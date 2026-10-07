# ofxLavVideoPlayer

A DirectShow video player for **openFrameworks 0.11.2 on Windows** that plays videos with [LAV Filters](https://github.com/Nevcairiel/LAVFilters) loaded directly from your app's data folder.

No codec pack, no installer, no `regsvr32`, no admin rights. Copy the LAV Filters files next to your app and it plays MP4, MKV, MOV and more on a clean Windows machine.

## Why

On Windows, openFrameworks 0.11.2 plays video through `ofDirectShowPlayer`, which relies on the codecs registered on the system. The usual advice is to install the K-Lite Codec Pack on every machine that runs your app, which is inconvenient for installations, exhibitions and distributed apps.

ofxLavVideoPlayer creates the LAV Filters directly from their `.ax` files (via `LoadLibrary` + `DllGetClassObject`) and inserts them into the DirectShow graph, so nothing has to be registered on the system.

## Features

- Drop-in replacement for the default player through `ofVideoPlayer::setPlayer()`
- Same API and behaviour as `ofDirectShowPlayer` (it is a rewrite of it)
- LAV Splitter Source, LAV Video Decoder and LAV Audio Decoder loaded without COM registration
- Optional fallback to the filters installed on the system when LAV cannot open a file
- File paths with non-ASCII characters (e.g. Japanese) are supported
- Independent of the core `ofDirectShowPlayer` (everything lives in its own namespace, no symbol conflicts)

## Requirements

- openFrameworks 0.11.2 (Visual Studio 2017 / 2019 / 2022)
- LAV Filters portable release (download separately, see below)

openFrameworks 0.12 and later include a Media Foundation based video player that plays common formats without extra codecs, so this addon targets 0.11.2.

## Installation

1. Clone or copy this repository into `openFrameworks/addons/ofxLavVideoPlayer`.
2. Add `ofxLavVideoPlayer` to your project with the Project Generator.
3. Download the **portable** LAV Filters release (`LAVFilters-x.xx-x64.zip` for x64 apps, `-x86.zip` for Win32 apps) from the [LAV Filters releases page](https://github.com/Nevcairiel/LAVFilters/releases).
4. Extract **all** files of the zip into your app's `bin/data/codecs/` folder.

```
bin/
└── data/
    └── codecs/
        ├── LAVSplitter.ax
        ├── LAVVideo.ax
        ├── LAVAudio.ax
        ├── LAVFilters.Dependencies.manifest
        ├── avcodec-lav-*.dll
        ├── avfilter-lav-*.dll
        ├── avformat-lav-*.dll
        ├── avutil-lav-*.dll
        ├── swresample-lav-*.dll
        ├── swscale-lav-*.dll
        ├── libbluray.dll
        └── IntelQuickSyncDecoder.dll
```

> **Important:** `LAVFilters.Dependencies.manifest` is required. Each `.ax` file depends on it to locate the FFmpeg DLLs, and loading fails with error 14001 without it. Do not mix files from different LAV Filters versions, and do not take files out of a codec pack; use the official portable zip as a whole.

## Usage

```cpp
// ofApp.h
#include "ofMain.h"
#include "ofxLavVideoPlayer.h"

class ofApp : public ofBaseApp {
public:
    void setup();
    void update();
    void draw();

    ofVideoPlayer video;
};

// ofApp.cpp
void ofApp::setup() {
    // Use the LAV-based backend instead of the default DirectShow player
    video.setPlayer(std::make_shared<ofxLavVideoPlayer>());
    video.load("movie.mkv");
    video.play();
}

void ofApp::update() {
    video.update();
}

void ofApp::draw() {
    video.draw(0, 0);
}
```

### Options

```cpp
// Use a different codec folder (absolute, or relative to bin/data/). Call before load().
ofxLavVideoPlayer::setCodecDirectory("C:/myapp/codecs");

// Disable the fallback to system-installed filters (default: enabled)
ofxLavVideoPlayer::setFallbackToSystemFilters(false);

// Check whether the loaded movie is played through LAV Splitter Source
auto player = video.getPlayer<ofxLavVideoPlayer>();
if (player && player->isUsingLav()) {
    ofLogNotice() << "Playing with LAV Filters";
}
```

## Troubleshooting

Errors are logged with the `ofxLavVideoPlayer` module name.

| Error | Cause | Fix |
|---|---|---|
| `error 14001` | `LAVFilters.Dependencies.manifest` is missing, or a DLL it lists is missing or from another version | Copy the whole portable zip into `bin/data/codecs/`. Event Viewer > Windows Logs > Application (source `SideBySide`) shows the exact file. |
| `error 193` | Architecture mismatch | Use the x64 LAV build for x64 apps and the x86 build for Win32 apps. |
| `error 126` | A dependent DLL is missing | Copy all DLLs from the portable zip. |
| `codec not found` | The `.ax` files are not in the codec folder | Check the folder, or call `setCodecDirectory()`. |
| `buffer sizes do not match` | RGB24 video whose width is not a multiple of 4 | Call `video.setPixelFormat(OF_PIXELS_RGBA)` before `load()`. |

To inspect the DirectShow graph, uncomment the `SaveGraphFile(...)` line in `ofxLavVideoPlayer.cpp`. It writes a `.grf` file that can be opened in [GraphStudioNext](https://github.com/cplussharp/graph-studio-next).

## How it works

1. `LAVVideo.ax` and `LAVAudio.ax` are loaded with `LoadLibraryEx` and their filters are created through `DllGetClassObject`, then added to the filter graph. DirectShow's Intelligent Connect tries filters already in the graph first, so they take priority over system codecs.
2. LAV Splitter Source opens the file through `IFileSourceFilter`, and each output pin is rendered.
3. As in `ofDirectShowPlayer`, a Sample Grabber receives the decoded RGB frames, and the default video renderer is replaced with a Null Renderer.

The loaded `.ax` modules stay loaded for the lifetime of the process. LAV settings are read from the registry (`HKEY_CURRENT_USER\Software\LAV`) if present; otherwise the defaults are used.

## License

The addon source code is released under the [MIT License](LICENSE.md). It is based on `ofDirectShowPlayer` from openFrameworks (MIT License), originally written by Theodore Watson.

LAV Filters is **not** included in this repository. LAV Filters is licensed under the GNU General Public License v2 and uses FFmpeg. If you distribute an application together with LAV Filters, make sure you comply with their licenses, and be aware that some codecs (e.g. H.264, HEVC) may be subject to patent licensing depending on your use and region.