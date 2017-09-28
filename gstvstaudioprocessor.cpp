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

#include <sys/types.h>
#include <sys/stat.h>
/* For g_stat () */
#include <glib/gstdio.h>

#include "plugin.h"
#include "gstvstaudioprocessor.h"

#include <gst/audio/audio.h>
#include <gst/base/base.h>

#include <common/memorystream.h>
#include <vst/hosting/module.h>
#include <vst/vstcomponent.h>
#include <vst/vsteditcontroller.h>
#include <vst/hosting/stringconvert.h>
#include <vst/hosting/parameterchanges.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>

#if defined(G_OS_WIN32)
#include <windows.h>
#include <shlobj.h>
#pragma comment(lib, "Shell32")
#endif

GST_DEBUG_CATEGORY_STATIC(gst_vst_audio_processor_debug);
#define GST_CAT_DEFAULT gst_vst_audio_processor_debug

using namespace Steinberg;

static GstElementClass *parent_class = nullptr;
static GQuark audio_processor_info_quark;

static void gst_vst_audio_processor_class_init(GstVstAudioProcessorClass * klass);
static void gst_vst_audio_processor_sub_class_init(GstVstAudioProcessorClass * klass);
static void gst_vst_audio_processor_init(GstVstAudioProcessor * self, GstVstAudioProcessorClass * klass);

static GstFlowReturn gst_vst_audio_processor_sink_chain(GstPad * pad,
    GstObject * parent, GstBuffer * buffer);
static gboolean gst_vst_audio_processor_sink_event(GstPad * pad,
    GstObject * parent, GstEvent * event);
static gboolean gst_vst_audio_processor_src_query(GstPad * pad,
    GstObject * parent, GstQuery * query);

static void gst_vst_audio_processor_finalize(GObject * object);
static void gst_vst_audio_processor_get_property(GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static void gst_vst_audio_processor_set_property(GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);

static void gst_vst_audio_processor_sub_get_property(GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static void gst_vst_audio_processor_sub_set_property(GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);

static GstStateChangeReturn gst_vst_audio_processor_change_state(GstElement *
    element, GstStateChange transition);

// The different states the audio processor can be in
typedef enum {
  STATE_NONE = 0,
  STATE_CREATED,
  STATE_INITIALIZED,
  STATE_SETUP,
  STATE_ACTIVE,
  STATE_PROCESSING,
} State;

enum {
  PROP_0 = 0,
  PROP_MAX_SAMPLES_PER_CHUNK,
};

#define DEFAULT_MAX_SAMPLES_PER_CHUNK (1024)

// Communication between edit controller and component happens over this. We
// don't really use this as we don't want to use the GUI provided by the
// controller.
class GstVstAudioProcessorComponentHandler: public Vst::IComponentHandler {
public:
  GstVstAudioProcessorComponentHandler(GstVstAudioProcessor * processor): processor(processor) { FUNKNOWN_CTOR }

  virtual ~GstVstAudioProcessorComponentHandler() { FUNKNOWN_DTOR }

  DECLARE_FUNKNOWN_METHODS

  tresult PLUGIN_API beginEdit(Vst::ParamID id) override {
    GST_FIXME_OBJECT(processor, "beginEdit not implemented");
    return kNotImplemented;
  }
  tresult PLUGIN_API performEdit(Vst::ParamID id, Vst::ParamValue valueNormalized) override {
    GST_FIXME_OBJECT(processor, "performEdit not implemented");
    return kNotImplemented;
  }
  tresult PLUGIN_API endEdit(Vst::ParamID id) override {
    GST_FIXME_OBJECT(processor, "endEdit not implemented");
    return kNotImplemented;
  }
  tresult PLUGIN_API restartComponent(int32 flags) override {
    // TODO: Maybe want to do something with the other ones?
    GST_DEBUG_OBJECT(processor, "restartComponent(0x%08x)", flags);

    if (flags & Vst::kLatencyChanged) {
      gst_element_post_message(GST_ELEMENT_CAST(processor),
          gst_message_new_latency(GST_OBJECT_CAST(processor)));
    }

    return kResultOk;
  }

  GstVstAudioProcessor * processor;
};

IMPLEMENT_REFCOUNT(GstVstAudioProcessorComponentHandler);
DECLARE_CLASS_IID(GstVstAudioProcessorComponentHandler, 0x8f2a46d5, 0x148a4e40, 0xab996b56, 0xc2c615cf);

tresult GstVstAudioProcessorComponentHandler::queryInterface(const char * iid, void ** obj) {
  QUERY_INTERFACE(iid, obj, FUnknown::iid, FUnknown);
  QUERY_INTERFACE(iid, obj, Vst::IComponentHandler::iid, Vst::IComponentHandler);
  *obj = nullptr;
  return kNoInterface;
}

struct _GstVstAudioProcessor {
  GstElement element;

  GstPad *srcpad, *sinkpad;

  // Properties
  gint max_samples_per_chunk;

  // Protected by object lock
  Vst::ParameterChanges *parameter_changes;
  gdouble *parameter_values;

  // State
  // Protected by stream lock
  GstSegment segment;
  GstAudioInfo info;
  GstClockTime latency;

  State state;
  std::shared_ptr<VST3::Hosting::Module> module;
  IPtr<Vst::IComponent> component;
  IPtr<Vst::IEditController> edit_controller;
  IPtr<Vst::IAudioProcessor> audio_processor;
  IPtr<GstVstAudioProcessorComponentHandler> component_handler;

  // Temporary buffer space used for deinterleaving
  gpointer in_data[2];
  gpointer out_data[2];
  guint data_len;
};

struct _GstVstAudioProcessorClass {
  GstElementClass parent_class;

  const GstVstAudioProcessorInfo *processor_info;
};

// Property definition
typedef struct {
  Vst::ParamID param_id;
  const gchar *name;
  const gchar *nick;
  const gchar *description;
  GType type;            // float, bool, int
  gint32 max_value;      // for int
  gdouble default_value;
  gboolean read_only;

  GParamSpec *pspec;
} GstVstAudioProcessorProperty;

// Class information extracted from component
struct _GstVstAudioProcessorInfo {
  const gchar *name;
  GstCaps *caps;
  const gchar *path;
  VST3::UID class_id;

  // properties[0] -> GObject property ID 1
  GstVstAudioProcessorProperty *properties;
  guint n_properties;
};

GType
gst_vst_audio_processor_get_type(void)
{
  static volatile gsize type = 0;

  if (g_once_init_enter(&type)) {
    GType _type;
    static const GTypeInfo info = {
      sizeof (GstVstAudioProcessorClass),
      nullptr,
      nullptr,
      (GClassInitFunc) gst_vst_audio_processor_class_init,
      nullptr,
      nullptr,
      sizeof (GstVstAudioProcessor),
      0,
      (GInstanceInitFunc) gst_vst_audio_processor_init,
      nullptr
    };

    _type = g_type_register_static(GST_TYPE_ELEMENT, "GstVstAudioProcessor",
        &info, (GTypeFlags) 0);

    g_once_init_leave(&type, _type);
  }
  return type;
}

static void
gst_vst_audio_processor_sub_class_init(GstVstAudioProcessorClass * klass)
{
  auto gobject_class = G_OBJECT_CLASS(klass);
  auto element_class = GST_ELEMENT_CLASS(klass);
  auto audio_processor_klass = GST_VST_AUDIO_PROCESSOR_CLASS(klass);

  auto processor_info = (const GstVstAudioProcessorInfo *)
      g_type_get_qdata(G_TYPE_FROM_CLASS(klass), audio_processor_info_quark);
  // This happens for the base class and abstract subclasses
  if (!processor_info)
    return;

  audio_processor_klass->processor_info = processor_info;

  // Add pad templates
  auto templ = gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, processor_info->caps);
  gst_element_class_add_pad_template(element_class, templ);

  templ = gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, processor_info->caps);
  gst_element_class_add_pad_template(element_class, templ);

  auto longname = g_strdup_printf("VST3 Audio processor - %s", processor_info->name);
  gst_element_class_set_metadata(element_class,
      processor_info->name,
      "Audio/Filter",
      longname, "Sebastian Dröge <sebastian@centricular.com>");
  g_free(longname);

  // Register all our properties, if any
  if (processor_info->n_properties > 0) {
    gobject_class->set_property = gst_vst_audio_processor_sub_set_property;
    gobject_class->get_property = gst_vst_audio_processor_sub_get_property;

    for (auto i = 0U; i < processor_info->n_properties; i++) {
      auto property = &processor_info->properties[i];
      GParamSpec *param_spec;

      if (property->type == G_TYPE_DOUBLE) {
        param_spec = g_param_spec_double(property->name, property->nick, property->description,
            -G_MAXDOUBLE, G_MAXDOUBLE,
            property->default_value,
            (GParamFlags) ((property->read_only ? G_PARAM_READABLE : G_PARAM_READWRITE) | GST_PARAM_CONTROLLABLE));
      } else if (property->type == G_TYPE_BOOLEAN) {
        param_spec = g_param_spec_boolean(property->name, property->nick, property->description,
            property->default_value > 0.5,
            (GParamFlags) ((property->read_only ? G_PARAM_READABLE : G_PARAM_READWRITE) | GST_PARAM_CONTROLLABLE));
      } else {
        param_spec = g_param_spec_int(property->name, property->nick, property->description,
            0, property->max_value,
            property->default_value,
            (GParamFlags) ((property->read_only ? G_PARAM_READABLE : G_PARAM_READWRITE) | GST_PARAM_CONTROLLABLE));
      }
      property->pspec = param_spec;

      g_object_class_install_property(gobject_class, i + 1, param_spec);
    }
  }
}

