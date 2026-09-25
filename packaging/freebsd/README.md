<!-- Modified by vii from komaruworld/mocktail. See the top-level README "About this fork". -->
# FreeBSD Linuxulator

FreeBSD Linuxulator support is experimental. I tested it on FreeBSD
15.1-RELEASE-p2 with a Fedora 44 x86_64 userspace.

FreeBSD returns the wrong filesystem type for `/proc`. Roblox treats this as a
broken install and disconnects with Error 304 after about a minute.
[`linuxulator.patch`](linuxulator.patch) makes Linuxulator return the same
value as Linux.

## Patch FreeBSD

Use a clean Git checkout that exactly matches the running FreeBSD kernel. The
tested source revision is `aadd58dddcbc78f4d5594827b46b5633552b15ce`. Run
the following commands as root.

```sh
fetch -o /root/linuxulator.patch https://raw.githubusercontent.com/sdqfrmnsyh/mokted/main/packaging/freebsd/linuxulator.patch
cd /usr/src
git apply /root/linuxulator.patch
make -j2 kernel-toolchain
make -j2 buildkernel KERNCONF=GENERIC MODULES_OVERRIDE=linux64
```

Install only the rebuilt module. Keep a backup and reboot instead of unloading
a live Linuxulator module.

```sh
cp -p /boot/kernel/linux64.ko /boot/kernel/linux64.ko.mocktail-backup
install -o root -g wheel -m 0444 /usr/obj/usr/src/amd64.amd64/sys/GENERIC/modules/usr/src/sys/modules/linux64/linux64.ko /boot/kernel/linux64.ko.mocktail-new
mv /boot/kernel/linux64.ko.mocktail-new /boot/kernel/linux64.ko
kldxref /boot/kernel
reboot
```

After reboot, verify the result from the Fedora userspace. It must print
`9fa0`.

```sh
chroot /compat/linux stat -f -c %t /proc
```

## Run Mokted

You can simply run the Mokted AppImage from the
[latest release](https://github.com/sdqfrmnsyh/mokted/releases/latest)
inside the Fedora userspace.

## Build Mokted in Linuxulator

Mokted is built as a Linux x86_64 binary inside the Fedora userspace. Do not
use the FreeBSD host compiler for this step. Install the dependencies and
clone the source with its submodules:

```sh
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

Run the result with `./build/mokted` from the Fedora userspace. An AppImage
from a Linux release is usually easier to distribute because it includes the
Mokted runtime libraries.

## Audio

Install the ALSA OSS plugin inside the Fedora userspace.

```sh
dnf install -y alsa-lib alsa-plugins-oss
```

On the FreeBSD host, use `cat /dev/sndstat` to find the correct audio device.
The number is system-specific: it may be `/dev/dsp0`, `/dev/dsp1`,
`/dev/dsp2`, or another device. Replace `/dev/dsp3` below with yours.

```sh
cat << 'EOF' > /etc/asound.conf
pcm.!default {
    type oss
    device /dev/dsp3
}
ctl.!default {
    type oss
    device /dev/dsp3
}
EOF
```

Launch Mokted with the ALSA audio driver.

```sh
SDL_AUDIO_DRIVER=alsa ./Mokted-x86_64.AppImage
```

Mokted cannot override this check because Roblox reads `/proc` directly. The
Linuxulator patch fixes the value before Roblox sees it.
