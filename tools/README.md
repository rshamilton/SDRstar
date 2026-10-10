# Developer tools

| Tool | What it is for |
| --- | --- |
| `fetch_test_samples.sh` | Downloads the optional public test recordings that are not stored in the repository (a 433 MHz sensor capture and an ADS-B capture). Pinned to exact upstream commits, SHA-256 checked. |
| `gen_afsk_samples.py` | Generates the synthetic APRS and EAS test audio (`core/tests/data/signals`). |
| `make_composite.py` | Builds a **synthetic** wideband IQ scene (WAV) from DSD-neo's small recorded fixtures at chosen frequencies (`--sig NAME@FREQ[:dB]`), used to drive the whole GUI under a virtual display. Synthetic: it proves behaviour, not RF realism. |
| `../tests/samples/` | Catalogue of the real public recordings used for validation, with the tools that inspect and slice them (see its README). |
| `../scripts/safety_audit.sh` | Re-checks the safety claims in `docs/SAFETY.md` from the source. |
