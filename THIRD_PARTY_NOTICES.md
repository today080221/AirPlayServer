# Third-Party Notices

## Personal desktop package

This fork's Windows x64 desktop ZIP is a personal modification based on upstream
v1.2.5. It includes SDL2 (zlib license), Dear ImGui 1.92.5 (MIT), the compiled
Bonjour client library (not the Bonjour service installer), and application-local
Microsoft Visual C++ release runtime files. Their notices are included under
`licenses/`. Apple source notices are extracted from the exact compiled source
files. The Microsoft runtime remains subject to Microsoft's redistribution terms;
its installed `Redist.txt` is included. Windows and the Bonjour service are
prerequisites. No MSYS2 runtime is needed by the inspected x64 PE imports.

The matching fork source ZIP is delivered beside the binary ZIP. Exact source
revision, source URL, baseline and runtime version are in `BUILD-INFO.json`.
The FFmpeg source URL/checksum and rebuild instructions below remain applicable;
vendored DLLs were reused without rebuilding FFmpeg or SDL2.

- SDL2 source: <https://github.com/libsdl-org/SDL/tree/release-2.30.12>
- ImGui source/license: <https://github.com/ocornut/imgui/tree/v1.92.5>
- Microsoft redistribution terms: <https://learn.microsoft.com/en-us/visualstudio/releases/2026/redistribution>

The release runtime contains open-source components in addition to code covered
by the repository's MIT license.

## FFmpeg 8.1.2

MirrorSim's receiver uses a minimal, dynamically linked FFmpeg build for H.264
video and AAC-ELD audio decoding. It is configured with GPL, nonfree, and
version-3 components disabled. Only `avcodec`, `avutil`, `swscale`, and the AAC
and H.264 decoders are enabled.

- License: LGPL-2.1-or-later
- Source: <https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz>
- Source SHA-256: `464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c`
- Rebuild script: `scripts/build-ffmpeg-windows.sh`
- Exact build configuration: `external/ffmpeg/licenses/FFMPEG_BUILD_INFO-*.txt`

The runtime archive includes the FFmpeg license and build information. Tagged
GitHub releases also attach the exact upstream source archive and checksum.

## libplist

The receiver contains libplist code for parsing AirPlay binary property lists.
Its source is available under `AirPlayServerLib/lib/plist`, and its public API
and Windows libraries are under `external/plist`.

- License: LGPL-2.1-or-later
- Upstream project: <https://github.com/libimobiledevice/libplist>

The runtime archive includes the LGPL-2.1 license text.

## PlayFair

The AirPlay receiver contains PlayFair interoperability code under
`AirPlayServerLib/lib/playfair`.

- License: GPL-3.0
- License text: `AirPlayServerLib/lib/playfair/LICENSE.md`

The runtime archive includes the GPL-3.0 license text. The corresponding source
is included in this repository and in each GitHub release's automatically
generated source archives.

## Other vendored source

The repository also contains cryptographic, networking, SDL, ImGui, and sample
application code that is not part of MirrorSim's five-file headless runtime in
every build. Their original copyright and license notices remain with their
source files and directories.
