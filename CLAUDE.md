# CLAUDE.md

Guidance for this personal fork. Read `docs/PERSONAL-BUILD.zh-CN.md` and
`docs/SOURCE-REVIEW.zh-CN.md` for the current delivery scope and limitations.
Do not infer current dependencies from older upstream architecture descriptions.

## Build and verification

Windows Release x64 uses Visual Studio 2026 v145 and an installed Windows SDK.
The repository vendors its link libraries and runtime DLLs under `external/`.

```powershell
pwsh -NoProfile -File scripts/Build-Windows.ps1 -VisualStudio 'N:\Microsoft Visual Studio\18\Enterprise'
pwsh -NoProfile -File scripts/Test-Windows.ps1
pwsh -NoProfile -File scripts/Package-Windows.ps1 -VisualStudio 'N:\Microsoft Visual Studio\18\Enterprise'
```

Tests require Bonjour running and free preferred test ports plus TCP 7100.
For a machine whose default ports are occupied, use the documented alternative
ports and explicit adapter conflict expectation in PERSONAL-BUILD. Never stop
another application to make tests pass. Packaging requires a clean committed
source tree, successful desktop tests, and matching runtime binary hashes.
Build logs/test evidence go under `build/`; untracked packages go under `dist/`.
No tool installation is performed by these scripts.

## Actual project layout

- `AirPlayLib`: C static library for HTTP/RTSP, pairing, FairPlay and RTP.
- `airplay2dll`: C++ wrapper, FFmpeg decoding, Bonjour registration, callback API.
- `dnssd`: Windows Bonjour client DLL; the Apple Bonjour service is external.
- `AirPlayServer`: Windows subsystem GUI using SDL2 and Dear ImGui.
- `MirrorSimAdapter`: headless adapter with external DNS-SD support.
- `AirPlayLibSmokeTests`: upstream protocol/library regression executable.
- `tests/PortRegressionTests`: additional x64 regression project built separately.

The desktop uses SDL2 (`external/SDL2`, header version 2.30.12), SDL_Renderer,
YUV textures and ImGui's SDL2/SDLRenderer2 backends. The old SDL 1.2 software
surface/scaling-thread description does not describe this baseline.

Runtime application files: `AirPlayServer.exe`, `airplay2dll.dll`, `dnssd.dll`,
`SDL2.dll`, `avcodec-62.dll`, `avutil-60.dll`, `swscale-9.dll`, plus the Microsoft
release CRT supplied by the packaging script. The inspected x64 imports do not
require `msys-2.0.dll`. libplist is statically linked from the vendored library;
an obsolete absolute MSYS2 library search path has been removed. FFmpeg 8.1.2
is the upstream minimal LGPL build; this task did not rebuild third-party DLLs.

## Receiver lifetime and ports

`WinMain` owns `CSDLPlayer`, which owns `CAirServer` and its callback. Network
threads must be stopped/joined before callback or player resources are freed.
The desktop shutdown path cancels PIN approval waits before joining producers.

5001 (RAOP) and 7001 (AirPlay) are preferred ports. Windows TCP binds use
SO_EXCLUSIVEADDRUSE; only bind errors 10048/10013 permit a same-socket retry at
port 0. Preserve actual bound ports through DNS-SD and RAOP SETUP eventPort.
7100 remains a fixed upstream compatibility listener; failure rolls back all
listeners. External DNS-SD mode fails if preferred ports cannot be retained.

Public APIs are in `airplay2dll/include/Airplay2Head.h`; the build copies that
header to `AirPlayServer/include/`. Keep both copies synchronized.
`fgServerGetPorts` and `fgServerIsRunning` expose bound ports/receiver health.
A window title is not evidence of a live listener or an iPhone connection.

## Scope of verification

Synthetic protocol tests exercise discovery, fallback and shutdown. They do not
replace iPhone discovery, pairing, media, rotation, reconnection or soak tests.
The source review is focused, not a complete security audit. Preserve the
original license notices and the separate personal-build identity.
