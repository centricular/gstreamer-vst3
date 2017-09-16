# gstreamer-vst3

[GStreamer](https://gstreamer.freedesktop.org/) plugin for
[Steinberg VST3](https://www.steinberg.net/en/company/technologies/vst3.html) audio plugins.

Currently only mono and stereo plugins that implement the `IAudioProcessor`
interface are supported. The plugin should work fine on Linux, Windows and
macOS.

To compile this, the [VST3 SDK](https://www.steinberg.net/en/company/developers.html) has to
be downloaded and compiled. The paths of the SDK and the build directory must
be provided to meson via `-Dvst-sdkdir=...` and `-Dvst-libdir=...`

Some sample VST plugins are included in the SDK.

## LICENSE

GStreamer and gstreamer-vst3 is licensed under the [Lesser General Public
License version 2.1](COPYING) or (at your option) any later version.

The Steinberg VST3 SDK is licensed under the [3-clause BSD
license](https://opensource.org/licenses/BSD-3-Clause).
