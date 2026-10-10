# SDRstar user guide

SDRstar is SDR++ with a smart layer on top. Everything you know from SDR++ still works the same way: the waterfall,
dragging the cursor, the left-hand panels, the demodulators, the recorder. This guide covers what SDRstar adds.

## The 30-second tour

1. Plug in the RTL-SDR, open SDRstar, press **Play** (top left). Pick a frequency (type it in the big number at the top,
   or drag the waterfall).
2. **Identified signals get labels** on the waterfall: a small tag above each signal whose protocol was really decoded
   ("P25 Phase 1 control channel", "DMR channel" ...). A signal keeps its label until it really changes. If you would
   rather see every signal found, untick **Only label signals that have been identified** in the SDRstar panel: the
   tags then also state what was measured ("12.5 kHz, steady", "FM broadcast", "wide FM-size signal"), never guesses.
3. **Rest your cursor on a signal** (the box you drag around is your cursor; SDR++ calls it a VFO). After a moment a
   **card** appears next to it: what the signal is, what that band is normally used for, and buttons for the sensible
   next steps.
4. Steady narrow signals are **identified automatically in the background** (a small "Identifying 852.0125 MHz ..."
   chip). If a digital protocol is really decoded, the label changes to its name.
5. On a **trunked system's control channel** the card says so and offers **Follow trunked system**.

## The card

| What the card shows | What it means |
| --- | --- |
| **Title** (e.g. "P25 Phase 1 control channel (trunked system)") | What was identified, or the measured description if nothing was. |
| **Subtitle** | System details (NAC, WACN, System, Site, RFSS for P25; colour code for DMR ...) or the frequency, width and strength. |
| **Band: ...** | What that part of the radio spectrum is normally used for. Only a hint; nothing is decided from it alone. |
| **Buttons** | The next steps (below). **x** closes the card for that signal. |

Buttons you may see:

* **Listen as FM / Listen (wide FM) / Listen (AM)**: switches your normal SDR++ radio to that demodulator.
* **Identify digital signal / Try again / Re-check**: runs the decoder test now, even if the automatic test already
  ran or gave up. It checks the digital modes that could live on that band (about 5 seconds).
* **Decode and listen**: for a conventional (non-trunked) digital voice channel. Plays the decoded voice. The cursor snaps
  onto the channel and stays there while you listen; moving it off the channel stops the decoding.
* **Follow trunked system**: see the next section.
* **Center on signal**: your cursor is a little off the signal's true centre; this snaps it on.
* **Read pager messages / Decode APRS / Read weather alerts / Read sensors / Read aircraft (ADS-B)**: the text decoders
  (below). Offered where the band suggests them.

A digital protocol is only **claimed** after enough error-checked data has really been decoded. If nothing is
claimed the card says "No digital protocol recognised" and tells you what that could mean (analog, encrypted, too weak,
or a mode that is not supported).

**Between transmissions.** Pagers, sensors and APRS only transmit now and then, so there may be nothing on the air where
your cursor rests. In a band where those live, the card then says *"Nothing on the air here right now"* and still offers
the decoder button (for example **Read pager messages**): start it and leave the cursor there to catch the next one.

**A signal keeps its identity.** Once a signal has been identified it keeps that name. A later test that disagrees does
not change it on its own: the different answer has to be seen twice, a few seconds apart, before the label changes. If
you press **Re-check** yourself you get the new answer at once. An identified signal is quietly re-checked every ten
minutes, so a channel that really changes protocol is noticed.

## Following a trunked system

A trunked system (public safety, utilities, business radio) announces every call on one *control channel*, and the
conversation itself happens on a *voice channel* that changes from call to call. Following means: stay on the control
channel, and when a call is announced, go and listen to its voice channel.

1. Put the cursor on the control channel. After it is identified the card reads, for example, *"P25 Phase 1 control
   channel (trunked system). NAC 0xD6 . WACN 92065 . System D5 . Site 1."* with **Follow trunked system**.
