# SDRstar recorded-signal test samples (catalogue only: the recordings themselves are NOT in this repository)

This folder holds the **catalogue** of the public recordings used to validate SDRstar (`MANIFEST.json`: source URL,
licence, SHA-256, format, sample rate, centre frequency, expected contents, verification narrative, status) and the tools
used to inspect and re-create them. The recordings add up to about 640 MB and several have no stated licence, so they are
not committed; download them from the source URLs in `MANIFEST.json` (the SHA-256 in each entry tells you whether you got
the right file). Small files that *are* licensed for redistribution live in `core/tests/data/signals`.

How the samples were chosen and checked (the point of the exercise was to avoid debugging a bad sample): every sample was
validated against its claimed content by an independent analysis (a spectrum plot that was actually looked at, plus a
reference decoder where one existed). 63 are `verified`, 8 `plausible`, 2 `rejected`. Things that bit us, so they do not
bite you: WAV headers can lie about the sample rate; 8-bit WAV is *unsigned*; float WAV can be un-normalised; SDR++'s File
Source reads every WAV as 16-bit, so convert 8-bit recordings first; HackRF captures contain a DC spike and spur combs;
rtl_433's `gfileNNN` captures assume 250 kS/s at 433.92 MHz.

The tools expect the data directory layout of the original collection (`<id>/<file>` next to `MANIFEST.json`); run them
from a folder where you placed the downloaded files that way. The text below is the original collection's README.

---

# SDRstar test samples

Public recordings (plus a few locally generated artefacts) that can be used as test inputs for SDRstar, each **validated against
its claimed content with an independent analysis** (spectrum PNG that was actually looked at, plus a reference decoder where one
could be built or was available).  Retrieved 2026-10-10.  Nothing that was downloaded was executed; it was only parsed as data.

* 63 samples **verified**, 8 **plausible** (spectrum consistent with the claim, no decoder confirmation), 2 **rejected** (see `_rejected/`), 639 MB kept.
* `MANIFEST.json` - machine-readable list of objects (source URL, licence, SHA-256, container/format/rate/centre/duration, expected contents, verification, caveats, status); `MANIFEST_META.json` holds the schema notes and the versions of the reference tools.
* `<id>/` - one directory per sample: the data file, `spectrum.png` (Welch PSD + waterfall; red dotted lines = expected carriers where annotated),
  `EXPECTED_CONTENTS.txt` (human-checkable description), `evidence.json` (measured stats), and decoder logs/expected outputs where they exist.
* `tools/inspect_iq.py` (loader + spectrum/stats), `tools/make_composite.py` (SYNTHETIC composites, channel extraction, REAL-data slicer),
  `tools/verify_manifest.py` (re-hash + load every entry), `tools/bin/` (locally built rtl_433, multimon-ng, dump1090 used for verification; see BUILD_NOTES.txt).
* `_index/` - catalogues I built while searching: headers of ~480 sigidwiki IQ WAV/ZIP files (rate/bits/size), the archive.org `SDR-IQ-*` items, the rtl_433_tests subset run.

## Quick start

```python
import sys; sys.path.insert(0, '/home/user/samples/tools')
from inspect_iq import load_entry, welch_psd
x, meta = load_entry('p25_dmr_800mhz_hackrf_sliceB_859p55_2400k')      # complex64, +-1.0 full scale
f, p_db = welch_psd(x, meta['sample_rate_hz'])
```
```
python3 -I tools/inspect_iq.py --id fm_hdradio_nrsc5_sample_1488k --plot /tmp/x.png --waterfall
python3 -I tools/inspect_iq.py --file some.cu8 --format cu8 --rate 2400000 --center 851e6
python3 -I tools/make_composite.py slice   --in <wide>.cu8 --in-rate 20e6 --in-center 862e6 --center 857.7e6 --rate 2.4e6 --out s.cu8   # REAL data, derived
python3 -I tools/make_composite.py extract --in <wide>.cu8 --in-rate 2.4e6 --offset=-990e3 --out-rate 48000 --out ch.iq --dsd-sidecar     # then: dsd-neo --iq-replay ch.iq.json
python3 -I tools/make_composite.py build   --out comp.cu8 --rate 2.4e6 --duration 3 --noise-dbfs -34 --add fixture.iq:300e3:30:loop   # SYNTHETIC
```

