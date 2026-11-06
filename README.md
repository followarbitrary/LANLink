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

The help output lists these flags: `--device-id`, `--peer-id`, `--listen-port`, `--connect`, `--display-id`, `--peer-display-id`, `--remote-side`, `--width`, `--height`, and `--help`. Device and display IDs are numeric; `--remote-side` accepts only `left` or `right`; `--connect` takes `HOST:PORT`.

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
  --remote-side right

# Windows PowerShell
.\build\debug\lanlink_app.exe `
  --device-id 1 --peer-id 2 `
  --listen-port 41000 --connect 192.168.1.20:41000 `
  --display-id 10 --peer-display-id 20 `
  --remote-side right
```

### Device B (ID 2, display 20)

Device B listens on the same port and does not pass `--connect`. Its reciprocal IDs and opposite direction describe device A to its left.

```sh
# macOS
./build/debug/lanlink_app \
  --device-id 2 --peer-id 1 \
  --listen-port 41000 \
  --display-id 20 --peer-display-id 10 \
  --remote-side left

# Windows PowerShell
.\build\debug\lanlink_app.exe `
  --device-id 2 --peer-id 1 `
  --listen-port 41000 `
  --display-id 20 --peer-display-id 10 `
  --remote-side left
```

Each process should print `LANLink listening on 0.0.0.0:41000`. When the peer session becomes active, both terminals print `LANLink connected`. Device A's `--connect` initiates the session; device B accepts it while listening. After the connection is established, move the pointer across the configured edge to exercise input forwarding. Connection failures are printed as `error:` or `runtime error:` messages. Press **Ctrl+C** in each terminal to stop the node and clean up input state.
