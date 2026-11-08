# iOS 26 on the Poco F1

This tree is GiaSen's `dipper` branch (Mi 8, same Snapdragon 845) plus the changes for the Poco F1. Bytesfence has already booted iOS 26 with the GPU on a real Poco F1, but the Wi-Fi, Bluetooth, sound, vibration and flashlight work is on the Mi 8. Those are the parts this tree moves over.

**State of this tree:** the kernel and the GPU library both built on GitHub Actions on 2026-11-06, with all three Poco F1 patches applied. Nothing has run on a phone yet.

## What was changed

| File | Why |
| --- | --- |
| `xnu/patches-12377/3100-beryllium-memory-tuning.patch` | Turns on the system cache, DDR and L3 tuning, which was gated to the Mi 8 although it only depends on the chip. `sweetmemtune=0` turns it off. |
| `xnu/patches-12377/3101-beryllium-haptics-torch.patch` | Vibration and flashlight. Limits the motor to 2812 mV: the Mi 8's 3100 mV would overdrive the Poco F1's motor. |
| `xnu/patches-12377/3102-beryllium-audio-board.patch` | Audio DSP and codec clock. Adds the second amp reset pin: the Mi 8's reset pin (gpio76) is the TAS2560's here, and the TAS2559 uses gpio12. |
| `xnu/patches-12377/3104-beryllium-amps.patch` | The player keeps the amps out of reset, so the tuning program stays loaded. It also checks the TAS2560's fault flags and plays the earpiece through the amps. |
| `xnu/patches-12377/3105-beryllium-amp-protects.patch` | `kern.sweet_audio_amp_protects`: the Poco F1's amps protect the speakers themselves, so `SweetSpeaker.driver` drops its power limits and its 12 dB make-up and only holds the peaks under -1 dBFS. |
| `xnu/patches-12377/3106-beryllium-amp-bus-lock.patch` | The bit-banged amp bus gets its own lock. The once-a-second amp check no longer holds the player's lock, which the driver needs on every cycle, so iOS no longer skips ahead (the crackle at full volume). |
| `xnu/patches-12377/3107-beryllium-amp-wake-lock.patch` | The same for the amp's sleep and wake scripts: the first sound after 5 s of silence no longer loses its start. |
| `xnu/patches-12377/3108-beryllium-volume-down-pull-up.patch` | Reads the volume-down key's pull-up (PM8998 PON_PULL_CTL) at start and switches it on if it's off, as Android does. On this phone it was already on (0x0f), so nothing is written: kern.sweet_keys_pon_pull shows the value. |
| `tools/audio/tas2559fw.py` | Makes the I2C scripts for both amps from the phone's firmware and calibration. The scripts hold TI's firmware, so they stay on the phone. |
| `xnu/patches-12377/0020-stage-bearssl.sh`, `xnu/vendor/sweet_bearssl-7bea48e` | From ferdza's fork. On GitHub Actions the kernel build no longer fails when bearssl.org is down. Other hosts need `GITHUB_WORKSPACE` set to the tree. |
| `.github/workflows/chlorophyll.yml` | Builds the GPU library on a macOS runner. Adapted from ferdza's fork. |
| `.github/workflows/xnu.yml` | Defaults to macOS 26.5 sources. The steps that package Apple SDK headers and closed-library symbol lists are off, so a public fork doesn't publish them. |
| `tools/beryllium-mkboot.sh` | Makes boot images in steps, adding one hardware block per level. |

## Mi 8 and Poco F1 differences

