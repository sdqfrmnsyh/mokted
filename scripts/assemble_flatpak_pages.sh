#!/usr/bin/env bash
# Modified by vii from komaruworld/mocktail. See README "About this fork".
# Copyright 2026 Mocktail Project Authors
# SPDX-License-Identifier: Apache-2.0

set -Eeuo pipefail
umask 022

readonly REPOSITORY="${1:?Flatpak repository path is required}"
readonly BUNDLE="${2:?Flatpak bundle path is required}"
readonly PUBLIC_KEY="${3:?GPG public key path is required}"
readonly OUTPUT="${4:?Pages output path is required}"
readonly BASE_URL="https://sdqfrmnsyh.github.io/mokted"
readonly APP_ID="io.github.sdqfrmnsyh.mokted"
readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly PROJECT_DIR="$(dirname -- "${SCRIPT_DIR}")"

[[ -d "${REPOSITORY}" && ! -L "${REPOSITORY}" ]] || {
  printf 'Flatpak repository is missing or unsafe: %s\n' "${REPOSITORY}" >&2
  exit 1
}
[[ -f "${BUNDLE}" && ! -L "${BUNDLE}" ]] || {
  printf 'Flatpak bundle is missing or unsafe: %s\n' "${BUNDLE}" >&2
  exit 1
}
[[ -s "${PUBLIC_KEY}" && ! -L "${PUBLIC_KEY}" ]] || {
  printf 'Flatpak public key is missing or unsafe: %s\n' "${PUBLIC_KEY}" >&2
  exit 1
}
[[ ! -e "${OUTPUT}" ]] || {
  printf 'Pages output already exists: %s\n' "${OUTPUT}" >&2
  exit 1
}

mkdir -p -- "${OUTPUT}"
cp -a -- "${REPOSITORY}" "${OUTPUT}/repo"
install -m 0644 -- "${BUNDLE}" "${OUTPUT}/Mokted-x86_64.flatpak"
install -m 0644 -- "${PUBLIC_KEY}" "${OUTPUT}/mokted-flatpak.gpg"
install -m 0644 -- \
  "${PROJECT_DIR}/packaging/${APP_ID}.svg" \
  "${OUTPUT}/mokted.svg"
touch -- "${OUTPUT}/.nojekyll"

readonly GPG_KEY="$(base64 --wrap=0 "${PUBLIC_KEY}")"

cat >"${OUTPUT}/mokted.flatpakrepo" <<EOF
[Flatpak Repo]
Title=Mokted
Url=${BASE_URL}/repo/
Homepage=https://github.com/sdqfrmnsyh/mokted
Comment=Mokted releases
Description=Signed x86_64 release builds of Mokted
Icon=${BASE_URL}/mokted.svg
GPGKey=${GPG_KEY}
EOF

cat >"${OUTPUT}/mokted.flatpakref" <<EOF
[Flatpak Ref]
Title=Mokted
Name=${APP_ID}
Branch=stable
Url=${BASE_URL}/repo/
RuntimeRepo=https://dl.flathub.org/repo/flathub.flatpakrepo
Homepage=https://github.com/sdqfrmnsyh/mokted
Comment=Roblox on Linux, tuned for a real PC
Icon=${BASE_URL}/mokted.svg
GPGKey=${GPG_KEY}
IsRuntime=false
EOF

install -m 0644 -- "${PROJECT_DIR}/site/index.html" "${OUTPUT}/index.html"
install -m 0644 -- "${PROJECT_DIR}/site/styles.css" "${OUTPUT}/styles.css"
install -m 0644 -- "${PROJECT_DIR}/site/terms.html" "${OUTPUT}/terms.html"
install -m 0644 -- "${PROJECT_DIR}/site/privacy.html" "${OUTPUT}/privacy.html"
