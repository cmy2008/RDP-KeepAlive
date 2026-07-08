# RDP Keep-Alive
> [!WARNING]
> This project is fully AI generation use.
Headless command-line tool that maintains RDP session connectivity by keeping the connection alive. Uses FreeRDP 3.x as the underlying library.

## Features

- **No GUI required** — pure console application
- **Configurable resolution** — `/size:WxH`, `/w`, `/h`
- **DPI scaling** — `/scale:N`, `/scale-desktop:N`, `/scale-device:N`
- **Graphics suppression** — `/no-render` to save server performance
- **Optional mouse keepalive** — `/mouse` for aggressive idle timeout servers
- **Log level control** — `/log-level:OFF|FATAL|ERROR|WARN|INFO|DEBUG|TRACE`
- **Certificate handling** — `/cert-ignore` to auto-accept server certs

## Quick Start

```powershell
# Production (quiet, low server load)
rdp-keepalive /v:server /u:user /p:pass /size:800x600 /bpp:16 /no-render /cert-ignore /log-level:WARN

# Debug (verbose output)
rdp-keepalive /v:server /u:user /p:pass /size:800x600 /no-render /cert-ignore /log-level:DEBUG

# With mouse keepalive (for servers requiring input)
rdp-keepalive /v:server /u:user /p:pass /size:800x600 /no-render /mouse /interval:30 /cert-ignore
```

## Parameters

| Parameter | Description |
|-----------|-------------|
| `/v:host[:port]` | RDP server address **(required)** |
| `/u:username` | Login username |
| `/p:password` | Login password |
| `/d:domain` | Domain |
| `/size:WxH` | Desktop resolution |
| `/scale:N` | DPI scaling percentage |
| `/bpp:N` | Color depth (16 or 32) |
| `/interval:N` | Keep-alive log interval in seconds (default: 60) |
| `/no-render` | Suppress graphics output (saves server performance) |
| `/mouse` | Use mouse move events for keep-alive |
| `/cert-ignore` | Accept all server certificates |
| `/log-level:N` | Log level: OFF, FATAL, ERROR, WARN, INFO, DEBUG, TRACE |

## Building

### Prerequisites

- Visual Studio 2026+ (MSVC)
- CMake 3.13+
- OpenSSL for Windows (install to `C:\Program Files\OpenSSL-Win64`)

### Build Steps

```powershell
# Clone FreeRDP
git clone https://github.com/FreeRDP/FreeRDP.git
cd FreeRDP
# Or copy FreeRDP source into the FreeRDP/ subdirectory

# Configure & Build
cmake -DCHANNEL_URBDRC=OFF -DWITH_SMARTCARD_EMULATE=OFF \
      -DCMAKE_PREFIX_PATH="C:\Program Files\OpenSSL-Win64" \
      -B build -S .
cmake --build build --config Release --target rdp-keepalive

# Executable: build/Release/rdp-keepalive.exe
```
