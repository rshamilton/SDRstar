# SDRstar architecture

SDRstar = **SDR++** (the waterfall UI you already like) + a **smart layer** that labels signals, identifies digital
ones, decodes them and follows trunked radio systems across the RTL-SDR's tuning range, packaged as a normal
double-click macOS app.

This page records *what* was built, *why* each part was chosen over the alternatives, how the pieces fit, and *where
the limits are* (the measured ones, not hoped-for ones).

## 1. The decision, in one paragraph

Re-implementing an SDR front end, a GPU waterfall, an RTL-SDR driver layer, audio output and a dozen decoders from
scratch would take months and could not be tested on a Mac that was not available. Instead SDRstar is built from parts
that are already proven on macOS, and only the part that does not exist anywhere (the "smart" layer) is new:

| Job | Component | Why this one |
| --- | --- | --- |
| GUI, waterfall, VFO dragging, demodulators (WFM/NFM/AM/SSB/CW/RAW), recorder, bookmarks, scanner, RTL-SDR source, audio sinks | **SDR++ 1.3.0** (GPL-3.0), pinned to one commit | It *is* the interface you asked for; upstream already ships macOS Intel + Apple Silicon builds; it has a clean module API (`iqFrontEnd.addVFO`, source manager, waterfall draw/input hooks). |
| Digital voice and control-channel decoding, trunk logic: P25 Phase 1/2, DMR (Tier II/III, Connect Plus, Capacity Plus), NXDN, dPMR, D-STAR, YSF, M17, EDACS, ProVoice | **DSD-neo** (GPL-3.0), a child process | Actively maintained successor of DSD-FME; takes PCM on stdin, has a JSON telemetry API, a rigctl-based trunk-follow protocol, a large regression suite of real captures, and macOS support. |
| IMBE/AMBE vocoder used by DSD-neo | **mbelib-neo** | Required by DSD-neo (patent note in `THIRD_PARTY.md`). |
| Pagers (POCSAG / FLEX), APRS (AFSK1200), weather alerts (EAS) | **multimon-ng** 1.6.2 | Standard tool. |
| Aircraft (ADS-B, 1090 MHz) | **SDRstar's own decoder** (`core/src/adsb.cpp`) | Small, well-specified, testable against published vectors; no extra program to ship. |
| 433 / 868 / 915 MHz sensors, TPMS, weather stations, utility meters | **rtl_433** 25.12 | Standard tool, 200+ device decoders. |
| USB driver for the RTL-SDR Blog V4 | **librtlsdr 2.0.3** | Native V4 / V4L support (HF upconverter handling, bias-tee GPIO). |
| Everything "smart": signal finder, classifier, cards, trunk-follow planner, status explanations | **`sdrstar_core`** (pure C++ library, `core/`) and the **`sdrstar_smart`** SDR++ module (`modules/sdrstar_smart`) | The part that does not exist anywhere else. |

SDRstar changes SDR++ in exactly two ways: a small pinned set of **patches** (`patches/sdrpp/`) and one added
**module**. `scripts/prepare_sdrpp.sh` fetches SDR++ at the pinned commit, applies the patches and drops the module in.
Nothing is forked or vendored. All pins live in `versions.env`.

Patches: `0001` bias-tee safety lock; `0002` a "mute" hook so the radio can be silenced while decoded voice plays;
`0003` window title and credits; `0004` defaults (settings folder `~/Library/Application Support/SDRstar`, the SDRstar
panel, RTL-SDR as the default source, the "SDRstar Voice" audio stream).

## 2. Process layout and data flow

```
SDRstar.app
 └─ Contents/
     ├─ MacOS/sdrstar              SDR++ main program (GUI, DSP graph)
     ├─ Frameworks/*.dylib         libsdrpp_core, librtlsdr, libusb, fftw, volk, portaudio, glfw, zstd, ...
     ├─ Plugins/*.dylib            SDR++ modules + sdrstar_smart.dylib
     └─ Resources/helpers/         dsd-neo, multimon-ng, rtl_433 (+ their dylibs): child processes
```