## The five most valuable samples for trunk-following / detection work

1. **`p25_dmr_800mhz_hackrf_sliceA_857p70_2400k`** - 2.4 MS/s cu8, 3.96 s: a real **P25 Phase 1 control channel** (NAC 0x451, WACN BEE00, SYSID 45C, site 21) at -990 kHz, a second CC of another system (NAC 0x346) at -540 kHz, P25 voice channels switching on/off (+10, +1010 kHz) and several unclassified carriers.  Decoder-confirmed.
2. **`p25_dmr_800mhz_hackrf_sliceB_859p55_2400k`** - same time window, other frequencies: three **P25 voice channels** (one appears at t~1.75 s, one at ~2.6 s) and a continuous **DMR TDMA carrier (Color Code 8, Tier III-style SLC_C_SYS_PARMS)** at +792 kHz.  Slices A, B, C come from the same 4 s of the same capture and are **time-aligned**, so they can emulate an RTL-SDR that hops between the CC and the voice channels it grants (the CC grants channels 0x0549/0x0559 = 859.4625/859.5625 MHz, which carry voice in slice B).
3. **`p25_dmr_800mhz_hackrf_sliceC_853p00_2400k`** - a second **P25 control channel (site 23, NAC 0x450)** + voice: gives a two-site system (site 21 + site 23) for site-hunting / CC-lost logic.
4. **`p25_dmr_800mhz_hackrf_20msps_2s`** - the 20 MS/s source excerpt (all of the above in one 20 MHz view, 2 CCs, 5+ voice channels, DMR carrier, LTE blocks, DC spike): ground truth for a wideband scanner / channelizer (resample or re-slice with `make_composite.py slice`).
5. **`iq_p25p1_c4fm_cc_bee0014c_24k`** (54 s continuous P25 C4FM control channel, NAC 0x140 WACN BEE00 SYSID 14C, 725 dsd-neo syncs) together with `audio_p25p2_cc_96k_mono` (P25 Phase 2 CC, WACN 92715 SYSID 1F8) - long, clean, channelized inputs for the CC state machine; `iq_p25p1_cqpsk_cc_920650d5_24k` for the CQPSK/simulcast variant.

## Coverage vs. the request (honest status)

| item | result |
|---|---|
| A1 FM broadcast slice | **Partial.** `fm_3stations_hackrf_slice_105p50_2400k`: 3 stereo WFM stations (pilot 16-28 dB) in 2.4 MS/s, but weak (HackRF, 8-bit, ~18 dB peak SNR). `fm_hdradio_nrsc5_sample_1488k`, `fm_hdradio_rtlsdrblog_samples1071_1488k`, `fm_cdr_106100_sdrsharp_900k`: one strong FM station each with hybrid-digital sidebands (+ an adjacent weak one).  No native RTL-SDR multi-station FM capture was found (sigidwiki/rtl-sdr.com/zenodo/archive.org tried). |
| A2 Airband AM / ACARS | **Not found as wideband.** The only candidate (HackRF 111.9 MHz capture) turned out to contain a spur comb, not ATC voice (`_rejected/airband_hackrf_119p80_spurs_only`).  Narrowband: `iq_acars_sigid_135k` (plausible), `iq_vdl2_sigid_48k` (plausible), `audio_acarsdec_test` (plausible; 4-ch 12.5 kHz audio), and dsd-neo's own `am_airband_real` fixture.  Use `make_composite.py build` with those if a wideband airband file is needed (synthetic). |
| A3 Trunked system slice | **Found** (best case): real 800 MHz P25 P1 CCs + voice + DMR carrier, see top-5.  Not found: MPT-1327 (only 'like-bits' audio on sigidwiki), Motorola Type II SmartNet, EDACS (dsd-neo has an audio-derived fixture), TETRA wideband (`iq_tetra_sigid_48k` is a single 25 kHz channel, plausible), NXDN/DMR Con+/Cap+ trunk captures, anything at 450 MHz / US 700 MHz. |
| A4 Conventional digital voice in wideband | DMR carrier (slice B) and P25 voice (A,B,C) only.  D-STAR/YSF/dPMR/NXDN exist only as the narrowband dsd-neo fixtures. |
| A5 ISM / rtl_433 | **36 rtl_433_tests captures** (rtl_433 output == upstream expected JSON for all) covering 433/868/915 MHz, cu8/cs8/cs16, 0.25-2.5 MS/s, plus the wideband `ism_433879_sdrtouch_1024k` (rtl_433 decodes Acurite-Tower + LaCrosse). LoRa file upstream is 0 bytes (rejected). 146 of the 148 capture+JSON pairs in my sparse-checked subset of rtl_433_tests reproduce exactly; the 2 mismatches are a device that needs a config file and an IDM/NetIDM double match. |
| A6 ADS-B / pager / AIS / APRS / APT / DAB | ADS-B: `adsb_dump1090_modes1_2000k` (only 0.18 s, one aircraft, dump1090-decoded).  Pager: FLEX/POCSAG/GSC audio decoded by multimon-ng, `iq_pocsag_sigid_128k` decoded from IQ; **no wideband pager capture** (a 930 MHz HackRF window showed only cellular).  AIS: `iq_ais_sigid_48k` plausible (9 CRC-valid HDLC frames).  APT: 2 verified samples.  DAB: 2 verified (null symbols exactly 96.0 ms).  APRS/AFSK1200: not obtained.  LoRa: not obtained.  welle.io/SatDump/SDRangel test files: none found in public repos. |
| B channelized / audio | P25 P1 (5 IF files, I/Q order verified), P25 P2 CC audio, DMR (I/Q order ambiguous), POCSAG 512/1200/2400, FLEX P2000, GSC (multimon-ng expected strings reproduced), APT, plus the already-available dsd-neo fixtures (not duplicated here). |