static void
gst_vst_audio_processor_class_init(GstVstAudioProcessorClass * klass)
{
  auto gobject_class = G_OBJECT_CLASS(klass);
  auto gstelement_class = GST_ELEMENT_CLASS(klass);

  parent_class = GST_ELEMENT_CLASS(g_type_class_peek_parent(klass));

  gobject_class->set_property = gst_vst_audio_processor_set_property;
  gobject_class->get_property = gst_vst_audio_processor_get_property;
  gobject_class->finalize = gst_vst_audio_processor_finalize;

  g_object_class_install_property (gobject_class, PROP_MAX_SAMPLES_PER_CHUNK,
      g_param_spec_int ("max-samples-per-chunk", "Max Samples per Chunk",
          "Maximum number of samples to process per chunk", 1,
          G_MAXINT, DEFAULT_MAX_SAMPLES_PER_CHUNK,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY)));

  gstelement_class->change_state = gst_vst_audio_processor_change_state;
}

static void
gst_vst_audio_processor_init(GstVstAudioProcessor * self, GstVstAudioProcessorClass * klass)
{
  auto sink_templ = gst_element_class_get_pad_template(GST_ELEMENT_CLASS(klass), "sink");
  self->sinkpad = gst_pad_new_from_template (sink_templ, "sink");
  gst_pad_set_chain_function (self->sinkpad,
      GST_DEBUG_FUNCPTR (gst_vst_audio_processor_sink_chain));
  gst_pad_set_event_function (self->sinkpad,
      GST_DEBUG_FUNCPTR (gst_vst_audio_processor_sink_event));
  GST_PAD_SET_PROXY_CAPS (self->sinkpad);
  gst_element_add_pad (GST_ELEMENT (self), self->sinkpad);

  auto src_templ = gst_element_class_get_pad_template(GST_ELEMENT_CLASS(klass), "src");
  self->srcpad = gst_pad_new_from_template (src_templ, "src");
  gst_pad_set_query_function (self->srcpad,
      GST_DEBUG_FUNCPTR (gst_vst_audio_processor_src_query));
  GST_PAD_SET_PROXY_CAPS (self->srcpad);
  gst_pad_use_fixed_caps (self->srcpad);
  gst_element_add_pad (GST_ELEMENT (self), self->srcpad);

  self->state = STATE_NONE;
  self->module = nullptr;
  self->component = nullptr;
  self->audio_processor = nullptr;
  self->edit_controller = nullptr;
  self->component_handler = nullptr;

  self->max_samples_per_chunk = DEFAULT_MAX_SAMPLES_PER_CHUNK;

  // Initialize all properties as stored here with their default values
  self->parameter_values = g_new0(gdouble, klass->processor_info->n_properties);
  for (auto i = 0U; i < klass->processor_info->n_properties; i++)
    self->parameter_values[i] = klass->processor_info->properties[i].default_value;
}

static void
gst_vst_audio_processor_finalize(GObject * object)
{
  auto self = GST_VST_AUDIO_PROCESSOR(object);

  if (self->parameter_changes)
    delete self->parameter_changes;
  g_free(self->parameter_values);

  G_OBJECT_CLASS(parent_class)->finalize(object);
}

