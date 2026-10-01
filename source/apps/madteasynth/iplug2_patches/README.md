# iPlug2 local patches

`apps/madteasynth/iplug2` is gitignored -- it is a 67 MB third-party checkout, so
it is not carried in this repo. But madteasynth does **not** build against a stock
iPlug2: five files are modified, and until this directory existed those
modifications lived on exactly one disk, untracked, with no way to tell them from
upstream code.

This directory is the tracked copy. `get_iplug2.bat` overlays these files onto a
provisioned iPlug2 tree, so the patches survive a re-clone, a new machine, or a
colleague starting from `git clone` + `download-iplug-sdks.sh`.

Paths here mirror their location under `iplug2/`, so the overlay is a plain
directory copy.

## What each one does

| File | Change |
| --- | --- |
| `IGraphics/IControl.h` | Adds `virtual void OnDropAt(const char* str, float x, float y)`, defaulting to `OnDrop(str)` so no existing control changes behaviour. |
| `IGraphics/IGraphics.cpp` | `OnDrop()` forwards the drop position (control-relative) to `OnDropAt` instead of discarding it. Separately, removes the `ReleaseMouseCapture()` in the double-click path -- it swallowed the trailing button-up, so a control mirroring a platform DOWN/UP pair never saw the UP and thought the button was still held. |
| `IGraphics/Platforms/IGraphicsWin.cpp` | `GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA` was integer division. A Windows Precision Touchpad sends deltas of ~4-20, all of which truncate to 0 and the gesture is lost. A notch mouse always sends +/-120 and so worked either way. |
| `Scripts/cmake/Deploy.cmake` | Deploys through `DeployCopy.cmake` (`cmake -P`) instead of inline POST_BUILD copy commands. |
| `Scripts/cmake/DeployCopy.cmake` | New file. Does the copy, and does not fail the build when the destination is locked or needs elevation -- deploying to `C:\Program Files\Common Files\VST3` from a non-elevated shell would otherwise break an otherwise-good build. |

`SteepSynthControl` overrides `OnDropAt`, so a tree missing the first two files
fails to compile with:

```
error C3668: 'SteepSynthControl::OnDropAt': method with override specifier
'override' did not override any base class methods
```

which reads as a bug in this app's own code, and is not one.

## Upgrading iPlug2

These are **whole-file overrides, not diffs**. That is deliberate -- the copy they
were made against carries no upstream commit id (iPlug2's own
`download-*.sh` scripts `rm -rf .git*`, and the checkout's `.git` is long gone),
so there is no base revision to diff against and a `.patch` would have nothing to
apply to.

The cost is that upgrading iPlug2 silently reverts any upstream change to these
five files. When you do upgrade: diff each file here against the new upstream
copy, re-apply the changes described above by hand, and refresh this directory.
