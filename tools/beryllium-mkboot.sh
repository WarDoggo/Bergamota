#!/bin/sh
# Boot image of the Poco F1 on iOS 26: tools/beryllium-mkboot.sh KERNEL OUT.img [ADT]
# Same idea as tools/dipper-mkboot.sh, but each hardware block can be added one at a time, so a bad boot points at one thing.
# Nothing is booted or flashed here. Test a boot from RAM with: fastboot boot OUT.img
#
#   LEVEL=1  display, touch, buttons, USB network + SSH, battery, charging   (CPU drawing)
#   LEVEL=2  + Wi-Fi
#   LEVEL=3  + GPU              needs the firmware in the ADT (tools/adt-gpufw.py)
#   LEVEL=4  + Bluetooth        needs RENEGADE_BT_FW in the root
#   LEVEL=5  + vibration, flashlight  (xnu 3101: the Poco F1's motor limit is 2812 mV)
#   LEVEL=6  + audio DSP, read-only probe (sweetaudio=1, writes nothing)
#   LEVEL=7  + audio DSP running, GLINK and APR, kernel player (sweetaudio=3). No sound until the amp scripts exist
#
#   ROOTPART=win        the iOS root is on that GPT partition, not userdata (Android's data stays)
#   EXTRA_ARGS="..."    more boot-args for a test boot
#
# Left out on purpose, from the Mi 8's line: sweetcharger_fcc=1700 (0.5 C of 3400 mAh; the driver's default here is 2000 mA,
# 0.5 C of the Poco F1's 4000 mAh). Bytesfence's Poco F1 boots got zero-copy scanout (99% of frames) with
# EXTRA_ARGS="sweetfb_direct=1 sweetfb_swapdepth=2 sweetfb_zcforce=1 sweetfb_scancopy=2": try it once LEVEL=3 is stable and the GPU
# firmware is confirmed loaded. It is one risky test: if the display SMMU translates after all, an unmapped fetch resets the phone.
cd "$(dirname "$0")/.." || exit 1
[ $# -ge 2 ] || { echo "usage: LEVEL=N [ROOTPART=win] $0 KERNEL OUT.img [ADT]" >&2; exit 1; }
LEVEL=${LEVEL:-1}
case "$LEVEL" in [1-7]) ;; *) echo "LEVEL must be 1..7" >&2; exit 1 ;; esac
CMDLINE="debug=0xc serial=1 cpus=8 keepsyms=1 -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetfb_noconsole=1 sweetfb_hz=60 sweetfb_vsync=1 sweetfb_scale=1 sweetfb_flip=1 sweetusb=2 sweetbattery=1 sweetfb_hw=1 sweetarm64e=1 launchd_disable_all_timeouts=1 ufslogsecs=1 sweettouchdelay=0 sweetcharger=2"
[ "$LEVEL" -ge 2 ] && CMDLINE="$CMDLINE sweetwlan=5 sweetwlandelay=60 sweet80211=3"
[ "$LEVEL" -ge 3 ] && CMDLINE="$CMDLINE sweetgpu=8 sweetgpufreq=gov sweetgpuhb=10 sweetgpuboost=7 sweetgpuubwc=1 sweetamp_yield=1"
[ "$LEVEL" -ge 4 ] && CMDLINE="$CMDLINE sweetbt=1 sweetbtfast=1 sweetbtibs=1"
[ "$LEVEL" -ge 5 ] && CMDLINE="$CMDLINE sweethap=3"
[ "$LEVEL" -eq 6 ] && CMDLINE="$CMDLINE sweetaudio=1"
[ "$LEVEL" -ge 7 ] && CMDLINE="$CMDLINE sweetaudio=3"
[ -n "$ROOTPART" ] && CMDLINE="$CMDLINE ufsroot=$ROOTPART"
CMDLINE="$CMDLINE${EXTRA_ARGS:+ $EXTRA_ARGS}"
[ "${#CMDLINE}" -le 607 ] || { echo "boot-args are ${#CMDLINE} bytes, the field holds 607" >&2; exit 1; }
RENEGADE_TARGET=ios26 BOARD=beryllium DTB="$PWD/dtb/beryllium/dtb" CMDLINE="$CMDLINE" \
	exec tools/renegade/mkboot-full.sh "$1" "${3:-enxerto/out/beryllium-n104-2x-wlan-gpu-hap.adt}" "$2"
