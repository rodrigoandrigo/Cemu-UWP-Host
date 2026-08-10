# Cemu UWP Host

Cemu UWP Host is a Universal Windows Platform front end for running the Cemu Wii U emulator inside a XAML application. It embeds a custom Cemu runtime through the `CemuEmbed` C API and presents the emulator through a native Direct3D 11 swap chain attached to a `SwapChainPanel`.

This project is experimental. The native Direct3D 11 renderer and the UWP-specific Cemu integration are under active development and do not yet provide the same compatibility as the upstream desktop Vulkan renderer.

## Current development status

### Series S memory policy

Developer Mode gives the packaged title a constrained memory budget. The renderer therefore uses fixed safety thresholds below the approximately 5120 MB process limit observed on the Series S:

| Process commit | Current behavior |
| --- | --- |
| Below **3840 MB** | Deferred shader compilation may resume. |
| **3968 MB** | Hysteresis release threshold for leaving the memory-pressure state. |
| **4096 MB** | Stop/defer new shader and driver-pipeline work and run the memory guard. |

These values are the current tested policy and should not be increased without new Series S measurements. They are deliberately lower than the platform limit so D3D11On12, `xbsc_xs.dll`, XAML, audio, and other process allocations retain headroom.

Current renderer memory work includes:

- A reusable **4 MB dynamic index ring**, with GPU retirement before wrapping, instead of retaining a separate native allocation for every indexed draw.
- Reusable uniform and cache-copy buffers, with reconstructible scratch allocations released under memory pressure.
- Texture trimming, transient-buffer release, GPU retirement, DXGI trimming, and heap compaction when the guard is active.
- Serialized shader translation/native pipeline creation and retryable shader failures when compilation is deferred by memory pressure.

### Graphics compatibility status

Recent work has hardened index decoding, programmable primitive restart, little-endian index formats, vertex-range validation, input-layout caching, GPU-side buffer-cache copies, and stream-output/rasterization ordering. These changes substantially reduce stale-buffer corruption and long stretched polygons, but they do **not** establish complete compatibility.

Known areas still under validation include:

- Title-specific geometry and texture artifacts, especially in previously unseen areas or after extended gameplay.
- Arbitrary Wii U geometry shaders that require a D3D11 compatibility fallback.
- Stream-output-heavy effects and less common GX2 primitive or vertex formats.
- Long-session stability while new shaders and D3D11On12 pipelines continue to appear.
- Graphic packs that assume Vulkan behavior or use executable modifications outside the host's safe policy.

Testing should be performed on the Series S with the same game area revisited after each change. When reporting a regression, include the full `LocalState\log.txt`, a screenshot or video, the approximate runtime before the issue, and whether the area had already compiled its shaders.

## Features

- Native Direct3D 11 rendering inside a XAML `SwapChainPanel`.
- Embedded Cemu lifecycle through `CemuEmbed_Create`, surface configuration, asynchronous initialization, pumping, and shutdown.
- Brokered folder access through the UWP folder picker without `broadFileSystemAccess`.
- Chunked copying for large games and graphic packs.
- Platform-assisted `StorageFile.CopyAsync` installation on Xbox, with automatic fallback to bounded sequential buffers when a storage provider does not support direct copying.
- A unified installer that automatically identifies an extracted base game, update, or DLC.
- Installed-game library with title name, Title ID, version, update, DLC, region, and graphic-pack information.
- Persistent library selection on Xbox: the D-pad moves focus, while `A` explicitly confirms a title and keeps it selected.
- Automatic mounting of an installed base game together with its update and DLC.
- Import and safe activation of Cemu graphic packs.
- Active-account information in the top command bar.
- Xbox controller discovery on the XAML apartment through `Windows.Gaming.Input::Gamepad`, with plain input snapshots forwarded to Cemu. SDL3-UWP remains statically integrated without passing apartment-affine WinRT objects to Cemu threads.
- Automatic player-one Wii U GamePad profile.
- GamePad virtual mouse:
  - Press the left and right shoulder buttons together to enable or disable it.
  - Move the pointer with the left thumbstick.
  - Press `A` for the left mouse button.
  - The captured controls are hidden from the emulated game while the mouse is active.
- XAudio 2.8 audio output for UWP.
- A top-bar toggle for Cemu's native FPS, draw-call, CPU, RAM, and VRAM performance overlay.

## Repository layout

The default project configuration expects the repositories to be siblings:

