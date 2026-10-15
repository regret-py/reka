# `.reka` profile format (v1)

A profile is one UTF-8 JSON file (comments allowed) holding everything needed to restore a REKA session: devices,
virtual I/O, strips, routing, plugin states, the control surface and the UI layout. The built-in starter profile
is [`resources/profiles/default.reka`](../resources/profiles/default.reka).

Loading is a **hot reload**: each section is compared with the running state, and only real differences are
applied (ARCHITECTURE.md §7). Saving is atomic (write to `*.tmp`, then rename). Files are watched and reloaded
within a second when edited externally.

```jsonc
{
  "format": "reka-profile",        // required
  "version": 1,                    // required; newer versions are refused with a clear message
  "name": "Streaming",

  "audio": {
    "master":    { "type": "ASIO", "input": "Focusrite USB ASIO", "output": "Focusrite USB ASIO",
                   "sampleRate": 48000, "bufferSize": 128 },          // type "" = software clock
    "secondary": [ { "type": "Windows Audio", "device": "Headset Earphone", "input": false, "bufferSize": 0 } ],
    "virtual": {
      "endpoints":    [ { "name": "REKA Input 1", "flow": "render",  "channels": 2, "sampleRate": 48000 },
                        { "name": "REKA Output 1", "flow": "capture", "channels": 2, "sampleRate": 48000 } ],
      "applications": [ { "executable": "Spotify.exe", "includeChildren": true } ]
    }
  },

  "mixer": {
    "panLaw": "constantPower",     // constantPower | constantPowerUnityCentre | linear
    "inputs": [ <strip> ... ],
    "buses":  [ <strip> ... ],     // buses use "sink" instead of "source"
    "routes": [ { "from": 1, "to": 100, "enabled": true, "gainDb": 0.0, "pan": 0.0,
                  "phase": false, "preFader": false, "eq": <eq> } ]
  },

  "hardware": {
    "serial":  { "enabled": true, "port": "auto", "baud": 115200 },   // or "COM7"
    "map":     [ 1, 2, 3, 100 ],   // physical channel n -> strip id; [] = on-screen order
    "display": { "ledBrightness": 64, "oledContrast": 200 },
    "midi":    { "input": "<device id>", "output": "<device id>",
                 "mappings": [ { "channel": 1, "type": "cc", "number": 7, "strip": 1, "target": "gain" } ] }
  },

  "ui": { ... see THEMES.md (theme, panels, strip template, overrides) ... }
}
```

### Strip

```jsonc
{
  "id": 1,                                   // unique across inputs and buses; persisted, never recycled
  "name": "Microphone",
  "colour": "#4DA3FF",                       // CSS order: #RRGGBB or #RRGGBBAA
  "source": { "port": "master", "left": 0, "right": -1 },   // right = -1: mono channel
  "gainDb": 0.0,                             // -100 (= -inf) ... +12
  "pan": 0.0,                                // -1 ... +1 (balance for stereo, pan for mono modes)
  "mute": false, "solo": false, "fxBypass": false,
  "phase": [false, false],                   // polarity L, R
  "mode": "mono",                            // stereo | mono | swap | left | right
  "eq": <eq>,
  "inserts": [ { "plugin": "VST3-Pro-Q 3-1f2e3d4c-5a6b7c8d",   // JUCE PluginDescription identifier
                 "name": "Pro-Q 3", "format": "VST3",
                 "sandboxed": true, "bypassed": false,
                 "state": "<base64 plugin chunk>" } ]
}
```

### EQ (8 bands)

```jsonc
{ "enabled": true,
  "bands": [ { "on": true, "type": "lowCut", "freq": 80, "gain": 0, "q": 0.707 }, ... 8 entries ... ] }
```

Band types: `bell`, `lowShelf`, `highShelf`, `lowCut`, `highCut`, `notch`, `bandPass`, `allPass`. Frequency is
clamped to 10 Hz…0.49 × sample rate, gain to ±36 dB and Q to 0.05…40.

### Port keys

| Key | Port |
|---|---|
| `master` | the master device (whichever device is configured; strips keep their channels when you switch interfaces) |
| `in:<device>` / `out:<device>` | a secondary device direction |
| `virtual:<name>` | a REKA virtual endpoint |
| `app:<executable>` | a per-application capture (lower-case executable name) |

A binding whose port is missing (unplugged, app not running) is kept and shown as *offline*. The strip renders
silence until the port reappears under the same key.

### Validation

The loader reports (and refuses) a missing or foreign `format`, an unknown future `version`, duplicate strip ids,
inserts without a plugin id and corrupt base64. Routes to unknown strips are dropped silently. Unknown keys are
ignored, so files stay forward-compatible within a version.
