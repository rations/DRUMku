# DRUMku

> This repository is the source of the `drumku` package for RenkuOS: the
> DRUMku VST3 plug-in and the standalone DRUMku app.

A native VST3 MIDI **drum sampler** for [RenkuOS](https://github.com/RenkuOS) (a Haiku fork),
written directly against the VST 3 SDK (no plug-in framework, no GUI toolkit).

DRUMku is a drum **rack** of loader slots. Each slot loads one `.wav` sample,
has its own volume, and is bound to a MIDI note. Incoming note-on events (e.g.
from an electronic drum kit over JACK) trigger the sample(s) bound to that
pitch. Samples are resampled to the engine's sample rate when loaded, so a
44.1 kHz sample plays correctly in a 48 kHz session.

DRUMku has its own native editor (the pads, a Load button, volume and MIDI
Learn per slot). It also exposes everything as VST3 parameters (per-slot volume
and note) plus a small `IDrumLoader` interface for the per-slot sample path, so
a host that cannot show the editor can still drive the rack.

## Standalone app

`DRUMku` (Applications in the Deskbar) plays DRUMku without a DAW: one window
with DRUMku's own editor, **DRUMku**, **Audio** and **MIDI** menus and a status
line.

- **Audio ▸ Media Kit / JACK** picks the audio system. The Media Kit is built
  in: DRUMku becomes one media node (Cortex shows it) whose output goes to the
  sound card's first two playback channels. JACK uses Kasen's JACK backend, loaded from the
  `add-ons/DRUMku/jack.so` add-on only when chosen, so the app runs without the
  jack package. If JACK is not running, the app says so and stays on the Media
  Kit.
- **MIDI** lists the MIDI sources (a drum kit, a keyboard, a sequencer); tick
  the ones that should play DRUMku, several at once if you like. On the first
  run every source is connected, so a kit that is plugged in just plays. A
  ticked source that is unplugged stays ticked and is connected again within a
  second of being plugged back in. On the
  Media Kit DRUMku's input is the Midi Kit endpoint "DRUMku MIDI in 1", which
  PatchBay can connect too.
- The status line shows the audio system, rate, buffer size, late cycles and
  the connected MIDI sources.
- Samples, notes, volumes, the audio system, the MIDI sources and the window
  position are restored on the next start (`~/config/settings/DRUMku`).
- **DRUMku ▸ Save preset… / Open preset…** keep whole kits in `.kasenpreset`
  files (first offered in `~/config/settings/DRUMku/presets`). They are the
  presets Kasen's effects window saves and opens, so a kit moves between the
  app and Kasen either way; the app reads and writes them itself and does not
  need Kasen. A preset stores where the samples are, not the samples.

## Building

The build system is CMake + Ninja against the [VST3-renku](https://github.com/rations/VST3-renku) port of the VST 3 SDK.
On the RenkuOS machine, with the SDK checkout as a sibling directory:

```
make            # configure + build (build/VST3/Release/DRUMku.vst3 and
                # the app, build/bin/Release/DRUMku)
make install    # copy the bundle into ~/config/non-packaged/add-ons/media/VST3/
make validate   # run the SDK validator against the installed bundle
```

Point the build at a specific SDK with `-DVST3_SDK_DIR=<path>` (default
`../VST3-renku/vst3sdk`).

## Install / packaging (RenkuOS)

- From source: `./build-from-source.sh` (installs `DRUMku.vst3` to
  `~/config/non-packaged/add-ons/media/VST3` and the app to
  `~/config/non-packaged/apps`).
- Package: `packaging/make-hpkg.sh` → `drumku-0.5.0-1-x86_64.hpkg`
  (`pkgman install ./drumku-*.hpkg`).
- The app icon is drawn by `tools/make_icon.py` (`standalone/icons/drumku.hvif`
  opens in Icon-O-Matic).

## Licence

MIT. VST is a trademark of Steinberg Media Technologies GmbH.
