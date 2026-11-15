# LANLink

LANLink is a command-line node for sharing keyboard and mouse input between two computers on the same local network. The `lanlink_app` executable is supported on macOS and Windows.

## Prerequisites

### macOS

- macOS with Xcode Command Line Tools (including Clang).
- CMake 3.23 or newer.
- A user session with permission to control the computer. Before starting a node, open **System Settings → Privacy & Security → Accessibility** and enable the terminal (or the `lanlink_app` binary) that will run LANLink. Without this permission, the macOS input backend cannot inject events.

### Windows

- Windows 10 or later.
- CMake 3.23 or newer.
- Visual Studio 2022 with the **Desktop development with C++** workload (the `debug-windows` preset uses the `Visual Studio 17 2022` generator).
- Allow `lanlink_app` through Windows Defender Firewall when Windows prompts. Permit inbound TCP traffic on the LAN port used below (`41000`) on the Private network profile.

### Third-party dependencies

GoogleTest 1.14.0 is included under [`third-party/googletest`](third-party/googletest), so configuring and running the CTest suite does not download dependencies or depend on a machine-local GoogleTest installation. The Windows release script bootstraps a fixed vcpkg source archive and installs the pinned Qt 6 and OpenSSL ports into its repository-local prefix. Other local Debug builds may still use an explicitly supplied Qt/OpenSSL SDK through `CMAKE_PREFIX_PATH`. See [`third-party/README.md`](third-party/README.md) for the dependency boundary and license details.

## Build

From the repository root, configure and build the Debug preset:

```sh
cmake --preset debug
cmake --build --preset debug --parallel
```

The preset selects Unix Makefiles on macOS and Visual Studio 17 2022 on Windows. The executable is `build/debug/lanlink_app` on macOS and `build\debug\lanlink_app.exe` on Windows.

Check the command-line interface with:

```sh
# macOS
./build/debug/lanlink_app --help

# Windows PowerShell
.\build\debug\lanlink_app.exe --help
```

The help output lists these flags: `--device-id`, `--peer-id`, `--listen-port`, `--connect`, `--group`, `--discovery-port`, `--display-id`, `--peer-display-id`, `--remote-side`, `--width`, `--height`, `--peer-width`, `--peer-height`, `--clipboard`, and `--help`. Device and display IDs are numeric; `--remote-side` accepts only `left` or `right`; `--connect` takes `HOST:PORT`. LANLink reads the local display bounds from the native backend at startup: macOS uses Quartz logical points and Windows uses the DPI-aware monitor coordinate space. The width and height flags remain optional overrides for diagnostics. Peer dimensions are no longer required because the handoff sends an edge ratio and the target maps it into its own detected display bounds.

Clipboard synchronization is opt-in with `--clipboard` and remains disabled by default for privacy. It supports UTF-8 text, HTML, and PNG data with a 4 MiB combined package limit including protocol metadata; a conservative metadata reserve keeps the payload below that wire limit. Native clipboard integration is available on macOS and Windows. The command-line and Qt desktop clients use the platform clipboard APIs. On macOS, Accessibility permission is required for input forwarding; clipboard synchronization itself does not add a separate permission requirement.

The Qt6 control panel also supports single-file transfer over the authenticated peer session. Select a file or drop it onto **文件传输** after a real peer is online; the file is sent in bounded chunks and the page reports accepted, progress, completed, failed, or canceled states. The receiver writes to the platform Downloads directory (falling back to a temporary directory when Downloads is unavailable), refuses to overwrite an existing file, and removes partial files after failure or cancellation. Files are limited to 4 GiB and basename-only UTF-8 names; resume, multi-file queues, directories, and overwrite confirmation are not supported.

### Qt6 desktop control panel

When Qt 6 is installed, the Debug preset also builds the optional `lanlink_desktop` Qt Widgets control panel. It starts the same discovery, transport, native input-forwarding, and optional clipboard-sync runtime as `lanlink_app` on a worker thread as soon as the window opens. Configure device/peer IDs, listen/discovery ports, group, optional `HOST:PORT`, display IDs, and left/right direction in **连接设置**; the same page can stop and restart the service. Clipboard synchronization is off by default and can be enabled from the settings page or tray menu. It transfers UTF-8 text, HTML, and PNG data within the 4 MiB package limit including protocol metadata per clipboard update.

