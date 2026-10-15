# Themes and layouts

REKA's look is data, not code. Two JSON documents control it:

* a **theme** (`%APPDATA%\REKA\Themes\<id>.json`, or built-in): colours, gradients, glass, metrics, fonts, meters,
  control style, window backdrop;
* the profile's **`ui` section**: which panels exist and where they dock, how a strip is composed, fader or knob,
  strip widths and per-strip overrides.

Edit either one in any text editor while REKA runs: the active user theme and the current profile are reloaded live.
The in-app **Theme editor** (toolbar → *Theme*) previews every change instantly and saves user themes.

## Theme reference

```jsonc
{
  "id": "midnight-glass", "name": "Midnight Glass", "dark": true,
  "colours": {                      // #RRGGBB or #RRGGBBAA; any key may be omitted
    "background", "backgroundTop", "backgroundBottom", "panel", "panelBorder", "panelHeader",
    "text", "textDim", "accent", "accentAlt", "mute", "solo", "fx",
    "fader", "faderTrack", "faderCap", "knob", "knobArc", "knobTrack", "button", "buttonOn",
    "meterLow", "meterMid", "meterHigh", "meterBackground", "meterPeak", "meterRms", "clip",
    "matrixOn", "matrixOff", "selection", "warning", "error", "success"
  },
  "gradients": {                    // angle in degrees (90 = top to bottom), 2+ stops
    "background": { "angle": 90, "stops": [[0.0, "#151B28"], [1.0, "#080A0F"]] },
    "panel":      { "angle": 90, "stops": [[0.0, "#1E2536E6"], [1.0, "#141924E6"]] }
  },
  "glass":   { "enabled": true, "blurRadius": 22, "tintOpacity": 0.55, "borderOpacity": 0.16, "highlight": 0.07, "shadow": 0.35 },
  "metrics": { "cornerRadius": 10, "stripWidth": 104, "stripSpacing": 6, "panelPadding": 8,
               "fontSize": 13, "headerFontSize": 15, "controlRadius": 6 },
  "fonts":   { "family": "", "monoFamily": "" },                 // "" = platform UI font
  "meters":  { "style": "peakRms",                               // peak | rms | peakRms | vu | lufs
               "decayDbPerSecond": 24, "peakHoldMs": 1500, "floorDb": -60,
               "midDb": -12, "highDb": -3, "gradient": true, "showLufs": true,
               "segments": 0 },                                  // >0 = LED-segment meters
  "controls": { "gainStyle": "fader", "knobArcThickness": 3, "faderCapHeight": 26, "faderTrackWidth": 4 },
  "window":   { "backdrop": "none" }                             // none | mica | acrylic | micaAlt (Windows 11)
}
```

**Glassmorphism.** With `glass.enabled`, REKA renders a backdrop (the background gradient plus soft `accent`,
`accentAlt` and `fx` colour fields), blurs it with `blurRadius`, and each panel samples its part of that backdrop
under a `tintOpacity` tint with a highlight and a hairline border: frosted glass that costs nothing per frame.
`window.backdrop` additionally asks Windows 11 (22H2+) for a real Mica/Acrylic system backdrop behind the window.
On older Windows, or where the window cannot be made translucent, the painted glass is what you see.

**Meters.** Peak with hold and decay, RMS (300 ms), VU (300 ms ballistics) or LUFS (momentary bar, short-term
overlay, integrated hold marker). Click a meter to reset its peak hold, clip LED and integrated loudness.

Built-in themes: `midnight-glass` (default), `studio-light`, `neon-console`.

## Layout reference (profile `ui` section)

```jsonc
"ui": {
  "theme": "midnight-glass",
  "panels": {                                   // inputs | buses | matrix | plugins
    "inputs":  { "dock": "center", "visible": true },
    "buses":   { "dock": "bottom", "visible": true, "size": 360 },   // size = height (bottom) or width (left/right)
    "matrix":  { "dock": "right",  "visible": false, "size": 420 },
    "plugins": { "dock": "left",   "visible": false, "size": 280 }
  },
  "strip": {
    "template": ["header", "source", "mode", "eq", "inserts", "sends", "pan", "gain", "buttons"],
    "gainStyle": "fader",                       // fader | knob
    "width": 104
  },
  "stripOverrides": { "1": { "gainStyle": "knob", "width": 140, "hidden": false } }
}
```

Strip sections, top to bottom in template order (`gain` is flexible and takes the remaining height):

| Section | Content |
|---|---|
| `header` | colour bar, name (double-click to rename), options menu |
| `source` | device / endpoint / application channel selector (sink selector on buses) |
| `mode` | stereo/mono/swap/L/R summing + per-channel polarity |
| `eq` | EQ button (opens the 8-band editor; right-click toggles) |
| `inserts` | plugin rack: power LED = bypass, click = editor, right-click = options, drop plugins here |
| `sends` | quick-route buttons to every bus (right-click = send level, pan, polarity, pre-fader, EQ) |
| `pan` | pan / balance knob |
| `gain` | fader or knob + meter |
| `buttons` | mute, solo, FX bypass |

Docking is available from the toolbar (*View* → panel → *Dock*), splitters are draggable, and strips can be hidden,
widened, recoloured or switched to knobs from their own menu. Everything is saved into the profile.

## Turning a mockup into a REKA design

1. Sample the mockup's colours into a copy of `midnight-glass.json` (or use the Theme editor's colour pickers).
2. Match radii, spacing, strip width and fonts in `metrics` / `fonts`.
3. Reorder or drop strip sections in `ui.strip.template` and place panels with `ui.panels`.
4. Anything a mockup needs beyond these knobs (a new control shape, a new section type) is a small addition to
   `RekaLookAndFeel` or a new `StripComponent::Widget`. The template engine picks it up by name.