| Part | Mi 8 | Poco F1 | Status |
| --- | --- | --- | --- |
| Display | Samsung OLED, command mode | LCD, video mode, 1080x2246 | Poco F1 path already in the tree and booted |
| Touch | ST FTS | NT36672A at 0x01 or FT8719 at 0x38, on i2c14 | Poco F1 path already in the tree and booted |
| Brightness | Panel DCS commands | WLED backlight | Poco F1 path already in the tree |
| Wi-Fi rail L23 | Refused, so left out | Voted | Gate stays correct |
| Bluetooth | gpio45-48 | Same pins and rails | Should carry over; running next to Wi-Fi is untested here |
| Amps | One TAS2557 | TAS2559 at 0x4c drives the earpiece (28.8 ohm), TAS2560 at 0x4d the loudspeaker (6.6 ohm), same I2C bus. The TAS2559's DSP feeds both | iOS's own sound plays clean through both. 3102's guess that the earpiece is on the TAS2560 was wrong; 3104 corrects it |
| Amp program | ROM mode, loads quickly | Downloaded over a bit-banged bus in about 17 s | A boot job loads it once (`org.bergamota.sweetspeaker-amp`); the on script only powers up, in 0.3 s |
| Earpiece | Codec EAR output | Through the TAS2559 alone, not the codec: same QUAT MI2S line | Amp at gain 15, the driver 7.5 dB lower; holds steady through iOS's call route (measured with the mic); clear and loud enough at the ear (the user) |
| Headphone jack | None | WCD9340: its own jack detector (MBHC), HPHL/HPHR outputs, headset mic on AMIC2 | Plugging in and out is detected (3111, `kern.sweet_jack`); the kernel player plays on the headphones, left and right on the right sides (3112, `kern.sweet_audio_out=2`); no iOS device yet |
| Motor limit | 3100 mV | 2812 mV | Patched |
| Flashlight | PMI8998 | PMI8998, both LEDs rear | Patched |
| Battery | 3400 mAh, 1700 mA charging | 4000 mAh, 2000 mA default | `sweetcharger_fcc=1700` left out; Enxerto's Mi 8 limit only applies to `BOARD=dipper` |
| Pin blocks | | gpio 11, 30, 76, 85, 86 EAST; 12, 23 SOUTH; 6, 31, 32, 58-61, 64 NORTH | Writing a pin through the wrong block resets the phone |

## Patch check

All 83 Mi 8 kernel patches were read for Mi 8-only values.

- **Stay Mi 8-only, correctly:** ST FTS touch, DCS brightness, the L23 Wi-Fi rail difference, the AOP command-db path, ldoa28.
- **Opened for the Poco F1:** memory tuning, vibration, flashlight, audio DSP, codec clock.
- **No board values:** frame pacing, GPU calls, big-core scheduling, UFS hibernate, deep idle, Wi-Fi power save, Bluetooth fixes.
- **Not taken yet:** ayudh20's one commit that fixes Adreno 630 SMMU faults. Look at it if GPU faults show up.

## Files that only come from the phone

Keep all of these out of git. The device tree holds the phone's serial number, and the firmware belongs to Qualcomm.

| File | Source |
| --- | --- |
| `dtb/beryllium/dtb` | `adb pull /sys/firmware/fdt dtb/beryllium/live.dtb`, then `python3 tools/extract-dtb.py backup/boot.img dtb/beryllium --live dtb/beryllium/live.dtb` |
| `a630_zap.mbn` | The phone's vendor firmware, split with `tools/zap-split.py` |
| Amp firmware | `tas2559_uCDSP.bin` from the phone's vendor firmware |
| Amp calibration | `tas2559_cal.bin` from the phone's `persist` partition |

`a630_gmu.bin`, `a630_sqe.fw`, `crbtfw21.tlv` and `crnv21.bin` come from linux-firmware.

## Build order

1. **Kernel:** run the `xnu.yml` workflow from the fork's default branch, about 20 minutes. A workflow can only be started by hand if it exists on the default branch, so make `poco-f1` the default before the first run.
2. **GPU library:** run the `chlorophyll.yml` workflow.
3. **Device tree (ADT):**

   ```bash
   python3 tools/mkadt.py enxerto/out/beryllium-n104-2x.adt --board beryllium --highmem --identity n104ap --display-scale 2 --points 414x896
   ```

   Then add Wi-Fi and the GPU firmware with `tools/adt-wlan.py` and `tools/adt-gpufw.py`. Name the result `enxerto/out/beryllium-n104-2x-wlan-gpu-hap.adt`.

