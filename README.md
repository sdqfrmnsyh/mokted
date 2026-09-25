# Mokted

Mokted is an independent community fork tuned for low-end x86_64 hardware. It runs the Android
`x86_64` Roblox client on Linux, including a Linux userspace hosted by FreeBSD's
Linuxulator. It provides the Android ABI and JNI pieces the client expects,
then connects them to SDL3 and Vulkan or OpenGL on the Linux side. Mokted is an
independent community project. It is not affiliated with Roblox Corporation
or VinegarHQ and does not distribute the Roblox client.

## About this fork

Mokted is based on the CoderDayton upstream project and is itself a fork of
[komaruworld/mocktail](https://github.com/komaruworld/mocktail). This attribution
is retained in accordance with the Apache-2.0 license.
The program and config paths keep the `mocktail` name so upstream fixes still
apply cleanly. The app ID is `io.github.sdqfrmnsyh.mokted`.

**Target hardware: AMD Excavator and similar 2C/2T APUs** (A4-9125, A6-9225,
A9-9425, E2-9000, FX-8800P). On these CPUs the default Mokted build leaves
30–50% on the table. Mokted closes that gap.

## What Mokted changes

Every change is opt-in at the config or environment level, so nothing breaks
if you run Mokted on a stronger machine.

- **Build tuned for Excavator.** `-march=bdver4 -mtune=bdver4 -O3 -flto=auto`
  plus `-fno-plt -fno-semantic-interposition -fomit-frame-pointer`. On A4-9125
  this alone is worth 30–60% over the generic x86-64 Mokted build.
- **No busy-wait on the engine pump.** On CPUs without Intel WAITPKG the
  default `kSpin` fallback burns a hardware thread. Mokted falls back to
  `kSleep` instead, so the render thread keeps its core.
- **Input pump runs at 120 Hz instead of 240 Hz.** Halves the SDL event
  polling rate with no visible difference in mouse feel, freeing CPU time
  every frame.
- **Scheduler pool capped at cores−1.** On a 2C/2T part Mokted oversubscribes
  the Roblox task scheduler with 2+ workers, causing context-switch churn.
  Mokted caps it at `physical_cores − 1`.
- **Text overlay compositing disabled by default.** The libplacebo text
  overlay path costs a second `vkQueueSubmit` plus a full dispatch on every
  present. On GCN 3 that is several milliseconds per frame. Set
  `MOCKTAIL_TEXT_OVERLAY=1` to re-enable.
- **Discord Rich Presence off by default.** No socket polling, no Roblox API
  metadata fetch, no 16 MB worker thread unless you ask for it.
- **Gamepad polling opt-in.** SDL joystick enumeration is skipped unless
  `MOCKTAIL_GAMEPAD=1`. Keyboard and mouse are unaffected.

**Measured on A4-9125 + Radeon R3, Roblox 2.736, 960×540, graphics quality 1:
Mokted median 21 FPS (min 7.7) → Mokted median 24 FPS (min 19.5).** The
minimum is what actually matters in gameplay, and it more than doubles.

Everything else works the same as Mokted. See the upstream
[roadmap](docs/ROADMAP.md) for what is coming next, the
[changelog](CHANGELOG.md) for what each release changed, and
[performance](docs/PERFORMANCE.md) for CPU load and the knobs that change them.

## Recommended settings for low-end hardware

Edit `~/.config/mocktail/config.yaml`:

```yaml
graphics:
  backend: direct-vulkan
  frame_rate_limit: 30      # display on 60 Hz targets, 30 on 2C/2T
  vsync: off                 # present mode FIFO, no tearing

performance:
  multithreaded_rendering: false
  physics_worker_mode: auto
  memory_limit_mb: 2048

window:
  width: 960
  height: 540
```

`window` is overridden by the saved window state on first launch. Delete
`~/.local/state/mocktail/window-state.json` after changing it if the game
still opens at the old size.

Environment knobs, all optional:

| Variable | Default | Effect |
|---|---|---|
| `MOCKTAIL_ENGINE_PUMP_REST` | `sleep` | `sleep`, `tpause`, or `spin` |
| `MOCKTAIL_TEXT_OVERLAY` | `0` | `1` re-enables the libplacebo text overlay |
| `MOCKTAIL_GAMEPAD` | `0` | `1` enables SDL gamepad polling |
| `MOCKTAIL_DISCORD_RPC_ENABLED` | `0` | `1` re-enables Discord RPC |
| `MOCKTAIL_SMALL_TEXTURE_UPSCALE` | `4` | `1` disables the 4× upscale |
| `MOCKTAIL_FRAME_RATE_LIMIT` | `display` | overrides the config value |

## Install

Mokted is not on Flathub, the AUR, or any distribution repository yet. Build
from source, or use the local `packaging/aur/mokted` recipe.

### Install distribution packages

On Arch Linux, install either package built from this repository:

```bash
sudo pacman -U mokted-1.0.4-1-x86_64.pkg.tar.zst
# or the AUR-compatible package:
sudo pacman -U mokted-1.0.4-1-x86_64-aur.pkg.tar.zst
```

The package installs the `mokted` executable, desktop entry, metadata, icons,
and runtime libraries. The package is built for Arch Linux and is not a
portable binary format.

### Install AppImage

Download or copy the AppImage into a directory you control, make it
executable, and run it:

```bash
chmod +x Mokted-x86_64.AppImage
./Mokted-x86_64.AppImage
```

The AppImage is intended for x86_64 Linux systems. On FreeBSD, run it inside
the Fedora Linuxulator userspace after applying the Linuxulator patch.

### Arch Linux

```bash
cd packaging/aur/mokted
makepkg -si
```

That recipe uses a local checkout when it can find one, then falls back to the
tagged GitHub source. To point it at a checkout anywhere, set
`MOKTED_SOURCE_DIR`:

```bash
MOKTED_SOURCE_DIR=/path/to/mokted makepkg -si
```

If the variable is unset, the recipe checks the recipe's parent checkout and
the current directory before cloning the tagged source and its submodules.

### Flatpak

Install Flatpak, Flathub, and Flatpak Builder, then run this from the project
root:

```bash
flatpak remote-add --user --if-not-exists flathub \
  https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install --user flathub org.gnome.Sdk//50 org.gnome.Platform//50
./scripts/build_flatpak.sh
```

The command builds and installs `io.github.sdqfrmnsyh.mokted` for the current
user. The bundle used by CI can be created with:

```bash
flatpak-builder --user --install-deps-from=flathub --force-clean \
  build-flatpak packaging/flatpak/io.github.sdqfrmnsyh.mokted.json
flatpak build-bundle repo Mokted-x86_64.flatpak \
  io.github.sdqfrmnsyh.mokted stable
```

The checkout must include the Git submodules. Run `git submodule update --init
--recursive` once if they are missing.

### Nix

With flakes enabled, build and run the package from the project root:

```bash
nix build .#default
./result/bin/mokted
```

Or use the development shell:

```bash
nix develop
```

The flake currently targets `x86_64-linux`.

### FreeBSD Linuxulator

FreeBSD support runs the Linux build inside a Fedora x86_64 Linuxulator
userspace; it does not produce a native FreeBSD executable. After applying the
kernel patch described in [the FreeBSD packaging guide](packaging/freebsd/README.md),
enter the Fedora userspace and install the build dependencies:

```bash
dnf install -y @development-tools cmake git ninja-build pkgconf lld \
  SDL3-devel SDL3_ttf-devel curl-devel openssl-devel \
  nlohmann-json-devel libyaml-devel libpng-devel libelf-devel \
  minizip-devel capstone-devel gtk4-devel libadwaita-devel \
  webkitgtk6.0-devel fontconfig-devel libglvnd-devel \
  libplacebo-devel utf8proc-devel vulkan-headers vulkan-loader-devel zlib-devel
git clone --recurse-submodules https://github.com/sdqfrmnsyh/mokted.git
cd mokted
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DMOCKTAIL_BINARY_NAME=mokted -DBUILD_TESTING=OFF
cmake --build build -j"$(nproc)"
```

The resulting `build/mokted` runs from the Fedora userspace. For a portable
artifact, build the AppImage on a supported Linux host and run it inside the
Linuxulator as described in the FreeBSD guide.

## How it works

```
Roblox APK -> signature and ABI checks -> Bionic + JNI -> SDL3 + Vulkan/OpenGL
```

The APK is checked before any native code is loaded. It is downloaded on first
launch and is not bundled with Mokted. The last working copy is kept in case an
update fails.

Screenshots:
![Roblox home](assets/screenshots/flatpak-home.png)
![Roblox gameplay](assets/screenshots/flatpak-gameplay-tower.png)
![Roblox experience](assets/screenshots/flatpak-gameplay-lobby.png)

## FFlag overrides

Put a JSON object in `$XDG_CONFIG_HOME/mocktail/fflags.json` (usually
`~/.config/mocktail/fflags.json`). Mokted applies it on the next launch.

```json
{
  "FFlagExample": "True",
  "DFIntExample": "120"
}
```

Note: `FIntDebugFRMQualityLevelOverride` and `FIntDebugForceMSAASamples` from
`fflags.json` take precedence over the values that `performance_policy.cc`
applies. If you set one, Mokted leaves it alone.

## Building

Linux `x86_64` is supported. Experimental FreeBSD 15.1 Linuxulator support has
been tested with an `x86_64` Fedora 44 userspace. On FreeBSD, Mokted runs inside
Linuxulator; it is not a native FreeBSD binary.

Building requires CMake 3.20+, Git, pkg-config, LLD, binutils, a C++17
compiler, SDL 3.4+, SDL3_ttf, Vulkan, EGL, libplacebo, fontconfig, libcurl,
OpenSSL, libelf, libyaml, libpng, minizip, Capstone 5, utf8proc, nlohmann/json,
GTK4, libadwaita 1.6+, and WebKitGTK 6.0.

### Arch Linux

```bash
sudo pacman -S --needed base-devel cmake git ninja pkgconf lld sdl3 sdl3_ttf \
  curl openssl nlohmann-json libyaml libpng libelf minizip capstone gtk4 \
  libadwaita webkitgtk-6.0 libutf8proc fontconfig libglvnd \
  libplacebo vulkan-headers vulkan-icd-loader zlib
```

### Ubuntu 26.04+

```bash
sudo apt update
sudo apt install build-essential cmake git ninja-build pkg-config lld \
  libsdl3-dev libsdl3-ttf-dev libcurl4-openssl-dev libssl-dev \
  nlohmann-json3-dev libyaml-dev libpng-dev libelf-dev libminizip-dev \
  libcapstone-dev libgtk-4-dev libadwaita-1-dev libwebkitgtk-6.0-dev \
  libutf8proc-dev libfontconfig1-dev libegl-dev libvulkan-dev \
  libplacebo-dev zlib1g-dev
```

### Fedora 44+

```bash
sudo dnf install gcc-c++ cmake git ninja-build pkgconf-pkg-config lld \
  SDL3-devel SDL3_ttf-devel libcurl-devel openssl-devel \
  nlohmann-json-devel libyaml-devel libpng-devel elfutils-libelf-devel \
  minizip-ng-compat-devel capstone-devel gtk4-devel libadwaita-devel \
  webkitgtk6.0-devel utf8proc-devel fontconfig-devel libglvnd-devel \
  vulkan-headers vulkan-loader-devel libplacebo-devel zlib-ng-compat-devel
```

### Build

```bash
git clone --recurse-submodules https://github.com/sdqfrmnsyh/mokted.git
cd mokted

# Excavator / low-end
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DMOCKTAIL_HOST_ARCH=bdver4 \
  -DMOCKTAIL_ENABLE_LTO=ON
ninja -C build -j2

# Generic x86-64 (drop -DMOCKTAIL_HOST_ARCH to let CMake pick native)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build -j2
```

`-j2` because LTO link on a 2C/2T part will thrash if you oversubscribe. Bump
to `-j3` or `-j4` on stronger hardware.

Run it:

```bash
./build/mocktail
```

To install for your user, with an app menu entry and Roblox link handling:

```bash
make install PREFIX=~/.local
```

Use `sudo make install` instead to install system-wide under `/usr`. To make
Roblox website links open the build tree copy without installing, run
`make register-url-handler`.

### Deb and RPM, via CPack

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DMOCKTAIL_HOST_ARCH=bdver4 \
  -DMOCKTAIL_ENABLE_LTO=ON \
  -DBUILD_TESTING=OFF \
  -DMOCKTAIL_PACKAGE_NAME=mokted
ninja -C build -j2
cd build && cpack -G DEB && cpack -G RPM
```

Outputs land in `build/`: `mokted_1.0.4_amd64.deb`,
`mokted-1.0.4-1.x86_64.rpm`. These are built against your host distro's shared
libraries, so they only run on that distro (or a close relative).

## Building distribution packages

Arch packages use the recipes in `packaging/arch/PKGBUILD` and
`packaging/aur/mokted/PKGBUILD`:

```bash
makepkg -Cfsri --noconfirm -p packaging/arch/PKGBUILD
makepkg -Cfsri --noconfirm -p packaging/aur/mokted/PKGBUILD
```

Alternatively, run `makepkg` from either recipe directory. The recipes first
use a local checkout when available and otherwise clone the matching GitHub
tag. Set `MOKTED_SOURCE_DIR=/path/to/mokted` to select a checkout explicitly.

For Debian and RPM packages, use CPack after configuring and compiling:

```bash
cmake -S . -B build-packages -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DMOCKTAIL_BINARY_NAME=mokted \
  -DMOCKTAIL_PACKAGE_NAME=mokted \
  -DBUILD_TESTING=OFF
cmake --build build-packages -j"$(nproc)"
cmake --build build-packages --target package
```

## Building Flatpak version

Install the Flatpak SDK and builder on Arch Linux:

```bash
sudo pacman -S --needed flatpak flatpak-builder
flatpak remote-add --user --if-not-exists flathub \
  https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install --user flathub org.gnome.Sdk//50 org.gnome.Platform//50
```

Build and install the current checkout:

```bash
./scripts/build_flatpak.sh \
  --build-dir build-flatpak \
  --manifest packaging/flatpak/io.github.sdqfrmnsyh.mokted.json \
  --jobs "$(nproc)"
flatpak run io.github.sdqfrmnsyh.mokted
```

To create a distributable bundle from the local repository:

```bash
flatpak-builder --user --install-deps-from=flathub --force-clean \
  build-flatpak packaging/flatpak/io.github.sdqfrmnsyh.mokted.json
flatpak build-bundle repo Mokted-x86_64.flatpak \
  io.github.sdqfrmnsyh.mokted stable
```

## Building AppImage

Install the AppImage build tools first. `patchelf` is required by the
relocation step:

```bash
sudo pacman -S --needed appstream appimagetool patchelf
```

Then build the standalone glibc AppImage:

```bash
MOCKTAIL_BUILD_JOBS="$(nproc)" \
  ./scripts/build_release.sh --libc glibc --mode standalone --clean
```

The output is written under `dist/` as
`Mokted-x86_64.AppImage` or a libc/mode-specific AppImage name.

## Known limitations

Mokted does not currently work with `hardened_malloc`. Using `hardened_malloc`
may cause Mokted to fail to start or crash during runtime. Run Mokted without
`hardened_malloc` enabled.

The binary is still called `mocktail` and the config directory is still
`~/.config/mocktail/`. That is deliberate: it keeps upstream fixes applying
cleanly. The desktop entry and package name are `mokted`, so `pacman -Qi
mokted` and the app menu both show the right thing.

## License

[Apache License 2.0](LICENSE). Third-party components keep their own licenses.

## Support

Support upstream, which does most of the heavy lifting:

- Nightcap: https://github.com/CoderDayton/nightcap
- Mocktail: https://github.com/komaruworld/mocktail
