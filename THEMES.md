# Making a Fathom theme

A theme changes how Fathom looks: colours, the selection shape, corners, motion, icons and,
if you like, its own font. It can't change the layout or how the keys work, so every theme
stays familiar and legible. Themes are plain text: no code runs.

You don't need a computer for most themes: on the H2, Settings → Interface → Theme →
**New theme** (or **Menu** on a theme) opens the theme editor. Pick dark or light and an
accent, pick any colour from 134 named ones, and choose the shapes, the font and the text
size; a preview shows it as you go, and it warns when text gets too faint
to read. **Save** writes it here as `Name.theme`, which you can then edit as below.

## Where themes go

On the H2's microSD card, in `fathom/themes/`:

| Form | Use it for |
|---|---|
| `fathom/themes/My Theme.theme` | Colours and shapes only |
| `fathom/themes/My Theme/theme.ini` | A theme with its own font: put the font files next to it, e.g. `fathom/themes/My Theme/fonts/MyFont.ttf` |

The player reads this folder when it starts, so restart the H2 (hold Power to turn it off, then
turn it on) and the theme appears in Settings → Interface → Theme. The built-in themes always come first.

The example below is a complete theme, and the built-in ones in `fathom/assets/themes/` are
good starting points: copy one into `fathom/themes/`, give it a new `name`, and change it.

## The file

`key = value` lines in sections. `;` starts a comment.

```ini
[theme]
name    = Night Owl          ; required: shown in the theme list
author  = Your name
licence = GPL-3.0-or-later   ; how others may share it (and its fonts)
format  = 1                  ; the theme format this file is written for
base    = dark               ; dark | light

[color]                      ; #RRGGBB
bg          = #0B1622        ; required: the background
surface     = #1B2B3D        ; required: cards, toasts, panels
text        = #EAF2FA        ; required: main text
text_muted  = #A9BBCD        ; required: secondary text (artist, times)
select_bg   = #F2C46D        ; required: the selection pill or bar
select_fg   = #1A1300        ; required: text on the selection
signal      = #7FD8BE        ; required: playing indicator, progress, "Bit+"
info        = #A9BBCD        ; optional: format chip, output name (default: signal)
mode        = #7FD8BE        ; optional: the top bar's play/pause/stop icon (default: signal); the other status icons use text
battery     = #A9BBCD        ; optional (default: text_muted)
warning     = #FFC845        ; optional
danger      = #FF6B6B        ; optional
artist      = #C9B8FF        ; optional: Now Playing's artist (default: text_muted)
album       = #A8D8FF        ; optional: Now Playing's album (default: text_muted)
chip_bg     = #2A2545        ; optional: the format chip (default: surface; info with stepped corners)
chip_fg     = #F1EDFF        ; optional: its text (default: text; select_fg with stepped corners)

[font]
family = atkinson            ; a built-in family: atkinson | arkpixel | vt323
; or your own font, relative to the theme's folder:
file       = fonts/MyFont.ttf
file_title = fonts/MyFont-Medium.ttf   ; optional, for titles and the header
; pixel sizes per role (optional; defaults 15, 20, 18, 22, 16):
meta  = 15                   ; times, counts, the header's battery
body  = 20                   ; list rows, and the artist on Now Playing
sub   = 18                   ; the help Menu shows on a setting, and Find cover's artist
title = 22                   ; the title on Now Playing (two lines)
head  = 16                   ; the header

[shape]
selection = pill             ; pill | bar
corner    = round            ; round | stepped | square
radius    = 10               ; 0..20 px
shadow    = none             ; none | hard
progress  = bar              ; bar | segments
cursor    = none             ; none | arrow
playing   = dot              ; dot | equalizer
motion    = smooth           ; smooth | stepped

[icons]                      ; optional: your own header icons, 16x16 PNG with transparency
play       = icons/play.png
pause      = icons/pause.png
shuffle    = icons/shuffle.png
repeat     = icons/repeat.png
repeat_one = icons/repeat-one.png
charging   = icons/charging.png
stop       = icons/stop.png       ; nothing playing
bluetooth  = icons/bluetooth.png  ; Bluetooth on (drawn faded until headphones are connected)
wifi       = icons/wifi.png       ; Wi-Fi on (drawn faded until a network is joined)

[decor]
icons = line                 ; line | pixel (Home's icons; pixel by default with arkpixel)
```

Any icon you leave out is drawn with the built-in symbol: the play state in your `mode` colour,
the others in `text`. Your icons are
drawn as they are (their own colours).

Keys this firmware doesn't know are ignored (the theme list says so), so a theme written for a
newer firmware still loads. `[art] style` and `[decor] header_pattern`, in older themes, are
ignored without a word: they were never drawn.

## Rules

A theme that breaks a **legibility rule** still loads, with "warning" in the theme list and the
reason shown when you pick it:

| Rule | Why |
|---|---|
| Text on background at least **7:1** contrast (`text` on `bg` and on `surface`, `select_fg` on `select_bg`) | Reading titles at arm's length, in daylight |
| Secondary text and indicators at least **4.5:1** on `bg` | Times, counts and icons stay readable |
| No text role under **15 px** | The smallest size that stays readable on the H2's screen |
| Your font covers at least the accented letters the default font does | Names in other languages shouldn't turn into boxes |
| Icons are 16x16 pixels (others are not used) | They sit in the 28 px header next to the battery |

Contrast is measured after the colour is reduced to the panel's RGB565, as the H2 shows it.

A theme with an **error** can't be used: the list shows "can't load" and why. Errors are a
colour that isn't `#RRGGBB`, a missing required colour, a value that isn't one of the choices
above, or a font or icon file that's missing or can't be read. Font and icon files must be inside the
theme's folder.

## Fonts

Bundle only fonts you're allowed to share, such as fonts under the SIL Open Font License, and
include their licence file in the theme's folder. Characters your font lacks are drawn with the
default font. Fonts load only when the theme is shown, but every font you pick stays in memory
until the player restarts, so keep font files small (a few hundred kB).

## Checking a theme on a computer

```bash
build/theme-check "path/to/My Theme"
```

It reports errors and warnings as the player would (contrast ratios included). To see the theme,
put it in `build/sim-data/themes/` and run the simulator: `build/sim/fathom-sim`, then
Settings → Interface → Theme, or `T` to cycle themes. `fathom-sim --screenshots DIR` also writes a picture
of each contributed theme's home screen.