static void
gst_vst_audio_processor_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec)
{
  auto self = GST_VST_AUDIO_PROCESSOR(object);

  switch (property_id) {
    case PROP_MAX_SAMPLES_PER_CHUNK:
      g_value_set_int (value, self->max_samples_per_chunk);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
gst_vst_audio_processor_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec)
{
  auto self = GST_VST_AUDIO_PROCESSOR(object);

  switch (property_id) {
    case PROP_MAX_SAMPLES_PER_CHUNK:
      self->max_samples_per_chunk = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
gst_vst_audio_processor_sub_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec)
{
  auto self = GST_VST_AUDIO_PROCESSOR(object);
  auto klass = GST_VST_AUDIO_PROCESSOR_GET_CLASS(self);

  if (property_id > klass->processor_info->n_properties + 1 || property_id == 0) {
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    return;
  }

  GST_OBJECT_LOCK(self);
  auto property = &klass->processor_info->properties[property_id - 1];
  if (property->type == G_TYPE_DOUBLE)
    g_value_set_double(value, self->parameter_values[property_id - 1]);
  else if (property->type == G_TYPE_BOOLEAN)
    g_value_set_boolean(value, self->parameter_values[property_id - 1] > 0.5);
  else
    g_value_set_int(value, self->parameter_values[property_id - 1]);
  GST_OBJECT_UNLOCK(self);
}

static void
gst_vst_audio_processor_sub_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec)
{
  auto self = GST_VST_AUDIO_PROCESSOR(object);
  auto klass = GST_VST_AUDIO_PROCESSOR_GET_CLASS(self);

  if (property_id > klass->processor_info->n_properties + 1 || property_id == 0) {
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    return;
  }

  GST_OBJECT_LOCK(self);
  auto property = &klass->processor_info->properties[property_id - 1];

  // Store value in our cache
  if (property->type == G_TYPE_DOUBLE) {
    self->parameter_values[property_id - 1] = g_value_get_double(value);
  } else if (property->type == G_TYPE_BOOLEAN) {
    self->parameter_values[property_id - 1] = g_value_get_boolean(value);
  } else {
    self->parameter_values[property_id - 1] = g_value_get_int(value);
  }

  // If we have an edit controller, convert our plain values to normalized
  // values and store the parameter changes and let the edit controller know
  // about it
  //
  // We always use plain values, but controller and component use normalized
  // values between 0.0 and 1.0
  if (self->edit_controller) {
    if (!self->parameter_changes)
      self->parameter_changes = new Vst::ParameterChanges();

    Steinberg::int32 idx = 0;
    auto queue = self->parameter_changes->addParameterData(property->param_id, idx);
    auto value = self->edit_controller->plainParamToNormalized(property->param_id,
        self->parameter_values[property_id - 1]);

    queue->addPoint(0, value, idx);
    self->edit_controller->setParamNormalized(property->param_id, value);
  }
  GST_OBJECT_UNLOCK(self);
}

static gboolean
gst_vst_audio_processor_open(GstVstAudioProcessor *self)
{
  auto klass = GST_VST_AUDIO_PROCESSOR_GET_CLASS(self);
  std::string err;
  std::string path(klass->processor_info->path);

  self->state = STATE_NONE;

  auto mod = VST3::Hosting::Module::create(path, err);
  if (!mod) {
    GST_ERROR_OBJECT(self, "Failed to load module '%s': %s", path.c_str(), err.c_str());
    return FALSE;
  }

  auto factory = mod->getFactory();

  auto component = factory.createInstance<Vst::IComponent>(klass->processor_info->class_id);
  if (!component) {
    GST_ERROR_OBJECT(self, "Failed to create instance for '%s'", klass->processor_info->name);
    return FALSE;
  }

  auto res = component->initialize(gStandardPluginContext);
  if (res != kResultOk) {
    GST_ERROR_OBJECT(self, "Component can't be initialized: 0x%08x", res);
    return FALSE;
  }

  // Check if this supports the IAudioProcessor interface
  IPtr<Vst::IAudioProcessor> audio_processor;
  Vst::IAudioProcessor *audio_processor_ptr = nullptr;
  if (component->queryInterface(Vst::IAudioProcessor::iid, (void **) &audio_processor_ptr) != kResultOk
      || !audio_processor_ptr) {
    GST_ERROR_OBJECT(self, "Component does not implement IAudioProcessor interface");
    return FALSE;
  }
  audio_processor = shared(audio_processor_ptr);

  // Get the controller
  IPtr<Vst::IEditController> edit_controller;
  Vst::IEditController *edit_controller_ptr = nullptr;
  if (component->queryInterface(Vst::IEditController::iid, (void**) &edit_controller_ptr) != kResultOk
      || !edit_controller_ptr) {
    FUID controller_cid;

    // ask for the associated controller class ID
    if (component->getControllerClassId(controller_cid) == kResultOk && controller_cid.isValid ()) {
      // create its controller part created from the factory
      edit_controller = factory.createInstance<Vst::IEditController>(controller_cid.toTUID());
      if (edit_controller) {
        // initialize the component with our context
        res = edit_controller->initialize(gStandardPluginContext);
        if (res != kResultOk) {
          GST_ERROR_OBJECT(self, "Can't initialize edit controller: 0x%08x", res);
          return FALSE;
        }
      }
    }
  } else {
    edit_controller = shared(edit_controller_ptr);
  }

  if (!edit_controller) {
    GST_ERROR_OBJECT(self, "No edit controller found");
    return FALSE;
  }

  // activate busses, just in case
  res = component->activateBus(Vst::MediaTypes::kAudio, Vst::BusDirections::kInput, 0, TRUE);
  if (res != kResultOk) {
    GST_ERROR_OBJECT(self, "Failed to activate input bus: 0x%08x", res);
    return FALSE;
  }
  res = component->activateBus(Vst::MediaTypes::kAudio, Vst::BusDirections::kOutput, 0, TRUE);
  if (res != kResultOk) {
    GST_ERROR_OBJECT(self, "Failed to activate output bus: 0x%08x", res);
    return FALSE;
  }

  self->component_handler = owned(new GstVstAudioProcessorComponentHandler(self));

  // the host set its handler to the controller
  edit_controller->setComponentHandler(self->component_handler);

  // connect the 2 components
  Vst::IConnectionPoint* iConnectionPointComponent = nullptr;
  Vst::IConnectionPoint* iConnectionPointController = nullptr;
  component->queryInterface(Vst::IConnectionPoint::iid, (void**)&iConnectionPointComponent);
  edit_controller->queryInterface(Vst::IConnectionPoint::iid, (void**)&iConnectionPointController);
  if (iConnectionPointComponent && iConnectionPointController) {
    iConnectionPointComponent->connect(iConnectionPointController);
    iConnectionPointController->connect(iConnectionPointComponent);
  }

  // synchronize controller to component by using setComponentState
  MemoryStream stream;
  if (component->getState(&stream) == kResultOk) {
    stream.truncate();
    edit_controller->setComponentState(&stream);
  }

  // synchronize our cached property values with the component and controller
  self->parameter_changes = new Vst::ParameterChanges();
  for (auto i = 0U; i < klass->processor_info->n_properties; i++) {
    auto property = &klass->processor_info->properties[i];

    if (property->read_only)
      continue;

    Steinberg::int32 idx = 0;
    auto queue = self->parameter_changes->addParameterData(property->param_id, idx);
    auto value = edit_controller->plainParamToNormalized(property->param_id,
        self->parameter_values[i]);

    queue->addPoint(0, value, idx);
    edit_controller->setParamNormalized(property->param_id, value);
  }

  self->state = STATE_INITIALIZED;
  self->module = mod;
  self->component = component;
  self->audio_processor = audio_processor;
  self->edit_controller = edit_controller;

  return TRUE;
}

static GstStateChangeReturn
gst_vst_audio_processor_change_state(GstElement * element,
    GstStateChange transition)
{
  auto self = GST_VST_AUDIO_PROCESSOR(element);
  auto state_ret = GST_STATE_CHANGE_SUCCESS;

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      gst_audio_info_init(&self->info);
      gst_segment_init(&self->segment, GST_FORMAT_TIME);
      if (!gst_vst_audio_processor_open(self))
        state_ret = GST_STATE_CHANGE_FAILURE;
      break;
    default:
      break;
  }

  state_ret = GST_ELEMENT_CLASS(parent_class)->change_state(element, transition);
  if (state_ret == GST_STATE_CHANGE_FAILURE)
    return state_ret;

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      if (self->state >= STATE_PROCESSING)
        self->audio_processor->setProcessing(false);
      if (self->state >= STATE_ACTIVE)
        self->component->setActive(false);
      self->state = STATE_SETUP;
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      if (self->state >= STATE_SETUP) {
        self->component->terminate();
        self->edit_controller->terminate();
      }
      self->state = STATE_NONE;
      self->audio_processor = nullptr;
      self->component = nullptr;
      self->edit_controller = nullptr;
      self->module = nullptr;
      self->component_handler = nullptr;

      g_free(self->in_data[0]);
      self->in_data[0] = nullptr;
      g_free(self->in_data[1]);
      self->in_data[1] = nullptr;
      g_free(self->out_data[0]);
      self->out_data[0] = nullptr;
      g_free(self->out_data[1]);
      self->out_data[1] = nullptr;
      self->data_len = 0;
      break;
    default:
      break;
  }

  return state_ret;
}