## Samples

### Trunked P25 / DMR (real RF, 800 MHz band)

| id | status | format | rate | centre | length | size | content (offsets relative to centre in MANIFEST) |
|---|---|---|---|---|---|---|---|
| `p25_dmr_800mhz_hackrf_20msps_2s` | verified | raw/cu8, 2 ch | 20.000 MS/s | 862.0000 MHz | 2.00 s | 80.0 MB | P25 Phase 1 control channel; P25 Phase 1 control channel; P25 Phase 1 control channel (other system) |
| `p25_dmr_800mhz_hackrf_sliceA_857p70_2400k` | verified | raw/cu8, 2 ch | 2.400 MS/s | 857.7000 MHz | 3.96 s | 19.0 MB | P25 Phase 1 control channel; P25 Phase 1 control channel (different system); unclassified narrowband carrier |
| `p25_dmr_800mhz_hackrf_sliceB_859p55_2400k` | verified | raw/cu8, 2 ch | 2.400 MS/s | 859.5500 MHz | 3.96 s | 19.0 MB | P25 Phase 1 voice/traffic; P25 Phase 1 voice; P25 Phase 1 voice |
| `p25_dmr_800mhz_hackrf_sliceC_853p00_2400k` | verified | raw/cu8, 2 ch | 2.400 MS/s | 853.0000 MHz | 3.96 s | 19.0 MB | P25 Phase 1 control channel; P25 Phase 1 voice/traffic; unclassified narrowband carrier |

### Broadcast: FM / HD Radio / DAB

| id | status | format | rate | centre | length | size | content (offsets relative to centre in MANIFEST) |
|---|---|---|---|---|---|---|---|
| `fm_3stations_hackrf_slice_105p50_2400k` | verified | raw/cu8, 2 ch | 2.400 MS/s | 105.5000 MHz | 5.96 s | 28.6 MB | WFM broadcast, stereo; WFM broadcast, stereo; WFM broadcast, stereo |
| `fm_hdradio_nrsc5_sample_1488k` | verified | raw/cu8, 2 ch | 1.488 MS/s | unknown | 16.91 s | 50.3 MB | WFM broadcast stereo + RDS + HD Radio hybrid; HD Radio (NRSC-5) lower digital sideband; HD Radio (NRSC-5) upper digital sideband |
| `fm_hdradio_rtlsdrblog_samples1071_1488k` | verified | raw/cu8, 2 ch | 1.488 MS/s | 107.1000 MHz | 20.00 s | 59.5 MB | WFM broadcast stereo + HD Radio hybrid; HD Radio lower digital sideband; HD Radio upper digital sideband |
| `fm_cdr_106100_sdrsharp_900k` | verified | wav/pcm_u8, 2 ch | 900.001 kS/s | 106.1000 MHz | 10.09 s | 18.2 MB | WFM broadcast stereo; China Digital Radio (CDR) hybrid digital sideband; China Digital Radio (CDR) hybrid digital sideband |
| `dabplus_9a_202928_2500k` | verified | wav/pcm_s16, 2 ch | 2.500 MS/s | 202.9280 MHz | 7.16 s | 71.6 MB | DAB (Eureka-147) OFDM ensemble, transmission mode I |
| `dab_2000k` | verified | wav/pcm_u8, 2 ch | 2.000 MS/s | unknown | 2.74 s | 11.0 MB | DAB OFDM ensemble, mode I |