2. Click it. The card becomes the following card. It always says what is happening right now:
   * *Waiting for calls on the control channel 851.1128 MHz* (and the system details);
   * *Following a call* (talkgroup, who is talking) while the voice plays through your speakers;
   * *Hopping to 860.7125 MHz: it is 4 MHz from the control channel, outside the 1.92 MHz window.* then
     *Back on the control channel ... (window moved)* when the call is over;
   * *Following paused: the receiver window was moved away from ...* if **you** retune the radio (type another
     frequency, drag the waterfall) so the channel being followed is no longer in view. Following resumes by itself
     when the channel is back in view, or press **Return to control channel** to put the window back;
   * *Cannot follow: ...* with the reason, when it can't (examples in the table below).
   Counters show how often the window moved, calls followed, hops and calls skipped. **Activity log** in the SDRstar
   side panel keeps the full history.
3. **Stop following** ends it, or just move the cursor off the channel (see the next paragraph).

**The cursor goes where the radio listens.** When following starts the cursor snaps onto the control channel, and when a
call is announced it moves onto that call's voice channel (and back when the call ends), so the cursor is always on what
you are hearing. **Clicking somewhere else on the waterfall means "stop listening to that"**: following stops. If you do
that during a call, SDRstar first goes back to the control channel you originally clicked (the receiver window as well, if
it had hopped away, and the cursor), and then stops.

### Why "the window" moves, and what "hop" means

An RTL-SDR can only see about **2.4 MHz at once** (1.92 MHz of that is usable). SDRstar uses the whole tuner range,
0.5 MHz to 1.766 GHz, by moving that window around. For each call:

| Situation | What SDRstar does | What you are told |
| --- | --- | --- |
| The voice channel is already inside the window | Moves a software channel to it. No hardware retune, nothing missed. | Nothing needed; the counter "Calls followed" goes up. |
| Control and voice channels both fit in one window, just not the current one | Retunes the dongle once so both stay in view. | "Window moves" goes up; the log says the window was moved. |
| The two are too far apart to see together | **Hop**: tunes to the voice channel for the call and returns after. While away, new calls on the control channel are **missed**. | A warning line: *Hopping to ... it is N MHz from the control channel, outside the 1.92 MHz window.* |
| Same, but you switched **Hop to out-of-range calls** off | Stays on the control channel and skips the call. | *Not following: ... is out of bandwidth (N MHz from the control channel).* plus the numbers. "Skipped" goes up. |
| Outside what the hardware can tune (below 0.5 MHz or above 1.766 GHz) | Skips it. | *Cannot follow: ... is outside the ... tuning range.* plus the range the hardware can tune. |
| You are playing a **recording** instead of a radio | Recordings cannot retune. | *Cannot follow: 853.1375 MHz is outside the recording tuning range. The recording can tune from ... to ...* |

Because it is a single tuner, **two simultaneous calls on far-apart frequencies cannot both be heard**, and
**encrypted calls are reported but not decoded**.

### DMR, NXDN and EDACS: a channel list lets it follow calls to other channels

A P25 control channel announces the frequency plan itself, so P25 needs nothing. DMR (Connect Plus, Capacity Plus,
Tier III), NXDN and EDACS do not announce where their voice channels are. Without a list SDRstar still plays the calls
that happen on the channel you are following; to follow calls onto the system's *other* channels you tell SDRstar which
channel number is which frequency. Open the **SDRstar** panel
(left side), type the path of a CSV file into **Channel list CSV**, and (optionally) a talkgroup list into
**Talkgroup list CSV**. The files use DSD-neo's format:

```
ChannelNumber(dec),frequency(Hz) (do not delete this line or won't import properly)
1,863093750
2,862093750
3,861093750
```

* The **first line is a header and is required** (any text). Then one `channel,frequency-in-Hz` pair per line. An
  optional third column is a name. No quotes, no commas inside fields. EDACS and Capacity Plus use `LSN` (logical slot
  number) in place of the channel number. You can get a ready-made list for your local system from public sites such
  as RadioReference, or copy and edit the examples in the DSD-neo repository (`examples/`).
* The talkgroup list is `id,mode,name` per line (again with a required header line): `A` allows a talkgroup, `B` or
  `DE` blocks it. Example: `1201,A,Fire Dispatch`.

## Text decoders

On bands where it makes sense, the card offers a text decoder; you can also pick one by hand in the SDRstar panel
(**Text decoders**: choose the type, rest the cursor on the signal, click **Decode at the cursor**).

