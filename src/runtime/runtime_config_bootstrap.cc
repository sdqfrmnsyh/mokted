// Modified by vii from komaruworld/mocktail. See README "About this fork".
#include "runtime/runtime_config_bootstrap.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

constexpr std::string_view kDefaultRuntimeConfig =
    R"yaml(# Modified by vii from komaruworld/mocktail. See README "About this fork".
# Mocktail configuration.
# The file is created once with mode 0600. Mocktail never replaces your edits.
#
# This fork (Mokted) hardcodes ultra-low-end optimizations as defaults in the
# binary, so a plain `mokted` launch already applies them. The values written
# here ARE those defaults - edit a line only to override it, and delete the
# file to fall back to every default again.
#
# Two rules before you edit:
#   * An unknown key is a hard error. Only use the keys documented below.
#   * `device:` is mutually exclusive with `input.touch_enabled` and
#     `compatibility.desktop_playability`.
#
# Hardcoded (not configurable here) ultra-low-end optimizations:
#   * Telemetry, analytics and crash reporting: all disabled.
#   * Textures: lowest quality (DFIntTextureQualityOverride=1), mip bias 5
#     (~1/1024 size), 16 MiB texture budget, 1x1 UI textures forced.
#   * Lighting: voxel/forward only; Future Is Bright, GI, SSAO, SSR, shadows,
#     bloom, DoF, HDR, tone mapping, and every other post effect forced off.
#   * Culling: frustum, portal, distance, Hi-Z and occlusion queries all on.
#   * Physics: 2 substeps, 2 constraint iterations, throttle when paused,
#     hidden, or unfocused.
#   * Scheduler: thread min 0, async min 1, auto limit 1 (single worker on 2C/2T).
#   * GC: Luau incremental GC, StepMul 300, GoalRatio 300, GoalCore 200.
#   * Particles: 200 max, 8 emitters. Audio: 8-16 channels, large DSP buffer.
#   * Discord RPC, gamepad polling and the text overlay are all off.
# Override any of these per key with a JSON object in
# ~/.config/mocktail/fflags.json (see README "FFlag overrides").

# Integer: configuration schema version. Only version 1 is supported.
version: 1

# One-line device preset. Short aliases pc, mobile, and console are accepted.
# Presets:
#   pc-windows-11 - desktop Windows PC: no touch, mouse + keyboard (default).
#   mobile-pixel-7 - Pixel 7: touch, no mouse, no keyboard.
#   console-ps5    - PlayStation 5 profile.
# Use the detailed mapping below instead for a custom profile.
device: pc-windows-11

# Detailed alternative (replace the scalar device line above). Cannot be
# combined with `device:`, `input.touch_enabled`, or
# `compatibility.desktop_playability`.
# device:
#   type: mobile           # Required. One of: pc, mobile, console, or a raw string.
#   platform: Android      # String: Build.PLATFORM-style platform name.
#   name: Google Pixel 9 Pro
#   manufacturer: Google
#   model: Pixel 9 Pro
#   brand: google
#   code: komodo           # String: Build.DEVICE-style code name.
#   sku: pixel-9-pro
#   soc_model: Google Tensor G4
#   touch: true            # Boolean: report a touchscreen.
#   mouse: false           # Boolean: report a mouse.
#   keyboard: false        # Boolean: report a hardware keyboard.

runtime:
  # Boolean (default: false): start without a visible SDL window.
  headless: false
  # String (optional): absolute path to a specific x86_64 libroblox.so instead
  # of the active verified payload. Leave unset to use the verified payload.
  # roblox_library: /absolute/path/to/libroblox.so

appearance:
  # String (default: roblox): window/theme source. `roblox` follows the theme
  # saved in the Roblox account. Supported overrides: dark, light, or system
  # (follow the desktop color scheme).
  theme: roblox