```text
Projects/
|-- Cemu/
|-- Cemu-UWP-Host/
`-- SDL3-uwp/
```

The modified Cemu runtime used by this host is maintained in the [rodrigoandrigo/Cemu repository](https://github.com/rodrigoandrigo/Cemu).

The following MSBuild properties can be overridden if a different layout is used:

| Property | Default | Purpose |
| --- | --- | --- |
| `CemuBuildDir` | `..\Cemu\bin` | Location of `Cemu_release.dll`, `Cemu_release.lib`, resources, and game profiles. |
| `CemuIncludeDir` | `..\Cemu\src` | Location containing `Cemu/CemuEmbed.h`. |

## Requirements

- A Windows 10 version 1903 (`10.0.18362.0`) or newer build PC. Developer Mode is required on the Xbox deployment target, not merely to compile the solution.
- Visual Studio 18 2026 with the MSVC v145 C++ toolchain and UWP C++ tools.
- Windows SDK `10.0.26100.0`.
- CMake `3.21.1` or newer, using the native/Visual Studio copy rather than MSYS2 CMake for the MSVC generator.
- A local checkout of the modified [rodrigoandrigo/Cemu](https://github.com/rodrigoandrigo/Cemu) source tree.
- A local checkout of SDL3-UWP.
- An x64 Direct3D 11-capable Windows system for desktop diagnostics validation target.

The host project contains Win32 and ARM64 configurations inherited from the original template, but the embedded Cemu runtime and packaged payload are currently configured for **x64**.

## Building

Run the following commands from **Developer PowerShell for VS 18**.

### 1. Configure the embedded Cemu runtime

```powershell
Set-Location C:\path\to\Cemu

cmake -S . -B build-msvc-d3d11 `
  -G "Visual Studio 18 2026" `
  -A x64 `
  -DCEMU_UWP=ON `
  -DENABLE_D3D11=ON `
  -DSDL3_UWP_SOURCE_DIR="C:\path\to\SDL3-uwp"
```

Use the native Visual Studio/CMake executable for this configuration. Do not configure the MSVC build with the MSYS2 UCRT64 copy of CMake.

`CEMU_UWP=ON` already forces the Direct3D 11 backend on; `ENABLE_D3D11=ON` is kept in the example to make the intended renderer explicit.

The first configuration may take some time because vcpkg builds the required static dependencies.

### 2. Build the Cemu DLL

```powershell
cmake --build build-msvc-d3d11 `
  --config Release `
  --target CemuBin `
  --parallel
```

The build must produce the following payload under `Cemu\bin`:

```text
Cemu_release.dll
Cemu_release.lib
resources/
gameProfiles/
```

`resources/sharedFonts/CafeCn.ttf` is required. The host validates this file before building because several titles depend on Cemu's shared-font resources.

### 3. Build and package the host

```powershell
Set-Location C:\path\to\Cemu-UWP-Host

& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" `
  .\Cemu-UWP-Host.vcxproj `
  /restore /m `
  /p:Configuration=Debug `
  /p:Platform=x64
```

The Debug host intentionally links and packages the Release Cemu runtime. To build a Release package, change `Configuration=Debug` to `Configuration=Release`.

The project always produces an **x64** application bundle and signs development packages with `Cemu-UWP-Host_TemporaryKey.pfx`. Install or trust the matching certificate as required when deploying through Xbox Device Portal. Replace the development signing identity and override or remove the project-specific `AppInstallerUri` before redistributing packages. The current package manifest version is `1.0.3.0`.

For a non-default repository layout, pass explicit paths:

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" `
  .\Cemu-UWP-Host.vcxproj `
  /m /p:Configuration=Debug /p:Platform=x64 `
  /p:CemuBuildDir="C:\path\to\Cemu\bin" `
  /p:CemuIncludeDir="C:\path\to\Cemu\src"
```

You can also open `Cemu-UWP-Host.slnx` in Visual Studio, select `x64`, choose **Local Machine**, and deploy or debug the application normally.

## Using the application

### Installing games, updates, and DLC

1. Open the **Library** tab.
2. Select **Install content**.
3. Choose an extracted Wii U title directory containing `code`, `content`, and `meta`.
4. Wait for the brokered copy and installation to finish.

The same button accepts base games, updates, and DLC. Content is identified from its metadata and installed into the correct MLC location. Updates and DLC are associated with their base game by Title ID.

Only use game files that you legally own and have extracted yourself. This project does not include games, title keys, console firmware, account credentials, or other copyrighted console data.

### Starting an installed title

With an Xbox controller:

1. Use the D-pad to move focus through the installed-game list.
2. Press `A` to confirm the focused title. The confirmed title remains selected while the D-pad is used to navigate to other controls.
3. Move focus to **Start game** and press `A`.

With a pointer or mouse:

1. Click a title in the installed-game list to confirm it.
2. Select **Start game**.

