# OCX Type 2 Teensy build: firmware + test workflow

## Package contents

- `ocx_type2_teensy41_decoder.ino` — real-time firmware for Teensy 4.1 + Teensy Audio Shield Rev D/D2
- `ocx_type2_wav_sim.py` — offline WAV simulator using the same approximate decoder structure

## What this firmware is and is not

This firmware is a practical real-time stereo decoder foundation for your ordered hardware.
It is intentionally conservative and tunable.

## Arduino / Teensy upload steps

1. Install Arduino IDE 2.x.
2. Open **File > Preferences**.
3. Add this URL under **Additional boards manager URLs**:

   `https://www.pjrc.com/teensy/package_teensy_index.json`

4. Open **Boards Manager**, search for **teensy**, install the PJRC package.
5. Connect the Teensy 4.1 with a **data-capable USB cable**.
6. Open `ocx_type2_teensy41_decoder.ino` in Arduino IDE.
7. Select:
   - **Board:** Teensy 4.1
   - **USB Type:** Serial
   - **CPU Speed:** 600 MHz
   - **Optimize:** Faster or Fastest
8. Click **Upload**.
9. If the Teensy Loader appears and waits, press the small **program button** on the Teensy once.
10. Open **Serial Monitor** at **115200 baud**.

## Expected startup behavior

On boot, the firmware prints a parameter dump and a help menu.
Default mode is **decode active**, bypass off.

## Serial commands

- `h` help
- `p` print current status
- `x` clear clip flags
- `b` bypass on/off
- `0` reload factory preset
- `i/I` input trim -/+ 0.5 dB
- `o/O` output trim -/+ 0.5 dB
- `s/S` strength -/+ 0.05
- `f/F` reference dB -/+ 1 dB
- `a/A` attack -/+ 0.5 ms
- `r/R` release -/+ 5 ms
- `c/C` sidechain HP -/+ 10 Hz
- `q/Q` sidechain shelf gain -/+ 1 dB
- `w/W` sidechain shelf freq -/+ 100 Hz
- `e/E` de-emphasis gain -/+ 1 dB
- `d/D` de-emphasis freq -/+ 50 Hz
- `t` toggle built-in 1 kHz tone
- `z/Z` tone level -/+ 1 dB

## Minimal bring-up procedure

1. Power the Teensy by USB.
2. Wire cassette source to **LINE IN**.
3. Wire amplifier / headphone amp to **LINE OUT** or use the shield headphone jack.
4. Boot and open Serial Monitor.
5. Send `p` and confirm firmware is alive.
6. Start with decoder active.
7. If you hear obvious overload or pumping, send:
   - `i` a few times to reduce input trim
   - `o` a few times to reduce output trim
8. If the Serial Monitor warns about clipping, reduce source level before touching the algorithm too much.

## Fast calibration suggestion

Use a steady midband signal first.
A 1 kHz tone or a stable music passage is good enough for first alignment.

1. Toggle bypass with `b`.
2. Match perceived loudness between bypass and decode using `i/I` and `o/O`.
3. Listen for too much brightness or dullness:
   - more negative de-emphasis = darker / less hiss
   - less negative de-emphasis = brighter
4. Listen for breathing/pumping:
   - too nervous -> increase release or reduce strength
   - too flat / under-decoded -> increase strength slightly

## Offline simulation option

Install dependencies:

```bash
pip install numpy scipy matplotlib
```

Run example:

```bash
python ocx_type2_wav_sim.py input.wav decoded.wav --plot
```

This is the fastest way to compare parameter changes without reflashing hardware.

## Practical advice

- Keep analog wiring short.
- Use a real USB data cable, not a charge-only cable.
- Do not chase the algorithm before basic levels are sane.
- Tweak in this order: source level, input trim, output trim, tonal balance, then dynamics.