4. **iOS root:** Mac, or Linux as root. The GPU step calls `otool` and `install_name_tool`, so a Linux host needs cctools as well.

   ```bash
   RENEGADE_BT_FW=DIR SWEET_QUIET=1 RENEGADE_CHL=1 RENEGADE_CHL_DIR=DIR/usr/local/lib/bgmtl tools/renegade/renegade update ROOT.simg --no-jb
   ```

   Leave `RENEGADE_AMP_FW` unset: it generates the Mi 8's single-amp scripts, which are wrong for this phone.

5. **Boot image:**

   ```bash
   LEVEL=1 tools/beryllium-mkboot.sh KERNEL OUT.img
   ```

## Test order

Change one level per boot and keep the log of each. `fastboot boot OUT.img` runs from RAM, so a bad image is gone after a restart.

| Level | Adds | Good result |
| --- | --- | --- |
| 1 | Display, touch, USB, battery | Home screen with CPU drawing, SSH over USB |
| 2 | Wi-Fi | Joins a WPA2 network |
| 3 | GPU | Home screen on the GPU. The first boot can look stuck while shaders compile |
| 4 | Bluetooth | Stays up next to Wi-Fi, then try pairing |
| 5 | Vibration, flashlight | Both work; the log shows the 2812 mV limit |
| 6 | Audio probe | Reads only; the log shows both amp reset pins |
| 7 | Audio DSP | DSP runs. The boot job loads the amps about 3 minutes after the start, and `sweetplay` plays a raw PCM file |

## Lessons from Bytesfence's real Poco F1

- `touch /usr/local/share/launchd-disable-memory-limits` and a reboot stopped thousands of memory kills.
- A zero-byte `/private/var/tmp/bgmtl.notrim` keeps the GPU's memory trimming off.
- Every executable copied in by hand needs `chmod 755`.
- Missing GPU firmware causes a spin of about 600 seconds, which is easy to mistake for a dead boot.
- A separate `win` partition keeps Android's data. Boot it with `ROOTPART=win`.
- TWRP in recovery plus `tools/hfs-cat-surgery.py` rescued a boot loop.

## Still to do with the phone

