# DRUMku

A native VST3 MIDI **drum sampler** for [Haiku](https://www.haiku-os.org/),
written directly against the VST 3 SDK (no plug-in framework, no GUI toolkit).

DRUMku is a drum **rack** of loader slots. Each slot loads one `.wav` sample,
has its own volume, and is bound to a MIDI note. Incoming note-on events (e.g.
from an electronic drum kit over JACK) trigger the sample(s) bound to that
pitch. Samples are resampled to the engine's sample rate when loaded, so a
44.1 kHz sample plays correctly in a 48 kHz session.

Because no VST3 plug-in ships a usable editor on Haiku, DRUMku exposes
everything as VST3 parameters (per-slot volume and note) plus a small
`IDrumLoader` interface for the per-slot sample path. A host such as
[jackDAW-haiku](https://github.com/rations) renders the rack — a Load button,
volume slider, and Learn checkbox per row — in its own effects window.

## Building

The build system is CMake + Ninja against a Haiku-patched VST 3 SDK. From the
Haiku machine, with the SDK checkout as a sibling directory:

```
make            # configure + build (build/VST3/Release/DRUMku.vst3)
make install    # copy the bundle into ~/config/non-packaged/add-ons/media/VST3/
make validate   # run the SDK validator against the installed bundle
```

Point the build at a specific SDK with `-DVST3_SDK_DIR=<path>` (default
`../VST3-haiku/vst3sdk`).

## Install / packaging (Haiku)

- From source: `./build-from-source.sh` (installs `DRUMku.vst3` to
  `~/config/non-packaged/add-ons/media/VST3`).
- Prebuilt package: `packaging/make-hpkg.sh` → `drumku-0.1.0-1-x86_64.hpkg`
  (`pkgman install ./drumku-*.hpkg`). HaikuPorts recipe: `packaging/drumku-0.1.0.recipe`.
- See the stack overview in `jackDAW-haiku/STACK.md`.

## Licence

MIT. VST is a trademark of Steinberg Media Technologies GmbH.