static void
deinterleave_data(GstVstAudioProcessor *self, gconstpointer in_data, guint len)
{
  if (self->info.finfo->format == GST_AUDIO_FORMAT_F32) {
    if (self->info.channels == 1) {
      memcpy (self->in_data[0], in_data, len * self->info.bpf);
    } else {
      const float *f_in_data = (const float *) in_data;

      for (auto i = 0; i < 2; i++) {
        float *f_out_data = (float *) self->in_data[i];

        for (auto j = 0U; j < len; j++) {
          f_out_data[j] = f_in_data[i + j * 2];
        }
      }
    }
  } else {
    if (self->info.channels == 1) {
      memcpy (self->in_data[0], in_data, len * self->info.bpf);
    } else {
      const double *f_in_data = (const double *) in_data;

      for (auto i = 0; i < 2; i++) {
        double *f_out_data = (double *) self->in_data[i];

        for (auto j = 0U; j < len; j++) {
          f_out_data[j] = f_in_data[i + j * 2];
        }
      }
    }
  }
}

static void
interleave_data(GstVstAudioProcessor *self, gpointer out_data, guint len)
{
  if (self->info.finfo->format == GST_AUDIO_FORMAT_F32) {
    if (self->info.channels == 1) {
      memcpy (out_data, self->out_data[0], len * self->info.bpf);
    } else {
      float *f_out_data = (float *) out_data;

      for (auto i = 0; i < 2; i++) {
        const float *f_in_data = (const float *) self->out_data[i];

        for (auto j = 0U; j < len; j++) {
          f_out_data[i + j * 2] = f_in_data[j];
        }
      }
    }
  } else {
    if (self->info.channels == 1) {
      memcpy (out_data, self->out_data[0], len * self->info.bpf);
    } else {
      double *f_out_data = (double *) out_data;

      for (auto i = 0; i < 2; i++) {
        const double *f_in_data = (const double *) self->out_data[i];

        for (auto j = 0U; j < len; j++) {
          f_out_data[i + j * 2] = f_in_data[j];
        }
      }
    }
  }
}

