# Felucca licensing

Felucca is free software. Its **code** is licensed under the GNU General Public License,
version 3 only (`GPL-3.0-only`, full text in `LICENSE`). Its **assets** are not part of
that licence: the icon atlas `assets/icons.png`, the panel image `docs/panel.jpg` and the drum sounds made by
`tools/gen_waves.py` (the Hügelton Sample Pack) are Copyright (C) 2026 Hügelton Instruments,
all rights reserved. Their licence terms will be published later. SLOOP's firmware does not contain the
Hügelton Sample Pack: its sampled drum kit is made of CC0 recordings (`assets/samples-cc0/KIT`), and
`gen_waves.py` only feeds the SLICE engine's demo loop, which SLOOP does not build.

Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments

## What is code (GPL-3.0-only)

Every file in this tree that carries an `SPDX-License-Identifier: GPL-3.0-only` header (a few source files
carry another: `firmware/src/fm6_core.c`, msfa, Apache-2.0, the USB MIDI host and the fble socket, MPL-2.0, all below; the
firmware built with them is GPL-3.0-only as a whole):

- the firmware: `firmware/` (app, HAL, update loader)
- the build script and tools: `build.sh`, `tools/`
- the web pages (installer, editor) and their tests: `web/` (not the Fukiai font, below)
- the host tests: `tests/`

You may use, study, change and share it under the GPL. If you distribute Felucca, or
firmware derived from it, you must also give your recipients its complete corresponding
source under the same licence. That includes devices that ship with modified Felucca
inside.

## USB MIDI host (MPL-2.0)

