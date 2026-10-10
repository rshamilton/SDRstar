#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# safety_audit.sh [patched-sdrpp-tree]
#
# Re-checks, from the source, the hardware- and system-safety claims in docs/SAFETY.md.  Each check prints PASS or FAIL
# with the evidence; the script exits non-zero if any check fails.  Cheap (a few greps), so CI runs it on every push.
#
# With a path to an SDR++ tree prepared by scripts/prepare_sdrpp.sh, it also audits that tree (the RTL-SDR source module
# after our patches).  Compatible with macOS' stock bash 3.2.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TREE="${1:-}"
FAILS=0

pass() { printf '  PASS  %s\n' "$1"; }
fail() { printf '  FAIL  %s\n' "$1"; FAILS=$((FAILS + 1)); }

# check <description> <expect: none|some> <grep -E pattern> <paths...>
check_grep() {
    local desc="$1" expect="$2" pat="$3"
    shift 3
    local hits
    hits="$(grep -rnE --include='*.cpp' --include='*.h' --include='*.hpp' --include='*.c' --include='*.py' --include='*.sh' --include='*.cmake' --include='CMakeLists.txt' --include='*.patch' -e "$pat" "$@" 2>/dev/null | grep -vE '^[^:]*:[0-9]+:[[:space:]]*(//|#|\*)' || true)"
    if [ "$expect" = none ]; then
        if [ -z "$hits" ]; then pass "$desc"; else fail "$desc"; printf '%s\n' "$hits" | sed 's/^/          /' | head -8; fi
    else
        if [ -n "$hits" ]; then pass "$desc"; else fail "$desc (not found)"; fi
    fi
}

SRC=("$ROOT/core/src" "$ROOT/core/include" "$ROOT/modules")

echo "== SDRstar safety audit ($ROOT)"

echo "-- the radio"
check_grep "our code never includes or calls the RTL-SDR driver (all radio access goes through SDR++'s source module)" none \
    'rtl-sdr\.h|rtlsdr_[a-z_]+\(|libusb' "${SRC[@]}"
check_grep "our code has no transmit, EEPROM or GPIO calls (the hardware is receive-only anyway)" none \
    'write_eeprom|set_gpio|[a-z_]*start_tx|[a-z_]*sync_tx|[a-z_]*enable_tx|[a-z_]*_set_tx' "${SRC[@]}"

echo "-- processes"
check_grep "no shell is ever involved: no system(), popen(), /bin/sh or sh -c" none \
    '\bsystem\(|\bpopen\(|/bin/sh|"sh"[[:space:]]*,[[:space:]]*"-c"' "${SRC[@]}"
n_exec="$(grep -rnE '\bexec(v|ve|vp|l|le|lp)\(' "${SRC[@]}" 2>/dev/null | grep -cvE '^[^:]*:[0-9]+:[[:space:]]*//' || true)"
if [ "$n_exec" = "1" ]; then pass "exactly one place starts programs (core/src/subprocess.cpp, execve with an argv array)"; else fail "expected exactly one exec call site, found $n_exec"; fi
check_grep "helper programs are located only inside the app bundle / explicit environment override (no PATH search)" none \
    '\bexecvp\(|\bexeclp\(|\bposix_spawnp\(' "${SRC[@]}"

echo "-- network"
check_grep "every socket address we bind or accept is loopback only (no INADDR_ANY, no 0.0.0.0)" none \
    'INADDR_ANY|0\.0\.0\.0|in6addr_any' "${SRC[@]}"
check_grep "loopback is used for the sockets we create" some 'INADDR_LOOPBACK' "$ROOT/core/src"
check_grep "the decoder API token file is created mode 0600" some '0600' "$ROOT/core/src/dsd_session.cpp"
check_grep "our code opens no outbound internet connections (no hostnames / URLs / DNS)" none \
    'getaddrinfo|gethostbyname|https?://[a-z]' "${SRC[@]}"

echo "-- helper decoders cannot touch the USB radio"
check_grep "rtl_433 is built with RTL-SDR and SoapySDR support off" some '-DENABLE_RTLSDR=OFF' "$ROOT/scripts/build_helpers.sh"
check_grep "multimon-ng and rtl_433 build recipes contain no hardware backends switched on" none \
    '-DENABLE_(RTLSDR|SOAPYSDR)=ON' "$ROOT/scripts"
check_grep "DSD-neo is built with RTL-SDR, SoapySDR and Airspy backends off" some '-DDSD_ENABLE_RTLSDR=OFF' "$ROOT/scripts/build_helpers.sh"
check_grep "DSD-neo is started with no frontend and no device backend (PCM on stdin only)" some '"--frontend"' "$ROOT/core/src/dsd_session.cpp"

echo "-- the bias tee (4.5 V DC on the antenna connector)"
if [ -f "$ROOT/patches/sdrpp/0001-rtl-sdr-bias-tee-safety.patch" ]; then
    check_grep "patch 0001 forces the bias tee off whenever settings are loaded" some 'biasT = false;' "$ROOT/patches/sdrpp/0001-rtl-sdr-bias-tee-safety.patch"
    check_grep "patch 0001 switches it off whenever the radio is released" some 'rtlsdr_set_bias_tee\(_this->openDev, 0\)' "$ROOT/patches/sdrpp/0001-rtl-sdr-bias-tee-safety.patch"
    check_grep "patch 0001 requires an explicit acknowledgement tick and never saves 'on'" some 'biasTAck' "$ROOT/patches/sdrpp/0001-rtl-sdr-bias-tee-safety.patch"
else
    fail "patches/sdrpp/0001-rtl-sdr-bias-tee-safety.patch is missing"
fi

if [ -n "$TREE" ]; then
    echo "-- patched SDR++ tree: $TREE"
    RTL="$TREE/source_modules/rtl_sdr_source/src/main.cpp"
    if [ -f "$RTL" ]; then
        check_grep "bias-tee safety lock is present in the patched RTL-SDR source" some 'SDRstar safety' "$RTL"
        check_grep "the saved bias-tee setting is never read back" none \
            'biasT[[:space:]]*=[[:space:]]*config\.conf' "$RTL"
        check_grep "nothing in the RTL-SDR source module writes the dongle's EEPROM or GPIO" none \
            'write_eeprom|set_gpio|rtlsdr_ir_' "$RTL"
        n_bias="$(grep -cE 'rtlsdr_set_bias_tee\(' "$RTL" | tr -d ' ')"
        echo "        (rtlsdr_set_bias_tee call sites: $n_bias; each is covered by the lock above)"
    else
        fail "patched RTL-SDR source not found at $RTL"
    fi
    check_grep "no SDR++ source module contains a transmit call (SDR++ is receive-only; this guards against a future upgrade)" none \
        '[a-z_]*start_tx|[a-z_]*sync_tx|[a-z_]*enable_tx|[a-z_]*_set_tx|txvga|set_tx' "$TREE/source_modules"
fi

echo "-- the app bundle template"
if [ -f "$ROOT/packaging/Info.plist.in" ]; then
    if grep -vE '^[[:space:]]*(<!--|[A-Za-z].*)$' "$ROOT/packaging/Info.plist.in" | grep -qE '<key>NS(Microphone|Camera|Location|Contacts|Calendars|Photo)[A-Za-z]*UsageDescription</key>'; then
        fail "Info.plist asks for microphone/camera/location/contacts/... access"
    else
        pass "Info.plist asks for no microphone, camera, location, contacts or photos access"
    fi
fi

echo
if [ "$FAILS" -eq 0 ]; then echo "safety audit: all checks passed"; exit 0; fi
echo "safety audit: $FAILS check(s) FAILED"
exit 1
