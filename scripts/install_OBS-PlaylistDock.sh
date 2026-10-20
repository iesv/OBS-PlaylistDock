#!/bin/bash
set -euo pipefail

package_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
bundle="$package_dir/obs-playlistdock.plugin"
plugins="$HOME/Library/Application Support/obs-studio/plugins"
target="$plugins/obs-playlistdock.plugin"

if pgrep -x OBS >/dev/null; then
  echo 'Close OBS before installing.' >&2
  exit 1
fi
if [[ ! -f "$bundle/Contents/MacOS/obs-playlistdock" ]]; then
  echo 'Extract the complete ZIP before running this script.' >&2
  exit 1
fi
if [[ -L "$plugins" || -L "$target" ]]; then
  echo 'The plugin installation path must not be a symbolic link.' >&2
  exit 1
fi
mkdir -p "$plugins"
ditto --noqtn "$bundle" "$target"
if xattr -r "$target" | grep -q 'com.apple.quarantine'; then
  xattr -dr com.apple.quarantine "$target"
fi
echo "Installed: $target"
if [[ -t 0 ]]; then
  for remaining in 5 4 3 2 1; do
    printf '\rInstalled. Closing in %s seconds; press any key to exit. ' "$remaining"
    if read -rsn 1 -t 1; then break; fi
  done
  printf '\n'
fi