File transfer uses the same worker-thread runtime and TCP session; no separate core service or port needs to be started. The transfer controls stay disabled until discovery reports an online peer, and canceling a send removes the receiver's temporary `.lanlink-*.part` file.

```sh
cmake --build cmake-build-debug --target lanlink_desktop
QT_QPA_PLATFORM=offscreen ./cmake-build-debug/lanlink_desktop >/tmp/lanlink-desktop.log 2>&1 & pid=$!; sleep 2; kill "$pid"; wait "$pid" || true # headless smoke test
```

Set `-DLANLINK_BUILD_DESKTOP=OFF` to skip the desktop target on machines without Qt 6; the CLI and core targets remain available.

## Packaging

The packaging scripts create a clean Release build, run the complete CTest suite (retrying a failed test up to ten times for timing-sensitive tests), and place the runnable artifacts and this README in a platform-specific archive. A test that remains failing still stops packaging. The scripts resolve the repository from their own location, so they can be invoked from any working directory.

### macOS

```sh
chmod +x scripts/package-macos.sh
./scripts/package-macos.sh
```

The script uses the host architecture reported by `uname -m` and writes `dist/lanlink-macos-<architecture>.tar.gz`. The archive always contains the CLI runtime (`lanlink_app`). When Qt6 and `macdeployqt` are available, it also contains a self-contained `lanlink_desktop.app` bundle with the Qt6 control panel, and creates `dist/lanlink-macos-<architecture>.dmg`. Double-click the DMG, then drag `LANLink.app` onto the `Applications` shortcut. The desktop app starts the Core runtime automatically when it opens; the device-management page reports the actual local platform and an active LAN IPv4 endpoint when one is available. The settings page's **跨屏键鼠转发** switch pauses or resumes forwarding in the runtime and restores local input when paused. Clipboard synchronization is opt-in and supports text, HTML, and PNG data within the 4 MiB package limit including protocol metadata. Input forwarding in both the app and CLI needs Accessibility permission under **System Settings → Privacy & Security → Accessibility**; clipboard synchronization itself does not add a separate permission requirement.

The macOS archive uses local ad-hoc signatures only; it is not Developer ID signed or notarized. Verify the package before copying it to another Mac:

```sh
tar -tzf dist/lanlink-macos-$(uname -m).tar.gz
tar -xzf dist/lanlink-macos-$(uname -m).tar.gz
file lanlink-macos-$(uname -m)/lanlink_app
test ! -e lanlink-macos-$(uname -m)/lanlink_desktop.app || file lanlink-macos-$(uname -m)/lanlink_desktop.app/Contents/MacOS/lanlink_desktop
shasum -a 256 dist/lanlink-macos-$(uname -m).tar.gz
shasum -a 256 dist/lanlink-macos-$(uname -m).dmg
```

The DMG is an ad-hoc signed drag-and-drop installer; it is not Developer ID signed or notarized. macOS may ask you to approve the app the first time it is opened.

The `file` output should report the same architecture as the machine that ran the packaging script (`arm64` or `x86_64`). For distribution outside a trusted development environment, sign and notarize the binary with an Apple Developer ID before sharing it.

### Windows

Run the PowerShell script from a PowerShell prompt. It uses the Visual Studio 17 2022 generator with the x64 platform and the dynamic MSVC runtime (`/MD`) required by the pinned vcpkg packages:

```powershell
.\scripts\package-windows.ps1
```

If the PowerShell execution policy blocks local scripts, use:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\package-windows.ps1
```

The script writes `dist\lanlink-windows-x64.zip`. It bootstraps the vcpkg commit and archive checksum recorded in `third-party\vcpkg-bootstrap.json` when the repository-local copy is absent, then installs the fixed `qtbase[gui,network,testlib,widgets]`, `qtsvg`, and `openssl` ports in classic mode into `third-party\vcpkg_installed`. It uses that same prefix for CMake, `windeployqt.exe`, and the OpenSSL runtime DLLs. The archive always contains `lanlink_app.exe`, `lanlink_desktop.exe`, the Qt runtime files, and the OpenSSL TLS runtime. A missing or corrupt download, failed package install, missing Qt deployment tool, or missing OpenSSL DLL stops packaging; the script does not fall back to `PATH`, `QTDIR`, or a global vcpkg installation. The first run needs network access unless the pinned archive and package cache are already available. The desktop executable embeds the LANLink application icon from `assets\icons\LANLink.ico`. Extract the archive, then run `.\lanlink_app.exe --help` from the extracted directory. Add `--clipboard` on both peers to enable UTF-8 text, HTML, and PNG synchronization (4 MiB combined package limit including protocol metadata). Allow the packaged executable through Windows Defender Firewall on the Private network profile when prompted so peers can connect.

The packaging script must be run on Windows with Visual Studio 2022. The macOS development host can validate the pinned vcpkg package graph, CMake configuration, CTest suite, and script contract, but cannot execute the native Windows package or `windeployqt.exe`.

## Automatic LAN discovery

When `--peer-id` and `--connect` are omitted, LANLink announces itself on the `lanlink` group over UDP port `41001` and waits for a visible peer. The lowest device ID selects the outgoing connection. The selected peer's display ID is used automatically, and the side is inferred from device ID ordering. Pass `--group NAME` and `--discovery-port PORT` when separate pairs share a network.

```sh
# Device A, lower ID, connects automatically
./build/debug/lanlink_app --device-id 1 --listen-port 41000