### Other wideband IQ (ADS-B, ISM, UHF, VDL2)

| id | status | format | rate | centre | length | size | content (offsets relative to centre in MANIFEST) |
|---|---|---|---|---|---|---|---|
| `ism_433879_sdrtouch_1024k` | verified | wav/pcm_u8, 2 ch | 1.024 MS/s | 433.8790 MHz | 28.42 s | 58.2 MB | ISM ASK/OOK burst (Acurite-Tower id 4700); ISM burst (LaCrosse-TX id 83) |
| `adsb_dump1090_modes1_2000k` | verified | raw/cu8, 2 ch | 2.000 MS/s | 1090.0000 MHz | 0.18 s | 0.7 MB | narrow CW/interference line; Mode S / ADS-B 1090 MHz extended squitter bursts (PPM, 8 us preamble + 56/112 us) |
| `uhf_421249_tdf_2400k` | plausible | wav/pcm_s16, 2 ch | 2.400 MS/s | 421.2490 MHz | 6.00 s | 57.6 MB | wide-ish carrier (wiki: 421.850 MHz Tour de France); narrow carrier; narrow carrier |
| `uhf_504625_multi_nbfm_250k` | plausible | wav/pcm_s16, 2 ch | 250.000 kS/s | 504.6250 MHz | 12.94 s | 12.9 MB | narrowband carrier; narrowband carrier; narrowband carrier |
| `vdl2_model_synthetic_1050k` | plausible | wav/pcm_s16, 2 ch | 1.050 MS/s | unknown | 0.38 s | 1.6 MB | VDL Mode 2 D8PSK burst (10.5 kbaud) |

### Channelized IQ / audio for decoders

