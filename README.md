# LV2-haiku

Native port of the **LV2 plugin hosting stack** to **Haiku OS**.

This repo vendors the upstream drobilla suite pristine under `lv2stack/` — the
[LV2](https://lv2plug.in) specification 1.18.11, zix 0.8.1, serd 0.32.10, sord 0.16.22,
sratom 0.6.22, and lilv 0.28.0 — and builds it natively as static libraries with each
project's own unmodified meson build. On top of that it adds:

- **`hosts/lv2jackhost`** — a headless JACK host with a stdin parameter REPL, for running
  and validating LV2 plugins live on Haiku hardware.
- **`hosts/lv2guihost`** — a native Interface Kit test host for LV2 plugin GUIs.
- **`ns/ui-haiku.lv2`** — a small LV2 extension bundle defining the **HaikuUI** UI type:
  a plugin UI whose `LV2UI_Widget` is a native `BView*`, exactly parallel to the spec's
  CocoaUI (`NSView*`). Plugins built for Haiku can ship native Interface Kit editors;
  `ui:showInterface` (UI owns its own `BWindow`) is supported as well.
- **`plugins/hkamp.lv2`** — a simple stereo amplifier plugin (with a native BView UI) used
  as the milestone test plugin.

Plugins install to `/boot/home/config/non-packaged/add-ons/lv2/` (user) or
`/boot/system/non-packaged/add-ons/lv2/` (system); the default `LV2_PATH` covers both plus
`~/.lv2` and `/boot/system/add-ons/lv2`.

## Building (on Haiku)

```sh
make deps       # pkgman install -y meson ninja pkgconfig
make            # builds the stack into build/stage + hosts + hkamp.lv2
make install-specs install   # LV2 spec bundles + hkamp into ~/config/non-packaged/add-ons/lv2
make validate   # lv2ls / lv2info smoke checks
```

The stack also builds unmodified on Linux (useful as a portability check); the resulting
static libraries and headers land in `build/stage/{lib,include}` and the lilv tools
(`lv2ls`, `lv2info`, `lv2apply`) in `build/stage/bin`.

## License

ISC — see `LICENSE`. The vendored upstream projects under `lv2stack/` are ISC/0BSD by
David Robillard and contributors; see their `COPYING` files.