```
 RTL-SDR V4 ─USB─▶ SDR++ source ─▶ IQ front end ─┬─▶ FFT ─▶ waterfall  ◀── labels + card   (sdrstar_smart draws on it)
  (librtlsdr)                                     ├─▶ your VFO ─▶ Radio module ─▶ audio (analog listening)
                                                  ├─▶ own raw-IQ FFT tap ─▶ SignalDetector       (sdrstar_smart)
                                                  └─▶ hidden channels (iqFrontEnd.addVFO)        (sdrstar_smart)
                                                        │ FM discriminator: 48 kHz s16 PCM  /  250 kS/s complex float
                                                        ▼
                               child processes, loopback only, private working directory each
                               ┌ dsd-neo      stdin = PCM, stdout = decoded voice, --api JSON, rigctl client ┐
                               │              ▲ rigctl "F <freq>" = retune request                            │
                               │   RigctlBridge ─▶ WindowPlanner ─▶ move channel / retune dongle / hop / refuse│
                               └ multimon-ng (22.05 kHz PCM)   rtl_433 (complex float IQ) ───────────────────┘
```

*Why child processes:* a crashing or hanging decoder can never take the GUI down; licences stay cleanly separated;
each tool can be upgraded independently; and end-of-input on stdin guarantees decoders die with the app.

*Hidden channels* are `iqFrontEnd.addVFO` digital down-converters: they live inside the DSP graph without drawing an
extra VFO box on the waterfall, so probes and the trunk follower never disturb your own cursor.

*The spectrum tap is our own.* SDR++'s FFT data is already zoomed for display, so SDRstar takes the raw IQ and runs
its own FFTs (several per snapshot, 8 snapshots a second) for detection.

The hardware is reached only through an abstraction (`RadioHost`: window description, retune, open/move/close a
channel). In the app it is implemented on SDR++ (`SdrppRadioHost`); in the tests by simulators that replay recordings.
That is what makes the whole trunk-following loop testable with the real decoders and no radio.

## 3. How "smart detection" works

Cheapest first; each layer only runs when the previous one found something worth looking at.

1. **Spectrum detector** (every snapshot, tiny CPU). Noise floor, runs of bins above floor + margin (default 10 dB),
   occupied bandwidth at -25 dB, power-weighted centre, tracking by absolute frequency (survives window moves), how
   steady or bursty each signal is, guards against the DC spike and the unusable band edges. It produces **labels that
   state facts only** ("12.5 kHz, steady", "wide FM-size signal"). Speculation ("could be a control channel") is kept in
   a separate note, never the label. A **band plan** (what each band is normally used for) adds hints.
2. **Candidate choice.** From the occupied width and band: 6.25 kHz-class signals try NXDN48 and dPMR; wider ones try P25
   Phase 1/2, DMR, NXDN96 and, depending on who uses the band, D-STAR / YSF / M17 (amateur-only) or EDACS / ProVoice
   (land-mobile-only).
3. **Parallel probe** (about 5 s, early exit on a confident claim). A hidden channel is opened on the signal; its
   FM-discriminator audio is fanned out to one DSD-neo process per candidate, each started with its *specific* preset.
4. **Evidence rules.** A protocol is **claimed** only when enough *error-checked* output was produced (for example P25:
   at least 3 sync + a NAC; DMR: a colour code; D-STAR: a callsign). The rules were derived from a measured confusion
   matrix (every fixture against every decoder; `core/tests/test_probe_evidence.cpp`). The best claim wins; a signal is
   declared *trunked* when a P25 control channel's checked messages cross a threshold or DMR/NXDN/EDACS control traffic
   is decoded.

