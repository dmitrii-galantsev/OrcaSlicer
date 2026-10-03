# This fork

Branch `fork` of `git@github.com:dmitrii-galantsev/OrcaSlicer.git` (remote `mine`), kept as a short
stack on top of upstream `origin/main` and rebased onto it, never merged. It is built as a Flatpak on
lsttdev4 and runs on the laptop and on a Steam Deck.

## What it adds

- **Touch mode** (`touch_input` in Preferences → Control, on by default on a Steam Deck): − / +
  steppers on numeric settings, spin boxes and the move/rotate/scale, painting, cut and jump-to-layer
  values; an on-screen keyboard for text fields, search boxes and renames (a long press on a number opens
  it too); finger-sized toolbar, sliders, object list rows and plate icons; pinch zoom, two-finger pan,
  twist and long-press menu in the 3D view; an Erase toggle in the painting gizmos.
- **Align / Distribute** for selected objects or parts, in the right-click menu (ported from BambuStudio).
- **Circle contour-hole compensation** (`enable_circle_compensation`, ported from BambuStudio 2.0), using
  the coefficients the BBL filament profiles already carry.
- Reconnect the last used printer on startup; keep filament presets whose type matches the AMS tray.
- Flatpak build fixes: real git hash in the About box, appstreamcli shim for the Nix flatpak-builder,
  clang routed through ccache, `scripts/flatpak/dev_loop.sh`.

## Build and deploy

From the laptop:

| what | command |
|---|---|
| rebase on upstream, build a bundle on lsttdev4, install here and on the Deck | `my_update_orca_slicer --build --deck` |
| same, without rebasing | `my_update_orca_slicer --build --no-rebase --deck` |
| run the working tree on the Deck without a bundle (~15 s per change) | `my_update_orca_slicer --dev` |

On lsttdev4 directly: `./build_flatpak.sh --ccache -j $(nproc)`. A warm rebuild takes about 7 minutes,
a cold one about 21; packing the bundle is 3-4 of those minutes. `--dev` needs one finished bundle build
in this checkout first, because flatpak-builder keys its module cache on the generated manifest's path.

## When a build fails

| symptom | cause | fix |
|---|---|---|
| `appstreamcli compose failed`, `E: file-read-error` | the Nix flatpak-builder leaks `GDK_PIXBUF_MODULE_FILE` into the host appstreamcli | the shim in `build_flatpak.sh` handles it; rerun the logged command with `--print-report=full` to see more |
| build dies a few minutes in when started from an agent shell | the caller's process group was killed | `setsid nohup ./build_flatpak.sh ... &` |
| stale `rofiles-fuse` mount after a killed build | flatpak-builder leaves the mount and its lock | `fusermount -uz .flatpak-builder/rofiles/rofiles-*`, then delete the dir and its `-lock` |