graphics:
  # String (default: direct-vulkan): graphics backend.
  #   direct-vulkan - Vulkan via the bundled loader. Default; fastest on AMD and
  #                   Intel with a working Vulkan driver.
  #   opengl        - OpenGL ES 3.0+ via the system GL driver.
  #   system        - Let the client pick (opengl family on this host).
  #   angle-vulkan  - ANGLE over Vulkan; compatibility fallback.
  # Use opengl or angle-vulkan only when Vulkan fails to start.
  backend: direct-vulkan
  # String or integer (default: -1): frame-rate cap override.
  #   -1        - leave Roblox's own Basic Settings cap in charge (default).
  #   display   - cap at the display refresh rate.
  #   unlimited - raise the cap to the scheduler maximum.
  #   <integer> - a fixed FPS cap, forwarded to DFIntTaskSchedulerTargetFps.
  # The fork sets no cap by default; the game runs at its natural rate.
  # frame_rate_limit: -1
  # String (default: auto): presentation synchronization.
  #   auto - lowest-latency synchronized present mode (mailbox or
  #          fifo-latest-ready) unless frame_rate_limit is unlimited.
  #   on   - same as auto, but never switches to an unthrottled present mode.
  #   off  - present immediately with no vsync; may tear, costs bandwidth.
  # vsync: auto
  # String (default: auto): ETC2 texture emulation.
  #   auto   - detect the GPU and emulate ETC2 only when it cannot decode
  #            natively.
  #   native - never emulate; use only if detection wrongly emulates on a GPU
  #            that does support ETC2.
  #   off    - disable ETC2 emulation entirely (saves CPU/GPU on GCN 1.3-era
  #            integrated GPUs).
  # etc2_emulation: auto

performance:
  # Boolean (default: true): size Roblox scheduler/render queues from every
  # physical CPU core. A place's Lua/main thread can still remain serial.
  multithreaded_rendering: true
  # String (default: latency): worker-pool sizing.
  #   latency    - keep Roblox-managed pool sizes and coalesce midphase work.
  #                Best on 2C/2T parts; the fork caps it at 1 worker.
  #   throughput - expose every physical core to the worker pools.
  #   auto       - let Mokted choose (latency-like on the low-end path).
  physics_worker_mode: latency
  # Integer MiB (default: 3072): hard RAM cap for the whole process tree.
  #   0 disables the cap. Under cgroup v2 the cap also disables swap; elsewhere
  #   an RSS+swap watchdog enforces it. 3072 is tuned for 7-8 GiB systems whose
  #   integrated GPU shares system RAM.
  memory_limit_mb: 3072
  # String (default: on): Feral GameMode request.
  #   on   - request a GameMode session and keep it, core pinning included.
  #   auto - request it, but release it if GameMode narrows the CPU affinity
  #          (useful on hybrid big.LITTLE CPUs; not needed on 2C/2T).
  #   off  - never contact GameMode.
  gamemode: on

audio:
  # String (default: default): output device. `default` follows the host system
  # output. To pin it, copy an exact SDL device name printed during startup.
  output_device: default
  # String (default: default): input device name, an `id:<number>` printed
  # during startup, or `disabled`. IDs disambiguate duplicate names but may
  # change between boots; prefer the exact device name when it is unique.
  input_device: default

input:
  # Boolean (default: false): send mouse-look deltas in the host's own units.
  # They are otherwise scaled by the window pixel density divided by the
  # desktop display scale, which cancels to 1.0 unless fractional scaling makes
  # the two disagree. Turn this on when first-person aiming feels too slow or
  # too fast under fractional scaling. The pointer position is unaffected.
  raw_mouse: false
  # Boolean (optional): force the touchscreen capability on or off. Cannot be
  # combined with the `device:` preset above. Leave unset unless you use a
  # detailed device mapping.
  # touch_enabled: false

# compatibility:
#   # Boolean (optional): advertise the place as playable on this device class so
#   # the Roblox client does not hide desktop-only experiences. Cannot be
#   # combined with the `device:` preset above. Uncomment this whole block only
#   # if you use a detailed device mapping instead of `device:`.
#   desktop_playability: false

