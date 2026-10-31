# furry-overlay

A tiny Windows tray app that pins an image to your desktop as a transparent, always-on-top, click-through overlay. It can show a local picture or fetch a random one from [e621](https://e621.net) every few minutes.

Written in C++ with the plain WinAPI, GDI+ and WinInet. No installer, no runtime dependencies.

## Features

- **Overlay window:** topmost, layered with per-pixel alpha (PNG transparency works), hidden from the taskbar and Alt+Tab.
- **Click-through lock:** mouse clicks pass through the image to whatever is below. Unlock it and drag the image with the left mouse button, the new position is saved.
- **Local image:** `image.png` next to the executable is shown at startup.
- **e621 auto-download:** fetches one random post at a time with your own tag filter, keeps it in memory (nothing is written to disk) and replaces the overlay. Only PNG and JPEG posts are used.
- **Tray menu:** scale (25-200%), maximum size presets, aspect-ratio fit, opacity (20-100%), download interval, lock toggle, reload and edit config.
- **Config file:** every change is saved to `config.json` next to the executable.

## Hotkeys

| Hotkey | Action |
|---|---|
| `Alt+T` | Lock / unlock click-through |
| `Alt+R` | Reload `config.json` |
| `Alt+N` | Download a new image now |

## Configuration

`config.json` is created the first time you change a setting or choose **Edit Config...** in the tray menu.

| Key | Default | Description |
|---|---|---|
| `x`, `y` | `5`, `5` | Overlay position in screen pixels (limited to ±10000) |
| `scale` | `1.0` | Image scale, 0.1 to 10 |
| `opacity` | `255` | 0 (invisible) to 255 (opaque) |
| `click_through` | `true` | Let mouse clicks pass through the overlay |
| `e621_enabled` | `true` | Download images automatically |
| `e621_tags` | `"rating:safe"` | Tag filter for e621 |
| `download_interval` | `30` | Seconds between downloads, at least 10 |
| `max_width`, `max_height` | `500`, `500` | Size limit after scaling, `0` means no limit |
| `aspect_fit` | `true` | Keep the aspect ratio when the size limit applies |

These are the defaults when no `config.json` exists. If a file exists but a key is missing, that key falls back to `x`/`y` 100, `e621_enabled` off, an interval of 300 seconds and no size limit.

Without `image.png` and `config.json`, nothing is shown until the first download finishes, which happens after one interval.

## Content note

e621 hosts adult content. The default filter is `rating:safe`, and `order:random` and `filetype:png` are added automatically unless your tags already contain `order:` or a file type. If you change the tags, what appears on your desktop is up to you.

## Build

1. Open `furry-overlay.sln` in Visual Studio 2022 (toolset v143).
2. Build the `x64` or `x86` configuration with the C++ latest language standard.
3. Put `image.png` next to the executable if you want a local picture.

GDI+, WinInet and Shell32 are linked through `#pragma comment`. JSON parsing uses the bundled [nlohmann/json](https://github.com/nlohmann/json) (MIT).
