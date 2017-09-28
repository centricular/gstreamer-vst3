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

## Environment variables

This plugin will parse two environment variables for the purpose of VST3
plugin discovery:

* `GST_VST3_PLUGIN_PATH`: A colon-separated list of paths to look for plugins
  into, eg `/home/foo:/home/bar`

* `GST_VST3_SEARCH_DEFAULT_PATHS`: If set to (case-insensitive) `no`, plugins
  will only be looked for in the paths listed in the `GST_VST3_PLUGIN_PATH`
  environment variable. The default behaviour is to search in the default paths.

## LICENSE

GStreamer and gstreamer-vst3 is licensed under the [Lesser General Public
License version 2.1](COPYING) or (at your option) any later version.

The Steinberg VST3 SDK is licensed under the [3-clause BSD
license](https://opensource.org/licenses/BSD-3-Clause).