| id | status | format | rate | centre | length | size | content (offsets relative to centre in MANIFEST) |
|---|---|---|---|---|---|---|---|
| `iq_pocsag_sigid_128k` | verified | wav/pcm_u8, 2 ch | 128.000 kS/s | unknown | 16.36 s | 4.2 MB | POCSAG 512/1200 bps 2-FSK (+-4.5 kHz); second FSK signal |
| `iq_p25p1_c4fm_cc_bee0014c_24k` | verified | wav/pcm_s24, 2 ch | 24.000 kS/s | unknown | 54.40 s | 7.8 MB | P25 Phase 1 C4FM control channel |
| `iq_p25p1_cqpsk_cc_920650d5_24k` | verified | wav/pcm_s24, 2 ch | 24.000 kS/s | unknown | 55.42 s | 8.0 MB | P25 Phase 1 CQPSK (simulcast) control channel |
| `iq_p25p1_cqpsk_vc_91fe5105_24k` | verified | wav/pcm_s24, 2 ch | 24.000 kS/s | unknown | 17.54 s | 2.5 MB | P25 Phase 1 CQPSK voice channel |
| `iq_p25p1_c4fm_vc_24k` | verified | wav/pcm_s24, 2 ch | 24.000 kS/s | unknown | 8.32 s | 1.2 MB | P25 Phase 1 C4FM voice channel |
| `iq_p25p1_wide_vc_24k` | verified | wav/pcm_s24, 2 ch | 24.000 kS/s | unknown | 6.74 s | 1.0 MB | P25 Phase 1 voice, "WIDE" pulse-shaping variant |
| `iq_dmr_sigid_48k` | verified | wav/pcm_s16, 2 ch | 48.000 kS/s | unknown | 34.10 s | 6.5 MB | DMR TDMA (MOTOTRBO-style) 4FSK |
| `audio_p25p2_cc_96k_mono` | verified | wav/pcm_s16, 1 ch | 96.000 kS/s | unknown | 5.30 s | 1.0 MB | P25 Phase 2 (TDMA) control channel, discriminator audio |
| `audio_noaa_apt_argentina` | verified | wav/pcm_s16, 1 ch | 11.025 kS/s | unknown | 822.41 s | 18.1 MB | NOAA APT audio (2400 Hz AM subcarrier, 2 lines/s) |
| `iq_noaa_apt_sigid_192k` | verified | wav/pcm_s16, 2 ch | 192.000 kS/s | unknown | 4.10 s | 3.1 MB | NOAA APT (FM-modulated 2400 Hz AM subcarrier) |
| `iq_ais_sigid_48k` | plausible | wav/pcm_s16, 2 ch | 48.000 kS/s | unknown | 42.90 s | 8.2 MB | AIS GMSK 9600 bd (161.975/162.025 MHz channel) |
| `iq_tetra_sigid_48k` | plausible | wav/pcm_s16, 2 ch | 48.000 kS/s | unknown | 41.70 s | 8.0 MB | TETRA pi/4-DQPSK 36 kbaud carrier (25 kHz channel) |
| `iq_acars_sigid_135k` | plausible | wav/pcm_f32, 2 ch | 135.498 kS/s | unknown | 3.46 s | 3.8 MB | AM carrier with ACARS MSK (1200/2400 Hz) bursts; narrow spur |
| `iq_vdl2_sigid_48k` | plausible | wav/pcm_s16, 2 ch | 48.000 kS/s | unknown | 46.90 s | 9.0 MB | VDL Mode 2 D8PSK 10.5 kbaud (136.x MHz) |
| `audio_acarsdec_test` | plausible | wav/pcm_s16, 4 ch | 12.500 kS/s | unknown | 4.31 s | 0.4 MB | ACARS audio, 4 channels (12.5 kHz, 16-bit) |
| `audio_pocsag512_22050` | verified | wav/pcm_s16, 1 ch | 22.050 kS/s | unknown | 4.46 s | 0.2 MB | pager audio (AFSK/FSK discriminator audio) |
| `audio_pocsag1200_22050` | verified | wav/pcm_s16, 1 ch | 22.050 kS/s | unknown | 2.49 s | 0.1 MB | pager audio (AFSK/FSK discriminator audio) |
| `audio_pocsag2400_22050` | verified | wav/pcm_s16, 1 ch | 22.050 kS/s | unknown | 1.60 s | 0.1 MB | pager audio (AFSK/FSK discriminator audio) |
| `audio_flex1600_p2000_22050` | verified | wav/pcm_s16, 1 ch | 22.050 kS/s | unknown | 51.61 s | 2.3 MB | pager audio (AFSK/FSK discriminator audio) |
| `audio_gsc_22050` | verified | wav/pcm_s16, 1 ch | 22.050 kS/s | unknown | 41.74 s | 1.8 MB | pager audio (AFSK/FSK discriminator audio) |

### rtl_433_tests ISM captures (expected JSON included)

