# Cemu UWP Host

Cemu UWP Host is a Universal Windows Platform front end for running the Cemu Wii U emulator inside a XAML application. It embeds a custom Cemu runtime through the `CemuEmbed` C API and presents the emulator through a native Direct3D 11 swap chain attached to a `SwapChainPanel`.

This project is experimental. The native Direct3D 11 renderer and the UWP-specific Cemu integration are under active development and do not yet provide the same compatibility as the upstream desktop Vulkan renderer.

## Features

- Native Direct3D 11 rendering inside a XAML `SwapChainPanel`.
- No Vulkan loader, DZN driver, or Vulkan ICD files in the application package.
- Embedded Cemu lifecycle through `CemuEmbed_Create`, surface configuration, asynchronous initialization, pumping, and shutdown.
- Brokered folder access through the UWP folder picker without `broadFileSystemAccess`.
- Chunked copying for large games and graphic packs.
- A unified installer that automatically identifies an extracted base game, update, or DLC.
- Installed-game library with title name, Title ID, version, update, DLC, region, and graphic-pack information.
- Automatic mounting of an installed base game together with its update and DLC.
- Import and safe activation of Cemu graphic packs.
- Active-account information in the top command bar.
- Automatic Xbox controller discovery through `Windows.Gaming.Input::Gamepad` and SDL3-UWP.
- Automatic player-one Wii U GamePad profile.
- GamePad virtual mouse:
  - Press the left and right shoulder buttons together to enable or disable it.
  - Move the pointer with the left thumbstick.
  - Press `A` for the left mouse button.
  - The captured controls are hidden from the emulated game while the mouse is active.
- XAudio 2.8 audio output for UWP.
- In-application error and diagnostic view.

## Repository layout

The default project configuration expects the repositories to be siblings:

```text
Projects/
|-- Cemu/
|-- Cemu-UWP-Host/
`-- SDL3-uwp/
```

The following MSBuild properties can be overridden if a different layout is used:

| Property | Default | Purpose |
| --- | --- | --- |
| `CemuBuildDir` | `..\Cemu\bin` | Location of `Cemu_release.dll`, `Cemu_release.lib`, resources, and game profiles. |
| `CemuIncludeDir` | `..\Cemu\src` | Location containing `Cemu/CemuEmbed.h`. |

## Requirements

- Windows 10 or Windows 11 with Developer Mode enabled.
- Visual Studio 18 2026 with the MSVC C++ toolchain and UWP C++ tools.
- Windows SDK `10.0.26100.0`.
- CMake available from the Developer PowerShell environment.
- A local checkout of the modified Cemu source tree.
- A local checkout of SDL3-UWP.
- An x64 Direct3D 11-capable GPU and current graphics drivers.

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

1. Select a title from the installed-game list.
2. Review its displayed version, update, DLC, region, and graphic-pack state.
3. Select **Start game**.

The tool tabs collapse when the title starts. They can be shown again from the top command bar without resizing the emulator surface.

### Importing graphic packs

1. Select a game in the library, or leave the selection empty to scan for all installed titles.
2. Select **Import graphicPacks**.
3. Choose either a `graphicPacks` directory or a parent directory containing one.

Files are copied in chunks into the application's persistent `graphicPacks` directory. The host enables compatible workaround packs while leaving executable mods and cheats disabled by its safe automatic policy.

### Controller and virtual mouse

Xbox-compatible controllers are detected automatically. When Cemu is ready, the host creates a player-one Wii U GamePad profile if a configured profile does not already exist.

While a game is running:

- Press `L + R` shoulder buttons together to toggle the virtual mouse.
- Use the left thumbstick to move the pointer.
- Press or hold `A` for the left mouse button.
- Press `L + R` again to close the virtual mouse.

The command bar reports controller connection, profile, virtual-mouse, and active-account status.

## Application data

Cemu configuration, the MLC, saves, accounts, logs, shader caches, installed titles, and imported graphic packs are stored in the UWP package data directories, primarily:

```text
%LOCALAPPDATA%\Packages\<package-family-name>\LocalState
```

Temporary brokered-copy staging data is stored under the package's `LocalCache` directory. Resetting or uninstalling the application can remove package-local data, so back up important saves before doing so.

## Troubleshooting

### `LNK1181: cannot open Cemu_release.lib`

Build the `CemuBin` target in Release mode first and verify that both `Cemu_release.dll` and `Cemu_release.lib` exist under `Cemu\bin`.

### Missing `CafeCn.ttf`

Ensure the Cemu runtime build populated `Cemu\bin\resources`, including `resources\sharedFonts\CafeCn.ttf`. Do not package only the DLL.

### A selected title is rejected

Verify that the selected folder is an extracted title with valid XML metadata in `code` and `meta`, plus the required `content` directory. An update or DLC also requires a matching installed base Title ID before it can be launched as a complete title.

### The controller is detected but does not control the game

Wait until the command bar reports **Wii U GamePad profile**. Reconnect the controller if profile creation was interrupted. When the virtual mouse is active, `A`, both shoulder buttons, and the left thumbstick are intentionally reserved for mouse control.

### Rendering artifacts or unsupported shaders

The Direct3D 11 backend translates Wii U shaders through GLSL/SPIR-V/HLSL compatibility paths and is still experimental. Some titles, graphic packs, arbitrary geometry shaders, mutable format aliases, or sampler swizzles may still behave differently from Vulkan.

Check the **Errors** tab and the package `LocalState\log.txt` file for detailed diagnostics.

## Current limitations

- x64 is the only configured embedded-runtime/package architecture.
- Direct3D 11 game compatibility is incomplete compared with upstream Vulkan.
- Online functionality requires the user's own valid console-derived files and is not configured by this host.
- Some desktop Cemu features and configuration dialogs are not exposed in the UWP interface.
- UWP sandbox restrictions require selected content to be copied into package-managed storage before Cemu can use normal filesystem access.

## Project structure

- `DirectXPage.xaml` and `DirectXPage.xaml.cpp`: command bar, library, diagnostics, rendering surface, controller status, and virtual mouse.
- `Cemu_UWP_HostMain.cpp`: C++/CX adapter between UWP storage objects and the C ABI.
- `Common/DeviceResources.cpp`: Direct3D device and XAML swap-chain setup.
- `Cemu/CemuEmbed.h` in the adjacent Cemu source tree: public embedded-runtime ABI.
- `Package.appxmanifest`: UWP identity, capabilities, and visual assets.

## License and third-party software

Cemu is separate software distributed under the Mozilla Public License 2.0; see `Cemu/LICENSE.txt` in the adjacent source tree. SDL and other dependencies retain their respective licenses. Review all applicable licenses before redistributing a binary package.

This host repository should include its own explicit license before it is published or redistributed independently.
