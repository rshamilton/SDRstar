# Safety: your Mac, your RTL-SDR, and what SDRstar can and cannot promise

You asked for "zero chance of hardware damage". Here is the straight answer.

* **Software side: SDRstar contains nothing that can command your radio or your Mac into harm.** The RTL-SDR is a
  receive-only device (there is no transmitter in it to misuse), SDRstar sends it only the ordinary tuning / gain /
  sample-rate commands SDR++ has always sent, and the one electrical control it exposes, the *bias tee*, is locked off
  by default. These statements are checked by an audit script that runs on every build (below), and the process-safety
  parts are tested for real.
* **Physical side: no software can make a radio immune to what you connect to it.** A strong transmitter next to the
  antenna, a static discharge, or a DC-shorted antenna with the bias tee on can damage a receiver whatever program
  is running. Section 2 lists the precautions that matter.

## 1. What SDRstar does about it (and how that was checked)

| Protection | Detail | Checked by |
| --- | --- | --- |
| **Receive-only.** | SDRstar has no transmit code at all, and the RTL-SDR has no transmit hardware. | `scripts/safety_audit.sh` (no transmit / EEPROM / GPIO calls in SDRstar's code or in SDR++'s source modules). |
| **Bias tee is off at every start**, never saved as "on", needs an explicit tick ("I need Bias T") every session, shows an orange "Bias T is ON" warning, and is switched off again when the radio is released. | The bias tee puts 4.5 V DC on the antenna connector. It is harmless to an ordinary antenna but can damage a DC-shorted antenna or a device that does not expect power. Upstream SDR++ remembers the setting between runs; SDRstar patches that out (`patches/sdrpp/0001-rtl-sdr-bias-tee-safety.patch`). | Audit script (patch markers, setting never read back); the patched source tree is audited in CI. |
| **No firmware / EEPROM access.** | SDRstar never writes the dongle's EEPROM, flashes firmware or configures any USB device other than through SDR++'s ordinary RTL-SDR source. | Audit script. |
| **The decoders cannot touch the USB radio.** | DSD-neo, multimon-ng and rtl_433 are compiled **without** any RTL-SDR / SoapySDR / Airspy support. They only receive audio or samples through a pipe from SDRstar. | Audit script checks the build recipes; the macOS CI bundles exactly those builds. |
| **No shell, one door for programs.** | Helper programs are started from a fixed path inside the app (never searched for on `PATH`), with an argument list, never through a shell. Exactly one place in the code starts programs. | Audit script. |
| **A crash or quit never leaves decoders running.** | Each decoder's input is a socket held by the app; if the app disappears (even by force-quit or crash) the kernel closes it, the decoder sees end-of-input and exits. A decoder that ignores polite requests is force-killed on stop. | `core/tests/test_subprocess.cpp`: the decoder was gone 0.02 s after its host was killed with SIGKILL. Also measured on the full app with six real DSD-neo probe processes running: after a force-kill (SIGKILL) none was left after 0.4 s. |
| **A slow or stuck decoder cannot freeze the app or eat memory.** | Samples are handed to decoders through a bounded, non-blocking queue that drops the oldest data when the decoder cannot keep up. | `test_subprocess.cpp`: 64 MB offered to a child that never reads took 6 ms and memory stayed bounded. |
| **Network: loopback only.** | The only sockets SDRstar creates are on `127.0.0.1` (decoder control). The decoder's control port is protected by a random per-session token in a file readable only by you (mode 0600) that is deleted afterwards. SDRstar opens no outbound internet connections. SDR++'s own optional network features (rigctl server, SDR++ server, network sink) do nothing until *you* switch them on. | Audit script (no `INADDR_ANY`, no hostnames/URLs, token mode). |
| **Decoders live in a private folder.** | Each decoder gets its own working directory and home inside SDRstar's settings folder, so it cannot read or write your files. In the soak test the whole folder stayed at 36 KB. | Soak test. |
| **No privacy permissions.** | The app's Info.plist asks for no microphone, camera, location, contacts or photos access. | Audit script and the macOS bundle verifier. |
| **Nothing is written to disk unless you ask.** | Besides the settings file and the small decoder folder, SDRstar writes only when you start a recording. | Soak test; code review. |
| **Hostile or garbled data cannot crash it.** | Everything parsed from outside the process (decoder telemetry and logs, sensor lines, rigctl commands, radio samples) is exception-safe and size-limited, and fuzz-tested. A retune request is only ever passed on if it is a finite, positive frequency. | `core/tests/test_fuzz.cpp` (found and fixed a real crash). |
| **Memory-safe, thread-safe code.** | The core library is run under AddressSanitizer / UndefinedBehaviourSanitizer and ThreadSanitizer. This found and fixed two real bugs (a use-after-free on teardown that crashed on macOS, and a data race in the trunk follower). All core tests are now clean under both (the one real-time test that cannot meet its deadlines at sanitizer speed reported no sanitizer findings either). | Test runs recorded in `docs/TESTING.md`. |
| **Long-running stability.** | 6 minutes of continuous trunk following and then 15 follow/stop cycles on the real GUI: memory flat (about 244 MB), thread and file-descriptor counts back to baseline after every cycle, no decoder left behind. | Soak run, `docs/TESTING.md`. |

