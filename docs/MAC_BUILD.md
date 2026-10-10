# Building SDRstar for macOS (Apple Silicon and Intel)

This page describes the pipeline that turns the repository into `SDRstar.app` and the two disk images
`SDRstar-AppleSilicon.dmg` / `SDRstar-Intel.dmg`: what each script does, how GitHub Actions runs it, how the caches
work, how to bump a pinned version, and what to do when something breaks.

The user-facing result is a normal double-click app. Nobody who *uses* SDRstar ever runs these commands.

## 1. Design rules

| Rule | Why |
| --- | --- |
| Every native library is built **from source** into a private prefix (`.deps/<arch>`) with `MACOSX_DEPLOYMENT_TARGET = MACOS_DEPLOYMENT_TARGET` (in `versions.env`). | Homebrew bottles are stamped with the OS version of the machine that built them. Linking the app against them would make it refuse to start on an older Mac. |
| Homebrew is used for **build tools only** (cmake, ninja, pkgconf, ccache) and never as a source of libraries. `PKG_CONFIG_LIBDIR` and `CMAKE_IGNORE_PREFIX_PATH` hide `/opt/homebrew` and `/usr/local` from every configure step. | Reproducible, relocatable app. |
| Helper decoders (dsd-neo, multimon-ng, rtl_433) are compiled **without any SDR hardware backend**. | They can never open or reconfigure the USB dongle; they only read from a pipe. |
| One architecture per build, built natively on a matching runner. | `arm64` on `macos-15`, `x86_64` on `macos-15-intel`; no universal binaries, no Rosetta. |
| Everything is pinned (`versions.env`, `scripts/deps.env`). | A build today and in a year gives the same app. |
| The app is **ad-hoc signed** only (no Apple Developer account). | Apple Silicon refuses unsigned arm64 code; ad-hoc is the minimum. Gatekeeper still asks the user to confirm once: see `packaging/README-dmg.txt`, which is also inside the disk image. |

**Current minimum macOS: 12.0** (`MACOS_DEPLOYMENT_TARGET` in `versions.env`). If a dependency ever refuses to build for
that target, lower the *ambition*, not the check: raise the number in `versions.env`; the verifier compares every
Mach-O against it.

## 2. The stages

All scripts live in `scripts/`, are `set -euo pipefail`, shellcheck-clean, compatible with macOS' stock bash 3.2, need
no sudo, and write only inside the repository.

| Stage | Script | Output | Cached in CI |
| --- | --- | --- | --- |
| 0. tool check | `check_tools.sh` | stops with install instructions if something is missing | - |
| 1. libraries | `build_deps.sh` | `.deps/<arch>/{lib,include,...}`: libusb, librtlsdr (pinned SHA), FFTW (single, threads), VOLK (+Mako in `.deps/venv`), GLFW, zstd, PortAudio, libsndfile, OpenSSL 3.5, mbelib-neo, Codec2 | yes (key A) |
| 2. helpers | `build_helpers.sh` | `.deps/<arch>/helpers/{dsd-neo,multimon-ng,rtl_433}` and `helpers/fixtures/iq` | yes (key B) |
| 3. SDR++ | `build_sdrpp.sh` | `build/<arch>/sdrpp-src` (patched tree via `prepare_sdrpp.sh`) and `build/<arch>/sdrpp-build` | ccache |
| 4. bundle | `bundle_mac.sh` (-> `bundle_app.py`) | `dist/<arch>/SDRstar.app`, signed | - |
| 5. verify | `verify_bundle.sh` (-> `verify_bundle.py`) | pass/fail report | - |
| 6. tests | `test_helpers.sh`, `test_core.sh`, `smoke_launch.sh` | pass/fail + logs | - |
| 7. package | `make_dmg.sh` | `dist/SDRstar-<AppleSilicon|Intel>.dmg` + `.sha256` | - |

`scripts/build_mac.sh` runs the whole chain on your own Mac. `scripts/ci_linux.sh`, `scripts/test_bundler_logic.sh`
and `scripts/ci_summary.sh` exist for CI.