| id | status | format | rate | centre | length | size | content (offsets relative to centre in MANIFEST) |
|---|---|---|---|---|---|---|---|
| `rtl433_124_spider_01_fr_1_433_92m_250k` | verified | raw/cu8, 2 ch | 250.000 kS/s | 433.9200 MHz | 0.52 s | 0.3 MB | Abarth-124Spider |
| `rtl433_bmw_g4_tpms_g001_433_92m_2500k` | verified | raw/cs16, 2 ch | 2.500 MS/s | 433.9200 MHz | 0.01 s | 0.1 MB | BMW-GEN5 |
| `rtl433_citroen_tpms_gfile001` | verified | raw/cu8, 2 ch | 250.000 kS/s | 433.9200 MHz | 0.52 s | 0.3 MB | Citroen |
| `rtl433_ford_tpms_gfile059` | verified | raw/cu8, 2 ch | 250.000 kS/s | 433.9200 MHz | 0.52 s | 0.3 MB | Ford |
| `rtl433_neptune_r900_g001_912m_2048k` | verified | raw/cu8, 2 ch | 2.048 MS/s | 912.0000 MHz | 0.06 s | 0.3 MB | Neptune-R900 |
| `rtl433_neptune_r900_g018_912_6m_1000k` | verified | raw/cu8, 2 ch | 1.000 MS/s | 912.6000 MHz | 0.07 s | 0.1 MB | Neptune-R900 |
| `rtl433_pt2262_gfile001` | verified | raw/cu8, 2 ch | 250.000 kS/s | 433.9200 MHz | 1.05 s | 0.5 MB | Generic-Remote |
| `rtl433_schrader_mrxbc5a4_tpms_g001_433_92m_2048k` | verified | raw/cs8, 2 ch | 2.048 MS/s | 433.9200 MHz | 0.02 s | 0.1 MB | Schrader-MRXBC5A4 |
| `rtl433_schrader_mrxbc5a4_tpms_g004_433_92m_2048k` | verified | raw/cs16, 2 ch | 2.048 MS/s | 433.9200 MHz | 0.02 s | 0.1 MB | Schrader-MRXBC5A4 |
| `rtl433_toyota_tpms_0d5aee3_g007_433_92m_250k` | verified | raw/cu8, 2 ch | 250.000 kS/s | 433.9200 MHz | 0.26 s | 0.1 MB | Toyota |
| `rtl433_acurite_ext` | verified | raw/cu8, 2 ch | 250.000 kS/s | 433.9200 MHz | 0.26 s | 0.1 MB | Acurite-00275rm |
| `rtl433_ambient_weather_g002_433_92m_250k` | verified | raw/cu8, 2 ch | 250.000 kS/s | 433.9200 MHz | 0.52 s | 0.3 MB | Ambientweather-F007TH |
| `rtl433_bresser_5in1_g001_868_3m_250k` | verified | raw/cu8, 2 ch | 250.000 kS/s | 868.3000 MHz | 0.26 s | 0.1 MB | Bresser-5in1 |
| `rtl433_current_cost_cc_2352watt` | verified | raw/cu8, 2 ch | 250.000 kS/s | 433.9200 MHz | 0.26 s | 0.1 MB | CurrentCost-TX |
| `rtl433_dsc_gfile001` | verified | raw/cu8, 2 ch | 250.000 kS/s | 433.9200 MHz | 0.26 s | 0.1 MB | DSC-Security |
| `rtl433_efergy_e2_classic_gfile001` | verified | raw/cu8, 2 ch | 250.000 kS/s | 433.9200 MHz | 0.26 s | 0.1 MB | Efergy-e2CT |
| `rtl433_emontx_g001_2500k` | verified | raw/cu8, 2 ch | 2.500 MS/s | 433.9200 MHz | 0.03 s | 0.1 MB | emonTx-Energy |
| `rtl433_ert_g001_912_6m_2400k` | verified | raw/cu8, 2 ch | 2.400 MS/s | 912.6000 MHz | 0.01 s | 0.0 MB | ERT-SCM |
| `rtl433_hanwell_ml4000_g001_434_052m_1000k` | verified | raw/cs8, 2 ch | 1.000 MS/s | 434.0520 MHz | 3.20 s | 6.4 MB | Hanwell-ML4000 |
| `rtl433_hideki_ch1_223_73` | verified | raw/cu8, 2 ch | 250.000 kS/s | 433.9200 MHz | 0.52 s | 0.3 MB | Hideki-TS04 |
| `rtl433_honeywell_honeywell_001` | verified | raw/cu8, 2 ch | 250.000 kS/s | 433.9200 MHz | 0.79 s | 0.4 MB | Honeywell-Security |
| `rtl433_idm_g001_912_6m_2359_3k` | verified | raw/cs16, 2 ch | 2.359 MS/s | 912.6000 MHz | 0.17 s | 1.6 MB | IDM, NETIDM |
| `rtl433_idm_g010_912_6m_2359_3k` | verified | raw/cu8, 2 ch | 2.359 MS/s | 912.6000 MHz | 0.06 s | 0.3 MB | NETIDM |
| `rtl433_insteon_g001_915m_1024k` | verified | raw/cu8, 2 ch | 1.024 MS/s | 915.0000 MHz | 0.08 s | 0.2 MB | Insteon |
| `rtl433_lacrosse_gfile001` | verified | raw/cu8, 2 ch | 250.000 kS/s | 433.9200 MHz | 0.52 s | 0.3 MB | LaCrosse-TX141THBv2 |
| `rtl433_m_bus_g002_868_95m_1200k` | verified | raw/cu8, 2 ch | 1.200 MS/s | 868.9500 MHz | 0.05 s | 0.1 MB | Wireless-MBus |
| `rtl433_m_bus_g001_868_9m_1000k` | verified | raw/cu8, 2 ch | 1.000 MS/s | 868.9000 MHz | 0.07 s | 0.1 MB | Wireless-MBus |
| `rtl433_nexus_g027_433_92m_250k` | verified | raw/cu8, 2 ch | 250.000 kS/s | 433.9200 MHz | 1.05 s | 0.5 MB | Nexus-TH |
| `rtl433_oregon_scientific_bthr968` | verified | raw/cu8, 2 ch | 250.000 kS/s | 433.9200 MHz | 0.79 s | 0.4 MB | Oregon-BHTR968 |
| `rtl433_oregon_scientific_099603a2` | verified | raw/cu8, 2 ch | 250.000 kS/s | 433.9200 MHz | 0.52 s | 0.3 MB | Oregon-v1 |
| `rtl433_prologue_prologue_191_60_ch2` | verified | raw/cu8, 2 ch | 250.000 kS/s | 433.9200 MHz | 1.05 s | 0.5 MB | Prologue-TH |
| `rtl433_silver_spring_mesh_g003_904_4m_1000k` | verified | raw/cu8, 2 ch | 1.000 MS/s | 904.4000 MHz | 0.07 s | 0.1 MB | SilverSpring-Mesh |
| `rtl433_silver_spring_mesh_g007_916_45m_1600k` | verified | raw/cu8, 2 ch | 1.600 MS/s | 916.4500 MHz | 0.04 s | 0.1 MB | SilverSpring-Mesh |
| `rtl433_thermopro_tp11_gfile001` | verified | raw/cu8, 2 ch | 250.000 kS/s | 433.9200 MHz | 0.52 s | 0.3 MB | Thermopro-TP11 |
| `rtl433_typhur_sync_gold_g002_915m_1000k` | verified | raw/cs16, 2 ch | 1.000 MS/s | 915.0000 MHz | 0.03 s | 0.1 MB | Typhur-SyncGold |
| `rtl433_x10_record_x10_b1_on_001` | verified | raw/cu8, 2 ch | 250.000 kS/s | 433.9200 MHz | 0.79 s | 0.4 MB | X10-RF |

