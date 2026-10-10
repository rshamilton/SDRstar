SDRstar - how to install and open it
====================================

1. INSTALL
   Drag the SDRstar icon onto the "Applications" folder in this window.
   (Then you can eject this disk image: right-click it in Finder, "Eject".)

2. THE FIRST TIME YOU OPEN IT, macOS WILL SAY IT "CANNOT VERIFY" THE APP
   This is expected and does not mean the app is damaged. SDRstar is free,
   open-source software and is not signed with a paid Apple Developer ID, so
   macOS Gatekeeper asks you to confirm once. Pick the line for your macOS:

   macOS 14 (Sonoma) or older
     Open the Applications folder, RIGHT-CLICK (or Control-click) SDRstar,
     choose "Open", then click "Open" in the dialog that appears.

   macOS 15 (Sequoia) or newer
     a) Double-click SDRstar once. macOS blocks it - click "Done".
     b) Open  Apple menu > System Settings > Privacy & Security.
     c) Scroll down to the message  "SDRstar was blocked ..."  and click
        "Open Anyway", then enter your password / use Touch ID.
     d) Double-click SDRstar again and click "Open".

   Either way macOS remembers your choice; you will not be asked again.

   If the steps above do not work (for example the dialog never offers
   "Open"), open the Terminal app (Applications > Utilities), paste this
   single line and press Return:

       xattr -dr com.apple.quarantine /Applications/SDRstar.app

   then double-click SDRstar again. That command only removes the
   "downloaded from the internet" flag from SDRstar; it changes nothing else.

3. USE IT
   * Plug in your RTL-SDR (for example the RTL-SDR Blog V4) with its antenna.
     No driver needs to be installed on a Mac.
   * Start SDRstar and press the Play button at the top left.
   * If the dongle is not found: unplug it, plug it in again (or try a
     different USB port / a short USB cable), then restart SDRstar.
   * The "bias tee" (power for active antennas) is always OFF when SDRstar
     starts and must be switched on by you each time. Never enable it unless
     your antenna or LNA actually expects power from the radio.

4. REMOVE IT
   Drag SDRstar from Applications to the Trash. Your settings live in your
   user Library folder (Library/Application Support); delete the folder there
   too if you want a completely clean removal.

5. WHAT IS INSIDE / LICENCES
   SDRstar is free software (GPL-3.0-or-later). It is built from SDR++, DSD-neo,
   multimon-ng, rtl_433, librtlsdr and other open-source libraries. Exact
   versions, source locations and licences are listed in the app: right-click
   SDRstar > Show Package Contents > Contents > Resources > licenses, and
   BUILD-INFO.txt in the Resources folder.

   Receiving radio signals is legal in most places, but what you may listen to
   and do with what you hear differs by country. SDRstar does not decrypt
   encrypted traffic. You are responsible for following the laws that apply
   to you.
