# RDP Keep-Alive Tool Implementation Plan

## Context

Build a **command-line RDP keep-alive tool** (`rdp-keepalive`) using the FreeRDP 3.28.1-dev0 library. The tool connects to an RDP server with user-specified parameters (host, port, username, password, domain, resolution, DPI/scaling) and keeps the session alive by periodically sending harmless input events to prevent server-side idle timeout disconnection.

**Why**: FreeRDP has no existing client-side keep-alive tool. TCP-level keepalive settings are deprecated in FreeRDP 3.x, and RDP Heartbeat PDUs are server-initiated only. The standard `wfreerdp` client is GUI-based (Win32 windows). A headless command-line tool fills this gap.

**Environment**: Windows 11, VS 2022 Community at `D:\Program Files\Microsoft Visual Studio\18\Community`, FreeRDP repo at `d:\Project\RDP-KeepAlive\FreeRDP\`.

---

## Approach

Create a new project `client/rdp-keepalive/` modeled after `client/Sample/` (the `sfreerdp` minimal client). Integrate via FreeRDP's auto-discovery mechanism (`ModuleOptions.cmake`) so it builds as part of the FreeRDP build tree. The project links only `freerdp-client freerdp winpr` — no GUI dependencies.

### Keep-Alive Strategy

The key finding from the codebase is that `freerdp_client_send_button_event()` with `PTR_FLAGS_MOVE` only sends data if `FreeRDP_MouseMotion = TRUE` (see `client/common/client.c:1779-1784`). The strategy:

1. Set `FreeRDP_MouseMotion = TRUE` in PreConnect
2. Replace `WaitForMultipleObjects(..., INFINITE)` with a 1-second timeout
3. After each timeout, check if the keep-alive interval has elapsed
4. Send a `PTR_FLAGS_MOVE` mouse event at (0,0) — this transmits an RDP PDU without moving the cursor, resetting the server's idle timer

---

## Files to Create

### 1. `client/rdp-keepalive/ModuleOptions.cmake`

Auto-discovery manifest for FreeRDP's extra client mechanism (`client/CMakeLists.txt:68-91`). Must set a non-"FreeRDP" vendor to be picked up.

```cmake
set(FREERDP_CLIENT_NAME "rdp-keepalive")
set(FREERDP_CLIENT_PLATFORM "CommandLine")
set(FREERDP_CLIENT_VENDOR "RDP-KeepAlive")
set(FREERDP_CLIENT_ENABLED 1)
```

### 2. `client/rdp-keepalive/CMakeLists.txt`

Mirrors `client/Sample/CMakeLists.txt`. Builds `rdp-keepalive` executable, links `freerdp-client freerdp winpr`. Uses `AddTargetWithResourceFile` for proper Windows binary metadata.

### 3. `client/rdp-keepalive/rdp_keepalive.h`

Context structure and constants:
```c
typedef struct {
    rdpClientContext common;        // MUST be first field
    UINT64 lastKeepAliveTick;       // timestamp of last keep-alive event
} rdpKeepAliveContext;

#define RDP_KEEPALIVE_DEFAULT_INTERVAL_MS 60000  // 60 seconds
```

### 4. `client/rdp-keepalive/rdp_keepalive.c` (main implementation)

All logic in a single file, following `client/Sample/tf_freerdp.c` patterns:

**Functions to implement:**

| Function | Purpose |
|---|---|
| `ka_pre_connect()` | Set `FreeRDP_MouseMotion=TRUE`, `FreeRDP_DeactivateClientDecoding=TRUE`, OS type, cert prefs, subscribe channel events |
| `ka_post_connect()` | `gdi_init(instance, PIXEL_FORMAT_XRGB32)`, register BeginPaint/EndPaint/DesktopResize/PlaySound callbacks |
| `ka_post_disconnect()` | `gdi_free(instance)`, unsubscribe channel events |
| `ka_client_new()` | Wire PreConnect/PostConnect/PostDisconnect callbacks on instance |
| `ka_client_free()` | Cleanup (no-op for our simple client) |
| `ka_client_start()` | No-op |
| `ka_client_stop()` | No-op |
| `ka_logon_error_info()` | Log logon errors |
| `RdpClientEntry()` | Fill `RDP_CLIENT_ENTRY_POINTS` with all callbacks, `ContextSize = sizeof(rdpKeepAliveContext)` |
| `ka_send_keepalive()` | Call `freerdp_client_send_button_event(cctx, FALSE, PTR_FLAGS_MOVE, 0, 0)` |
| `ka_thread_proc()` | Connect + event loop with timeout-based keep-alive (1s `WaitForMultipleObjects` timeout, check elapsed time) |
| `ka_print_usage()` | Print help text |
| `main()` | Create context, parse CLI, start, run thread, stop, free |

**Event loop modification** (key difference from Sample):
```c
// Instead of WaitForMultipleObjects(..., INFINITE):
#define KA_TICK_MS 1000
DWORD timeout = KA_TICK_MS;
UINT64 lastKA = GetTickCount64();