Each component of stages 1 and 2 has a *stamp* (`.deps/<arch>/.stamps/<name>`, `helpers/.stamps/<name>`) recording
`<pin>|<arch>|<deployment target>|<recipe revision>`. A re-run skips every component whose stamp still matches, so
the scripts are idempotent and a pin bump rebuilds only what changed.

### Bundle layout

```
SDRstar.app/Contents/
  Info.plist                      from packaging/Info.plist.in (version from VERSION, LSMinimumSystemVersion from versions.env)
  MacOS/sdrstar                   SDR++ main program (renamed)
  Frameworks/*.dylib              libsdrpp_core + every non-system library, de-duplicated, id @rpath/<name>
  Plugins/*.dylib                 SDR++ modules + sdrstar_smart
  Resources/                      SDR++ resources, SDRstar.icns, BUILD-INFO.txt, licenses/
  Resources/helpers/{dsd-neo,multimon-ng,rtl_433}
```

`bundle_app.py` copies each non-system dependency into `Frameworks`, rewrites every reference to `@rpath/<name>`, deletes
all other `LC_RPATH` entries and adds exactly one, relative to the file (`@loader_path/../Frameworks` for
`MacOS/` and `Plugins/`, `@loader_path/../../Frameworks` for `Resources/helpers/`, `@loader_path` inside
`Frameworks/`). It refuses to copy any library that does not come from `.deps/<arch>` or the SDR++ build tree, which is
how a Homebrew leak is caught at the source. Signing happens last (`install_name_tool` invalidates signatures): every
Mach-O inner-first with `codesign --force --sign - --timestamp=none`, then the app.

### What `verify_bundle.py` checks

1. **Closure**: every Mach-O references only `/usr/lib`, `/System/Library` or `@rpath`/`@loader_path` names that resolve to
   a file *inside* the bundle; every `LC_RPATH` is relative and stays inside; dylib ids are `@rpath/...`.
2. **Architecture and minimum OS**: `lipo -archs` is exactly the expected one, and each binary's declared minimum macOS
   (`LC_BUILD_VERSION minos` or `LC_VERSION_MIN_MACOSX`) is <= `MACOS_DEPLOYMENT_TARGET`.
3. **Shape**: Info.plist keys (identifier `org.sdrstar.sdrstar`, executable, icon, `NSAppSleepDisabled`, no
   microphone/camera/location usage strings), helpers, resources, all expected plugins.
4. **Signature**: `codesign --verify --deep --strict` and ad-hoc.

