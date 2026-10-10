# SDRstar architecture

SDRstar = **SDR++** (the waterfall UI you already like) + a **smart layer** that identifies signals for you,
decodes digital ones, and follows trunked radio systems across the RTL-SDR's tuning range, packaged as a normal
double-click macOS app.

This page records *what* was built, *why* each part was chosen over the alternatives, and *where the limits are*.

## 1. The decision, in one paragraph

Re-implementing an SDR front end, a GPU waterfall, an RTL-SDR driver layer, audio output, and a dozen decoders from
scratch would take months and would be untestable on a Mac I do not have. Instead SDRstar is built from parts that
are already proven on macOS:

| Job | Component | Why this one |
| --- | --- | --- |
| GUI, waterfall, VFO dragging, demodulators (WFM/NFM/AM/SSB/CW/RAW), recorder, bookmarks, scanner, RTL-SDR source, audio sinks | **SDR++ 1.3.0** (GPL-3.0), pinned to one commit | It *is* the interface you asked for; upstream CI already ships macOS Intel + Apple Silicon builds; it has a clean module API (`createVFO`, `iqFrontEnd.addVFO`, `sourceManager.tune`, waterfall draw/input hooks). |
| Digital voice + control-channel decoding and trunk logic: P25 Phase 1/2, DMR (Tier II/III, Connect Plus, Capacity Plus, XPT), NXDN, dPMR, D-STAR, YSF, M17, EDACS, ProVoice | **DSD-neo** (GPL-3.0), run as a child process | Actively maintained successor of DSD-FME; ships a JSON telemetry/control API, accepts PCM on stdin, has macOS release builds, and a large regression suite of real captures. |
| IMBE/AMBE vocoder used by DSD-neo | **mbelib-neo** | Required by DSD-neo (see patent note in `THIRD_PARTY.md`). |
| Pagers (POCSAG/FLEX), AFSK/APRS, DTMF, EAS | **multimon-ng** | Standard tool. |
| 433/315/868/915 MHz ISM sensors, TPMS, weather stations | **rtl_433** | Standard tool, 200+ device decoders. |
| USB driver for the RTL-SDR Blog V4 | **librtlsdr 2.0.3** (osmocom) | Native V4/V4L support (HF upconverter handling, bias-tee GPIO). |
| Everything "smart": signal finder, classifier, popup, trunk-follow planner, status explanations | **`sdrstar_core` + `sdrstar_smart` module** (this repo) | The part that does not exist anywhere else. |

SDRstar changes SDR++ in exactly two ways: a pinned set of small **patches** (`patches/sdrpp/`, currently the
RTL-SDR bias-tee safety lock) and one added **module** (`modules/sdrstar_smart`). `scripts/prepare_sdrpp.sh` fetches
SDR++ at the pinned SHA, applies the patches and drops the module in. Nothing is forked or vendored.

## 2. Process layout

```
SDRstar.app
 └─ Contents/
     ├─ MacOS/sdrstar                 SDR++ main program (GUI, DSP graph)
     ├─ Frameworks/*.dylib            libsdrpp_core, librtlsdr, libusb, fftw, volk, portaudio, glfw, zstd ...
     ├─ Plugins/*.dylib               SDR++ modules + sdrstar_smart.dylib
     └─ Resources/helpers/            dsd-neo, multimon-ng, rtl_433 (+ their dylibs)  -- child processes
```

```
 RTL-SDR V4 ──USB──▶ SDR++ source ──▶ IQ front end ──┬─▶ FFT ──▶ waterfall ◀── overlay + popup  (sdrstar_smart)
  (librtlsdr)                                         ├─▶ user's VFO ──▶ Radio module ──▶ audio (analog listening)
                                                      └─▶ hidden channels (iqFrontEnd.addVFO)  ◀── sdrstar_smart
                                                              │  FM discriminator, 48 kHz, s16
                                                              ▼
                                      ┌──────────── child processes (loopback only) ─────────────┐
                                      │ dsd-neo   stdin=PCM  stdout=decoded voice  --api JSON     │
                                      │           rigctl client ◀──▶ RigctlBridge (retune requests)│
                                      │ multimon-ng / rtl_433                                      │
                                      └───────────────────────────────────────────────────────────┘
```

Why child processes: a crashing or hanging decoder can never take the GUI down; licences stay cleanly separate;
each tool can be upgraded independently; and stdin-EOF guarantees decoders die with the app.

*Hidden channels* are `iqFrontEnd.addVFO` digital down-converters: they exist inside the DSP graph without drawing an
extra VFO box on the waterfall, so probes and the trunk follower never disturb your own cursor.

## 3. How "smart detection" works

Four layers, cheapest first, each one only runs when the previous one found something worth looking at:

1. **Spectrum scan (every frame, tiny CPU).** Robust noise-floor estimate, peak/occupied-bandwidth detection with
   hysteresis, per-signal tracking (how long, how bursty). Plus a **band-plan hint** (87.5-108 MHz is broadcast FM,
   118-137 MHz is AM airband, 851-869 MHz is US public-safety trunking ...). Produces labels drawn on the waterfall.
2. **Channel fingerprint (when the cursor rests on a signal, ~1 s).** A hidden 48 kHz channel is opened on the
   signal. From its IQ we measure bandwidth, FM-discriminator level histogram (2 vs 4 deviation levels), and symbol
   rate (cyclostationary line). That separates analog voice, AM, 2-level FSK data, 4-level digital voice, and
   TDMA-style burst signals and narrows the candidate protocols.
