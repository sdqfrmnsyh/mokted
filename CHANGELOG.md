# Mokted Changelog

All important changes to the Mokted project will be documented in this dossier.

## [1.0.4] - 2026-09-24

### Custom Performance & Optimization (Low-End Hardware)
- **Build Tuned**: Added custom compilation optimization for AMD Excavator architecture (`-march=bdver4 -mtune=bdver4 -O3 -flto=auto -fno-plt -fno-semantic-interposition-fomit-frame-pointer`).
- **Engine Pump**: Removes *busy-wait* on CPUs without Intel WAITPKG; by default uses *fallback* `kSleep` so that *thread render* doesn't overload *thread hardware*.
- **Input Pump**: Reduces SDL's *polling input rate* from 240 Hz to 120 Hz to free up CPU time on each frame without sacrificing mouse responsiveness.
- **Task Scheduler**: Limits Roblox's *scheduler pool* to a maximum of `physical_cores - 1` to prevent heavy *context-switch churn* on 2C/2T processors.
- **Text Overlay**: Disables *libplacebo text overlay compositing* by default to save time *dispatch* GPU (can be reactivated with `MOCKTAIL_TEXT_OVERLAY=1`).
- **Discord RPC**: Disables *Discord Rich Presence* by default to prevent *socket polling* and eliminates *worker thread* 16 MB (can be activated with `MOCKTAIL_DISCORD_RPC_ENABLED=1`).
- **Gamepad**: Disables *joystick*/SDL gamepad enumeration by default; polling is only done if `MOCKTAIL_GAMEPAD=1` (keyboard and mouse are not affected).

### Identity & Configuration Changes
- Change the application identity to Mokted (is an independent *fork* of komaruworld/mocktail and CoderDayton/nightcap).
- Replaces desktop entry, App ID (`io.github.sdqfrmnsyh.mokted`), icons, metadata, sites, workflows, scripts, and tests to Mokted identity.
- Maintains the configuration path in `~/.config/mocktail/` and the default program name `mocktail` so that the improvements (*fixes*) of *upstream* can still be applied smoothly.
- Maintains *upstream* attribution and Apache-2.0 license terms.

### Packaging & Distribution
- Added *build* support for distribution packages `.deb` and `.rpm` via CPack.
- Fixed Arch Linux and AUR recipes so they don't depend on local paths, and supports the local fallback variable `MOKTED_SOURCE_DIR`.
- Added Flatpak build with ID `io.github.sdqfrmnsyh.mokted` as well as script to generate *bundle* (`Mokted-x86_64.flatpak`).
- Added standalone AppImage creation script (`Mokted-x86_64.AppImage`) with glibc base.
- Added Nix flake configuration for execution and *development shell* (`x86_64-linux`).
- Added support and build instructions for FreeBSD Linuxulator (using *userspace* Fedora x86_64).
- Align *launcher*, *updater*, *smoke test*, and *release script* with the Mokted executable identity.
- Create a `dist/all-in-one` directory containing Arch packets, AUR packets, Flatpak filesystems, Flatpak archives, binary Mokted, README, and checksums.

### Validation
- *Original packaging contract test* passed.
- *Flatpak packaging contract test* passed on manifest.
- Checksum of *all-in-one* artifacts successfully verified.
- Binary Mokted from the *bundle* successfully runs the `--help` using the *runtime libraries* of the *bundle*.