SLOOP 2.9's USB MIDI host is Adrien Tibi's (Recursive Field Instruments) and Kerem Kilic's (Ellic Studio,
keremimo), written for [Melodee](https://github.com/keremimo/melodee) and taken from felucca-bsp's pull request
[#7](https://github.com/hugelton/felucca-bsp/pull/7) (its commits `ea3d2e3` and `4f5d2b5`), where it is MPL-2.0:

- `firmware/src/usb_host.c`, `firmware/src/usb_host_desc.h`, `tests/usb_host_test.c`: MPL-2.0 (their headers);
- the host routines in `firmware/hal/fm1_usb.h` (`fm1_usb_host_attach`, `fm1_usb_line`): MPL-2.0, in a GPL-3.0-only file.

The full text is in `LICENSES/MPL-2.0.txt`. MPL-2.0 names the GPL as a secondary licence (section 3.3) and these
files are not marked incompatible with it, so a SLOOP firmware that contains them is GPL-3.0-only as a whole, while
the files themselves stay MPL-2.0: their changes are offered under MPL-2.0 too. SLOOP's integration around them
(`usb.c`, `main.c`, `seq.c`, the GLO → USB page) is GPL-3.0-only.

Melodee's own copies are GPL-3.0-only. The authors licensed their parts of the port under MPL-2.0, in their words:
Adrien Tibi in Melodee pull request [#75](https://github.com/keremimo/melodee/pull/75#issuecomment-6384184014)
("For the licensing, of course, feel free to do so :)", 2026-10-24, answering keremimo's request to licence his
part as MPL), and Kerem Kilic in felucca-bsp pull request #7's description ("I also license my contributions to
this port under MPL-2.0."). The Melodee commits the port was made from (at `e9eab97`): `0d705bf` (Adrien's
enumeration, hub handling, input, PHY setup, descriptor parser and SIE / bus tests), `198c8fd` (Adrien's input
toggle and retry fixes), `b4380f9` (Kerem's MIDI output, endpoint checks, ACK ownership and recovery tests),
`eb57309` (Kerem's disconnect debounce), `44a311a` (Kerem's idle probe and hub connection-change fixes); felucca-bsp's
`NOTICE` has their full hashes. The register sequences follow the public JieLi AC79 SDK; no SDK source is copied.

## Bluetooth MIDI socket (FELUCCA_FBLE, MPL-2.0)

The socket for fble, the separately installed Bluetooth MIDI module (`tools/build.py --fble`; not in the default
build), is Jacek Mariański's, MPL-2.0 as fble's own files are:

- `firmware/src/fble.h`, `firmware/src/fble_load.h` (the module's interface and loader check, copies of fm1-base's of
  fble's milestone M5 with SLOOP's RAM windows), `firmware/src/fble_host.c`, `firmware/src/fble_host.h`,
  `tests/fble_test.c`: MPL-2.0 (their headers). `fble_host.c`'s log and interrupt attach follow Charles Vestal's
  fm1-base `device/radio.c`, and its central (FIRST / CHOOSE / reconnect) follows ignis32's MIDIMON `app/ble.c`, both
  MPL-2.0;
- the hooks around it (`main.c`, `usb.c`, `audio.c`, `editor.c`, `params.c`, `ui_draw.c`, `ui_input.c`,
  `hal/fm1_guard.h`, `hal/fm1_flash.h`, `firmware/app_fble.ld.in`, `tools/build.py`): GPL-3.0-only, as the files.

As with the USB MIDI host, a firmware containing them is GPL-3.0-only as a whole. The module itself is not part of
this tree and is not linked into the firmware: it is written into the flash on its own and contains JieLi's
Bluetooth libraries (binary, Apache-2.0 SDK). The FBLE build is for now a private build (GPL-3.0 allows private
use); a distributable one needs the questions in fble's `docs/BLE_MODULE.md` (licence section) answered first.

## Additional permission (GPL-3.0 section 7)

As an additional permission under GPL-3.0 section 7, you may combine Felucca, or a work
based on it, with the Felucca Assets (above), and convey the combination.
This is allowed even though the Felucca Assets are not licensed under the GPL, provided
that:

- you follow the GPL for every part that is not a Felucca Asset; and
- you follow the terms published for the assets.

The Felucca Assets are data (wavetables, icons, sample data). They are not program
code. A firmware image built from the GPL sources with replacement assets, or with no
assets, is entirely governed by the GPL.

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Instrument and drum samples (Versilian Studios VSCO-2 CE, VCSL; Sonic Pi: SCRCH) | CC0 1.0 | `assets/samples-cc0/`, provenance in `ATTRIBUTION.txt` there |
| Terminus font 8x16: the FM-1's screen (Terminus Bold, ter-u16b, SLOOP 2.6), and the web editor's font (the regular ter-u16n; `tools/gen_webfont.py` makes it a TrueType font inlined in `web/editor.html`) | SIL OFL 1.1 | `assets/fonts/ter-u16b.bdf`, `assets/fonts/ter-u16n.bdf`, `assets/fonts/Terminus-LICENSE.txt` |
| Fukiai icon font (Hügelton Instruments), web editor only | MIT | `web/fukiai.ttf`, `web/FUKIAI-LICENSE.txt` |
| CrispyZebra by Leo Kuroshita (<https://github.com/hugelton/CrispyZebra>): the PHASE engine's waveforms are a C port of its oscillator | GPL-3.0 | `firmware/src/eng_phase.c` |
| klattsch by Tony Gies (<https://github.com/tgies/klattsch>): design reference for the VOICE (formant) engine; no code copied. Formant data from Klatt (1980) / Hillenbrand et al. (1995) | MIT (klattsch) | credit only |
| msfa by Google Inc. and Pascal Gauthier, from Dexed (<https://github.com/asb2m10/dexed>): the FM6 engine's synthesis (SLOOP 2.4), ported to integer C for Felucca 1.0 by Leo Kuroshita (Dexed itself is GPL-3.0; only msfa is used; the FM6 factory patches are Felucca's own) | Apache-2.0 | `firmware/src/fm6_core.c`, `LICENSES/Apache-2.0-msfa.txt` |
| DaisySP (Electrosmith, Emilie Gillet) and Rings' sympathetic strings (Emilie Gillet): the PHYS engine's models (SLOOP 2.5, from Felucca 1.0, ported to fixed point by Leo Kuroshita) | MIT | `firmware/src/phys_dsp.c`, `firmware/src/phys_symp.c` (the notice is in each file) |
| USB MIDI host by Adrien Tibi and Kerem Kilic, from Melodee (<https://github.com/keremimo/melodee>) by way of felucca-bsp pull request #7 (<https://github.com/hugelton/felucca-bsp/pull/7>): SLOOP 2.9's GLO → USB → PORT = HOST (above) | MPL-2.0 | `firmware/src/usb_host.c`, `usb_host_desc.h`, the host routines in `firmware/hal/fm1_usb.h`, `tests/usb_host_test.c`, `LICENSES/MPL-2.0.txt` |
| fble socket (Bluetooth MIDI module host, FELUCCA_FBLE builds only) by Jacek Mariański, after fm1-base by Charles Vestal (<https://github.com/charlesvestal/fm1-base>) and MIDIMON by ignis32 (<https://github.com/ignis32/midimonitor_fm1>) (above) | MPL-2.0 | `firmware/src/fble.h`, `fble_load.h`, `fble_host.c`, `fble_host.h`, `tests/fble_test.c` |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` are read from your SDK checkout at build time and placed in the package; no SDK files are in this tree | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

## Contributions

Contributions are welcome under GPL-3.0-only. By submitting one, you agree that it may be
combined with the Felucca Assets under the section 7 permission above.

## Trademarks

"Felucca" and "Hügelton Instruments" are names of Hügelton Instruments.

"M-VAVE" and "FM-1" are trademarks of their respective owners. Felucca is independent
firmware that runs on FM-1 hardware. It is not affiliated with, endorsed by or supported
by those owners.

## Radio

Felucca never enables the Bluetooth / Wi-Fi radio of the hardware in its default build. Only an FBLE build
(`tools/build.py --fble`) with a separately installed Bluetooth module, and GLO → BLE → RADIO set to ON by the
user, turns on the Bluetooth radio; the Wi-Fi radio is never enabled.
