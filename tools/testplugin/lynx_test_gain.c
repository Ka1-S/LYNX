/* A minimal CLAP effect (stereo pass-through at half gain) for testing LYNX's plugin scan and FL-project plugin
 * matching without third-party plugins:
 *   zig cc -shared -O2 -target x86_64-windows-gnu -Ithird_party/clap/include tools/testplugin/lynx_test_gain.c \
 *       -o "LYNX Test Gain.clap"
 *   LYNX.exe --scantest <folder with it> report.txt
 */
#include <clap/clap.h>
#include <stdlib.h>
#include <string.h>

static const char* const features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, NULL };

static const clap_plugin_descriptor_t descriptor = {
    CLAP_VERSION_INIT, "org.lynx.test-gain", "LYNX Test Gain", "LYNX", "https://github.com/Ka1-S/LYNX-SHARE", "", "", "1.0.0",
    "Half-gain pass-through for scan tests", features
};

static uint32_t ports_count (const clap_plugin_t* p, bool in) { (void) p; (void) in; return 1; }
static bool ports_get (const clap_plugin_t* p, uint32_t index, bool in, clap_audio_port_info_t* info)
{
    (void) p;
    if (index != 0) return false;
    memset (info, 0, sizeof (*info));
    info->id = in ? 0 : 1;
    strcpy (info->name, in ? "In" : "Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
static const clap_plugin_audio_ports_t audio_ports = { ports_count, ports_get };

static bool p_init (const clap_plugin_t* p) { (void) p; return true; }
static void p_destroy (const clap_plugin_t* p) { free ((void*) p); }
static bool p_activate (const clap_plugin_t* p, double r, uint32_t a, uint32_t b) { (void) p; (void) r; (void) a; (void) b; return true; }
static void p_deactivate (const clap_plugin_t* p) { (void) p; }
static bool p_start (const clap_plugin_t* p) { (void) p; return true; }
static void p_stop (const clap_plugin_t* p) { (void) p; }
static void p_reset (const clap_plugin_t* p) { (void) p; }
static clap_process_status p_process (const clap_plugin_t* p, const clap_process_t* proc)
{
    (void) p;
    if (proc->audio_inputs_count < 1 || proc->audio_outputs_count < 1) return CLAP_PROCESS_CONTINUE;
    for (uint32_t c = 0; c < 2 && c < proc->audio_inputs[0].channel_count && c < proc->audio_outputs[0].channel_count; ++c)
        for (uint32_t i = 0; i < proc->frames_count; ++i)
            proc->audio_outputs[0].data32[c][i] = 0.5f * proc->audio_inputs[0].data32[c][i];
    return CLAP_PROCESS_CONTINUE;
}
static const void* p_extension (const clap_plugin_t* p, const char* id)
{
    (void) p;
    return strcmp (id, CLAP_EXT_AUDIO_PORTS) == 0 ? &audio_ports : NULL;
}
static void p_main_thread (const clap_plugin_t* p) { (void) p; }

static uint32_t f_count (const clap_plugin_factory_t* f) { (void) f; return 1; }
static const clap_plugin_descriptor_t* f_descriptor (const clap_plugin_factory_t* f, uint32_t i) { (void) f; return i == 0 ? &descriptor : NULL; }
static const clap_plugin_t* f_create (const clap_plugin_factory_t* f, const clap_host_t* host, const char* id)
{
    (void) f; (void) host;
    if (strcmp (id, descriptor.id) != 0) return NULL;
    clap_plugin_t* p = (clap_plugin_t*) calloc (1, sizeof (clap_plugin_t));
    p->desc = &descriptor;
    p->init = p_init; p->destroy = p_destroy; p->activate = p_activate; p->deactivate = p_deactivate;
    p->start_processing = p_start; p->stop_processing = p_stop; p->reset = p_reset; p->process = p_process;
    p->get_extension = p_extension; p->on_main_thread = p_main_thread;
    return p;
}
static const clap_plugin_factory_t factory = { f_count, f_descriptor, f_create };

static bool e_init (const char* path) { (void) path; return true; }
static void e_deinit (void) {}
static const void* e_factory (const char* id) { return strcmp (id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : NULL; }

CLAP_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, e_init, e_deinit, e_factory };
