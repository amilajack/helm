# Helm in the browser

This target runs Helm's original C++ engine and JUCE editor. Build and serve:

```sh
./web/scripts/build.sh
python3 web/scripts/serve.py
# Open http://127.0.0.1:8080/helm.html in Chrome, then Enable audio.
```

The build installs pinned Emscripten **6.0.10** into `.toolchains`, uses the same
**JUCE 9.0.4** commit as the native products, and writes generated files under
`build/wasm`. CMake, Python 3, Git and a C++ build tool are required. No generated
JavaScript or Wasm binary is committed. The native AU/VST3/standalone targets
remain Apple Silicon only; browser Wasm is platform-neutral.

## Migration decisions

The implementation follows [Surge's browser migration](https://github.com/amilajack/surge/tree/644d2db8c8d062818b2e2436fe386402a6709ea7)
at revision `644d2db8c8d062818b2e2436fe386402a6709ea7`:

- Shared production DSP and original JUCE components. Helm's GL-only drawing
  methods are replaced by software paints using its existing paths, backgrounds
  and modulation geometry. No JavaScript synthesis or replacement control panel.
- An anchor-checked, build-local JUCE overlay supplies browser windowing, input,
  message dispatch, text composition, asynchronous clipboard/file dialogs and
  accessibility. The pinned JUCE checkout stays unchanged. The imported platform
  code is GPL-3.0-or-later; attribution and dependency licenses are preserved.
  Browser key constants originate from Dreamtonics/juce_emscripten and retain
  their original JUCE copyright and license notice. The wasm32 target explicitly
  declares little-endian byte order for JUCE binary streams and archives.
- Emscripten's C++ AudioWorklet, Wasm Workers and a fixed 512 MiB shared memory.
  A nonblocking gate transfers engine ownership between audio and control work;
  audio never waits for UI locks. Brief control transfers fade out/in. Routing,
  preset construction, MIDI learning and filesystem operations run on the control
  thread. Painting uses snapshots and does not hold the audio gate.
- A 16 ms control pump is independent of animation frames. MIDI uses a bounded,
  timestamped SPSC queue with panic-on-overflow, device selection and explicit
  browser permission. The original on-screen/computer keyboard also works.
- Audio startup, suspend, resume, processor failure, pending imports and close
  are distinct states. Worklet stacks are retired only after confirmed context
  closure, using the same version-checked SDK retirement shim. Offline rendering
  refuses to overlap an AudioContext. Callback allocation counters cover the
  malloc family; an advance-reserved pool handles unavoidable callback requests.
- IndexedDB persists user files, banks, preferences and MIDI maps. Failures are
  visible and retryable; an unreadable database is never overwritten. Factory
  presets have a small startup catalog and hash-verified, on-demand objects.
  As on native Helm, downloaded factory presets become editable local copies;
  edits and deletions survive reload. Current unsaved session state is not
  automatically restored. Imported patches are validated before application;
  banks are staged and validated before one directory rename. Exports use the
  browser's asynchronous file picker. The User files dialog downloads individual files or
  complete folders while keeping their browser copies. CJK/emoji fallback fonts load on demand
  and are pinned by SHA-256.
- Native/Wasm sample comparison uses production DSP files and a test-only seeded
  PRNG, including repeatability checks and a relative RMS tolerance of `1e-5`.
  Source inventory hashes make shared-code changes require a fresh port review.
- Static distribution uses a content-addressed version directory, integrity
  manifest, isolation headers and a corresponding-source archive. No hosted
  service or deployment is needed to play the synth.

Helm has no Surge wavetable/Lua/MSEG/convolution-editor workflows to migrate.
Native device drivers, OS Bluetooth pairing, plugin hosting, direct UDP and the
native installer updater are outside the browser platform. Browser audio/MIDI
selection replaces device-driver dialogs. Desktop products remain supported as
explicitly requested for this migration. Web MIDI and file picker permissions,
real audio hardware deadlines, physical IME and screen-reader behavior still
need device-level checks; headless tests do not establish those results.

## Validation

Use installed stable Google Chrome, matching the reference migration:

```sh
npm ci --prefix web
npm --prefix web test
python3 web/scripts/feature-inventory.py

cmake -S . -B build/dsp-native -DHELM_DSP_PARITY_ONLY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/dsp-native --parallel 4
source .toolchains/emsdk-6.0.10/emsdk_env.sh
emcmake cmake -S . -B build/dsp-wasm -DHELM_DSP_PARITY_ONLY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/dsp-wasm --parallel 4
python3 web/scripts/check-parity.py
```

Browser tests exercise the original GUI, live and offline audio, lifecycle
recovery, allocation audit, legacy controls, modulation, all factory presets,
failed downloads, invalid patches, transactional bank import/export, cancellation,
Unicode editing, accessibility actions, user-file downloads and persistence. `build/wasm/parity-results.json`
records numerical comparison results. A test-only PRNG replaces libc randomness
in the independent parity renderers; production randomness is unchanged.

After reviewing a shared-source change and updating its coverage, refresh the
inventory with `python3 web/scripts/feature-inventory.py --update`.

## Static distribution and licensing

### Vercel

Import `amilajack/helm` with the repository root as the Root Directory and
Framework Preset **Other**. The root `vercel.json` installs CMake/Python when
needed, builds the pinned Emscripten target, and publishes `build/web-dist`.
No application secrets or JavaScript dependency installation are required.
The project supplies isolation headers on every route and immutable caching
for content-addressed assets. Pushes to the connected production branch deploy
automatically; other branches receive preview deployments.

When Vercel supplies source files without Git metadata, the build recovers the
exact deployed commit's Git index so the downloadable corresponding source
contains the same tracked files. Local toolchains, build outputs, environment
files and Vercel account metadata are excluded from uploads and source archives.

### Other static hosts

```sh
python3 web/scripts/package-static.py
HELM_TEST_DISTRIBUTION=1 npm --prefix web test
python3 web/scripts/serve.py --directory build/web-dist
```

Upload the **contents** of `build/web-dist` to a static HTTPS host. `_headers` and
`vercel.json` provide COOP `same-origin`, COEP `require-corp`, CORP `same-origin`,
and caching rules. Other hosts must supply equivalent headers for every asset,
including workers, fonts and errors. Localhost is supported for development;
opening the HTML as a `file:` URL is not. Keep the root entry uncached and retain
old version directories while clients may still use them.

The site includes Helm's GPL license, JUCE's license and a source download. JUCE
9 is available under AGPLv3 or commercial terms; do not reuse the old JUCE 5
licensing assumptions. The source archive contains Helm and the pinned JUCE 9
source, including the overlay/build scripts. Noto fonts carry SIL OFL 1.1 notices
beside their assets. Build an extracted archive with the same commands; its
bundled JUCE source is detected automatically.