Two things that were tried and **rejected** because measurement said no: DSD-neo's own auto-detect mode (it guessed
wrong for D-STAR, dPMR and CQPSK voice) and signal-shape tricks (spectral flatness, baud-rate lines) to tell digital from
analog (they do not separate them on real recordings). That is why SDRstar tests specific decoders and trusts only
decoded evidence.

Background identification is polite: one job at a time, only steady narrow signals, skip a weak signal sitting next to a
much stronger one (probably its splatter), and signals that never turn out to be digital are re-tested ever more rarely
(1.5 min, 4.5 min, 13 min, then hourly).

## 4. How trunk following works (and where the limits are)

The decoder (DSD-neo) owns the trunking *state machine* (grants, hang time, return to the control channel). SDRstar owns
the *radio*:

1. The user rests the cursor on a control channel; the card says "P25 control channel ... **Follow trunked system**".
2. SDRstar opens a hidden channel on it, starts DSD-neo in trunking mode, and starts a **loopback rigctl bridge**.
3. When a call is granted, DSD-neo sends `F <voice-frequency>` over rigctl. The bridge hands that to the **window
   planner** (`core/src/window_planner.cpp`, pure logic):
   * voice channel already inside the capture window: just move the hidden channel (instant, nothing missed);
   * not inside, but control + voice fit together in about 1.9 MHz: retune the dongle **once** so both stay visible;
   * too far apart for one RTL-SDR (about 2.4 MS/s, 80 % of it usable): **hop**: retune to the voice channel for the call
     (the control channel is lost meanwhile, and the UI says so), then return when the call ends; or, if hopping is off,
     **refuse with a plain-English reason** ("... is out of bandwidth (N MHz from the control channel)");
   * outside what the hardware can tune (below 0.5 MHz or above 1.766 GHz): refuse and say so;
   * with a recording as the source (cannot retune): say what range the recording covers.
4. Decoded voice comes back on DSD-neo's stdout and plays through an SDR++ audio stream ("SDRstar Voice"), so volume
   and output device work as usual; the radio is muted meanwhile.

A retune is budgeted at about a quarter of a second (not yet measured on real hardware); the channel is muted across the gap so no junk reaches the decoder.

**Limits.** One RTL-SDR is one tuner, so simultaneous calls on far-apart frequencies cannot both be heard, and control
traffic is missed during a hop. Encrypted calls are reported, not decoded. P25 announces its frequency plan; DMR, NXDN and
EDACS need a channel list (CSV) from the user. NXDN96 and dPMR decode weakly through the audio path.

## 5. Text decoders

`HelperDecoder` (`core/src/helper_decoder.cpp`) runs a text-output tool on a hidden channel; each tool is a table entry
(`helperSpecs()`): pagers, APRS and EAS use multimon-ng fed 22.05 kHz audio (48 kHz discriminator output through a
polyphase resampler); ISM sensors use rtl_433 fed a 250 kS/s complex-float channel (`-r cf32:-`). Repeated copies of a
message are collapsed into one line with a count. Each helper runs in a private working directory with its own HOME.
ADS-B is the one decoder written in SDRstar itself (`core/src/adsb.cpp`): a Mode S pulse-position demodulator with CRC-24
verification and single-bit repair, plus the message decoder (callsign, altitude, CPR position, velocity); it plugs into the
same interface as a "native" helper and shows its aircraft table instead of text lines. It was written rather than bundled
because the only small standalone decoder is unmaintained, and the format is simple enough to verify against published
reference messages and a real recording.
Adding a decoder is a new entry plus (if needed) a card button.

## 6. Safety and testing

Both have their own pages: [SAFETY.md](SAFETY.md) (every protection, how it was checked, the physical precautions
software cannot provide) and [TESTING.md](TESTING.md) (what was tested with what, the bugs that testing found, the
measured weak spots, and what could not be tested here).

Short version of the open item: the live RTL-SDR path on a real Mac (USB streaming, real retune latency, audio device,
the GUI's OpenGL window on a real display) has not been exercised, because the build environment had no radio and no
Mac. Everything around it has.
