# Mad Tea Synth

A MIDI instrument plugin where sound and visuals are code-driven. (platform: vst3, Windows x64, only tested in Reaper)

Read [short introduction here](https://teadrinker.net/blog/what-time-is-it.html#mad-tea-synth), or read about [the journey making it](https://teadrinker.net/blog/mad-tea-synth.html) 

### Warning - alpha version - this is software is not stable!

## Events

Events are triggered by midi notes, and run while the note is held:
- **Sound Code** - Return audio sample (−1 to 1)
- **Visual Code** - Runs every frame

Code boxes runs [Tea++](https://github.com/teadrinker/tea-plus-plus#tea) and come with these values:

| | |
|---|---|
| **`t`** | seconds since note-on |
| **`beat`** | global song position in beats |
| **`note`** | MIDI note number |
| **`vel`** | velocity, 0–127 |
| **`rate`** | pitch ratio (1.0 at root) |
| **`hz`** | note frequency in Hz (A4 = 440) |
| **`phase`** | t * hz |

## Events properties

- **`name`** - Display Name
- **`channel`** - MIDI channel, 1–16.
- **`note`** - MIDI note when `rate` is `1.0` (original pitch).
- **`type`** - `f64` (default), `f32`, or `fix` (`fx22`, Q10.22 fixed point).
- **`transpose`** - semitones added to the played note before pitching.
- **`order`** - visual draw order; lower draws first.

### Misc

- **Drop a `.wav`** on the sound editor to attach it to that sound; drop it outside to create a new event for it.
- **Drop a `.png`** anywhere to paste it as scanline spans plus the `line()` loop that draws them.
- **Drop a file** on the path field to link it, a text file that can hold all event info and code.
- **`file is master`** - when ticked, the file on disk has priority and overwrites the patch whenever it changes; unticked, the file is only read when you press **Load**.


## Sample per event 

(To use samples, you first need to specify a file in the field next to "file is master" checkbox.
The dir of that file is used as the root of samples, use relative paths)

Sample is specified in sound code, as comment, **`// WAV path, chain... \\`**

Chain steps, comma-separated after the path and applied in order:

- **`degrade=X`** - downsample by X semitones for a rougher sound; pitch is compensated.
- **`trim_end=X [Y] [B]`** - cut X% off the end, then eat inward while quieter than a B-bit LSB; Y fades out.
- **`trim_start=X [Y] [B]`** - the same from the start; Y fades in.
- **`trim=X [Y] [B]`** - shorthand: percent cut and fade at the end, silence gate at both ends.
- **`fade_end=Y` / `fade_start=Y`** - linear fade over the last/first Y% of the trimmed length.
- **`lp=X` / `hp=X`** - 12 dB/oct biquad low/high pass at X Hz.
- **`q=X`** - Q for those filters (default 0.7071).


#### Audio API

Every event can have a streaming sample: 

 - **`smp(rate, filter)`** - filter: `0` nearest, `1` linear, `2` smoothstep

#### Visual API

- **`width` / `height` / `stride`** - compile-time constants for selected export target
- **`screen[i]`** - framebuffer, index as `x + y*stride`.
- **`palette[i]`** - 256 entries (does nothing if not supported by platform)
- **`rgb(r, g, b)`** - build a palette entry from three 0–255 channels.
- **`putpixel(x, y, c)`** 
- **`rect(x, y, w, h, col)`**


#### Antialiased Visual API

Very pebble aligned at this point, will try to generalize/clean this up

- **`point(x, y, amount, blend)`** - antialiased point
- **`line(x1, y1, x2, y2, stroke_width [, alpha, blend])`**
- **`circle(x, y, radius [, alpha, blend])`** 
- **`ellipse(x, y, radius_w, radius_h [, alpha, blend])`** 
- **`rect(x, y, w, h, col [, alpha, blend])`** 
- **`background(col [, transparency])`** - fullscreen fill
- **`glyph(x, y, size, stroke_width, ascii [, alpha, blend])`** 
- **`text(x, y, size, stroke_width, "str" [, alpha, blend, letter_spacing, line_height])`** 

- **`font(id)`** - `0`/ `1` Idealist Hacker Mono, `2` Assembly Line (currently broken on pebble build)
- **`text_align(id)`** - `0` default(centered), `1` left, `2` center, `3` right.

- **`image_alloc(w, h)`** - 8-bit **greyscale** image, zero-filled. Only valid for current frame.
- **`push_target(id)`** - set drawing target.
- **`pop_target()`** - restore previous target.
- **`image_getpixel(id, x, y)`** - 0–255 grey at a whole-number coordinate.
- **`image_sample(id, x, y)`** - the same, but bilinear at fractional coordinates.
- Inside a pushed target a colour argument is a 0–255 grey, so map it back yourself: `putpixel(x, y, image_getpixel(img,u,v) * 3 / 255 * 0b010101)`.

**`blend`** - a bit set: `1` invert, `2`/`4`/`8` leave red/green/blue untouched.

Negative radius/stroke_width draws wireframe


## Install

Unzip `madteasynth.vst3.zip` to  `C:\Program Files\Common Files\VST3\`


## Examples


To get the project `what-time-is-it` to load samples, drop the file `what-time-is-it.txt` into the edit field next to the "file is master" checkbox.


## Building

`build_vst.bat` - Ideally, it should build the vst and copy it to `C:\Program Files\Common Files\VST3`

Dependencies (Windows x64, run from cmd):

- **Visual Studio 2022 or 2026** (Community, Professional, Enterprise or Build Tools) with the **Desktop development with C++** workload. It includes MSVC and the bundled CMake, which is found automatically. Any CMake 3.14+ works too, on PATH or via `set CMAKE=C:\path\to\cmake.exe`.
- **Python 3** on PATH - the CMake configure step runs `codesynth/tools/gen_template_embed.py`.
- **Git** on PATH and **internet access** on the first run - `get_iplug2.bat` clones iPlug2 and the VST3/CLAP SDKs, and iPlug2's CMake fetches WIL. Already have an iPlug2 checkout? `build_vst.bat --source C:\path\to\iplug2`
- **Write access to `C:\Program Files\Common Files\VST3`** for the final copy (run cmd as Administrator if it fails). The built plugin is also at `apps\madteasynth\build\out\madteasynth.vst3`.


## Third Party Code

- **[QOA](https://qoaformat.org/)** - Dominic Szablewski (MIT)
- **[stb_image.h](https://github.com/nothings/stb/blob/master/stb_image.h)** 2.30 - Sean Barrett (MIT / public domain)
- **[tinywav](https://github.com/mhroth/tinywav)** - Martin Roth (ISC)
- **[iPlug2](https://github.com/iPlug2/iPlug2)** - plugin framework (zlib-style) by Oli Larkin, some files are patched, see `iplug2_patches/`.
- **VST3 SDK** 3.8.0 - Steinberg (MIT)
- **CLAP SDK** 1.2.9 and **clap-helpers** - free-audio (MIT)
- **WIL** - Windows Implementation Libraries, Microsoft (MIT), fetched by iPlug2's CMake
- **Roboto** font - Google (Apache 2.0), used in Iplug, but not in Mad Tea Synth UI