| Decoder | What it reads | Notes |
| --- | --- | --- |
| Pager messages | POCSAG (512 / 1200 / 2400 baud) and FLEX pager messages | Pager traffic is often readable by anyone, so the privacy rules where you live matter. Corrupted messages show as garbage text; that is the radio link, not a bug. |
| APRS | AFSK 1200 packets (amateur radio position and weather reports, 144.390 MHz in North America) | |
| Weather alerts | EAS / SAME alert headers (NOAA weather radio) | Rare; nothing shows for long stretches. |
| Aircraft (ADS-B) | Aircraft broadcasts on 1090 MHz: callsign, altitude, speed, heading, climb rate and position, one line per aircraft (heard at least twice; dropped after 90 s of silence) | Pick it in the SDRstar panel and press **Listen on 1090 MHz**: it always listens at exactly 1090 MHz, wherever your cursor is, and moves the receiver window there if needed (and says so). Needs the default 2.4 MS/s sample rate and an antenna with a view of the sky; range is line of sight. |
| ISM sensors | 433 / 868 / 915 MHz devices: weather stations, tyre-pressure sensors, door sensors, utility meters ... (200+ types, via rtl_433) | Many sensors transmit only every 30-60 seconds, and send each message several times; repeats are shown once with a count, e.g. `(x3)`. |

Received messages appear on the card and, in full, in the **Messages** list in the SDRstar panel. The decoder listens
exactly where your cursor is, and **follows the cursor**: while a text decoder is on, moving the cursor to another
frequency moves the decoder there, and the messages already collected stay. The card says whether there is a signal under
the cursor ("Signal at the cursor: 17 dB above the noise") and offers **Center on signal** when the cursor is a little off
it (a pager channel several kHz off hears nothing). **Stop** ends it and leaves the cursor where it is. A text decoder
runs *instead of* an identification / follow job. If you move the receiver window away from the signal a decoder is
listening to, the decoder stops and says so.

## The SDRstar panel (left side)

| Control | Does |
| --- | --- |
| **Label signals on the waterfall** | Turns the little tags on and off. |
| **Only label signals that have been identified** | On (default): only signals whose protocol was decoded get a tag, and a tag stays the same until the signal really changes. Off: every signal found is tagged with what can be measured of it. |
| **Quietly identify steady digital signals** | The automatic background identification. Off = only when you click. Signals that are tested and turn out not to be digital are re-tested only rarely (the wait grows from 1.5 minutes to an hour). |
| **Sensitivity** | How far (in dB) above the noise a signal must rise to be labelled. Lower finds weaker signals but gives more false labels. |
| **Identify signal under the cursor** | Same as the card's Identify button. |
| **Status** and **Stop** | What the smart layer is doing now; Stop ends any job. |
| **Text decoders** | Manual decoder picker and the Messages list. |
| **Hop to out-of-range calls** | The hop option above. Also on the following card. |
| **Channel list / Talkgroup list CSV** | For DMR / NXDN / EDACS (above). |

## Sound

Analog listening uses SDR++'s normal path (volume, device and mute in the top bar and the **Sinks** panel). Decoded
digital voice (a followed trunked call, or **Decode and listen**) plays through a second audio stream named
**SDRstar Voice**, on the same output device. That second stream is only opened while you follow a trunked system or
decode a digital voice channel (and closed when you stop), and while it is open it is fed continuously, so it never
holds up your normal radio audio. Decoded voice is delayed by about a fifth of a second: it is queued briefly so a call
plays smoothly instead of stuttering. While following or decoding, your normal radio is muted so you do not hear digital
noise on top of the voice; stopping the job unmutes it.

## Settings you may want

* **Gain** (Source panel): too high = false signals and everything looks strong; too low = weak signals vanish. For
  trunking, start in the middle.
* **Sample rate** (Source panel): 2.4 MS/s is the default and gives the widest window. Lower rates use less CPU but
  shrink the window (a trunk follower can then follow fewer calls without hopping).
* **Bias tee**: leave it off unless your antenna or amplifier needs power from the radio. Turning it on needs a
  deliberate tick every session. See [SAFETY.md](SAFETY.md).

## Where things are stored

`~/Library/Application Support/SDRstar` holds the settings (including the SDRstar panel's) and a small `sdrstar_work`
folder the decoders use. Nothing is written elsewhere unless you start a recording.

## When something does not work

See the troubleshooting table in [INSTALL-MAC.md](../INSTALL-MAC.md#troubleshooting). If a card or the status line
tells you *why* something cannot be done, trust it: those messages are generated from the same numbers the planner used
to decide.