static GstFlowReturn
gst_vst_audio_processor_sink_chain(GstPad * pad, GstObject * parent,
    GstBuffer * in_buffer)
{
  auto self = GST_VST_AUDIO_PROCESSOR(parent);
  auto klass = GST_VST_AUDIO_PROCESSOR_GET_CLASS(self);
  auto ret = GST_FLOW_OK;

  if (self->state < STATE_SETUP) {
    gst_buffer_unref(in_buffer);
    GST_ERROR_OBJECT(self, "Not negotiated yet");
    return GST_FLOW_NOT_NEGOTIATED;
  }

  if (!GST_BUFFER_PTS_IS_VALID (in_buffer)) {
    GST_ERROR_OBJECT(self, "Need buffers with valid timestamps");
    gst_buffer_unref(in_buffer);
    return GST_FLOW_ERROR;
  }

  // FIXME: Can we drain somehow? We should on disconts
  if (GST_BUFFER_IS_DISCONT (in_buffer)) {
    GST_DEBUG_OBJECT(self, "Discontinuity, restarting component");
    if (self->state >= STATE_PROCESSING)
      self->audio_processor->setProcessing(false);
    if (self->state >= STATE_ACTIVE)
      self->component->setActive(false);
  }

  if (self->state < STATE_ACTIVE) {
    GST_DEBUG_OBJECT(self, "Activating component");
    auto res = self->component->setActive(true);
    if (res != kResultOk) {
      GST_ERROR_OBJECT(self, "Failed to set active: %08x", res);
      gst_buffer_unref(in_buffer);
      return GST_FLOW_ERROR;
    }

    self->state = STATE_ACTIVE;
  }

  if (self->state < STATE_PROCESSING) {
    GST_DEBUG_OBJECT(self, "Set component to processing");
    auto res = self->audio_processor->setProcessing(true);
    if (res != kResultOk && res != kNotImplemented) {
      GST_ERROR_OBJECT(self, "Failed to set processing: %08x", res);
      gst_buffer_unref(in_buffer);
      return GST_FLOW_ERROR;
    }

    self->state = STATE_PROCESSING;
  }

  // We process the input buffer in chunks of at most the configured
  // max-samples-per-chunk, and while doing so keep track of our current
  // timestamp, stream time and sample position
  GstMapInfo in_map;
  gst_buffer_map(in_buffer, &in_map, GST_MAP_READ);

  auto in_data = in_map.data;
  auto num_samples = in_map.size / self->info.bpf;

  auto stream_time = gst_segment_to_stream_time(&self->segment, GST_FORMAT_TIME, GST_BUFFER_PTS(in_buffer));
  auto sample_position = (gint64) gst_util_uint64_scale(GST_BUFFER_PTS(in_buffer), self->info.rate, GST_SECOND);
  auto sample_start_position = sample_position;

  do {
    gst_object_sync_values(GST_OBJECT_CAST(self), stream_time +
        gst_util_uint64_scale(sample_position - sample_start_position, GST_SECOND, self->info.rate));

    auto chunk_size = MIN(self->data_len, num_samples);

    // Fill input buffers and metadata
    deinterleave_data(self, (gconstpointer) in_data, chunk_size);
    Vst::AudioBusBuffers input;
    input.numChannels = self->info.channels;
    input.silenceFlags = GST_BUFFER_FLAG_IS_SET(in_buffer, GST_BUFFER_FLAG_GAP) ? G_MAXUINT64 : 0;
    if (self->info.finfo->format == GST_AUDIO_FORMAT_F32)
      input.channelBuffers32 = (Vst::Sample32 **) self->in_data;
    else
      input.channelBuffers64 = (Vst::Sample64 **) self->in_data;

    // Fill output buffer metadata
    Vst::AudioBusBuffers output;
    output.numChannels = self->info.channels;
    output.silenceFlags = 0;
    if (self->info.finfo->format == GST_AUDIO_FORMAT_F32)
      output.channelBuffers32 = (Vst::Sample32 **) self->out_data;
    else
      output.channelBuffers64 = (Vst::Sample64 **) self->out_data;

    // Set up process context with information about the system state
    Vst::ProcessContext process_context;
    process_context.state = Vst::ProcessContext::kPlaying |
        Vst::ProcessContext::kRecording |
        Vst::ProcessContext::kSystemTimeValid;
    process_context.sampleRate = self->info.rate;
    process_context.projectTimeSamples = sample_position;
    // FIXME: Should we pretend real-time processing here?
    process_context.systemTime = gst_util_get_timestamp();

    // Set up process data
    Vst::ProcessData data;
    data.processMode = Vst::kPrefetch;
    data.symbolicSampleSize = self->info.finfo->format == GST_AUDIO_FORMAT_F32 ? Vst::kSample32 : Vst::kSample64;
    data.numSamples = chunk_size;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = &input;
    data.outputs = &output;
    data.processContext = &process_context;

    // Check if we have any pending input parameter changes
    GST_OBJECT_LOCK(self);
    auto parameter_changes = self->parameter_changes;
    self->parameter_changes = nullptr;
    GST_OBJECT_UNLOCK(self);
    data.inputParameterChanges = parameter_changes;

    // Allocate space for any output parameter changes
    Vst::ParameterChanges out_parameter_changes;
    data.outputParameterChanges = &out_parameter_changes;

    // And finally do the actual processing of this chunk
    auto res = self->audio_processor->process(data);

    // We have to delete the pointer here, the processor does not do that
    if (parameter_changes) {
      delete parameter_changes;
      data.inputParameterChanges = nullptr;
    }

    // Update out parameter changes
    auto out_changes_count = out_parameter_changes.getParameterCount();
    for (auto i = 0; i < out_changes_count; i++) {
      auto queue = out_parameter_changes.getParameterData(i);
      auto point_count = queue->getPointCount();
      if (point_count > 0) {
        Vst::ParamValue value;
        Steinberg::int32 sample_offset = 0;

        if (queue->getPoint(point_count - 1, sample_offset, value) == kResultOk) {
          GstVstAudioProcessorProperty *prop = nullptr;
          auto param_id = queue->getParameterId();
          auto plain_value = self->edit_controller->normalizedParamToPlain(param_id, value);

          guint k;
          for (k = 0; k < klass->processor_info->n_properties; k++) {
            if (klass->processor_info->properties[k].param_id == param_id) {
              prop = &klass->processor_info->properties[k];
              break;
            }
          }

          // Cache the new value and notify anybody interested
          if (prop) {
            self->parameter_values[k] = plain_value;
            g_object_notify_by_pspec(G_OBJECT(self), prop->pspec);

          }

          // And let the edit controller know about this change too
          self->edit_controller->setParamNormalized(param_id, value);
        }
      }
    }

    if (res != kResultOk) {
      ret = GST_FLOW_ERROR;
      break;
    }

    // If there's output, allocate a new buffer and fill it
    // FIXME: We assume that input length == output length currently
    // for the timestamp calculation. This is not necessarily true:
    // there could be latency involved. But none of the plugins this was
    // tested with makes use of that
    if (data.numSamples != (gint) chunk_size) {
      GST_FIXME_OBJECT(self, "Output number of samples different than input: %d != %lu", data.numSamples, chunk_size);
    }

    if (data.numSamples > 0) {
      auto out_buffer = gst_buffer_new_and_alloc(data.numSamples * self->info.bpf);
      GstMapInfo out_map;

      gst_buffer_map(out_buffer, &out_map, GST_MAP_WRITE);
      auto out_data = out_map.data;
      interleave_data(self, (gpointer) out_data, data.numSamples);
      gst_buffer_unmap(out_buffer, &out_map);

      GST_BUFFER_PTS(out_buffer) = GST_BUFFER_PTS(in_buffer) +
          gst_util_uint64_scale(sample_position - sample_start_position, GST_SECOND, self->info.rate);
      GST_BUFFER_DURATION(out_buffer) = gst_util_uint64_scale(chunk_size, GST_SECOND, self->info.rate);

      ret = gst_pad_push(self->srcpad, out_buffer);
    }

    num_samples -= chunk_size;
    in_data += chunk_size * self->info.bpf;
    sample_position += chunk_size;
  } while (ret == GST_FLOW_OK && num_samples > 0);

  gst_buffer_unmap(in_buffer, &in_map);
  gst_buffer_unref(in_buffer);

  return ret;
}

static gboolean
gst_vst_audio_processor_sink_event(GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  auto self = GST_VST_AUDIO_PROCESSOR(parent);
  gboolean ret = FALSE;

  switch (GST_EVENT_TYPE(event)) {
    case GST_EVENT_CAPS:{
      GstCaps *caps;
      GstAudioInfo info;
      gboolean changed;

      gst_event_parse_caps(event, &caps);

      ret = gst_audio_info_from_caps(&info, caps);
      changed = ret && !gst_audio_info_is_equal(&info, &self->info);
      if (ret && changed) {
        GST_DEBUG_OBJECT(self, "Got caps %" GST_PTR_FORMAT, caps);

        self->info = info;

        // Need to shut down the component to be able to configure any new
        // sample rate, channel configuration or sample format
        // FIXME: Can we drain somehow?
        if (self->state >= STATE_PROCESSING)
          self->audio_processor->setProcessing(false);
        if (self->state >= STATE_ACTIVE)
          self->component->setActive(false);
        self->state = STATE_SETUP;

        Vst::SpeakerArrangement arrangement[1] = { info.channels == 1 ? Vst::SpeakerArr::kMono : Vst::SpeakerArr::kStereo };
        auto res = self->audio_processor->setBusArrangements(arrangement, 1, arrangement, 1);
        if (res != kResultOk) {
          GST_ERROR_OBJECT(self, "Failed to set bus arrangments: 0x%08x", res);
          self->state = STATE_INITIALIZED;
          ret = FALSE;
          gst_event_unref(event);
          break;
        }

        Vst::ProcessSetup setup = {
          Vst::kPrefetch,
          info.finfo->format == GST_AUDIO_FORMAT_F32 ? Vst::kSample32 : Vst::kSample64,
          self->max_samples_per_chunk,
          (double) info.rate
        };

        res = self->audio_processor->setupProcessing(setup);
        if (res != kResultOk) {
          GST_ERROR_OBJECT(self, "Failed to setup processing: %08x", res);
          self->state = STATE_INITIALIZED;
          ret = FALSE;
          gst_event_unref(event);
          break;
        }

        // Reallocate our buffers for both channel
        auto bps = info.bpf / info.channels;
        g_free(self->in_data[0]);
        self->in_data[0] = g_malloc0(bps * self->max_samples_per_chunk);
        g_free(self->in_data[1]);
        self->in_data[1] = info.channels == 2 ? g_malloc0(bps * self->max_samples_per_chunk) : nullptr;
        g_free(self->out_data[0]);
        self->out_data[0] = g_malloc0(bps * self->max_samples_per_chunk);
        g_free(self->out_data[1]);
        self->out_data[1] = info.channels == 2 ? g_malloc0(bps * self->max_samples_per_chunk) : nullptr;
        self->data_len = self->max_samples_per_chunk;

        // Update latency
        auto latency_samples = self->audio_processor->getLatencySamples();
        auto latency = gst_util_uint64_scale_int(latency_samples, GST_SECOND, info.rate);
        if (latency != self->latency) {
          self->latency = latency;
          GST_DEBUG_OBJECT(self, "Latency changed to %" GST_TIME_FORMAT, GST_TIME_ARGS(latency));
          gst_element_post_message(GST_ELEMENT_CAST(self), gst_message_new_latency(GST_OBJECT_CAST(self)));
        }

        GST_DEBUG_OBJECT(self, "Finished setup for new caps");

        self->state = STATE_SETUP;
      } else if (!ret) {
        GST_ERROR_OBJECT(self, "Invalid caps");
        ret = FALSE;
      } else {
        // Nothing changed
        ret = TRUE;
      }

      if (ret)
        ret = gst_pad_event_default(pad, parent, event);
      else
        gst_event_unref (event);

      break;
    }
    case GST_EVENT_FLUSH_STOP:
      gst_segment_init(&self->segment, GST_FORMAT_TIME);
      // Shut down component, it will be started again on next buffer
      // FIXME: Is there a better way of flushing?
      if (self->state >= STATE_PROCESSING)
        self->audio_processor->setProcessing(false);
      if (self->state >= STATE_ACTIVE)
        self->component->setActive(false);
      self->state = STATE_SETUP;
      ret = gst_pad_event_default(pad, parent, event);
      break;
    case GST_EVENT_SEGMENT:
      gst_event_copy_segment(event, &self->segment);
      if (self->segment.format != GST_FORMAT_TIME) {
        gst_event_unref(event);
        ret = FALSE;
      } else {
        ret = gst_pad_event_default(pad, parent, event);
      }
      break;
    case GST_EVENT_EOS:
      // FIXME: Can we drain somehow?
      ret = gst_pad_event_default(pad, parent, event);
      break;
    default:
      ret = gst_pad_event_default(pad, parent, event);
      break;
  }

  return ret;
}