integrations:
  fleasion:
    # Boolean (default: false): trust Fleasion's CA without editing Roblox files.
    enabled: false
    # String (default: env): match Fleasion's routing mode. Supported: env, hosts.
    # proxy_mode: env
    # Integer (default: 58443): Fleasion's local HTTP proxy port in env mode.
    # proxy_port: 58443
    # Optional absolute path to Fleasion's public CA. Default:
    # $XDG_CONFIG_HOME/Fleasion/proxy_ca/ca.crt or
    # ~/.config/Fleasion/proxy_ca/ca.crt. Never select ca.key.
    # ca_certificate: /home/user/.config/Fleasion/proxy_ca/ca.crt
  discord_rpc:
    # Boolean (default: false): publish Mokted activity to Discord Desktop.
    # Off by default in this fork: no socket polling, no 16 MB worker thread.
    # This never signs in to Discord and never reads an account token.
    enabled: false
    # Boolean (default: false): show the current Roblox experience name.
    show_place_name: false
    # Boolean (default: false): show how long the current session has run.
    show_elapsed_time: false
    join:
      # Boolean (default: false): let friends open the current experience. When
      # Roblox provides a public server ID, the button targets that server.
      enabled: false
      # Boolean (default: false): never expose private or reserved joins.
      public_servers_only: false
      # button_label: Join Server
    # English defaults. Uncomment only the lines you want to customize. Text and
    # image fields accept {place_name}, {place_icon}, and {creator_name}. A
    # field that expands an empty placeholder is left out, and a "" title shows
    # the app name. `title`, `state`, and the images fields may be "" to hide
    # them.
    # application_id: 123456789012345678
    # text:
    #   browsing: Browsing experiences
    #   joining: Joining an experience
    #   playing: "Playing {place_name}"
    #   state: "by {creator_name}"
    #   unknown_place: Unknown experience
    #   title: Roblox
    # images:
    #   large: "{place_icon}"
    #   large_text: "{place_name}"
    #   small: roblox_small
    #   small_text: Roblox

window:
  # Integer (default: 1280): initial window width in logical desktop units.
  width: 1280
  # Integer (default: 720): initial window height in logical desktop units.
  height: 720
  # String (default: Roblox): window title.
  title: Roblox
  # Boolean (default: false): render at physical display-pixel density instead
  # of the logical desktop resolution. Enable only for sharper high-DPI output;
  # it costs fill rate and is not recommended on integrated graphics.
  high_dpi: false

display:
  # Boolean (default: false): enable exclusive fullscreen.
  #   true  - bypass the compositor (X11) and use a native display mode (for
  #           example 1024x768). May reduce input latency but can cause
  #           display-mode switching issues on some systems.
  #   false - borderless fullscreen (default, safer, better compatibility).
  exclusive_fullscreen: false

network:
  # Boolean (default: false): follow the host HTTP/SOCKS5 proxy and
  # automatically use its current port. Do not combine with a fixed proxy below.
  use_system_proxy: false
  # Optional fixed HTTP proxy. Uncomment both values together. The host must not
  # contain a scheme such as http://.
  # String (optional, default: disabled): proxy host name or IP address.
  # proxy_host: 127.0.0.1
  # Integer (optional, default: disabled): proxy TCP port from 1 to 65535.
  # proxy_port: 8080
  # Absolute path (optional): custom PEM CA bundle. Mocktail reads this file
  # directly and does not replace it during Roblox payload updates.
  # ca_bundle: /home/user/.config/mocktail/cacert.pem

updates:
  # Boolean (default: true): check the latest Roblox version, derive an exact
  # HostAbi profile, run two isolated canaries with the selected graphics
  # backend, and promote only on success. An existing current payload is
  # preserved when probation fails.
  automatic: true
  # String (default: apk-pure): direct x86_64 APK provider selection. Both
  # accepted values resolve the provider chain, which currently starts with
  # APKPure. Supported: auto, apk-pure.
  source: apk-pure
  # Boolean (default: false): reserved for desktop update integrations.
  # `mocktail_updater` never launches another process after changing the active
  # payload.
  launch_after_update: false
)yaml";

std::string ErrorMessage(std::string_view action, int error_number) {
  return std::string(action) + ": " + std::strerror(error_number);
}

bool WriteAll(int file, std::string_view contents, std::string* error) {
  const char* next = contents.data();
  std::size_t remaining = contents.size();
  while (remaining != 0) {
    const ssize_t written = write(file, next, remaining);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      *error = ErrorMessage("cannot write first-run configuration", errno);
      return false;
    }
    next += written;
    remaining -= static_cast<std::size_t>(written);
  }
  return true;
}