while (!freerdp_shall_disconnect_context(context)) {
    nCount = freerdp_get_event_handles(context, handles, ARRAYSIZE(handles));
    status = WaitForMultipleObjects(nCount, handles, FALSE, timeout);
    
    // Always check events (even on timeout)
    freerdp_check_event_handles(context);
    
    // Check keep-alive timer
    UINT64 now = GetTickCount64();
    if (now - lastKA >= ka_interval_ms) {
        ka_send_keepalive(context);
        lastKA = now;
    }
}
```

**Command-line parameters** (using `freerdp_client_settings_parse_command_line` for standard args + custom `COMMAND_LINE_ARGUMENT_A` for tool-specific):

| Parameter | Setting Key | Description |
|---|---|---|
| `/v:host[:port]` | `FreeRDP_ServerHostname` | RDP server address |
| `/u:username` | `FreeRDP_Username` | Login username |
| `/p:password` | `FreeRDP_Password` | Login password |
| `/d:domain` | `FreeRDP_Domain` | Domain |
| `/size:WxH` | `FreeRDP_DesktopWidth/Height` | Resolution (e.g., `1920x1080`) |
| `/w:width` | `FreeRDP_DesktopWidth` | Desktop width |
| `/h:height` | `FreeRDP_DesktopHeight` | Desktop height |
| `/scale:percent` | `FreeRDP_DesktopScaleFactor` + `DeviceScaleFactor` | DPI scaling |
| `/scale-desktop:percent` | `FreeRDP_DesktopScaleFactor` | Desktop scale only |
| `/scale-device:percent` | `FreeRDP_DeviceScaleFactor` | Device scale only |
| `/bpp:depth` | `FreeRDP_ColorDepth` | Color depth (default 32) |
| `/admin` | `FreeRDP_ConsoleSession` | Connect to admin session |
| `/interval:N` | *(custom)* | Keep-alive interval in seconds (default 60) |
| `/cert-ignore` | *(custom)* | Accept all certificates without prompting |
| `/+nego` | `FreeRDP_NegotiateSecurityLayer` | Enable protocol security negotiation |
| `/restricted-admin` | `FreeRDP_RestrictedAdminModeRequired` | Restricted admin mode |
| `/network:auto\|lan\|broadband\|modem` | `FreeRDP_NetworkAutoDetect` | Network type |

---

## Files to Modify

### `client/CMakeLists.txt` — No modification needed!

The auto-discovery loop at lines 67-91 scans for `*/ModuleOptions.cmake` and auto-adds non-FreeRDP-vendor clients. Since our `FREERDP_CLIENT_VENDOR` is `"RDP-KeepAlive"`, it will be picked up automatically when `WITH_CLIENT=ON`.

---

## Dependencies

- **OpenSSL**: Required by FreeRDP. Must be installed and pointed to via `CMAKE_PREFIX_PATH` or `FREERDP_EXTERNAL_SSL_PATH`. On Windows, a pre-built OpenSSL can be installed via vcpkg or downloaded from https://slproweb.com/products/Win32OpenSSL.html
- **FreeRDP core**: Built from source (the unified build handles winpr + freerdp together)

---

## Build Instructions

### Prerequisites

1. Install OpenSSL for Windows (e.g., Win64 OpenSSL v3.x from slproweb.com, install to `C:\OpenSSL` or use vcpkg)
2. Ensure CMake and Ninja are available (VS 2022 includes CMake; Ninja can be installed via `winget install Ninja-build.Ninja`)

### Build Steps (from VS 2022 Developer Command Prompt)

```powershell
# Enter FreeRDP dir
cd d:\Project\RDP-KeepAlive\FreeRDP

# Configure (first time)
cmake -GNinja `
  -DCMAKE_BUILD_TYPE=Release `
  -DWITH_SERVER=OFF `
  -DWITH_CLIENT_SDL=OFF `
  -DWITH_PROXY=OFF `
  -DWITH_SHADOW=OFF `
  -DWITH_PLATFORM_SERVER=OFF `
  -DWITH_FFMPEG=OFF `
  -DWITH_SWSCALE=OFF `
  -DWITH_CAIRO=OFF `
  -DWITH_WIN_CONSOLE=ON `
  -DWITH_VERBOSE_WINPR_ASSERT=OFF `
  -DCMAKE_PREFIX_PATH=C:\OpenSSL `
  -B build -S .

# Build everything (FreeRDP + rdp-keepalive)
cmake --build build --config Release

# The executable will be at:
# build\client\rdp-keepalive\rdp-keepalive.exe
```

### Quick Rebuild (after source changes)
```powershell
cmake --build build --config Release --target rdp-keepalive
```

### Usage Example
```powershell
rdp-keepalive.exe /v:192.168.1.100 /u:Administrator /p:pass123 /size:1920x1080 /scale:150 /interval:30
```

---

## Verification

1. **Build verification**: `cmake --build build --config Release --target rdp-keepalive` succeeds, produces `rdp-keepalive.exe`
2. **Help output**: `rdp-keepalive.exe /help` prints usage
3. **Connection test**: Run against a real RDP server (e.g., Windows VM with RDP enabled). Verify:
   - Successful connection with specified resolution and DPI
   - Periodic keep-alive messages logged to stdout
   - Session remains active beyond the server's idle timeout
4. **Parameter test**: Test various combinations of `/size`, `/scale`, `/scale-desktop`, `/scale-device`, `/interval`
5. **Error handling**: Test invalid host, wrong credentials, connection refused — should produce clear error messages and exit cleanly