static gboolean
gst_vst_audio_processor_src_query(GstPad * pad,
    GstObject * parent, GstQuery * query)
{
  auto self = GST_VST_AUDIO_PROCESSOR(parent);
  gboolean ret = FALSE;

  switch (GST_QUERY_TYPE(query)) {
    case GST_QUERY_LATENCY:{
      if ((ret = gst_pad_peer_query(self->sinkpad, query))) {
        GstClockTime latency;
        GstClockTime min, max;
        gboolean live;

        gst_query_parse_latency(query, &live, &min, &max);

        GST_DEBUG_OBJECT(self, "Peer latency: min %"
            GST_TIME_FORMAT " max %" GST_TIME_FORMAT,
            GST_TIME_ARGS(min), GST_TIME_ARGS(max));

        latency = self->latency;

        GST_DEBUG_OBJECT(self, "Our latency: min %" GST_TIME_FORMAT
            ", max %" GST_TIME_FORMAT,
            GST_TIME_ARGS(latency), GST_TIME_ARGS(latency));

        min += latency;
        if (max != GST_CLOCK_TIME_NONE)
          max += latency;

        GST_DEBUG_OBJECT(self, "Calculated total latency : min %"
            GST_TIME_FORMAT " max %" GST_TIME_FORMAT,
            GST_TIME_ARGS(min), GST_TIME_ARGS(max));

        gst_query_set_latency(query, live, min, max);
      }

      break;
    }
    default:
      ret = gst_pad_query_default(pad, parent, query);
      break;
  }

  return ret;
}

static gchar *
create_type_name(const gchar * parent_name, const gchar * class_name)
{
  auto parent_name_len = strlen(parent_name);
  auto class_name_len = strlen(class_name);
  auto upper = true;

  auto typified_name = g_new0(gchar, parent_name_len + 1 + class_name_len + 1);
  memcpy(typified_name, parent_name, parent_name_len);
  typified_name[parent_name_len] = '-';

  for (auto i = 0U, k = 0U; i < class_name_len; i++) {
    if (g_ascii_isalnum(class_name[i])) {
      if (upper)
        typified_name[parent_name_len + 1 + k++] =
            g_ascii_toupper(class_name[i]);
      else
        typified_name[parent_name_len + 1 + k++] =
            g_ascii_tolower(class_name[i]);

      upper = false;
    } else {
      /* Skip all non-alnum chars and start a new upper case word */
      upper = true;
    }
  }

  return typified_name;
}

static gchar *
create_element_name(const gchar * prefix, const gchar * class_name)
{
  auto prefix_len = strlen(prefix);
  auto class_name_len = strlen(class_name);

  auto element_name = g_new0(gchar, prefix_len + class_name_len + 1);
  memcpy(element_name, prefix, prefix_len);

  for (auto i = 0U, k = 0U; i < class_name_len; i++) {
    if (g_ascii_isalnum(class_name[i])) {
      element_name[prefix_len + k++] = g_ascii_tolower(class_name[i]);
    }
    /* Skip all non-alnum chars */
  }

  return element_name;
}

#if defined(__linux__) && !defined(__BIONIC__)
static void
register_system_dependencies(GstPlugin *plugin)
{
  gst_plugin_add_dependency_simple (plugin, NULL, "/usr/lib/vst3:/usr/local/lib/vst3", ".vst3",
      (GstPluginDependencyFlags) (GST_PLUGIN_DEPENDENCY_FLAG_RECURSE |
      GST_PLUGIN_DEPENDENCY_FLAG_FILE_NAME_IS_SUFFIX));
  gst_plugin_add_dependency_simple (plugin, "HOME/.vst3", NULL, ".vst3",
      (GstPluginDependencyFlags) (GST_PLUGIN_DEPENDENCY_FLAG_RECURSE |
      GST_PLUGIN_DEPENDENCY_FLAG_FILE_NAME_IS_SUFFIX));
}
#elif defined(G_OS_WIN32)
static void
register_system_dependencies(GstPlugin *plugin)
{
  PWSTR wideStr {};
  char commonpath[MAX_PATH];
  gchar *path;
  if (FAILED (SHGetKnownFolderPath (FOLDERID_ProgramFilesCommon, 0, NULL, &wideStr)))
    return;

  if (!WideCharToMultiByte(CP_ACP, 0, wideStr, -1, commonpath, MAX_PATH, NULL, NULL))
    return;

  path = g_build_filename (commonpath, "VST3", NULL);
  gst_plugin_add_dependency_simple (plugin, NULL, path, ".vst3",
      (GstPluginDependencyFlags) (GST_PLUGIN_DEPENDENCY_FLAG_RECURSE |
      GST_PLUGIN_DEPENDENCY_FLAG_FILE_NAME_IS_SUFFIX));
  g_free (path);
}
#else
static void
register_system_dependencies(GstPlugin *plugin)
{
  GST_FIXME_OBJECT (plugin, "Implement plugin dependencies support for this platform");
}
#endif

