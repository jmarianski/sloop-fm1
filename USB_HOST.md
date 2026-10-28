# USB MIDI host (SLOOP 2.9)

The FM-1's USB-C port can be the **host** of a class-compliant USB-MIDI controller or synth: GLO → **USB** →
**PORT** = **HOST**. This page is for developers; how to use it is in [GUIDE.md](GUIDE.md#a-usb-midi-controller-straight-into-the-fm-1-29).

The driver is Adrien Tibi's and Kerem Kilic's, from [Melodee](https://github.com/keremimo/melodee), as ported to
felucca-bsp in its [pull request #7](https://github.com/hugelton/felucca-bsp/pull/7) (commits `ea3d2e3`, `4f5d2b5`).
It is MPL-2.0 by its authors' permission (see [LICENSING.md](LICENSING.md#usb-midi-host-mpl-20)); the files keep
their MPL-2.0 headers. SLOOP's own integration around it is GPL-3.0-only as the rest.

## Files

| File | What |
| --- | --- |
| `firmware/src/usb_host.c`, `usb_host_desc.h` | the host (MPL-2.0): enumeration, one hub, the descriptor parser, MIDI IN and OUT on the 2 kHz poll |
| `firmware/hal/fm1_usb.h` | `fm1_usb_host_attach`, `fm1_usb_line`: the PHY in host mode, D+ / D- (MPL-2.0 additions) |
| `firmware/src/usb.c` | the hooks: `usb_poll` gives the SIE to the host while it is on, `usb_retry` and `usb_detach` know it; `midi_out_try`, `midi_out_room`, `midi_out_link`, `midi_enqueue`, `midi_usb_lost` |
| `firmware/src/main.c` | `usb_host_task(fm1_ms)` every main-loop pass, before `usb_retry` |
| `firmware/src/seq.c` | `midi_usb_lost`: the notes and pedals held from MIDI end |
| `firmware/src/ui_input.c`, `ui.c`, `params.c` | GLO → USB → PORT (asks: a second detent within 1.5 s), the host's messages on the top bar |
| `tests/usb_host_test.c` | the host against a model of the SIE, a keyboard, a hub and a USB stick (ASan / UBSan) |

`FELUCCA_USB_HOST` (felucca.c, default 1) builds it in; 0 leaves the firmware as 2.8.3 (no GLO → USB page). The
update loader never has it (`FELUCCA_LOADER` forces 0), nor does the recovery path (`recovery.c` never switches the
host on).

## How it fits

- **Never kept.** Every power-on is a device; PORT is not in a project, the settings or the editor's backups. An
  update, the web editor and USB RESCUE always find the FM-1 as before. The editor can read PORT but not set it
  (what the port is wins every frame).
- **MIDI in.** What the controller sends lands in SLOOP's MIDI ring on **cable 0**, as from a computer: it plays the
  tracks by their channels, and with SYNC = **USB** its clock leads. SysEx from the device is ignored (no update,
  no editor over the host port).
- **MIDI out.** What SLOOP sends on USB (the keys, SEQ, CLOCK OUT) goes to the device when it has an OUT endpoint:
  `midi_out_event` is `midi_out_try`, which takes the host's ring in host mode.
- **A device that goes** (unplugged, a fault, the mode switched): `midi_usb_lost`. seq.c then ends every note held
  from MIDI (`midi_in_lost`, the path of a full ring) and lets go of every sustain pedal. The TRS jack's notes end
  too: SLOOP does not keep who holds a note, only that MIDI does.
- **Messages** (on the top bar, briefly): *USB: HOST*, *USB: DEVICE*, *USB HOST: MIDI IN*, *USB HOST: NOT MIDI*,
  *USB HOST: LOW SPEED*, *USB HOST: MIDI LOST*.

## Limits

- **No power on the port.** The FM-1 gives no 5 V (VBUS) in host mode: the device needs a **powered hub** or an
  **OTG adapter that powers it** (a Y cable with a charger input).
- Full-speed USB-MIDI 1.0 devices, directly or behind one hub (up to 7 ports, the first MIDI device found is used).
  No low-speed devices, nested hubs, MIDI 2.0, SysEx from the device, nor a second device at once.
- Enumeration runs in the main loop and blocks it, up to seconds with a hub or a device that does not answer. The
  sound and the 2 kHz poll go on (interrupts); the screen and the keys wait.
- The host's MIDI out keeps a packet until it is acknowledged; a stall, 16 errors or 500 ms without one stops the
  session and enumerates again. Notes SLOOP sent to a synth that went are not ended on its return.

## Tests

`tests/run_tests.sh` runs `tests/usb_host_test.c` four ways: host, host with the USB audio input (as the FM-1 is
built), device only and loader. What it cannot test is the controller itself: the register sequences come from the
AC79 SDK's USB0 host code and need a bench.

To try on an FM-1 (not done for this change): a keyboard through a powered hub and through a powered OTG
adapter; a synth on the MIDI out; both at once; unplug and plug again while notes and the pedal are held; the
clock from the device with SYNC = USB; back to DEVICE, then the web editor, USB audio and an update; the audio
under heavy MIDI traffic (no clicks: `usb_poll` runs in the TIMER5 interrupt).