Before launching, review the selected title's displayed version, update, DLC, region, and graphic-pack state.

The tool tabs collapse when the title starts. They can be shown again from the top command bar without resizing the emulator surface.

### Importing graphic packs

1. Confirm a game in the library to restrict the policy to that title. If no title has been confirmed since launch, the policy scans all installed titles.
2. Select **Import graphicPacks**.
3. Choose either a `graphicPacks` directory or a parent directory containing one.

Files are copied in chunks into the application's persistent `graphicPacks` directory. The host enables compatible workaround packs while leaving executable mods and cheats disabled by its safe automatic policy.

### Controller and virtual mouse

Xbox-compatible controllers are detected automatically by the host on the XAML apartment. The host captures plain input snapshots and forwards them through `CemuEmbed_SetHostGamepadState`; WinRT controller objects do not cross into Cemu worker threads. When Cemu is ready, the host creates a player-one Wii U GamePad profile if a configured profile does not already exist.

Before a game starts:

- Use the D-pad to navigate the XAML interface.
- Press `A` on a library item to confirm that game.
- Moving focus with the D-pad does not change the confirmed game. Press `A` on another library item to replace the selection.

While a game is running:

- Press `L + R` shoulder buttons together to toggle the virtual mouse.
- Use the left thumbstick to move the pointer.
- Press or hold `A` for the left mouse button.
- Press `L + R` again to close the virtual mouse.

The command bar reports controller connection, profile, virtual-mouse, and active-account status.

## Application data

The host maps Cemu's writable paths to the UWP package directories as follows:

| Package location | Contents |
| --- | --- |
| `LocalState` | `settings.xml`, the MLC (`mlc01`), saves, accounts, installed content, `log.txt`, imported graphic packs, and D3D11 native/SPIR-V driver caches. |
| `LocalCache` | Transferable and precompiled Cemu shader caches plus temporary brokered-copy staging data. |
| `InstalledLocation` | Read-only packaged payload such as `Cemu_release.dll`, `resources`, and `gameProfiles`. |

On a Windows diagnostic installation, package data is normally rooted under:

```text
%LOCALAPPDATA%\Packages\<package-family-name>\
```

Resetting or uninstalling the application can remove both `LocalState` and `LocalCache`, so back up important saves before doing so. Driver and transferable shader caches are intentionally separate and should not be treated as interchangeable files.

## Troubleshooting

### Missing `CafeCn.ttf`

Ensure the Cemu runtime build populated `Cemu\bin\resources`, including `resources\sharedFonts\CafeCn.ttf`. Do not package only the DLL.

### A selected title is rejected

Verify that the selected folder is an extracted title with valid XML metadata in `code` and `meta`, plus the required `content` directory. An update or DLC also requires a matching installed base Title ID before it can be launched as a complete title.

### The controller is detected but does not control the game

Wait until the command bar reports **Wii U GamePad profile**. Reconnect the controller if profile creation was interrupted. When the virtual mouse is active, `A`, both shoulder buttons, and the left thumbstick are intentionally reserved for mouse control.

### The selected game changes while navigating with the D-pad

The library distinguishes focus from confirmation. Move focus with the D-pad and press `A` on the intended game once. The selected game should remain fixed while you navigate to **Start game**. To select a different game, focus it and press `A` again.

### Rendering artifacts or unsupported shaders

The Direct3D 11 backend translates Wii U shaders through GLSL/SPIR-V/HLSL compatibility paths and is still experimental. Some titles, graphic packs, arbitrary geometry shaders, mutable format aliases, or sampler swizzles may still behave differently from Vulkan.

Check the **Errors** tab and the package `LocalState\log.txt` file for detailed diagnostics.

## Project structure

- `DirectXPage.xaml` and `DirectXPage.xaml.cpp`: command bar, library, diagnostics, rendering surface, controller status, and virtual mouse.
- `Cemu_UWP_HostMain.cpp`: C++/CX adapter between UWP storage objects and the C ABI.
- `Common/DeviceResources.cpp`: Direct3D device and XAML swap-chain setup.
- `Cemu/CemuEmbed.h` in the adjacent Cemu source tree: public embedded-runtime ABI.
- `Package.appxmanifest`: UWP identity, capabilities, and visual assets.
- `LICENSE`: Apache License 2.0 terms for this host repository.

## License and third-party software

The Cemu UWP Host source in this repository is licensed under the Apache License 2.0; see `LICENSE`. Cemu is separate software distributed under the Mozilla Public License 2.0; see `Cemu/LICENSE.txt` in the adjacent source tree. SDL and other dependencies retain their respective licenses. Review all applicable notices before redistributing a binary package.
