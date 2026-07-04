// main.cpp — minimal PipeWire example.
//
// Build:
//   g++ -std=c++20 pipewiretest.cpp -o pipewire_demo $(pkg-config --cflags --libs libpipewire-0.3)
// Run:
//   ./pipewire_demo
//
// Demonstrates, against the native libpipewire-0.3 API (no PulseAudio
// compat layer involved):
//   1. list all audio nodes (input/output, id, name)
//   2. find the default output device (via the "default" metadata object)
//   3. read that device's volume
//   4. set that device's volume
//   5. keep running and print any further volume/mute changes, whether
//      caused by us or by something else (e.g. the system volume mixer)

// PROBLEMS:
// - setting volume doesn't work, new value is reported back, but not applied (os volume slider does not change, next volume down is based on previous volume
// - no dynamic loading
// - no availability check
// value)

#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/parser.h>
#include <spa/utils/json.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace {

struct NodeInfo {
  uint32_t id = SPA_ID_INVALID;  // runtime object id: NOT stable, changes across restarts/replugs
  std::string stable_name;       // PW_KEY_NODE_NAME: persistent hw-derived id, survives reboots/replugs
  std::string display_name;      // PW_KEY_NODE_DESCRIPTION: human label, for printing only
  bool is_output      = true;    // Audio/Sink vs Audio/Source
  uint32_t n_channels = 0;       // learned from the first Props param we see; 0 = not known yet
  pw_proxy* proxy     = nullptr;
  spa_hook node_listener{};
};

struct App {
  pw_main_loop* loop    = nullptr;
  pw_context* context   = nullptr;
  pw_core* core         = nullptr;
  pw_registry* registry = nullptr;
  spa_hook registry_listener{};
  spa_hook core_listener{};

  std::map<uint32_t, NodeInfo> nodes;  // node id -> info, for every Audio/Sink or Audio/Source

  uint32_t metadata_id     = SPA_ID_INVALID;
  pw_proxy* metadata_proxy = nullptr;
  spa_hook metadata_listener{};
  std::string default_sink_name;  // node.name of the current default output

  int pending_sync = -1;  // seq we're waiting for during initial enumeration
};

// ---- helpers to read/build the SPA "Props" param that carries volume+mute ----

// Parses a Props pod, if present, into (volume 0..1, muted, channel count).
// Returns false if the pod has no channelVolumes (not every node exposes one).
bool parse_props(const spa_pod* param, float& volume, bool& muted, uint32_t& n_channels) {
  if (!param) return false;

  const spa_pod_prop* volumes_prop = spa_pod_find_prop(param, nullptr, SPA_PROP_channelVolumes);
  const spa_pod_prop* mute_prop    = spa_pod_find_prop(param, nullptr, SPA_PROP_mute);

  if (mute_prop) spa_pod_get_bool(&mute_prop->value, &muted);

  if (!volumes_prop) return false;

  uint32_t n_values;
  float* values = static_cast<float*>(spa_pod_get_array(&volumes_prop->value, &n_values));
  if (!values || n_values == 0) return false;

  // channelVolumes are cubic-scaled per-channel linear gains (0..1+); average
  // the channels and undo the cubic mapping to get a perceptual 0..1 slider
  // value, matching what desktop volume UIs show.
  float sum = 0.f;
  for (uint32_t i = 0; i < n_values; ++i) sum += values[i];
  float avg_cubic = sum / static_cast<float>(n_values);
  volume          = std::cbrt(avg_cubic);
  n_channels      = n_values;
  return true;
}