Other checks: `test_helpers.sh` (helpers run `-h` from a *relocated copy* of the app with `DYLD_*` unset; DSD-neo decodes
DSD-neo's own `p25p1_c4fm_cc.iq` fixture -- `NAC/CC: 140` and `Identifier Update`; multimon-ng decodes DTMF),
`test_core.sh` (builds `core/` natively, runs every ctest including the integration tests against the bundled
dsd-neo, fails if they were skipped), `smoke_launch.sh` (see section 4).

## 3. Building on your own Mac

```sh
git clone <repo> && cd SDRstar
scripts/build_mac.sh            # about 30-60 minutes the first time, minutes afterwards
open dist/arm64/SDRstar.app     # or dist/x86_64/SDRstar.app
```

* It works from any directory and keeps everything in `.deps/`, `build/` and `dist/` inside the repository.
* It builds for the CPU of the Mac you run it on.
* **It never installs anything for you.** `scripts/check_tools.sh` runs first; if the Xcode Command Line Tools, cmake or
  pkg-config are missing it stops, says why each is needed and prints the exact command (`xcode-select --install`,
  `brew install cmake pkgconf`), then waits for you to run it. Optional: `brew install ninja ccache` for speed.
* One thing is installed *inside the repository*: VOLK needs the Python template library Mako at build time; if your
  python3 does not have it, `.deps/venv` is created and Mako is installed there (pure Python, nothing global).
* Options: `--skip-tests`, `--no-dmg`, `--smoke` (opens the window for 15 s), `--clean` (full rebuild).

## 4. GitHub Actions (`.github/workflows/build.yml`)

The repository is private, so macOS minutes are billed at 10x. The expensive jobs are therefore gated.

| Job | Runs | Cost | What |
| --- | --- | --- | --- |
| `plan` | always | ~0 | reads the trigger / head commit message, decides which macOS builds run |
| `linux` | every push | 1x | shellcheck + syntax checks; builds mbelib-neo and DSD-neo at the pinned commits against apt libraries; builds `core/` and runs **all** tests including the DSD-neo integration tests; runs `test_bundler_logic.sh` |
| `linux-recipes` | every push | 1x | dry run of the macOS recipes on Linux: `build_deps.sh`, `build_helpers.sh`, helper tests, core tests against that helper, and `build_sdrpp.sh` (so CMake flags, the module list and the `sdrstar_smart` module are compiled before any macOS minute is spent) |
| `mac` (matrix) | see below | 10x | the real build, one leg per architecture, `fail-fast: false`, needs `linux` and `linux-recipes` to be green |
| `release` | both `mac` legs green | 1x | refreshes the `nightly` pre-release |

**When do the macOS jobs run?**

* the head commit message of a push contains `[mac]` -> both architectures; `[mac-arm]` -> Apple Silicon only;
  `[mac-intel]` -> Intel only (put the token anywhere in the message);
* a manual run (**Actions -> build -> Run workflow**, choose `both`, `arm64` or `x86_64`);
* a pushed tag `v*`.

Any other push (including the ones from other people) only runs the Linux jobs. `release` runs only when *both*
architectures built and passed everything.

Runners: `macos-15` (Apple Silicon) and `macos-15-intel` (x86_64), the same labels SDR++'s own CI uses. If GitHub
retires the Intel label, change `runner` in the `plan` job.

### Caches

| Cache | Path | Key | Saved |
| --- | --- | --- | --- |
| libraries (A) | `.deps/<arch>` minus `helpers/` | `deps-<arch>-<CACHE_VERSION>-hash(versions.env, scripts/deps.env, scripts/build_deps.sh, scripts/lib/buildenv.sh)` | right after `build_deps.sh` succeeds |
| helpers (B) | `.deps/<arch>/helpers` | `helpers-<arch>-<CACHE_VERSION>-hash(A's inputs, scripts/build_helpers.sh)` | right after `build_helpers.sh` succeeds |
| ccache | `.ccache` | `ccache-<arch>-<CACHE_VERSION>-<sha>`, restore prefix `ccache-<arch>-<CACHE_VERSION>-` | always, after the SDR++ build |

Libraries and helpers are saved *immediately* (separate `actions/cache/save` steps), so a failure in a later stage does
not throw away an expensive earlier one. To discard everything, bump `CACHE_VERSION` at the top of the workflow.
Because the key includes `scripts/lib/buildenv.sh` (where the compiler/linker flags live), changing flags there
invalidates the caches automatically; `common.sh` changes do not.

### Artifacts and the nightly pre-release

Each macOS leg uploads `dmg-<arch>` (the `.dmg` and `.dmg.sha256`) and `logs-<arch>` (bundle, verify, helper, core,
launch logs, ctest log, `BUILD-INFO.txt`), and writes a Markdown summary to the run page.

`release` downloads both DMGs, re-checks the checksums and publishes a **pre-release tagged `nightly`** with the built-in
`GITHUB_TOKEN` (`permissions: contents: write`, no secrets). An existing `nightly` release is deleted together with its
tag and re-created, so the tag always points at the commit the files were built from and nothing is force-pushed.
Because the repository is private, only people with access to it can download from the Releases page.

### The launch test

`smoke_launch.sh` records three levels and reports each honestly in its summary:

1. **plugin dlopen**: every `Plugins/*.dylib` is `dlopen`ed with `RTLD_NOW` by a tiny C program (needs no window server);
2. **headless launch**: `sdrstar --server --addr 127.0.0.1` (SDR++'s server mode needs no GL) must stay up 15 s, log
   `Ready, listening` and load the source plugins;
3. **GUI launch**: the real GUI for 15 s. A CI runner may have no usable window server; then the script records
   `GUI NOT AVAILABLE`, which is neither a pass nor a bundle failure. If the GUI does come up it must log `Ready.` and
   load every plugin.

## 5. Bumping a pinned version

1. **SDR++, DSD-neo, mbelib-neo, librtlsdr, multimon-ng, rtl_433, VOLK, libusb tag, minimum macOS**: edit
   `versions.env`. Git pins are checked after fetching: the commit must equal the pin or, for an annotated tag object,
   peel to it. For libusb also update `LIBUSB_SHA256` in `scripts/deps.env` (release tarball checksum).
2. **GLFW, zstd, PortAudio, libsndfile, Codec2**: edit the `*_SHA` (full *commit* sha; `git rev-parse <tag>^{commit}`) and
   tag in `scripts/deps.env`.
3. **FFTW, OpenSSL**: edit version, URL and `*_SHA256` in `scripts/deps.env` (take the checksum from the project's
   release page and compare with `shasum -a 256` of the download).
4. Commit with `[mac]` in the message. The cache keys include these files, so exactly the needed rebuild happens.
5. If you changed a *recipe* (flags) without changing a pin, bump `RECIPE_REV` in the script (invalidates the local stamps)
   and, in CI, change the script itself (its hash is in the cache key) or `CACHE_VERSION`.
6. Run `scripts/test_bundler_logic.sh` if you touched `bundle_app.py` / `verify_bundle.py`; it needs `clang lld llvm` and runs
   on Linux.

## 6. Troubleshooting

| Symptom | Likely cause / fix |
| --- | --- |
| `build_mac.sh` says tools are missing | Run the command it prints; it never installs for you. |
| `checksum mismatch for ...` | A download changed or a version was bumped without its checksum. Update `scripts/deps.env`. |
| `X depends on Y, which resolves to /opt/homebrew/...` (bundler) | A library leaked in from Homebrew. Find which configure step picked it up (usually a missing `-DCMAKE_DISABLE_FIND_PACKAGE_*=ON` or a pkg-config file); build it in `build_deps.sh` instead. |
| verifier: `requires macOS 15.0, newer than the deployment target` | A binary was not built with the deployment target, usually a foreign library or a build that bypassed `setup_build_env`. |
| verifier: `LC_RPATH ... leaves the bundle` / `does not resolve inside the bundle` | A library is missing from `Frameworks`; check `bundle_app.py` output and the `otool -L` of the named file. |
| `install_name_tool: larger updated load commands do not fit` | A binary was linked without `-headerpad_max_install_names`; every flag set in `setup_build_env` includes it, so some project overrides `LDFLAGS`. |
| App will not open: "SDRstar cannot be opened because the developer cannot be verified" | Expected for ad-hoc signing; see `packaging/README-dmg.txt` (right-click Open, or *Privacy & Security -> Open Anyway* on macOS 15, or `xattr -dr com.apple.quarantine /Applications/SDRstar.app`). |
| App crashes on launch on Apple Silicon with a code-signature error | Something was modified after signing. Run `scripts/bundle_mac.sh` again (it re-signs) and `codesign --verify --deep --strict --verbose=2 dist/arm64/SDRstar.app`. |
| macOS job skipped | The commit message has no `[mac]` / `[mac-arm]` / `[mac-intel]`, or `linux` / `linux-recipes` failed. |
| `hdiutil: create failed - Resource busy` | Transient on runners; `make_dmg.sh` retries three times. |
| Dependency cache seems stale | Bump `CACHE_VERSION` in `build.yml`, or delete the caches on the repository's Actions -> Caches page. |
| `GUI NOT AVAILABLE` in the launch summary | The runner has no window server. Plugin loading was still proven by levels 1 and 2. Test the GUI on a real Mac. |

## 7. What is *not* tested here

* The live RTL-SDR path (USB streaming, tuner retune latency, bias tee, audio device) needs real hardware and a real Mac.
* The GUI on a CI runner (see above).
* Gatekeeper behaviour of the downloaded DMG (needs the quarantine flag a browser adds).