3. **Protocol confirmation (a few seconds).** DSD-neo is started with the *specific* preset(s) the fingerprint
   suggests (not its slow general auto-hunt, which was measured to flip between false protocols), and its JSON
   telemetry is read: decoded frames, NAC/colour code, WACN/system/site, control-channel FEC statistics.
4. **Evidence thresholds.** A protocol is only *claimed* once enough FEC-verified frames were decoded; before that the
   popup says "probing ...". This is deliberate: the measured first guess of DSD-neo's auto mode was wrong for D-STAR,
   dPMR, and CQPSK voice fixtures.

*Trunked* is declared when a P25 control channel's FEC-verified TSBK count crosses a threshold (and it announces
its own frequency/site), or when DMR/NXDN/EDACS control-channel messages are decoded.

## 4. How trunk following works (and where the limits are)

The decoder (DSD-neo) owns the trunking *state machine* (grants, hang time, return to control channel) — it is the
most battle-tested part and handles P25 Phase 1/2, DMR Tier III / Connect Plus / Capacity Plus, NXDN and EDACS.
SDRstar owns the *radio*:

1. User drags the cursor onto a control channel; the popup says "P25 control channel ... **Follow trunked system**".
2. SDRstar opens a hidden channel on it, starts DSD-neo with `-T`, and starts a loopback **rigctl bridge**.
3. When a call is granted, DSD-neo sends `F <voice-frequency>` over rigctl. The bridge hands that to the
   **window planner** (`core/src/window_planner.cpp`, pure logic, unit-tested):
   * voice channel already inside the capture window  -> just move the hidden channel (instant, no hardware retune);
   * not inside, but control + voice fit together in ~1.9 MHz  -> retune the dongle *once* so both stay visible;
   * too far apart for one RTL-SDR (~2.4 MS/s, ~80 % usable)  -> **hop**: retune to the voice channel for the call
     (the control channel is lost meanwhile, and the UI says so), then return when the call ends; or, if the user
     disabled hopping, **refuse with a plain-English reason** ("voice channel is 5.2 MHz from the control channel;
     this receiver can only see 1.92 MHz at once");
   * outside what the hardware can tune (above 1.766 GHz, below 0.5 MHz)  -> refuse and say so.
4. Decoded voice comes back on DSD-neo's stdout and is played through SDR++'s normal audio sink (volume slider, device
   selection all work).

Honest limits: one RTL-SDR is one tuner, so *simultaneous* calls on far-apart frequencies cannot both be heard, and
control-channel traffic is missed during a hop. Encrypted calls are reported as encrypted and not decoded.
DMR/NXDN/EDACS systems do not broadcast their frequency plan the way P25 does: the user supplies a channel list
(CSV) for those (P25 plans are learned automatically from the control channel).

## 5. Hardware safety design

* RTL-SDR is a **receive-only** device: SDRstar contains no transmit path of any kind.
* **Bias tee** (4.5 V on the antenna port) is OFF at every launch, is never saved as ON, needs an explicit
  acknowledgement tick each time, and is switched off whenever the dongle is released (patch 0001).
* SDRstar never writes the dongle's EEPROM, flashes firmware, or touches any USB device other than via SDR++'s
  RTL-SDR source.
* Child decoders are built **without** their own RTL-SDR/Soapy backends, so they *cannot* grab or reconfigure the
  USB device; they only ever receive PCM through a pipe.
* All sockets bind to `127.0.0.1` only; the decoder API is token-protected with a random per-session token file
  (mode 0600); helper processes get a private working directory and environment.
* CPU: decoders are bounded (one process per active channel, PCM feeding drops data rather than blocking when a
  decoder falls behind) so a runaway cannot freeze the UI or overheat the machine by queueing unbounded work.
* The DSP never writes to disk unless the user starts a recording.

(Software cannot protect a receiver from physical overload — e.g. an antenna sitting next to a transmitter — so
`docs/SAFETY.md` also lists the real-world precautions.)

## 6. Testing strategy (no hardware needed)

* `core/tests` — unit tests for the planner, rigctl protocol, process management, telemetry parsing and the signal
  analysis code, plus **integration tests that run the real DSD-neo binary** on real recorded fixtures.
* Fixtures come from DSD-neo's own regression suite (2-s 48 kHz I/Q captures of P25 C4FM/CQPSK/Phase 2, DMR Tier III,
  NXDN, dPMR, D-STAR, YSF, M17, EDACS, ProVoice and analog NFM/AM, each already asserted by that project) plus
  public sample recordings found and *independently verified* (spectrum viewed, decoded by a reference decoder)
  before use (`docs/TESTING.md` lists each and what was checked).
* GUI testing: SDR++ is run under a virtual display with software OpenGL, fed a recorded wideband file through its
  File Source, and screenshotted.
* macOS: the GitHub Actions workflow builds the real `.app` for Apple Silicon and Intel, checks that no library
  outside the bundle is referenced, ad-hoc signs it, launches it, and produces the `.dmg`.

What could **not** be tested here and is flagged in the README: the live RTL-SDR hardware path on a real Mac (USB
streaming, tuner retune latency, audio device), because no hardware is available in the build environment.