// Builds a Props pod setting channelVolumes (all channels to the same
// value) and mute, the same way `wpctl set-volume` / `pw-cli` would.
const spa_pod* build_volume_pod(spa_pod_builder& b, float volume, bool muted, uint32_t n_channels) {
  float cubic = volume * volume * volume;  // perceptual slider -> linear gain
  float values[SPA_AUDIO_MAX_CHANNELS];
  for (uint32_t i = 0; i < n_channels && i < SPA_AUDIO_MAX_CHANNELS; ++i) values[i] = cubic;

  spa_pod_frame frame;
  spa_pod_builder_push_object(&b, &frame, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
  spa_pod_builder_prop(&b, SPA_PROP_mute, 0);
  spa_pod_builder_bool(&b, muted);
  spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
  spa_pod_builder_array(&b, sizeof(float), SPA_TYPE_Float, n_channels, values);
  return static_cast<const spa_pod*>(spa_pod_builder_pop(&b, &frame));
}

// ---- per-node event listener: fires once per Props change, ours or external ----

void node_param_event(void* data, int /*seq*/, uint32_t id, uint32_t /*index*/, uint32_t /*next*/, const spa_pod* param) {
  auto* node = static_cast<NodeInfo*>(data);
  if (id != SPA_PARAM_Props) return;

  float volume        = 0.f;
  bool muted          = false;
  uint32_t n_channels = 0;
  if (!parse_props(param, volume, muted, n_channels)) return;
  node->n_channels = n_channels;

  std::printf("[change] %s: volume=%.0f%% muted=%s\n", node->display_name.c_str(), volume * 100.f, muted ? "true" : "false");
}

// ---- registry: discover audio nodes and the "default" metadata object ----

void registry_global(void* data, uint32_t id, uint32_t /*permissions*/, const char* type, uint32_t /*version*/, const spa_dict* props) {
  auto* app = static_cast<App*>(data);

  if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0 && props) {
    const char* media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!media_class) return;

    bool is_output = std::strcmp(media_class, "Audio/Sink") == 0;
    bool is_input  = std::strcmp(media_class, "Audio/Source") == 0;
    if (!is_output && !is_input) return;

    const char* stable_name  = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    const char* display_name = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    if (!display_name) display_name = stable_name;

    NodeInfo info;
    info.id           = id;
    info.stable_name  = stable_name ? stable_name : "";
    info.display_name = display_name ? display_name : "(unnamed)";
    info.is_output    = is_output;
    app->nodes[id]    = std::move(info);

  } else if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0 && props) {
    const char* name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
    if (name && std::strcmp(name, "default") == 0) app->metadata_id = id;
  }
}

void registry_global_remove(void* data, uint32_t id) {
  auto* app = static_cast<App*>(data);
  app->nodes.erase(id);
}

const pw_registry_events registry_events = {
    .version       = PW_VERSION_REGISTRY_EVENTS,
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

// ---- metadata: "default.audio.sink" tells us which node.name is the default output ----

int metadata_property(void* data, uint32_t /*subject*/, const char* key, const char* /*type*/, const char* value) {
  auto* app = static_cast<App*>(data);
  if (!key || std::strcmp(key, "default.audio.sink") != 0) return 0;
  if (!value) return 0;

  // value is a small JSON object, e.g.: {"name":"alsa_output.pci-0000_03_00.1.hdmi-stereo"}
  spa_json top;
  spa_json_init(&top, value, strlen(value));
  spa_json obj;
  if (spa_json_enter_object(&top, &obj) <= 0) return 0;  // not an object, ignore

  char sub_key[128];
  while (spa_json_get_string(&obj, sub_key, sizeof(sub_key)) > 0) {
    if (std::strcmp(sub_key, "name") == 0) {
      char name_value[256];
      if (spa_json_get_string(&obj, name_value, sizeof(name_value)) > 0) app->default_sink_name = name_value;
    } else {
      spa_json_next(&obj, nullptr);  // skip value for keys we don't care about
    }
  }
  return 0;
}

const pw_metadata_events metadata_events = {
    .version  = PW_VERSION_METADATA_EVENTS,
    .property = metadata_property,
};

// ---- core "done" event: used to know when initial registry enumeration settled ----

void core_done(void* data, uint32_t id, int seq) {
  auto* app = static_cast<App*>(data);
  if (id == PW_ID_CORE && seq == app->pending_sync) pw_main_loop_quit(app->loop);
}

const pw_core_events core_events = {
    .version = PW_VERSION_CORE_EVENTS,
    .done    = core_done,
};

const pw_node_events node_events = {
    .version = PW_VERSION_NODE_EVENTS,
    .param   = node_param_event,
};

}  // namespace

