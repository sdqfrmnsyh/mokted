<!-- Modified by vii from komaruworld/mocktail. See README "About this fork". -->
# FAQ

## How do I turn Discord RPC on (or off)?

RPC is **off by default** in this fork. That avoids socket polling and the
16 MB worker thread. It never signs in to Discord and never reads an account
token.

To turn it on, launch Mokted once to create the config, then close it. Open
`config.yaml`:

- Native / AppImage: `~/.config/mocktail/config.yaml`
- Flatpak: `~/.var/app/io.github.sdqfrmnsyh.mokted/config/mocktail/config.yaml`

For native installs, a custom `$XDG_CONFIG_HOME` replaces `~/.config`.

Set `enabled` to `true` under `integrations.discord_rpc`:

```yaml
integrations:
  discord_rpc:
    enabled: true
```

Save the file and restart Mokted. Set `enabled` back to `false` to turn RPC
off again. `show_place_name`, `show_elapsed_time` and `join.enabled` all
default to `false` as well; enable them individually if you want that
information shown.

## Can Discord count Mokted as Roblox play time or quests?

No. Discord only credits play time and quests to games its own scanner
detects, and Rich Presence never counts. On Linux the scanner only matches
games with a Linux entry in Discord's list. Roblox has none, so no process
name Mokted could use would be detected. Only Roblox or Discord can change
that.

## Where can I find logs for a bug report?

Mokted saves logs automatically. Reproduce the issue, close Mokted, and
attach `latest.log`:

- Native / AppImage: `~/.local/state/mocktail/logs/latest.log`
- Flatpak: `~/.var/app/io.github.sdqfrmnsyh.mokted/.local/state/mocktail/logs/latest.log`

For native installs, a custom `$XDG_STATE_HOME` replaces `~/.local/state`.

## Can I play in VR?

VR is experimental and lives on upstream's `vr` branch, which does not
include the Mokted changes. Install the
[build dependencies](README.md#building), then switch to that branch and
build:

```bash
git remote add upstream https://github.com/komaruworld/mocktail.git
git fetch upstream
git switch -c vr upstream/vr
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMOCKTAIL_ENABLE_VR=ON
cmake --build build -j4
```

Start WiVRn or SteamVR/ALVR, connect your headset, then run:

```bash
./build/mocktail -vr
```