bool InspectExistingConfig(const std::filesystem::path& config_file,
                           bool* exists, std::string* error) {
  struct stat metadata = {};
  if (lstat(config_file.c_str(), &metadata) != 0) {
    if (errno == ENOENT) {
      *exists = false;
      return true;
    }
    *error = ErrorMessage("cannot inspect first-run configuration", errno);
    return false;
  }
  *exists = true;
  if (!S_ISREG(metadata.st_mode)) {
    *error = S_ISLNK(metadata.st_mode)
                 ? "refusing symlink runtime configuration"
                 : "runtime configuration must be a regular file";
    return false;
  }
  return true;
}

}  // namespace

std::string_view DefaultRuntimeConfigYaml() { return kDefaultRuntimeConfig; }

RuntimeConfigBootstrapResult EnsureRuntimeConfigFile(
    const std::filesystem::path& config_file) {
  RuntimeConfigBootstrapResult result;
  if (config_file.empty() || config_file.filename().empty()) {
    result.error = "first-run configuration path must name a file";
    return result;
  }

  bool exists = false;
  if (!InspectExistingConfig(config_file, &exists, &result.error)) {
    return result;
  }
  if (exists) {
    return result;
  }

  std::filesystem::path parent = config_file.parent_path();
  if (parent.empty()) {
    parent = ".";
  }
  std::error_code filesystem_error;
  const bool parent_created =
      std::filesystem::create_directories(parent, filesystem_error);
  if (filesystem_error) {
    result.error = "cannot create configuration directory " + parent.string() +
                   ": " + filesystem_error.message();
    return result;
  }
  if (parent_created) {
    const int parent_directory =
        open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (parent_directory < 0 || fchmod(parent_directory, S_IRWXU) != 0) {
      const int operation_error = errno;
      if (parent_directory >= 0) {
        (void)close(parent_directory);
      }
      result.error = ErrorMessage("cannot secure configuration directory",
                                  operation_error);
      return result;
    }
    if (close(parent_directory) != 0) {
      result.error =
          ErrorMessage("cannot close configuration directory", errno);
      return result;
    }
  }

  std::string temporary_pattern =
      (parent / ("." + config_file.filename().string() + ".tmp.XXXXXX"))
          .string();
  std::vector<char> temporary_path(temporary_pattern.begin(),
                                   temporary_pattern.end());
  temporary_path.push_back('\0');
  const int temporary_file = mkstemp(temporary_path.data());
  if (temporary_file < 0) {
    result.error = ErrorMessage("cannot create temporary configuration", errno);
    return result;
  }

  const auto discard_temporary = [&temporary_path]() {
    (void)unlink(temporary_path.data());
  };
  if (fcntl(temporary_file, F_SETFD, FD_CLOEXEC) != 0 ||
      fchmod(temporary_file, S_IRUSR | S_IWUSR) != 0) {
    const int operation_error = errno;
    (void)close(temporary_file);
    discard_temporary();
    result.error =
        ErrorMessage("cannot secure temporary configuration", operation_error);
    return result;
  }
  if (!WriteAll(temporary_file, kDefaultRuntimeConfig, &result.error)) {
    (void)close(temporary_file);
    discard_temporary();
    return result;
  }
  if (fsync(temporary_file) != 0) {
    const int operation_error = errno;
    (void)close(temporary_file);
    discard_temporary();
    result.error = ErrorMessage("cannot synchronize first-run configuration",
                                operation_error);
    return result;
  }
  if (close(temporary_file) != 0) {
    const int operation_error = errno;
    discard_temporary();
    result.error =
        ErrorMessage("cannot close first-run configuration", operation_error);
    return result;
  }

  if (link(temporary_path.data(), config_file.c_str()) != 0) {
    const int publish_error = errno;
    discard_temporary();
    if (publish_error == EEXIST) {
      bool raced_entry_exists = false;
      if (!InspectExistingConfig(config_file, &raced_entry_exists,
                                 &result.error)) {
        return result;
      }
      if (!raced_entry_exists) {
        result.error = "runtime configuration disappeared during creation";
      }
      return result;
    }
    result.error =
        ErrorMessage("cannot publish first-run configuration", publish_error);
    return result;
  }
  result.status = RuntimeConfigBootstrapStatus::kCreated;
  if (unlink(temporary_path.data()) != 0) {
    result.error = ErrorMessage(
        "configuration was created but its temporary link cannot be removed",
        errno);
  }
  return result;
}

}  // namespace runtime
}  // namespace mocktail
