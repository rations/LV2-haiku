/*
 * hkamp — simple stereo amplifier, the milestone test plugin for the Haiku
 * LV2 port. Structure follows the canonical LV2 "eg-amp" example plugin
 * (ISC, David Robillard / Steve Harris), extended to stereo.
 *
 * run() is in the audio threading class and lv2:hardRTCapable: no
 * allocation, locking, I/O, or logging here.
 */
#include "lv2/core/lv2.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#define HKAMP_URI "https://github.com/rations/LV2-haiku/plugins/hkamp"

typedef enum {
    HKAMP_GAIN = 0,
    HKAMP_IN_L = 1,
    HKAMP_IN_R = 2,
    HKAMP_OUT_L = 3,
    HKAMP_OUT_R = 4
} PortIndex;

typedef struct {
    const float *gain;
    const float *in_l;
    const float *in_r;
    float *out_l;
    float *out_r;
} Hkamp;

static LV2_Handle
instantiate(const LV2_Descriptor *descriptor, double rate, const char *bundle_path,
            const LV2_Feature *const *features)
{
    (void)descriptor;
    (void)rate;
    (void)bundle_path;
    (void)features;

    return (LV2_Handle)calloc(1, sizeof(Hkamp));
}

static void
connect_port(LV2_Handle instance, uint32_t port, void *data)
{
    Hkamp *amp = (Hkamp *)instance;

    switch ((PortIndex)port) {
        case HKAMP_GAIN:
            amp->gain = (const float *)data;
            break;
        case HKAMP_IN_L:
            amp->in_l = (const float *)data;
            break;
        case HKAMP_IN_R:
            amp->in_r = (const float *)data;
            break;
        case HKAMP_OUT_L:
            amp->out_l = (float *)data;
            break;
        case HKAMP_OUT_R:
            amp->out_r = (float *)data;
            break;
    }
}

static void
activate(LV2_Handle instance)
{
    (void)instance;
}

/* Gain in dB to linear coefficient; -90 dB and below is silence. */
#define DB_CO(g) ((g) > -90.0f ? powf(10.0f, (g)*0.05f) : 0.0f)

static void
run(LV2_Handle instance, uint32_t n_samples)
{
    const Hkamp *amp = (const Hkamp *)instance;

    /* A conforming host connects every port before run(), but a port left
       unconnected must not crash the plugin. */
    if (!amp->gain || !amp->in_l || !amp->in_r || !amp->out_l || !amp->out_r) {
        return;
    }

    const float coef = DB_CO(*amp->gain);

    for (uint32_t pos = 0; pos < n_samples; pos++) {
        amp->out_l[pos] = amp->in_l[pos] * coef;
        amp->out_r[pos] = amp->in_r[pos] * coef;
    }
}

static void
deactivate(LV2_Handle instance)
{
    (void)instance;
}

static void
cleanup(LV2_Handle instance)
{
    free(instance);
}

static const void *
extension_data(const char *uri)
{
    (void)uri;
    return NULL;
}

static const LV2_Descriptor descriptor = {HKAMP_URI, instantiate, connect_port, activate,
                                          run,       deactivate,  cleanup,      extension_data};

LV2_SYMBOL_EXPORT
const LV2_Descriptor *
lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : NULL;
}
