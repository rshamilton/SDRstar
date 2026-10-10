# Installing SDRstar on your Mac

You do not need Terminal, Homebrew or any developer tools to *use* SDRstar. You need one file (a `.dmg` disk image),
and you install it the way you install most Mac apps: drag it into **Applications**.

## 1. What you need

* A Mac running **macOS 12 (Monterey) or newer**, Apple Silicon or Intel.
* An **RTL-SDR** (the RTL-SDR Blog V4 is what SDRstar is designed around), its antenna, and a USB port.
  No driver has to be installed on a Mac.
* The right file for your Mac. Open the Apple menu, **About This Mac**:
  * "Chip: **Apple M1 / M2 / M3 / M4 ...**" → you need **`SDRstar-AppleSilicon.dmg`**
  * "Processor: **Intel ...**" → you need **`SDRstar-Intel.dmg`**

## 2. Get the disk image

The `.dmg` files are produced by GitHub (the repository builds the Mac app in the cloud, because that is the only
way to build a genuine Mac app without a Mac in the loop). Your repository is `rshamilton/SDRstar`.

### Option A: download it from GitHub (recommended)

**If a build already exists**

1. Open <https://github.com/rshamilton/SDRstar/releases> (log in to GitHub: the repository is private).
2. Open the pre-release called **nightly**.
3. Under **Assets**, click the `.dmg` for your Mac (see section 1). It downloads like any file.

**If there is no release yet, ask GitHub to build one**

1. Open <https://github.com/rshamilton/SDRstar/actions>, click the workflow named **build** in the left list.
2. Click **Run workflow** (right side), choose the branch `claude/awesome-newton-b30toa`, and under the macOS option
   choose **arm64** if you have an Apple Silicon Mac, **x86_64** for Intel (only one: see the cost note below),
   then click the green **Run workflow** button.
3. Wait. The *first* build compiles everything from source and is slow (roughly one to two hours); later builds reuse
   a cache and take far less. Refresh the page; a green tick means done.
4. Click the finished run, scroll to **Artifacts**, download **`dmg-arm64`** (or `dmg-x86_64`). It arrives as a `.zip`;
   double-click it to get the `.dmg` inside.

> **A cost note, because the repository is private.** GitHub gives a private repository a monthly allowance of
> build minutes (2,000 on the Free plan: check *Settings → Billing and plans*), and counts every **macOS** minute as
> **ten**. One from-scratch Mac build can therefore use a large part of a month's allowance, which is why SDRstar's
> workflow only builds for macOS when you ask it to (or when a commit message contains `[mac]`) and builds only one
> architecture if you choose one. If the allowance runs out, GitHub stops the build and says so; nothing is charged
> unless you have set a spending limit above zero. Two ways around it: build on your own Mac (Option B, free), or
> make the repository public (GitHub does not charge for macOS minutes on public repositories).

### Option B: build it on your own Mac

Only if you prefer. You need the Xcode Command Line Tools and two small build tools. The build script **asks before
installing anything** and tells you the exact command; it never installs anything on its own.

```sh
git clone https://github.com/rshamilton/SDRstar.git
cd SDRstar
git checkout claude/awesome-newton-b30toa
scripts/build_mac.sh        # 30-60 minutes the first time
open dist/arm64/SDRstar.app # (dist/x86_64/... on an Intel Mac); or use the .dmg in dist/
```

Details and options (`--skip-tests`, `--smoke`, `--clean` ...) are in [docs/MAC_BUILD.md](docs/MAC_BUILD.md).

## 3. Install it

1. Double-click the `.dmg`. A window opens showing the SDRstar icon and an Applications folder.
2. **Drag SDRstar onto Applications.**
3. Eject the disk image (right-click it in Finder → *Eject*). You can delete the `.dmg`.

## 4. First launch: the "cannot verify" message (once)

SDRstar is free software and is not signed with a paid Apple Developer ID, so macOS asks you to confirm the first
time. This is expected; it does not mean the file is damaged.

* **macOS 14 (Sonoma) or older:** open **Applications**, **right-click** (or Control-click) **SDRstar** → **Open** →
  click **Open** in the dialog.
* **macOS 15 (Sequoia) or newer:** double-click SDRstar once (macOS blocks it, click *Done*). Open **Apple menu →
  System Settings → Privacy & Security**, scroll down to *"SDRstar was blocked…"*, click **Open Anyway**, authenticate,
  then double-click SDRstar again and click **Open**.
* If neither offers an *Open* button, open **Terminal** (Applications → Utilities), paste this one line, press Return,
  then double-click SDRstar again:

  ```sh
  xattr -dr com.apple.quarantine /Applications/SDRstar.app
  ```

  That only removes the "downloaded from the internet" flag from SDRstar. It changes nothing else.

macOS remembers your answer; you are not asked again for that copy of the app.

## 5. First run

1. Plug in the RTL-SDR with its antenna attached **before** starting the app. A short USB cable (or a direct port
   rather than a hub) avoids most connection trouble.
2. Open **SDRstar** from Applications. The source is already set to **RTL-SDR**; press the **Play** button at the
   top left.
3. Drag the cursor on the waterfall. Rest it on a signal and the card appears. See the
   [user guide](docs/USER_GUIDE.md).

Your settings are kept in `~/Library/Application Support/SDRstar`. The antenna's bias tee (power for active
antennas/LNAs) is **off** every time you start; read the [safety page](docs/SAFETY.md) before ever turning it on.

## Updating and removing

* **Update:** get the new `.dmg` and drag SDRstar onto Applications again; choose *Replace*. Settings are kept.
* **Remove:** drag SDRstar from Applications to the Trash. To also remove settings, delete
  `~/Library/Application Support/SDRstar`.

## Troubleshooting

| Symptom | What to try |
| --- | --- |
| "SDRstar is damaged and can't be opened" | The quarantine flag again: run the `xattr` line from section 4. |
| Play does nothing / "no devices found" | Unplug and re-plug the RTL-SDR, try another port or a shorter cable, then restart SDRstar. Close any other SDR app (only one program can use the dongle). |
| It runs but the waterfall is flat | Check the antenna is connected, the gain slider (Source panel) is not at the bottom, and you are on a frequency with signals (try 88-108 MHz FM broadcast). |
| Very strong signals everywhere / ghost signals | Lower the gain. Near a big transmitter, move the antenna away and read the safety page. |
| No sound | Check the speaker icon / volume at the top left and the audio device in the **Sinks** panel. Decoded digital voice plays through the same sink ("SDRstar Voice"). |
| Cards say a decoder is missing | The app bundle is incomplete: re-download the `.dmg` (the helpers live in `SDRstar.app/Contents/Resources/helpers`). |
| High CPU / fan noise | Lower the sample rate in the Source panel, or lower the FFT rate in the Display panel. |
| It crashes | Open **Console.app** → *Crash Reports* → `sdrstar`. Please keep that report and `Contents/Resources/BUILD-INFO.txt` (right-click the app → *Show Package Contents*) when asking for help. |