## Lessons / pitfalls found while validating

* **SDR#/HDSDR 2-channel WAV: ch0 = I, ch1 = Q** and this is what decodes correctly for the sigidwiki P25 `*_IF.wav` files (dsd-neo syncs only in stored order). The sigidwiki `DMR.zip` file decodes *better with I/Q swapped* (44 vs 13 DMR syncs): treat that file as possibly mirrored (`possible_iq_order_flip`).  Always confirm I/Q order with a decoder or a known asymmetric signal; DAB/AM/FM do not reveal it.
* **WAV header rates can be wrong**: `vdl2_model_16b_1050kHz.wav` says 44,100 Hz but is a 1.05 MS/s recording (file name + dumpvdl2 README).  Manifest `sample_rate_hz` is the truth, `wav_header_rate_hz` the header; the loader honours the manifest.  SDR# file names carry the centre frequency (`..._106100kHz_IQ.wav`), the header does not (HDSDR files carry it in an `auxi` chunk; the 20 MS/s archive.org files are RF64 with data size 0xFFFFFFFF - use the file length).
* **8-bit WAV is unsigned** (= cu8), HackRF-origin WAVs have a +2.5 LSB DC bias; 24-bit PCM at 24 kS/s (P25 IF); **float32 WAV can be un-normalised** (`iq_acars_sigid_135k` peaks at ~93).
* **rtl_433_tests**: files named `gNNN_<freq>M_<rate>k` carry rate/centre; `gfileNNN`/`name.cu8` files do not and rtl_433 assumes 250 kS/s and 433.92 MHz - the match against the expected JSON is the only evidence for those.
* **HackRF captures have spur combs and clock harmonics** (lines every 2.362 MHz, 120.000 MHz = 6x20 MHz) that look like carriers; the 'airband' lines in the 111.9 MHz capture are exactly that.  Useful negative examples, dangerous as positives.  A DC spike (+53 dB) sits at the capture centre.
* **Identity by decoder cross-check is strong**: the P25 control channel announces its own channel-plan frequencies; they coincide (within ~2 kHz) with the spectral lines, which verifies the centre-frequency/sample-rate labels without trusting the file name.  Carriers sit ~2 kHz low (tuner ppm error of the original recorder).
* Network-sourced IQ is scarce: sigidwiki has ~480 WAV/ZIP recordings (by my header scan) but mostly narrow (<=250 kS/s) or 'unknown' signals; its 'claimed identity' is often only a file-name.  archive.org `SDR-IQ-*` items (uploader anthony96922) hold huge 20 MS/s HackRF captures that are readable piecewise with HTTP range requests - that is where the real trunked-system slice came from.
* `dsd-neo --iq-replay` needs a sidecar with a non-zero `center_frequency_hz` ("Please specify a frequency"); it did not sync on the NXDN48 fixture with default options (NXDN presumably needs its mode flag); 1 s windows of a 25 dB-SNR P25 CC only yield a few TSBKs, so give it >= 2-4 s.
* Composite round trip works: P25 CC (NAC 140 SYSID 14C) and DMR fixtures injected into a 2.4 MS/s cu8 composite are decoded again by dsd-neo after `extract` (tested); the NXDN fixture did not decode even in its original form (needs NXDN options).

