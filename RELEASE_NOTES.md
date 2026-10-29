# Taper 102 release notes

## v0.1.0

First public release.

Taper 102 models an Ampex ATR-100 series tape machine, derived from the manual's record and reproduce
circuits, with the machine's alignment procedure done in software.

- **Setups.** 456 15 ips NAB, 456 30 ips AES, ATR Master 15 ips IEC and ATR Master 30 ips AES. Each fixes
  the speed, equalisation, tape, operating level (456 at 370 nWb/m, ATR Master at 320) and the aligned bias.
- **Controls.** Setup, Input and Output (−24 to +24 dB), Link (Output follows Input), 0 VU dBFS
  (−18 / −15 / −12 / −9, default −12) and a latency-aligned Bypass, with IN / OUT sample-peak meters.
- **Chain.** Record equaliser and clip, tape medium with bias, saturation and record-zone loss, reproduce
  head loss and contour, reproduce equaliser. Everything between the oversampler's filters runs at 176.4 or
  192 kHz. Latency 5 samples at 44.1 and 48 kHz, 4 at 96 kHz, 0 at 176.4 kHz and above.
- **Checked** against the manual's response envelopes and published curves after alignment; 0 VU in
  returns 0 VU out in all 24 tape, equalisation and operating-level combinations. No machine was
  measured. The head contour (derived from Jack Endino's published 30 ips curve), the record gap (calibrated to the Audio magazine test of May 1977), the second
  harmonic (chosen within the manual's limit), the 0 VU choices and the palette are choices of this
  plug-in; the README says which.
- **Known limits.** No noise, wow or flutter. The 3 % distortion point on ATR Master is about 2.3 dB
  under the tape datasheet's +12 dB. The 15 ips head contour is scaled from a 30 ips curve. Details are in the
  README.

Packages:

- macOS Universal: AU and VST3, arm64 + x86_64; ad-hoc signed, not notarized.
- Windows x64: VST3, unsigned.

Identity: `com.c0dypeng.taper102`, manufacturer `Cdpg`, plug-in code `T102`.
