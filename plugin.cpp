/*
 * Copyright (C) 2017 Sebastian Dröge <sebastian@centricular.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "plugin.h"
#include <vst/hosting/hostclasses.h>
#include <vst/hosting/plugprovider.h>
#include <base/source/fstring.h>

#include <string>

#include "gstvstaudioprocessor.h"

using namespace Steinberg;

namespace Steinberg {
  DEF_CLASS_IID(Vst::IPlugProvider)

  FUnknown* gStandardPluginContext = nullptr;
};

class GStreamerHostApplication: public Vst::HostApplication {
public:
  GStreamerHostApplication() { }

  tresult PLUGIN_API getName(Vst::String128 name) override
  {
    String str ("GStreamer VST Plugin");
    str.copyTo16 (name, 0, 127);
    return kResultTrue;
  }
};

DECLARE_CLASS_IID(GStreamerHostApplication, 0x696c109c, 0x40dd4aed, 0xb272bebe, 0xc27b75d8)

static gboolean
plugin_init(GstPlugin * plugin)
{
  gStandardPluginContext = new GStreamerHostApplication();

  gst_vst_audio_processor_register(plugin);

  return TRUE;
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    vst3,
    "Steinberg VST3 plugin",
    plugin_init, VERSION, "LGPL", GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)