## Examined and not kept

* `_rejected/rtl433_lora_lora01_868m_1000k` - 0-byte file upstream.
* `_rejected/airband_hackrf_119p80_spurs_only` - 'airband' slice is a HackRF spur comb.
* `_rejected/sigid_445113_overloaded_comb` (note + spectrum only, data not kept) - ADC-overloaded picket fence, clipping, no identity.
* `_rejected/sigid_flex_pager_20150816_patient_data` (note only) - decodes, but the pages contain real patient names/medical details; excluded for privacy.
* Looked at but not kept: sigidwiki 170.046 MHz (single keyed carrier), 462.914 MHz UVK5 text (narrow FSK), 433.693 MHz Flinders freezer telemetry and HDSDR 433.92 MHz (rtl_433 decodes nothing), 859.57 MHz 250 kS/s (looks like an iDEN carrier: 18 kHz flat-top), 380.48 MHz (wiki: DisplayPort interference), HackRF 807.1/930/465 MHz windows (cellular/LTE blocks, no narrow channels), zenodo 'FM IQ from balloon' (Digital RF/HDF5 in a 4.3 GB zip, only ~260 kB per second of data i.e. low rate; not pursued).

## Provenance, licences, handling

* Licences: GPL/BSD for the GitHub-sourced files (see MANIFEST), **none stated** for sigidwiki, archive.org (uploader anthony96922) and rtl-sdr.com/mediafire items - treat those as internal test inputs and do not redistribute.
* The mediafire HD Radio zip also contained Windows executables/DLLs; they were never run and were deleted.  All files were downloaded into separate scratch directories and read only as data with `python3 -I`.
* Decoded content: P25 traffic metadata (unit IDs, talkgroups) is real public-safety radio metadata; pager samples are real pager traffic (the one with patient data was excluded).
* Blocked / unreachable hosts (HEAD probe through the proxy): sourceforge.net, radioreference.com and forums.radioreference.com (403), data.csail.mit.edu (403), wiki.gnuradio.org (403), cdn.discordapp.com (403); no connection: mega.nz, nrao.edu, kb.rtl-sdr.com, data.sdrplay.com, mirbsd.org, sdrtrunk.io.  rtl-sdr.com REST API is access-restricted (401, iThemes) but the HTML pages work.  github.com via git clone worked.
