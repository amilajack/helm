# Helm in the browser

This target runs Helm's original C++ engine and JUCE editor. Build and serve:

```sh
pnpm install --frozen-lockfile
pnpm typecheck
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

## WAM 2 instrument

The build also emits a standard Web Audio Module 2 instrument in `web/wam`.
After static packaging, a host can import the stable **`/wam/index.js`** URL;
`/wam/descriptor.json` describes the plugin and `/wam/host.html` opens a small
example host. The stable entry point forwards to the current version's assets,
so saved host projects can retain the same plugin URL across deployments.

The adapter uses `@webaudiomodules/sdk` **0.0.12** and API **2.0.0-alpha.6**.
It implements the WAM constructor, host group/instance identity, parameter
queries/updates, normalized automation, timestamped MIDI/MPE channel messages,
transport sync, full state round trips, event connections, GUI lifecycle and
node destruction. All 139 legacy parameter IDs remain available. Native values
are preserved, including the small fractional values in historical presets;
the GUI uses the original step/display hints when editing them.

Each WAM instance owns a separate fixed 128 MiB Wasm heap and the production C++
engine inside the host's AudioWorklet. It creates no independent AudioContext.
A 32 MiB pool is reserved before playback; C++ callback allocation counters
include MIDI/automation and rendering. The SDK itself manages JavaScript event
scheduling. Events take effect at their WAM processing-slice sample offsets;
Helm supplies its own control smoothing. Plugin construction, state/routing
changes and destruction run between callbacks. Unlike the standalone editor,
this headless WAM target needs neither pthreads nor SharedArrayBuffer, so it can
also load in non-isolated WAM hosts.

The WAM GUI provides searchable controls, MIDI audition, modulation routes,
hash-verified factory presets, and `.helm` import/export. The original JUCE
editor remains available through the standalone browser page. WAM persistence
belongs to the host's `getState`/`setState` workflow. Incoming MIDI program/bank
selection, native MIDI-learn maps and SysEx are not exposed by this adapter.
Legacy `.helm` conversions are checked against the original reader for every
bundled factory patch. Invalid state is rejected before changing the instrument.

Static hosting must allow cross-origin module/asset fetches. The Vercel config
and local server supply `Access-Control-Allow-Origin: *`; the standalone page
also retains its COOP/COEP requirements. The source archive includes unchanged
SDK/API package source and their license declarations; MPL-covered RingBuffer
code retains its separate notice and license.

## WCLAP and native CLAP

Helm also builds as a [WCLAP](https://github.com/WebCLAP): a CLAP plugin compiled
to wasm32 that browser hosts (via `wclap-host-js`) and native hosts (via
`wclap-bridge`) can load. The same wrapper, `src/clap/HelmClap.cpp`, uses only the
CLAP C API (headers pinned to **1.2.10**), not JUCE, so it also builds a native
`Helm.clap`.

```sh
pnpm install --frozen-lockfile
./web/scripts/build-wclap.sh   # pinned wasi-sdk 34 → build/wclap/Helm.wclap.tar.gz
pnpm test:wclap                # ABI, state, resources, and audio parity with the WAM build
pnpm test:wclap-host           # the WebCLAP browser test host, pinned by commit

cmake -S . -B build/clap-native -DHELM_CLAP_ONLY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/clap-native && ctest --test-dir build/clap-native
```

The module is a `wasm32-wasip1` reactor. It exports `clap_entry`, `malloc`,
`free`, its memory and one growable function table, and imports only
`wasi_snapshot_preview1`. It needs no threads or SharedArrayBuffer. The bundle
contains `module.wasm`, the hash-addressed factory library, `COPYING`,
`CLAP-LICENSE.txt` and `SOURCE.txt`; files sit at the archive root. A static
release serves it at **`/wclap/Helm.wclap.tar.gz`**, a copy of the current
version, and also at a versioned path. To try it, open
`https://webclap.github.io/browser-test-host/?module=<bundle URL>`.

- **Engine:** the headless instrument (`src/headless/HelmInstrument.cpp`) is
  shared with the WAM, so both render identical audio. Notes, MIDI and parameter
  events are sample-accurate: blocks split at event times into chunks of at most
  256 frames. Sample rates from 1 kHz to 192 kHz are accepted; mopo's buffers
  stop at 192 kHz.
- **Parameters:** all 139 legacy IDs. Each CLAP parameter ID is the FNV-1a hash
  of the string ID, so IDs stay stable across versions. Value text uses the
  original display rules.
- **State:** the same JSON schema v1 as the WAM's `getState`, so saved states
  move between WAM and CLAP hosts. Legacy `.helm` patches are converted in the
  GUI, as in the WAM. Invalid state is rejected before it changes the instrument.
- **GUI:** the WAM GUI runs in a `clap.webview/3` (draft) webview. It is compiled
  into the module and talks to the plugin through JSON messages. Factory
  presets are read from the bundle's `library/`; only `manifest.json` and
  64-hex object names are served. Hosts without webview support show their
  generic parameter UI. This includes native DAWs, because the native `.clap`
  has no Cocoa window.
- **Threads:** GUI edits reach the audio thread through a bounded lock-free
  queue and are reported to the host as parameter and gesture events. Loading a
  patch takes a lock that the audio thread only tries; if it is busy, that block
  renders silence instead of waiting.

`clap-validator` 0.4.1 runs in CI against the native plugin. The
`process-varying-sample-rates` check is excluded because it also requests
384–768 kHz.

## Validation

Use installed stable Google Chrome, matching the reference migration:

```sh
pnpm install --frozen-lockfile
pnpm typecheck
pnpm test:browser
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
Unicode editing, accessibility actions, user-file downloads and persistence.
WAM tests additionally cover multiple instances, offline MIDI timing, live
automation/audio, callback allocations, preset-reader parity, state validation,
GUI controls and destruction. `build/wasm/parity-results.json`
records numerical comparison results. A test-only PRNG replaces libc randomness
in the independent parity renderers; production randomness is unchanged.

After reviewing a shared-source change and updating its coverage, refresh the
inventory with `python3 web/scripts/feature-inventory.py --update`.

## Static distribution and licensing

### Vercel

Import `amilajack/helm` with the repository root as the Root Directory and
Framework Preset **Other**. The root `vercel.json` installs CMake/Python when
needed, builds the pinned Emscripten target, and publishes `build/web-dist`.
The browser workspace uses pnpm 12.14.0 and TypeScript 7.0.2. The install
command uses the frozen workspace lockfile; no application secrets are required.
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
pnpm test:distribution
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
