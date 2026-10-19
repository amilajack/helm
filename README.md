## Helm
Helm is a free, cross-platform, polyphonic synthesizer that runs on GNU/Linux, Mac, and Windows as a standalone program and as a LV2/VST/AU/AAX plugin. Release is in beta so there are outstanding bugs. Please send any bugs found to matthewtytel@gmail.com

### Installing:
Go to [tytel.org/helm](http://tytel.org/helm) for builds.

### Building:
If you want to build AAX plugins You'll need its SDK located in ~/srcs  
For AU on the Mac you'll want to put the CoreAudio SDK in /Applications/Xcode.app/Contents/Developer/Extras/CoreAudio  
Nothing extra is needed for building VST/LV2 plugins

#### Linux
To build and install the standalone, lv2 and vst plugin:
```bash
make
sudo make install
```

Other make commands:
```bash
# Build just the Linux standalone executable:
make standalone

# Build just the Linux LV2 plugin:
make lv2

# Build just the Linux VST plugin
make vst

# Install just the Linux standalone executable:
sudo make install_standalone

# Install just the Linux LV2 plugin:
sudo make install_lv2

# Install just the Linux VST plugin
sudo make install_vst
```

The standalone executable is built to standalone/builds/linux/build and installed to /usr/bin
The LV2 plugin is built to builds/linux/LV2 and installed to /usr/lib/lv2
The VST plugin is built to builds/linux/VST and installed to /usr/lib/lxvst

#### macOS (Apple Silicon)

The CMake build produces native **arm64-only** VST3, Audio Unit, and standalone
bundles for **macOS 11 or later**. It requires Xcode with its command-line tools,
CMake 3.22+, Git, and internet access on the first configure. JUCE **9.0.4** is
fetched at commit `3fb5cbc0340758596dac39f7c8de47d562b4896e`; no separate VST SDK
or CoreAudio SDK installation is needed.

```bash
cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release --parallel 4
```

Release outputs:

- `build/arm64-release/HelmPlugin_artefacts/Release/VST3/Helm.vst3`
- `build/arm64-release/HelmPlugin_artefacts/Release/AU/Helm.component`
- `build/arm64-release/HelmStandalone_artefacts/Release/Helm.app`

Use `macos-arm64-debug` for Debug builds. Both presets explicitly select arm64;
Intel and Universal binaries are not part of this build. Factory presets, images,
and fonts are included. User presets retain their existing location under
`~/Library/Audio/Presets/Helm`.

Builds are ad-hoc signed for local use and do not install anything automatically.
To install, copy `Helm.vst3` to `~/Library/Audio/Plug-Ins/VST3`, `Helm.component`
to `~/Library/Audio/Plug-Ins/Components`, and `Helm.app` to your Applications
folder. Restart/rescan your native DAW after installation; AU discovery may
require logging out and back in. Distribution signing, notarization, and an
installer are not included.

Validation:

```bash
cmake --build build/arm64-release --target HelmPluginTests --parallel 4
ctest --test-dir build/arm64-release --output-on-failure
cmake -DBUILD_DIR=build/arm64-release -DCONFIG=Release -P cmake/VerifyBundles.cmake
```

The tests load the actual VST3 and AU bundles, check 139 legacy parameter names
and IDs, restore state and presets, and render MIDI at 44.1/48/96 kHz with mono
and stereo output and block sizes 1/64/255/256/257/1024. AU tests register the
factory in the test process, without installing the plugin. CTest isolates
presets/configuration in the build directory. For editor lifecycle checks, run
`HelmPluginTests` with the absolute plugin path, absolute
`tests/legacy-parameters.txt` path, and `--editor`.

For independent validation, run pluginval at strictness level 5 against the VST3
bundle. After installing and registering the AU, run `auval -v aumu Helm Tyte -strict`.
The GitHub Actions macOS workflow builds both configurations, runs regression
tests, verifies architecture/deployment target/signatures, and archives the bundles.

The CMake build is isolated from the old Projucer-generated Windows/Linux projects
and their vendored JUCE. Shared-source compatibility guards retain those legacy
build paths; they are not modernized or validated by the macOS workflow.

**Dependency licensing:** Helm's own source retains its existing GPLv3-or-later
notices. JUCE 9 is dual-licensed under AGPLv3 and the commercial JUCE licence;
see [JUCE 9's licence](https://github.com/juce-framework/JUCE/blob/9.0.4/LICENSE.md)
and its bundled dependency notices when distributing this build. The upgrade
does not select or purchase a commercial licence.

#### Windows
Open /standalone/builds/vs15/Helm.sln for standalone version  
Open /builds/vs15/Helm.sln for plugin versions

### Features:
 - 32 voice polyphony
 - Interactive visual interface
 - Powerful modulation system with live visual feedback
 - Dual oscillators with cross modulation and up to 15 unison oscillators each
 - Sub oscillator with shuffle waveshaping
 - Oscillator feedback and saturation for waveshaping
 - 12 different waveforms
 - Blending between 12 or 24dB low/band/high pass filter
 - Low/Band/High Shelf filters
 - 2 monophonic and 1 polyphonic LFO
 - Step sequencer
 - Lots of modulation sources including polyphonic aftertouch
 - Simple arpeggiator
 - Effects: Formant filter, stutter, delay, distortion, reverb

![alt tag](http://tytel.org/static/images/helm_screenshot.png)

## Browser / WebAssembly

Helm also builds its original C++ engine and JUCE interface for the browser.
See [web/README.md](web/README.md) for the pinned toolchain, local server,
AudioWorklet requirements, tests and static distribution instructions.
