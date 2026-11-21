# bl-probe: the backlight PWM, measured and tried by eye

Debug tools for the H2's backlight (X1000 TCU PWM channel 0 on PC25). `bl-probe` reads and writes the
timer through `/dev/mem`; `bl-compare.sh` uses it to switch the PWM frequency
live, so frequencies can be compared by eye without a new kernel.

Build (Buildroot's toolchain, in the build container), then copy both files to
the card's `fathom/test/backlight/`:

```sh
docker/run.sh sh -c '/cache/br-h2/host/bin/mipsel-buildroot-linux-musl-gcc -static -O2 -Wall \
    -o build/bl-probe tools/bl-probe/bl_probe.c -lm'
```

## bl-probe

| Command | What it does |
|---|---|
| `bl-probe regs` | the channel's registers, decoded: source, prescaler, period, duty |
| `bl-probe scope [S]` | samples the PC25 pin against the counter for S seconds: the duty on the pin, samples that disagree with the registers, register rewrites meanwhile |
| `bl-probe phase [S]` | the counter's phase against the system OST, sampled every 2 ms with the CPU idle in between (`BL_PHASE_RAW=1` prints the samples) |
| `bl-probe set PERIOD_NS PERMILLE [pclk\|ext]` | one setting, until the kernel next writes the brightness |
| `bl-probe follow PERIOD_NS [pclk\|ext] [SCALE]` | keeps PERIOD_NS, following the kernel's duty fraction (times SCALE); on SIGTERM puts the kernel's setting back |

Reading needs nothing; `set` and `follow` change the backlight (take the H2
lock in an agent session).

## bl-compare.sh

```sh
sh /mnt/sd/fathom/test/backlight/bl-compare.sh list
sh /mnt/sd/fathom/test/backlight/bl-compare.sh 20k        # 20 kHz, the player's brightness
sh /mnt/sd/fathom/test/backlight/bl-compare.sh 10k 0.5    # 10 kHz at half the duty
sh /mnt/sd/fathom/test/backlight/bl-compare.sh tour 20    # each in turn, 20 s each
sh /mnt/sd/fathom/test/backlight/bl-compare.sh off        # the kernel's own (20 us, 50 kHz)
```

The setting follows Menu + wheel, dimming and screen off; a reboot ends it.

## Findings (2026-11-21)

- Fathom's kernel: PCLK (100.8 MHz) /1, TDFR 3024 (the counter runs 0..TDFR,
  so a period is 3025 counts, 30.01 us), output high from 0 to TDHR. The
  stock kernel: EXT (24 MHz), 720 counts, low first and high for the last
  "duty" counts. Same frequency and duty: the waveforms differ only in phase.
- The counter keeps exact time against the OST with the CPU idling (no clock
  gating, no stalls, no restarts): residual within the OST's 0.67 us tick over
  30 s.
- The stock player has five brightness levels: 47, 50, 55, 100 and 250 (of
  255; default 55). Its dimmest sits at the same turn-on edge Fathom found
  (36 dark, 49 lit), so the stock firmware runs the backlight IC there too.
- The pin follows the registers exactly (`scope`: 0 disagreeing samples in
  8.8 million at level 1), and nothing rewrites the PWM while the screen is
  on: the player writes the brightness only when it changes (level, dim,
  screen off), and the PWM core skips a write of the same state.
- On USB at 100 % (where the flicker shows; `sh bl-compare.sh pmic`, kernel
  patch 0025): the AXP2101 says "charge done" with the battery idle, so the
  H2 runs from USB through the PMIC's power path: VBUS 5.22 V, VSYS 4.50 V,
  VBAT 4.29 V; USB input limit (reg 0x16) 500 mA; charge current (0x62)
  100 mA with termination (0x63) at 125 mA; DCDCs not forced to PWM (0x81 =
  0). Nothing changed in 60 s of samples (14/s). On battery VSYS is VBAT.