# Device B, higher ID, listens and accepts automatically
./build/debug/lanlink_app --device-id 2 --listen-port 41000
```

Use `--peer-id` to restrict discovery to one device. Explicit `--display-id`, `--peer-display-id`, and `--remote-side` values take precedence over discovered defaults. Use `--connect HOST:PORT` for an explicit two-node connection. Discovery continues to poll while the node runs, while an outgoing endpoint selected from discovery remains pinned for reconnects.

## Topology synchronization

Topology edits made in the desktop control panel are broadcast as complete snapshots to the configured discovery group. Each snapshot carries the originating device ID and a monotonically ordered revision, so peers can retain the newest state they have received. A node periodically re-announces its own complete snapshot on that group, allowing nodes that start later or miss a datagram to converge eventually. Applying a remote snapshot updates the local runtime and desktop view without broadcasting that remote update again. The current desktop editor presents the two-device layout as two editable device cards.

To exercise clipboard synchronization during a two-device test, pass `--clipboard` to both processes. The setting is local to each process, so leaving it off on either peer keeps that peer from publishing or applying clipboard updates.

## Two-device LAN test

Use two computers on the same LAN. Replace `192.168.1.20` below with **device B's real LAN IPv4 address** (for example, from `ipconfig` on Windows or `ifconfig`/`networksetup` on macOS). Keep TCP port `41000` available on both devices and allow it through the Windows firewall where applicable. Run both commands after building.

### Device A (ID 1, display 10)

Device A listens on all local interfaces and actively connects to device B. The remote display is to the right.

```sh
# macOS
./build/debug/lanlink_app \
  --device-id 1 --peer-id 2 \
  --listen-port 41000 --connect 192.168.1.20:41000 \
  --display-id 10 --peer-display-id 20 \
  --remote-side right --clipboard

# Windows PowerShell
.\build\debug\lanlink_app.exe `
  --device-id 1 --peer-id 2 `
  --listen-port 41000 --connect 192.168.1.20:41000 `
  --display-id 10 --peer-display-id 20 `
  --remote-side right --clipboard
```

### Device B (ID 2, display 20)

Device B listens on the same port and does not pass `--connect`. Its reciprocal IDs and opposite direction describe device A to its left.

```sh
# macOS
./build/debug/lanlink_app \
  --device-id 2 --peer-id 1 \
  --listen-port 41000 \
  --display-id 20 --peer-display-id 10 \
  --remote-side left --clipboard

# Windows PowerShell
.\build\debug\lanlink_app.exe `
  --device-id 2 --peer-id 1 `
  --listen-port 41000 `
  --display-id 20 --peer-display-id 10 `
  --remote-side left --clipboard
```

Each process should print `LANLink listening on 0.0.0.0:41000`. When the peer session becomes active, both terminals print `LANLink connected`. Device A's `--connect` initiates the session; device B accepts it while listening. After the connection is established, move the pointer across the configured edge to exercise input forwarding. Connection failures are printed as `error:` or `runtime error:` messages. Press **Ctrl+C** in each terminal to stop the node and clean up input state.

To inspect a handoff without logging every normal mouse delta, set `LANLINK_DEBUG=1` before
starting either node. The debug stream reports edge crossings, acquire/activate messages,
suppression changes, target cursor warps, and lease revocations.

```sh
LANLINK_DEBUG=1 ./build/debug/lanlink_app \
  --device-id 1 --peer-id 2 --listen-port 41000 --connect 192.168.1.20:41000
```