static void
list_paths_with_vst3_extension (VST3::Hosting::Module::PathList &paths,
    const gchar *path, gboolean recurse)
{
  GDir *dir;
  const gchar *dirent;
  gchar *filename;

  dir = g_dir_open (path, 0, NULL);
  if (!dir)
    return;

  while ((dirent = g_dir_read_name (dir))) {
    GStatBuf file_status;

    filename = g_build_filename (path, dirent, NULL);
    if (g_stat (filename, &file_status) < 0) {
      g_free (filename);
      continue;
    }

    if (g_str_has_suffix (dirent, ".vst3"))
      paths.push_back (filename);

    if ((file_status.st_mode & S_IFDIR) && recurse)
      list_paths_with_vst3_extension (paths, filename, recurse);

    g_free (filename);
  }

  g_dir_close (dir);
}

void
gst_vst_audio_processor_register(GstPlugin * plugin)
{
  const gchar *main_exe_path;
  const gchar *paths_env_var;
  const gchar *search_default_paths_env_var;
  gboolean search_default_paths = TRUE;
  VST3::Hosting::Module::PathList paths;

  GST_DEBUG_CATEGORY_INIT (gst_vst_audio_processor_debug, "vst-audio-processor", 0,
      "VST Audio Processor");

  search_default_paths_env_var = g_getenv("GST_VST3_SEARCH_DEFAULT_PATHS");
  if (search_default_paths_env_var)
    search_default_paths = g_ascii_strcasecmp(search_default_paths_env_var, "no");
  gst_plugin_add_dependency_simple (plugin, "GST_VST3_SEARCH_DEFAULT_PATHS", NULL, NULL,
      GST_PLUGIN_DEPENDENCY_FLAG_NONE);

  GST_INFO ("Search default paths: %d", search_default_paths);

  register_system_dependencies(plugin);

  audio_processor_info_quark = g_quark_from_static_string ("gst-vst-audio-processor-info");

  if (search_default_paths)
    paths = VST3::Hosting::Module::getModulePaths();

#ifdef HAVE_GST_EXE_PATH
  main_exe_path = gst_get_main_executable_path ();
  if (main_exe_path && search_default_paths) {
    gchar *appdir = g_path_get_dirname (main_exe_path);
    gchar *vst3_exe_path;
#if defined(__linux__) && !defined(__BIONIC__)
    const gchar vst3_path[] = "vst3";
#else
    const gchar vst3_path[] = "VST3";
#endif

    vst3_exe_path = g_build_filename (appdir, vst3_path, NULL);

    GST_INFO_OBJECT (plugin, "Looking up plugins in executable path %s", vst3_exe_path);
    list_paths_with_vst3_extension (paths, vst3_exe_path, TRUE);
    gst_plugin_add_dependency_simple (plugin, NULL, vst3_path, ".vst3",
        (GstPluginDependencyFlags) (GST_PLUGIN_DEPENDENCY_FLAG_RECURSE |
        GST_PLUGIN_DEPENDENCY_FLAG_PATHS_ARE_RELATIVE_TO_EXE |
        GST_PLUGIN_DEPENDENCY_FLAG_FILE_NAME_IS_SUFFIX));
    g_free (vst3_exe_path);
  }
#endif

  paths_env_var = g_getenv ("GST_VST3_PLUGIN_PATH");
  if (paths_env_var) {
    GStrv path_list = g_strsplit (paths_env_var, G_SEARCHPATH_SEPARATOR_S, 0);
    int i;

    for (i = 0; path_list[i]; i++) {
      GST_INFO_OBJECT (plugin, "Looking up plugins in env path %s", path_list[i]);
      list_paths_with_vst3_extension (paths, path_list[i], TRUE);
    }
    g_strfreev (path_list);
  }
  gst_plugin_add_dependency_simple (plugin, "GST_VST3_PLUGIN_PATH", NULL, ".vst3",
      (GstPluginDependencyFlags) (GST_PLUGIN_DEPENDENCY_FLAG_RECURSE |
      GST_PLUGIN_DEPENDENCY_FLAG_FILE_NAME_IS_SUFFIX));

  for (auto& path: paths) {
    std::string err;
    auto mod = VST3::Hosting::Module::create(path, err);
    if (!mod) {
      GST_ERROR("Failed to load module '%s': %s", path.c_str(), err.c_str());
      continue;
    }

    GST_DEBUG("Loaded module '%s' with name '%s'", path.c_str(), mod->getName().c_str());

    auto factory = mod->getFactory();

    auto factory_info = factory.info();
    GST_DEBUG("Vendor: %s, URL: %s, e-mail: %s",
              factory_info.vendor().c_str(),
              factory_info.url().c_str(),
              factory_info.email().c_str());

    for (auto& class_info: factory.classInfos()) {
      GST_DEBUG("\t Class: %s, category: %s", class_info.name().c_str(),
                class_info.category().c_str());

      GTypeQuery type_query;
      g_type_query(gst_vst_audio_processor_get_type(), &type_query);
      auto type_name = create_type_name(type_query.type_name, class_info.name().c_str());

      if (g_type_from_name (type_name)) {
        GST_DEBUG("\t Skipping already registered %s", type_name);
        continue;
      }

      auto component = factory.createInstance<Vst::IComponent>(class_info.ID());
      if (!component) {
        GST_DEBUG("\t Failed to create instance for '%s'", class_info.name().c_str());
        continue;
      }

      auto res = component->initialize(gStandardPluginContext);
      if (res != kResultOk) {
        GST_DEBUG("\t Component can't be initialized: 0x%08x", res);
        continue;
      }

      // Check if this supports the IAudioProcessor interface
      IPtr<Vst::IAudioProcessor> audio_processor;
      Vst::IAudioProcessor *audio_processor_ptr = nullptr;
      if (component->queryInterface(Vst::IAudioProcessor::iid, (void **) &audio_processor_ptr) != kResultOk
          || !audio_processor_ptr) {
          GST_DEBUG("\t Component does not implement IAudioProcessor interface");
          continue;
      }
      audio_processor = shared(audio_processor_ptr);

      // Get the controller
      IPtr<Vst::IEditController> edit_controller;
      Vst::IEditController *edit_controller_ptr = nullptr;
      if (component->queryInterface(Vst::IEditController::iid, (void**) &edit_controller_ptr) != kResultOk
          || !edit_controller_ptr) {
        FUID controller_cid;

        // ask for the associated controller class ID (could be called before processorComponent->initialize ())
        if (component->getControllerClassId(controller_cid) == kResultOk && controller_cid.isValid ()) {
          // create its controller part created from the factory
          edit_controller = factory.createInstance<Vst::IEditController>(controller_cid.toTUID());
          if (edit_controller) {
            // initialize the component with our context
            res = edit_controller->initialize(gStandardPluginContext);
            if (res != kResultOk) {
              GST_DEBUG("\t Can't initialize edit controller: 0x%08x", res);
              continue;
            }
          }
        }
      } else {
        edit_controller = shared(edit_controller_ptr);
      }

      if (!edit_controller) {
        GST_DEBUG("\t No edit controller found");
        continue;
      }

      // Get input audio bus. We only support components with a single audio
      // input and no event inputs
      auto count = component->getBusCount(Vst::MediaTypes::kAudio, Vst::BusDirections::kInput);
      if (count != 1) {
        GST_DEBUG("\t Unsupported number of audio input busses %d", count);
        continue;
      }

      count = component->getBusCount(Vst::MediaTypes::kEvent, Vst::BusDirections::kInput);
      if (count != 0) {
        GST_DEBUG("\t Unsupported number of event input busses %d", count);
        continue;
      }

      Vst::BusInfo bus_info;
      res = component->getBusInfo(Vst::MediaTypes::kAudio, Vst::BusDirections::kInput, 0, bus_info);
      if (res != kResultOk) {
        GST_DEBUG("\t Failed to get audio input bus info: 0x%08x", res);
        continue;
      }

      // TODO: Anything we can do with the bus info?

      // Get output audio bus. We only support components with a single audio
      // output and no event outputs
      count = component->getBusCount(Vst::MediaTypes::kAudio, Vst::BusDirections::kOutput);
      if (count != 1) {
        GST_DEBUG("\t Unsupported number of audio output busses %d", count);
        continue;
      }

      count = component->getBusCount(Vst::MediaTypes::kEvent, Vst::BusDirections::kOutput);
      if (count != 0) {
        GST_DEBUG("\t Unsupported number of event output busses %d", count);
        continue;
      }

      res = component->getBusInfo(Vst::MediaTypes::kAudio, Vst::BusDirections::kOutput, 0, bus_info);
      if (res != kResultOk) {
        GST_DEBUG("\t Failed to get audio output bus info: 0x%08x", res);
        continue;
      }

      // TODO: Anything we can do with the bus info?

      // Check which sample sizes the component supports
      auto caps = gst_caps_new_empty();

      if (audio_processor->canProcessSampleSize(Vst::kSample32) == kResultOk) {
        gst_caps_append(caps, gst_caps_from_string(
            "audio/x-raw, "
            "format=(string) " GST_AUDIO_NE (F32) ", "
            "layout=(string) interleaved, "
            "rate=(int) [0, MAX]"));
      }
      if (audio_processor->canProcessSampleSize(Vst::kSample64) == kResultOk) {
        gst_caps_append(caps, gst_caps_from_string(
            "audio/x-raw, "
            "format=(string) " GST_AUDIO_NE (F64) ", "
            "layout=(string) interleaved, "
            "rate=(int) [0, MAX]"));
      }

      // Check if the component can do mono-mono and/or stereo-stereo
      Vst::SpeakerArrangement inputs[1];
      Vst::SpeakerArrangement outputs[1];

      GValue channels = G_VALUE_INIT;
      GValue tmp = G_VALUE_INIT;
      g_value_init(&channels, GST_TYPE_LIST);
      g_value_init(&tmp, G_TYPE_INT);

      inputs[0] = outputs[0] = Vst::SpeakerArr::kMono;
      if (audio_processor->setBusArrangements(inputs, 1, outputs, 1) == kResultOk) {
        g_value_set_int(&tmp, 1);
        gst_value_list_append_value(&channels, &tmp);
      }

      inputs[0] = outputs[0] = Vst::SpeakerArr::kStereo;
      if (audio_processor->setBusArrangements(inputs, 1, outputs, 1) == kResultOk) {
        g_value_set_int(&tmp, 2);
        gst_value_list_append_value(&channels, &tmp);
      }

      if (gst_value_list_get_size(&channels) == 1) {
        gst_caps_set_simple(caps, "channels", G_TYPE_INT, g_value_get_int(&tmp), nullptr);
      } else {
        gst_caps_set_value(caps, "channels", &channels);
      }

      g_value_unset(&channels);
      g_value_unset(&tmp);

      // Get properties
      auto n_properties = edit_controller->getParameterCount();
      auto properties = g_new0(GstVstAudioProcessorProperty, n_properties);
      for (auto i = 0, k = 0; i < n_properties; i++) {
        Vst::ParameterInfo parameter_info;

        if (edit_controller->getParameterInfo(i, parameter_info) != kResultOk) {
          n_properties--;
          continue;
        }

        auto title = VST3::StringConvert::convert(parameter_info.title);
        auto short_title = VST3::StringConvert::convert(parameter_info.shortTitle);
        auto units = VST3::StringConvert::convert(parameter_info.units);

        properties[k].param_id = parameter_info.id;

        auto prop_name = g_ascii_strdown(short_title.length() != 0 ? short_title.c_str() : title.c_str(), -1);
        g_strcanon(prop_name, G_CSET_A_2_Z G_CSET_a_2_z G_CSET_DIGITS "-+", '-');
        /* satisfy glib2 (argname[0] must be [A-Za-z]) */
        if (!((prop_name[0] >= 'a' && prop_name[0] <= 'z') ||
              (prop_name[0] >= 'A' && prop_name[0] <= 'Z'))) {
          auto tempstr = prop_name;
          prop_name = g_strconcat("param-", prop_name, nullptr);
          g_free (tempstr);
        }

        auto duplicates = 0;
        for (auto j = 0; j < k; j++) {
          if (strcmp(properties[j].name, prop_name) == 0) {
            duplicates++;
          }
        }
        if (duplicates > 0) {
          auto tempstr = prop_name;
          prop_name = g_strdup_printf("%s-%d", prop_name, duplicates);
          g_free(tempstr);
        }

        properties[k].name = prop_name;
        properties[k].nick = g_strdup(short_title.length() != 0 ? short_title.c_str() : title.c_str());
        properties[k].description = units.length() != 0 ?
            g_strdup_printf("%s (%s)", title.c_str(), units.c_str()) :
            g_strdup(title.c_str());

        if (parameter_info.stepCount == 0) {
          properties[k].type = G_TYPE_DOUBLE;
        } else if (parameter_info.stepCount == 1) {
          properties[k].type = G_TYPE_BOOLEAN;
        } else {
          properties[k].type = G_TYPE_INT;
          properties[k].max_value = parameter_info.stepCount;
        }

        properties[k].default_value = edit_controller->normalizedParamToPlain(parameter_info.id,
            parameter_info.defaultNormalizedValue);
        properties[k].read_only = parameter_info.flags & Vst::ParameterInfo::kIsReadOnly;

        k++;
      }

      // And finally register our sub-type
      GTypeInfo type_info = { 0, };
      type_info.class_size = type_query.class_size;
      type_info.instance_size = type_query.instance_size;
      type_info.class_init = (GClassInitFunc) gst_vst_audio_processor_sub_class_init;

      auto type = g_type_register_static(gst_vst_audio_processor_get_type(), type_name, &type_info, (GTypeFlags) 0);

      auto processor_info = g_new0(GstVstAudioProcessorInfo, 1);
      processor_info->name = g_strdup(class_info.name().c_str());
      processor_info->caps = caps;
      processor_info->path = g_strdup(path.c_str());
      processor_info->class_id = class_info.ID();
      processor_info->properties = properties;
      processor_info->n_properties = n_properties;

      g_type_set_qdata(type, audio_processor_info_quark, processor_info);

      auto element_name =
          create_element_name("vstaudioprocessor-", processor_info->name);

      gst_element_register(plugin, element_name, GST_RANK_NONE, type);

      g_free(element_name);

      edit_controller->terminate();
      component->terminate();
    }
  }
}

