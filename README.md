# OBS-PlaylistDock

A compact VLC playlist dock for OBS Studio. Supports Windows x64 and macOS ARM64, with English, Simplified Chinese and Traditional Chinese interfaces.

- Control existing VLC sources, preserve their playlist order and save the full queue with OBS.
- Search by filename or `#number`; press Enter to play the first match and Ctrl/Cmd+F to search.
- Drag files into the list, reorder tracks, select multiple entries and remove them with Del.
- Aligned track numbers and cached file sizes in the list.
- Missing local files are marked when loading or refreshing; hover over a file size to see its folder in one line, with long paths shortened in the middle.
- Reveal a track in Explorer or Finder from the context menu; missing files open their containing folder.
- Retain selection and scroll position when switching VLC sources.
- Undo the last removal with Ctrl/Cmd+Z or locate the playing track from the context menu.
- Compact playback controls, click or drag seeking, elapsed/remaining time and track count at the top right.
- Search moves to a second row when the dock is narrow.
- Save stop-at-end, play-next or repeat mode separately for each VLC source; new sources default to stop.
- Automatically switch to a unique matching scene on track activation; use preview in Studio Mode.
- Restore the complete VLC playlist and original settings.
- Theme-aware icons and no routine action notifications.

[Download](https://github.com/iesv/OBS-PlaylistDock/releases/latest) the ZIP for your platform. Close OBS, extract it and run `install_OBS-PlaylistDock.bat` on Windows, or `bash install_OBS-PlaylistDock.sh` on macOS. The macOS installer removes the plugin quarantine attribute.

Requires OBS Studio 32.0.4 and VLC.

Author: [haliluya.net](https://haliluya.net). [GPL-2.0-or-later](LICENSE).