Run the audit yourself any time: `scripts/safety_audit.sh` (optionally with the path of a prepared SDR++ tree).

## 2. Physical precautions (the part software cannot do)

**For the RTL-SDR**

* **Never connect the antenna port to a transmitter**, a transmitter's feedline, or an antenna that is sharing a
  feedline or sitting right next to a transmitting antenna. Even a few watts a short distance away can be
  far above what the receiver's input tolerates. If you must receive near a transmitter, use proper attenuation or
  filtering, and keep the antennas apart.
* **Static and lightning.** Touch something grounded before handling the antenna or connector, especially in dry
  weather or with a long outdoor antenna. Unplug the antenna during storms, and use a lightning arrestor / grounding
  scheme for any outdoor antenna.
* **Bias tee: only if your antenna or LNA needs it.** Many wire, loop and magnetic antennas are DC-shorted; with the bias
  tee on they short its supply. Check the product's documentation first. Leave it off for ordinary antennas.
* **Heat and cables.** The dongle gets warm; give it air, keep it out of the sun, use a short good USB cable (long or
  poor cables cause dropouts that look like software bugs), and avoid unpowered hubs.
* **Strong-signal behaviour is not damage.** Ghost signals, a raised noise floor or "everything is everywhere" mean the
  front end is overloaded. Lower the gain or move the antenna; that is not harming the radio but it ruins reception.

**For your Mac**

* SDRstar is an ordinary user app: it needs no administrator password, installs no drivers or system extensions, and
  changes nothing outside its own settings folder. Removing it is dragging it to the Trash.
* A radio app keeps the CPU busy. If the fans run hard, lower the sample rate (Source panel) or the waterfall refresh
  rate (Display panel). Normal for continuous decoding on a laptop; not a fault.

## 3. What has *not* been tested

* **A real RTL-SDR on a real Mac.** The build environment had neither. The logic, the decoders on real recordings and
  the complete GUI were tested using recorded radio data instead of live hardware, and the macOS app is built and checked
  by GitHub's macOS machines, which have no radio and no display. What that leaves unverified is the live USB path:
  streaming, retune timing on the real dongle, and the Mac's audio device handling. All of that is SDR++'s existing,
  widely used code, but it has not been exercised in *this* assembled app.
* **Physical overload and electrical behaviour** of the RTL-SDR (see section 2).
* **Encrypted traffic** is not decoded, by design.

If anything ever looks wrong with the hardware (very hot dongle, burning smell, the Mac disconnecting USB devices),
unplug the dongle first and ask questions afterwards.