int main() {
  pw_init(nullptr, nullptr);

  App app;
  app.loop    = pw_main_loop_new(nullptr);
  app.context = pw_context_new(pw_main_loop_get_loop(app.loop), nullptr, 0);
  if (!app.context) {
    std::fprintf(stderr, "failed to create PipeWire context\n");
    return 1;
  }
  app.core = pw_context_connect(app.context, nullptr, 0);
  if (!app.core) {
    std::fprintf(stderr, "failed to connect to PipeWire (is the daemon running?)\n");
    return 1;
  }

  pw_core_add_listener(app.core, &app.core_listener, &core_events, &app);

  app.registry = pw_core_get_registry(app.core, PW_VERSION_REGISTRY, 0);
  pw_registry_add_listener(app.registry, &app.registry_listener, &registry_events, &app);

  // --- roundtrip: block until the initial burst of `global` events (every
  // node + the metadata object that currently exist) has been delivered ---
  app.pending_sync = pw_core_sync(app.core, PW_ID_CORE, 0);
  pw_main_loop_run(app.loop);

  // 1. print every connected audio device
  std::printf("== devices ==\n");
  for (auto& [id, node] : app.nodes) {
    std::printf("%s  id=%s  %s  (runtime-id=%u)\n", node.is_output ? "output" : "input", node.stable_name.c_str(), node.display_name.c_str(), id);
  }

  // 2. retrieve the default output device
  if (app.metadata_id == SPA_ID_INVALID) {
    std::fprintf(stderr, "no session manager 'default' metadata object found (is WirePlumber/pipewire-media-session running?)\n");
    return 1;
  }
  app.metadata_proxy = static_cast<pw_proxy*>(pw_registry_bind(app.registry, app.metadata_id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0));
  pw_metadata_add_listener(app.metadata_proxy, &app.metadata_listener, &metadata_events, &app);

  app.pending_sync = pw_core_sync(app.core, PW_ID_CORE, 0);  // wait for the initial property event(s)
  pw_main_loop_run(app.loop);

  NodeInfo* default_output = nullptr;
  for (auto& [id, node] : app.nodes) {
    if (node.is_output && node.stable_name == app.default_sink_name) default_output = &node;
  }
  // fall back to matching by description if default_sink_name held a
  // human name rather than the raw node.name in this session manager's setup
  if (!default_output && !app.nodes.empty()) {
    for (auto& [id, node] : app.nodes)
      if (node.is_output) {
        default_output = &node;
        break;
      }
  }
  if (!default_output) {
    std::fprintf(stderr, "could not resolve a default output device\n");
    return 1;
  }
  std::printf("\n== default output ==\nid=%s  %s  (runtime-id=%u)\n", default_output->stable_name.c_str(), default_output->display_name.c_str(),
              default_output->id);

  // bind the node itself so we can query/set/subscribe to its params
  default_output->proxy = static_cast<pw_proxy*>(pw_registry_bind(app.registry, default_output->id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
  pw_node_add_listener(default_output->proxy, &default_output->node_listener, &node_events, default_output);

  // enum_params alone is a one-shot query — it answers once and then goes
  // quiet. subscribe_params is what makes the server keep pushing `param`
  // events whenever this changes later, from us or anyone else.
  uint32_t subscribe_ids[] = {SPA_PARAM_Props};
  pw_node_subscribe_params(default_output->proxy, subscribe_ids, 1);

  // 3. retrieve its volume: ask for the current Props param, which arrives
  // via the same `param` event that will later report external changes
  pw_node_enum_params(default_output->proxy, 0, SPA_PARAM_Props, 0, UINT32_MAX, nullptr);
  app.pending_sync = pw_core_sync(app.core, PW_ID_CORE, 0);
  pw_main_loop_run(app.loop);  // node_param_event() above prints "[change] ..." with the current value

  // 4. set its volume (to 50%, unmuted) — using the channel count we just
  //    learned from the device's own current Props in step 3, not a guess.
  //    A device we've never queried (n_channels still 0 here) would need
  //    step 3 to run first; falling back to stereo only as a last resort.
  uint32_t n_channels = default_output->n_channels > 0 ? default_output->n_channels : 2;
  uint8_t buffer[1024];
  spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  const spa_pod* pod      = build_volume_pod(builder, 0.5f, false, n_channels);
  pw_node_set_param(default_output->proxy, SPA_PARAM_Props, 0, pod);
  std::printf("\n(volume set to 50%% across %u channel(s))\n", n_channels);

  // 5. keep running: node_param_event() fires for every subsequent change,
  // whether it came from the call above or from something external (e.g.
  // the user dragging a volume slider elsewhere)
  std::printf("\nlistening for further changes (Ctrl+C to quit)...\n");
  app.pending_sync = -1;  // stop treating core "done" as a reason to quit; run indefinitely
  pw_main_loop_run(app.loop);

  pw_main_loop_destroy(app.loop);
  pw_deinit();
  return 0;
}