- Amp gain must be 15 (measured 2026-11-07 with the phone's own handset mic): TI's speaker tuning (program 0, configurations 4 and 14, the one Android loads) protects the speaker correctly only at DAC gain 15, the value Android always sets. At 12, a steady 1 kHz tone at -9 dBFS played at full level for 1.5-2 s, then the speaker turned itself down about 26 dB within 3 s and kept bouncing up about 12 dB and down again. At 9 it dropped the same 26 dB and stayed down. At 15 it held one level for 20 s, and a 200 Hz bass tone settled about 2 dB lower within 4 s, then held. The scripts now default to 15; a quieter speaker has to come from the signal, not the amp gain. The 19.2 MHz clock patch (`tools/renegade/timebase_cache.py`) is on the phone: 8 of the 85 cache files change, and a message tone from iOS played with no dropped frames or gaps.
- Full volume: at full volume Apple's speaker chain (the iPhone 11's) already reaches full scale, so the driver adds no make-up here. A loud tone held its level for 28 s with no skips and no amp faults. The user then heard no crackle, but the level kept dipping: the driver's 200 Hz bass cut raised the peaks of bass-heavy music into its -1 dBFS limiter (up to 3.6 dB off in Safari). Where the amps protect the speakers, the guard now has no bass cut and its limiter sits at 0 dBFS, so it takes nothing off (guard_test: full-scale 1 kHz and -3 dBFS 150 Hz pass untouched). Waiting for the user's listen.
- Volume stepping down by itself: during a held-phone listen the volume-down key (PM8998 RESIN) reported 7 presses of 83-144 ms that the user didn't make. Each one was a clean, steady read. Loud sound, full screen brightness and full processor load gave none with the phone lying still, and the pull-up is on. So the switch really closed while the phone was held. The next test is a held-phone listen with the key log running.
- Apple's speaker protection (`spp3`, AUSpeakerProtectionV3) runs open-loop here: MobileGestalt has no SpTS speaker data for it and the amps send it no feedback. On a steady -3 dBFS 150 Hz tone at full volume it lowers the output 2.7 dB over 25 s and keeps going; 1 kHz at loud-music power is untouched. Its settings are not in any file on the phone (only built-in defaults in the dyld cache).
- Earpiece (measured 2026-11-07 with the handset mic): at Android's handset gain 7 on the loaded speaker tuning (configuration 4), a steady 1 kHz tone at -9 dBFS started 7.5 dB under its gain 15 level, then sank another 15 dB within 2.5 s and stayed down: the same wrong protection as the speaker. At gain 15 it held one level for 20 s. The earpiece script now plays at 15 (`wsl-ampbuild.sh`), and SweetSpeaker.driver takes the 7.5 dB off the receiver's signal instead (`kProtectsRecTrimDB`, only where `kern.sweet_audio_amp_protects` = 1), so the top of the call volume is as loud as Android's earpiece. Through iOS's own route (`sweetaqin -t 1000 -9 -e`: PlayAndRecord without the speaker option) iOS picked the Receiver, the driver started the earpiece with gain 0.4217 (-7.5 dB) and the tone held within 0.15 dB for 22 s; no amp faults, and the speaker still played normally afterwards. The user listened at the ear: clear, and the top level is loud enough. Android's own earpiece tuning (configuration 13) is not used: loading it means reloading the coefficients at every switch.
- Microphone: the kernel's handset-mic capture (`kern.sweet_audio_rec`, `sweetrec`) works on the Poco F1 once MIC BIAS3 is on (ANA_MICB3 0x625 = 0x62, 2.7 V). The device tree routes AMIC3 from MIC BIAS3, while the Mi 8 code switches on MIC BIAS1: on the old kernel iOS's recordings got -73 dBFS of noise from the kernel and no sound. Patch 3109 powers MIC BIAS3 on the Poco F1 and sets ADC3 to Android's gain 12 (one step is about 1.5-2 dB); the Mi 8 keeps its values. On the 3109 kernel iOS's own recording path (`sweetaqin`, category Record) hears a quiet test tone at one level run after run, with nothing written by hand. Apple's mic chain for the iPhone 11 adds about +23 dB (mode Measurement) and an automatic level control in mode Default, so loud sounds close to the phone may clip in apps; a voice test decides whether the driver needs a mic trim. For level tests use a quiet tone through the kernel player: `sweetaq` through iOS sometimes plays 25-33 dB quieter (it seems to get the ringer's volume).
- The codec's first start used to log two lines that look like faults but aren't: "SFR Init: wdog or kernel error suspected." is the DSP's default crash-reason text, and "SMMU bank 3 fault status 0x400" is only the status register's format field (AArch64 tables) with no fault bits set. Patch 3110 skips both (3108, the volume-key pull-up check, is gone: the pull-up was already on). Checked 2026-11-08 on a RAM boot: the codec came up through all stages with neither line, 0 panics, iOS's mic path heard the quiet test tone at -14.9 dBFS as on the 3109 kernel, and the speaker played a 6 s tone in full.
- Headphone jack (2026-11-08: detection and the kernel's playback built, no iOS device yet):
  - **iOS side.** VirtualAudio, the plug-in iOS's audio server routes with (`/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin`, a separate 7.3 MB file, not in the dyld cache), still has the wired-headphone code of the iPhones that had a jack. A "Codec" device class (`Device_Codec_Aspen`) makes "wired-headphones", "wired-microphone" and "line-out" ports from the routing database's port list. A jack handler (`HALJackStateHandler`) listens to `kAudioDevicePropertyJackIsConnected` on the device's input and output. A USB path makes "USB headphones" output and "USB headset" input ports. The iPhone 11 has no jack, so its routing database may have no wired entries for "Codec"; its USB headphone route must exist, since Lightning headphones use it. First driver test: a SweetSpeaker.driver device that is there only while headphones are plugged in, tried first as "Codec" with the jack property, then as a USB device. The one iOS accepts decides the rest.
  - **Plug detection: works (3111, 2026-11-08).** The WCD9340's jack detector (MBHC). Its interrupt pin goes to TLMM 54 (`wcd9xxx-irq`). Insertion and removal are codec interrupt 8 (`MBHC_SW_DET`): status 0x0411-0x0414, mask 0x0409, clear 0x0419. The device tree sets the jack's left-channel and ground switches as normally open (`hphl-swh` 1, `gnd-swh` 1). First tried by hand with `sweetreg` (`phone-jack-arm.sh`, `phone-jack-watch.sh`): plugging in and out gave one interrupt each, and ANA_MBHC_RESULT_3 bit 4 (SWCH_LEVEL_REMOVE) reads 1 with nothing in the jack and 0 with a plug. Patch 3111 does it in the kernel: a "sweet jack" thread brings the codec up 20 s after the DSP, arms the detector the way Linux's `wcd_mbhc_initialise` does, polls the status twice a second, takes the state from the switch level at each interrupt and arms the other edge, and checks every 10 s that the detector is still on. `kern.sweet_jack`: bit 0 plugged in, bit 1 detector on, bits 8 and up the number of changes. On the phone: "SWEET JACK: detector on, nothing plugged in", then "plugged in" and "unplugged" for a real plug, `kern.sweet_jack` = 514 (2 changes, detector on); the mic and speaker levels did not change. Headphones and headsets are not told apart yet (that needs MIC BIAS2 and the MBHC ADC).
  - **Playback: works from the kernel player (3112, 2026-11-08).** Android's `headphones` path uses SLIMbus 6 RX with 2 channels, SLIM RX2 and RX3 into RX INT1 (HPHL) and RX INT2 (HPHR), class-H in low-power mode by default (HiFi for its high-quality mode); Xiaomi's overlay turns the compander off, sets HPH gain 17 and digital 83 (84 = 0 dB). Patch 3112 follows Xiaomi's own kernel for the compander-off case (techpack `wcd934x.c` and `wcd9xxx-common-v2.c`: class-H LOHIFI, the Tavil 1.1 values) in the order the Mi 8's earpiece stage (3067) found to work: the path clocked and muted, class-H and the DACs, then SLIMbus channels 144 and 145 into codec RX ports 16 and 17 (RX0 and RX1 into RX INT1 and INT2), then the PAs and unmute, AFE `SLIMBUS_0_RX` last; the off path runs backwards. `kern.sweet_audio_out=2` brings it up and the player's next start plays stereo on it; 0 or 1 take it down when the player is not on it. `kern.sweet_hph_gain` = (HPH PA index << 8) | digital dB: index 0 is loudest and Android's "HPHL Volume" is 24 minus it, the default is index 7 and -20 dB (Xiaomi's headphones setting: index 7 and -1 dB). Silent check on a RAM boot (quietest PA step, digital mute, nothing plugged in): the registers as expected (0606 07, 0608 cd, 0609 f4, 0b55 24, 0b69 24), a 6 s tone through the player with 0 errors, no over-current, the path down cleanly, the speaker and mic unchanged afterwards. First listen (`phone-hph-listen.sh`: 1 kHz at -30 dBFS, digital -30 dB, PA index 7; 2 s left only, 1 s gap, 2 s right only): the user heard left, then right. Two things for the next kernel change: the log line's interrupt labels use the wrong bits (in 0x0411 the OCPL/OCPR faults are bits 2 and 3, the CNPL/CNPR ramps bits 5 and 6; the code unmasks bits 0-3, which only matters for the reading), and `PL_OP_EAR_OFF` from SweetSpeaker.driver puts the player's output back to the speaker without taking the headphone path down.
  - **Headset mic.** Android's `headset-mic`: AMIC2 at ADC2 gain 5, MIC BIAS2 at 1.8 V (0x0623).
  - **Not needed here.** `hph-en0/en1` (codec pins 4 and 5) belong to Qualcomm's "HiFi Function" control, which none of Android's paths set; USB-C analog audio is off in the device tree.
  - Kernel and driver builds both run on GitHub Actions.
- More memory: about 3.95 GB is in use now, and the low bank could add about 1.2 GB.
