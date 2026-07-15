/* lv2host_core — shared LV2 hosting core (see lv2host_core.h). */
#include "lv2host_core.h"

#include <lv2/atom/atom.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/log/log.h>
#include <lv2/parameters/parameters.h>

#include <zix/status.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Flush-to-zero / denormals-are-zero. JACK does not set FTZ/DAZ on a client's
   process thread, so plugins whose filter/reverb feedback paths produce
   subnormal floats stall the CPU and blow the RT deadline; re-arm it in every
   callback (a plugin may clear MXCSR itself). Two register writes — cheap. */
#if defined(__SSE__) || defined(__x86_64__)
#include <pmmintrin.h>
#include <xmmintrin.h>
static inline void rt_set_denormal_mode(void)
{
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
}
#else
static inline void rt_set_denormal_mode(void)
{
}
#endif

/* ---- URID map/unmap (thread-safe; growth only on the non-RT map path) ---- */

typedef struct {
    char **uris; /* index = id - 1 */
    uint32_t n;
    uint32_t cap;
    pthread_mutex_t lock;
} UridTable;

static UridTable urids = {NULL, 0, 0, PTHREAD_MUTEX_INITIALIZER};

static LV2_URID urid_map_cb(LV2_URID_Map_Handle handle, const char *uri)
{
    (void)handle;
    if (!uri || !*uri) {
        return 0;
    }
    pthread_mutex_lock(&urids.lock);
    for (uint32_t i = 0; i < urids.n; i++) {
        if (!strcmp(urids.uris[i], uri)) {
            pthread_mutex_unlock(&urids.lock);
            return i + 1;
        }
    }
    if (urids.n == urids.cap) {
        uint32_t cap = urids.cap ? urids.cap * 2 : 64;
        char **grown = (char **)realloc(urids.uris, cap * sizeof(char *));
        if (!grown) {
            pthread_mutex_unlock(&urids.lock);
            return 0;
        }
        urids.uris = grown;
        urids.cap = cap;
    }
    char *dup = strdup(uri);
    if (!dup) {
        pthread_mutex_unlock(&urids.lock);
        return 0;
    }
    urids.uris[urids.n++] = dup;
    LV2_URID id = urids.n;
    pthread_mutex_unlock(&urids.lock);
    return id;
}

static const char *urid_unmap_cb(LV2_URID_Unmap_Handle handle, LV2_URID urid)
{
    (void)handle;
    const char *s = NULL;
    pthread_mutex_lock(&urids.lock);
    if (urid >= 1 && urid <= urids.n) {
        s = urids.uris[urid - 1];
    }
    pthread_mutex_unlock(&urids.lock);
    return s;
}

static LV2_URID_Map urid_map = {NULL, urid_map_cb};
static LV2_URID_Unmap urid_unmap = {NULL, urid_unmap_cb};
static LV2_Feature feat_map = {LV2_URID__map, &urid_map};
static LV2_Feature feat_unmap = {LV2_URID__unmap, &urid_unmap};
static LV2_Feature feat_bounded = {LV2_BUF_SIZE__boundedBlockLength, NULL};
static LV2_Feature feat_powof2 = {LV2_BUF_SIZE__powerOf2BlockLength, NULL};

LV2_URID lv2host_map_uri(const char *uri)
{
    return urid_map_cb(NULL, uri);
}

LV2_URID_Map *lv2host_urid_map(void)
{
    return &urid_map;
}

LV2_URID_Unmap *lv2host_urid_unmap(void)
{
    return &urid_unmap;
}

/* ---- Log feature (never called from our own RT path) ---- */

static int log_vprintf_cb(LV2_Log_Handle handle, LV2_URID type, const char *fmt, va_list ap)
{
    (void)handle;
    (void)type;
    return vfprintf(stderr, fmt, ap);
}

static int log_printf_cb(LV2_Log_Handle handle, LV2_URID type, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = log_vprintf_cb(handle, type, fmt, ap);
    va_end(ap);
    return r;
}

static LV2_Log_Log lv2_log = {NULL, log_printf_cb, log_vprintf_cb};
static LV2_Feature feat_log = {LV2_LOG__log, &lv2_log};

/* ---- Worker (per instance): RT schedules into a ring, a dedicated non-RT
   thread runs work(), responses return through a second ring and are applied
   from the RT context after run(). jalv's model. ---- */

static LV2_Worker_Status worker_write_packet(ZixRing *target, uint32_t size, const void *data)
{
    ZixRingTransaction tx = zix_ring_begin_write(target);
    if (zix_ring_amend_write(target, &tx, &size, sizeof(size)) ||
        zix_ring_amend_write(target, &tx, data, size)) {
        return LV2_WORKER_ERR_NO_SPACE;
    }
    zix_ring_commit_write(target, &tx);
    return LV2_WORKER_SUCCESS;
}

static LV2_Worker_Status worker_respond_cb(LV2_Worker_Respond_Handle handle, uint32_t size,
                                           const void *data)
{
    Lv2hWorker *w = (Lv2hWorker *)handle;
    return worker_write_packet(w->responses, size, data);
}

static ZixThreadResult worker_thread_fn(void *arg)
{
    Lv2hWorker *w = (Lv2hWorker *)arg;
    char buf[LV2HOST_MAX_PACKET];
    for (;;) {
        zix_sem_wait(&w->sem);
        if (w->quit) {
            break;
        }
        uint32_t size = 0;
        while (zix_ring_read_space(w->requests) >= sizeof(size)) {
            zix_ring_read(w->requests, &size, sizeof(size));
            if (size > sizeof(buf)) { /* oversized: drain and drop */
                zix_ring_skip(w->requests, size);
                continue;
            }
            zix_ring_read(w->requests, buf, size);
            if (w->iface && w->iface->work) {
                w->iface->work(w->handle, worker_respond_cb, w, size, buf);
            }
        }
    }
    return (ZixThreadResult)0;
}

/* RT-safe: copy the job into the ring and wake the worker. No malloc/lock. */
static LV2_Worker_Status worker_schedule_cb(LV2_Worker_Schedule_Handle handle, uint32_t size,
                                            const void *data)
{
    Lv2hWorker *w = (Lv2hWorker *)handle;
    if (!w->requests) {
        return LV2_WORKER_ERR_UNKNOWN; /* not yet wired */
    }
    LV2_Worker_Status st = worker_write_packet(w->requests, size, data);
    if (st == LV2_WORKER_SUCCESS) {
        zix_sem_post(&w->sem);
    }
    return st;
}

/* Apply pending responses then end_run — called from the RT thread. */
static void worker_apply_responses(Lv2hWorker *w)
{
    if (!w->active || !w->iface) {
        return;
    }
    char buf[LV2HOST_MAX_PACKET];
    uint32_t size = 0;
    while (zix_ring_read_space(w->responses) >= sizeof(size)) {
        zix_ring_read(w->responses, &size, sizeof(size));
        if (size > sizeof(buf)) {
            zix_ring_skip(w->responses, size);
            continue;
        }
        zix_ring_read(w->responses, buf, size);
        if (w->iface->work_response) {
            w->iface->work_response(w->handle, size, buf);
        }
    }
    if (w->iface->end_run) {
        w->iface->end_run(w->handle);
    }
}

static void worker_init(Lv2hWorker *w, LilvInstance *inst)
{
    const LV2_Worker_Interface *iface =
        (const LV2_Worker_Interface *)lilv_instance_get_extension_data(inst, LV2_WORKER__interface);
    if (!iface) {
        w->active = false;
        return;
    }
    w->iface = iface;
    w->handle = lilv_instance_get_handle(inst);
    w->requests = zix_ring_new(NULL, LV2HOST_RING_BYTES);
    w->responses = zix_ring_new(NULL, LV2HOST_RING_BYTES);
    if (!w->requests || !w->responses) {
        zix_ring_free(w->requests);
        zix_ring_free(w->responses);
        w->requests = NULL;
        w->responses = NULL;
        w->active = false;
        return;
    }
    if (zix_ring_mlock(w->requests) != ZIX_STATUS_SUCCESS ||
        zix_ring_mlock(w->responses) != ZIX_STATUS_SUCCESS) {
        fprintf(stderr, "warning: failed to lock worker rings in memory\n");
    }
    if (zix_sem_init(&w->sem, 0) != ZIX_STATUS_SUCCESS) {
        zix_ring_free(w->requests);
        zix_ring_free(w->responses);
        w->requests = NULL;
        w->responses = NULL;
        w->active = false;
        return;
    }
    w->quit = 0;
    if (zix_thread_create(&w->thread, 65536, worker_thread_fn, w) != ZIX_STATUS_SUCCESS) {
        zix_sem_destroy(&w->sem);
        zix_ring_free(w->requests);
        zix_ring_free(w->responses);
        w->requests = NULL;
        w->responses = NULL;
        w->active = false;
        return;
    }
    w->active = true;
}

static void worker_destroy(Lv2hWorker *w)
{
    if (!w->active) {
        return;
    }
    w->quit = 1;
    zix_sem_post(&w->sem);
    zix_thread_join(w->thread);
    zix_sem_destroy(&w->sem);
    zix_ring_free(w->requests);
    zix_ring_free(w->responses);
    w->active = false;
}

/* ---- Feature allow-list: a plugin that requires anything else is rejected
   before instantiation rather than crashing at run time. ---- */

static bool feature_supported(const char *uri)
{
    static const char *ok[] = {LV2_URID__map,
                               LV2_URID__unmap,
                               LV2_OPTIONS__options,
                               LV2_BUF_SIZE__boundedBlockLength,
                               LV2_BUF_SIZE__powerOf2BlockLength,
                               LV2_WORKER__schedule,
                               LV2_LOG__log,
                               NULL};
    for (int i = 0; ok[i]; i++) {
        if (!strcmp(uri, ok[i])) {
            return true;
        }
    }
    return false;
}

static bool check_required_features(const LilvPlugin *p)
{
    bool ok = true;
    LilvNodes *req = lilv_plugin_get_required_features(p);
    if (req) {
        LILV_FOREACH(nodes, it, req)
        {
            const LilvNode *f = lilv_nodes_get(req, it);
            const char *uri = lilv_node_as_uri(f);
            if (!uri || !feature_supported(uri)) {
                fprintf(stderr, "error: plugin requires unsupported feature %s\n",
                        uri ? uri : "(?)");
                ok = false;
            }
        }
        lilv_nodes_free(req);
    }
    return ok;
}

/* ---- RT path ---- */

/* JACK xrun callback: count only — never log from a JACK callback thread. */
static int xrun_cb(void *arg)
{
    Lv2Host *h = (Lv2Host *)arg;
    h->n_xruns++;
    return 0;
}

typedef struct {
    uint32_t port;
    float value;
} CtlWrite;

/* Drain queued control writes into the value store — RT context only. */
static void apply_ctl_writes(Lv2Host *h)
{
    CtlWrite cw;
    while (zix_ring_read_space(h->ctl_ring) >= sizeof(cw)) {
        if (zix_ring_read(h->ctl_ring, &cw, sizeof(cw)) != sizeof(cw)) {
            break;
        }
        if (cw.port < h->n_ports) {
            h->ctl[cw.port] = cw.value;
        }
    }
}

/* Re-arm this instance's atom ports before run(): input ports become an empty
   but valid LV2_Atom_Sequence; output ports advertise their capacity as a
   Chunk. RT-safe: only touches the two header fields. */
static inline void reset_atoms(Lv2Host *h, int k)
{
    for (uint32_t a = 0; a < h->n_atoms; a++) {
        LV2_Atom_Sequence *seq = (LV2_Atom_Sequence *)h->atoms[a].buf[k];
        if (!seq) {
            continue;
        }
        if (h->atoms[a].is_input) {
            seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
            seq->atom.type = h->urid_seq;
        } else {
            seq->atom.size = h->atoms[a].capacity;
            seq->atom.type = h->urid_chunk;
        }
    }
}

/* Run the plugin in place on L/R (reference: the JackDAW LV2 backend). */
static void host_run(Lv2Host *h, float *L, float *R, uint32_t n)
{
    if (h->n_audio_out == 0) {
        return;
    }
    if (h->dual_mono) {
        lilv_instance_connect_port(h->inst[0], h->ain[0], L);
        lilv_instance_connect_port(h->inst[0], h->aout[0], h->outA);
        lilv_instance_connect_port(h->inst[1], h->ain[0], R);
        lilv_instance_connect_port(h->inst[1], h->aout[0], h->outB);
        reset_atoms(h, 0);
        reset_atoms(h, 1);
        lilv_instance_run(h->inst[0], n);
        lilv_instance_run(h->inst[1], n);
        worker_apply_responses(&h->workers[0]);
        worker_apply_responses(&h->workers[1]);
        memcpy(L, h->outA, (size_t)n * sizeof(float));
        memcpy(R, h->outB, (size_t)n * sizeof(float));
        return;
    }

    /* First one/two audio ports to L/R; surplus ports to scratch so run()
       never touches an unconnected port. */
    if (h->n_audio_in > 0) {
        lilv_instance_connect_port(h->inst[0], h->ain[0], L);
    }
    if (h->n_audio_in > 1) {
        lilv_instance_connect_port(h->inst[0], h->ain[1], R);
    }
    for (int i = 2; i < h->n_audio_in; i++) {
        lilv_instance_connect_port(h->inst[0], h->ain[i], h->dummy_in);
    }
    lilv_instance_connect_port(h->inst[0], h->aout[0], h->outA);
    if (h->n_audio_out > 1) {
        lilv_instance_connect_port(h->inst[0], h->aout[1], h->outB);
    }
    for (int i = 2; i < h->n_audio_out; i++) {
        lilv_instance_connect_port(h->inst[0], h->aout[i], h->dummy_out);
    }

    reset_atoms(h, 0);
    lilv_instance_run(h->inst[0], n);
    worker_apply_responses(&h->workers[0]);
    memcpy(L, h->outA, (size_t)n * sizeof(float));
    if (h->n_audio_out > 1) {
        memcpy(R, h->outB, (size_t)n * sizeof(float));
    } else {
        memcpy(R, h->outA, (size_t)n * sizeof(float));
    }
}

static int process_cb(jack_nframes_t nframes, void *arg)
{
    Lv2Host *h = (Lv2Host *)arg;
    rt_set_denormal_mode();
    apply_ctl_writes(h);

    const float *inL = (const float *)jack_port_get_buffer(h->jin[0], nframes);
    const float *inR = (const float *)jack_port_get_buffer(h->jin[1], nframes);
    float *outL = (float *)jack_port_get_buffer(h->jout[0], nframes);
    float *outR = (float *)jack_port_get_buffer(h->jout[1], nframes);

    if (nframes > h->max_block) { /* buffer grew beyond what we pre-allocated */
        h->block_too_big = 1;
        memset(outL, 0, (size_t)nframes * sizeof(float));
        memset(outR, 0, (size_t)nframes * sizeof(float));
        return 0;
    }

    memcpy(h->bufL, inL, (size_t)nframes * sizeof(float));
    memcpy(h->bufR, inR, (size_t)nframes * sizeof(float));
    host_run(h, h->bufL, h->bufR, nframes);
    memcpy(outL, h->bufL, (size_t)nframes * sizeof(float));
    memcpy(outR, h->bufR, (size_t)nframes * sizeof(float));
    return 0;
}

/* ---- Setup / teardown ---- */

static bool host_instantiate(Lv2Host *h, double sr)
{
    const LilvPlugin *p = h->plugin;
    LilvNode *n_audio = lilv_new_uri(h->world, LILV_URI_AUDIO_PORT);
    LilvNode *n_control = lilv_new_uri(h->world, LILV_URI_CONTROL_PORT);
    LilvNode *n_input = lilv_new_uri(h->world, LILV_URI_INPUT_PORT);
    LilvNode *n_output = lilv_new_uri(h->world, LILV_URI_OUTPUT_PORT);
    LilvNode *n_atom = lilv_new_uri(h->world, LV2_ATOM__AtomPort);
    LilvNode *n_cv = lilv_new_uri(h->world, LV2_CORE__CVPort);
    LilvNode *n_toggled = lilv_new_uri(h->world, LV2_CORE__toggled);
    LilvNode *n_integer = lilv_new_uri(h->world, LV2_CORE__integer);
    LilvNode *n_enum = lilv_new_uri(h->world, LV2_CORE__enumeration);
    bool ok = false;

    h->n_ports = lilv_plugin_get_num_ports(p);
    h->ctl = (float *)calloc(h->n_ports ? h->n_ports : 1, sizeof(float));
    h->ain = (uint32_t *)calloc(h->n_ports ? h->n_ports : 1, sizeof(uint32_t));
    h->aout = (uint32_t *)calloc(h->n_ports ? h->n_ports : 1, sizeof(uint32_t));
    h->params = (Lv2hParam *)calloc(h->n_ports ? h->n_ports : 1, sizeof(Lv2hParam));
    h->atoms = (Lv2hAtomPort *)calloc(h->n_ports ? h->n_ports : 1, sizeof(Lv2hAtomPort));
    h->bufL = (float *)calloc(h->max_block, sizeof(float));
    h->bufR = (float *)calloc(h->max_block, sizeof(float));
    h->outA = (float *)calloc(h->max_block, sizeof(float));
    h->outB = (float *)calloc(h->max_block, sizeof(float));
    h->dummy_in = (float *)calloc(h->max_block, sizeof(float));
    h->dummy_out = (float *)calloc(h->max_block, sizeof(float));
    uint32_t misc_bytes = h->max_block * (uint32_t)sizeof(float);
    if (misc_bytes < 8192) {
        misc_bytes = 8192;
    }
    h->misc = calloc(1, misc_bytes);
    h->ctl_ring = zix_ring_new(NULL, LV2HOST_RING_BYTES);
    float *mins = (float *)calloc(h->n_ports ? h->n_ports : 1, sizeof(float));
    float *maxs = (float *)calloc(h->n_ports ? h->n_ports : 1, sizeof(float));
    float *defs = (float *)calloc(h->n_ports ? h->n_ports : 1, sizeof(float));
    if (!h->ctl || !h->ain || !h->aout || !h->params || !h->atoms || !h->bufL || !h->bufR ||
        !h->outA || !h->outB || !h->dummy_in || !h->dummy_out || !h->misc || !h->ctl_ring ||
        !mins || !maxs || !defs) {
        fprintf(stderr, "error: out of memory\n");
        goto done;
    }
    if (zix_ring_mlock(h->ctl_ring) != ZIX_STATUS_SUCCESS) {
        fprintf(stderr, "warning: failed to lock control ring in memory\n");
    }

    lilv_plugin_get_port_ranges_float(p, mins, maxs, defs);

    for (uint32_t i = 0; i < h->n_ports; i++) {
        const LilvPort *port = lilv_plugin_get_port_by_index(p, i);
        if (!port) {
            fprintf(stderr, "error: port %u missing\n", i);
            goto done;
        }
        bool is_in = lilv_port_is_a(p, port, n_input);
        bool is_out = lilv_port_is_a(p, port, n_output);

        if (lilv_port_is_a(p, port, n_audio)) {
            if (is_in) {
                h->ain[h->n_audio_in++] = i;
            }
            if (is_out) {
                h->aout[h->n_audio_out++] = i;
            }
        } else if (lilv_port_is_a(p, port, n_atom)) {
            Lv2hAtomPort *ap = &h->atoms[h->n_atoms++];
            ap->index = i;
            ap->is_input = is_in;
            ap->capacity = misc_bytes;
        } else if (lilv_port_is_a(p, port, n_control)) {
            float d = defs[i];
            if (d != d) { /* NaN guard */
                d = 0.0f;
            }
            h->ctl[i] = d;
            if (is_in) {
                Lv2hParam *pr = &h->params[h->n_params++];
                pr->port_index = i;
                LilvNode *pn = lilv_port_get_name(p, port);
                snprintf(pr->name, sizeof(pr->name), "%s", pn ? lilv_node_as_string(pn) : "param");
                if (pn) {
                    lilv_node_free(pn);
                }
                pr->min = mins[i];
                pr->max = maxs[i];
                pr->def = d;
                if (pr->max <= pr->min) {
                    pr->max = pr->min + 1.0f;
                }
                pr->toggled = lilv_port_has_property(p, port, n_toggled);
                pr->integer = lilv_port_has_property(p, port, n_integer);
                pr->enumeration = lilv_port_has_property(p, port, n_enum);
                LilvScalePoints *sp = lilv_port_get_scale_points(p, port);
                if (sp) {
                    pr->n_scale_points = lilv_scale_points_size(sp);
                    lilv_scale_points_free(sp);
                }
            }
        }
    }

    h->dual_mono = (h->n_audio_in == 1 && h->n_audio_out == 1);
    h->n_inst = h->dual_mono ? 2 : 1;

    h->urid_seq = lv2host_map_uri(LV2_ATOM__Sequence);
    h->urid_chunk = lv2host_map_uri(LV2_ATOM__Chunk);

    /* Options: block length + sample rate (many plugins require these). */
    h->opt_minblk = 1;
    h->opt_maxblk = (int32_t)h->max_block;
    h->opt_seqsize = (int32_t)misc_bytes;
    h->opt_srate = (float)sr;
    const LV2_URID urid_int = lv2host_map_uri(LV2_ATOM__Int);
    const LV2_URID urid_float = lv2host_map_uri(LV2_ATOM__Float);
    LV2_Options_Option *o = h->options;
    o[0].context = LV2_OPTIONS_INSTANCE;
    o[0].subject = 0;
    o[0].key = lv2host_map_uri(LV2_BUF_SIZE__minBlockLength);
    o[0].size = sizeof(int32_t);
    o[0].type = urid_int;
    o[0].value = &h->opt_minblk;
    o[1] = o[0];
    o[1].key = lv2host_map_uri(LV2_BUF_SIZE__maxBlockLength);
    o[1].value = &h->opt_maxblk;
    o[2] = o[0];
    o[2].key = lv2host_map_uri(LV2_BUF_SIZE__sequenceSize);
    o[2].value = &h->opt_seqsize;
    o[3].context = LV2_OPTIONS_INSTANCE;
    o[3].subject = 0;
    o[3].key = lv2host_map_uri(LV2_PARAMETERS__sampleRate);
    o[3].size = sizeof(float);
    o[3].type = urid_float;
    o[3].value = &h->opt_srate;
    memset(&o[4], 0, sizeof(o[4]));
    h->feat_opts.URI = LV2_OPTIONS__options;
    h->feat_opts.data = h->options;

    for (int k = 0; k < h->n_inst; k++) {
        Lv2hWorker *w = &h->workers[k];
        w->schedule.handle = w;
        w->schedule.schedule_work = worker_schedule_cb;
        w->feature.URI = LV2_WORKER__schedule;
        w->feature.data = &w->schedule;

        int nf = 0;
        h->features[k][nf++] = &feat_map;
        h->features[k][nf++] = &feat_unmap;
        h->features[k][nf++] = &h->feat_opts;
        h->features[k][nf++] = &feat_bounded;
        h->features[k][nf++] = &feat_powof2;
        h->features[k][nf++] = &feat_log;
        h->features[k][nf++] = &w->feature;
        h->features[k][nf] = NULL;

        h->inst[k] = lilv_plugin_instantiate(p, sr, h->features[k]);
        if (!h->inst[k]) {
            fprintf(stderr, "error: lilv_plugin_instantiate failed\n");
            goto done;
        }
        for (uint32_t a = 0; a < h->n_atoms; a++) {
            h->atoms[a].buf[k] = calloc(1, sizeof(LV2_Atom_Sequence) + h->atoms[a].capacity);
            if (!h->atoms[a].buf[k]) {
                fprintf(stderr, "error: out of memory\n");
                goto done;
            }
        }
        reset_atoms(h, k);

        /* Connect EVERY port up front: control -> its slot in the value
           store; audio/CV -> scratch (audio re-bound per process call);
           atom -> its own sequence buffer; anything else -> zeroed misc. */
        for (uint32_t i = 0; i < h->n_ports; i++) {
            const LilvPort *port = lilv_plugin_get_port_by_index(p, i);
            if (lilv_port_is_a(p, port, n_control)) {
                lilv_instance_connect_port(h->inst[k], i, &h->ctl[i]);
            } else if (lilv_port_is_a(p, port, n_audio)) {
                lilv_instance_connect_port(
                    h->inst[k], i, lilv_port_is_a(p, port, n_input) ? h->dummy_in : h->dummy_out);
            } else if (lilv_port_is_a(p, port, n_atom)) {
                void *buf = h->misc;
                for (uint32_t a = 0; a < h->n_atoms; a++) {
                    if (h->atoms[a].index == i) {
                        buf = h->atoms[a].buf[k];
                        break;
                    }
                }
                lilv_instance_connect_port(h->inst[k], i, buf);
            } else if (lilv_port_is_a(p, port, n_cv)) {
                lilv_instance_connect_port(
                    h->inst[k], i, lilv_port_is_a(p, port, n_input) ? h->dummy_in : h->dummy_out);
            } else {
                lilv_instance_connect_port(h->inst[k], i, h->misc);
            }
        }
        lilv_instance_activate(h->inst[k]);
        worker_init(&h->workers[k], h->inst[k]);
    }
    ok = true;

done:
    free(mins);
    free(maxs);
    free(defs);
    lilv_node_free(n_audio);
    lilv_node_free(n_control);
    lilv_node_free(n_input);
    lilv_node_free(n_output);
    lilv_node_free(n_atom);
    lilv_node_free(n_cv);
    lilv_node_free(n_toggled);
    lilv_node_free(n_integer);
    lilv_node_free(n_enum);
    return ok;
}

static void host_destroy(Lv2Host *h)
{
    for (int k = 0; k < h->n_inst; k++) {
        if (h->inst[k]) {
            lilv_instance_deactivate(h->inst[k]);
            worker_destroy(&h->workers[k]); /* stop thread before freeing */
            lilv_instance_free(h->inst[k]);
            h->inst[k] = NULL;
        }
    }
    for (uint32_t a = 0; a < h->n_atoms; a++) {
        free(h->atoms[a].buf[0]);
        free(h->atoms[a].buf[1]);
    }
    zix_ring_free(h->ctl_ring);
    h->ctl_ring = NULL;
    free(h->atoms);
    free(h->params);
    free(h->ctl);
    free(h->ain);
    free(h->aout);
    free(h->bufL);
    free(h->bufR);
    free(h->outA);
    free(h->outB);
    free(h->dummy_in);
    free(h->dummy_out);
    free(h->misc);
}

void lv2host_autoconnect(Lv2Host *h)
{
    const char **play = jack_get_ports(h->client, NULL, JACK_DEFAULT_AUDIO_TYPE,
                                       JackPortIsPhysical | JackPortIsInput);
    if (play) {
        for (int i = 0; i < 2 && play[i]; i++) {
            jack_connect(h->client, jack_port_name(h->jout[i]), play[i]);
        }
        jack_free(play);
    }
    const char **cap = jack_get_ports(h->client, NULL, JACK_DEFAULT_AUDIO_TYPE,
                                      JackPortIsPhysical | JackPortIsOutput);
    if (cap) {
        for (int i = 0; i < 2 && cap[i]; i++) {
            jack_connect(h->client, cap[i], jack_port_name(h->jin[i]));
        }
        jack_free(cap);
    }
}

bool lv2host_write_control_port(Lv2Host *h, uint32_t port_index, float val)
{
    if (!h->ctl_ring || port_index >= h->n_ports) {
        return false;
    }
    for (uint32_t i = 0; i < h->n_params; i++) {
        if (h->params[i].port_index == port_index) {
            if (val < h->params[i].min) {
                val = h->params[i].min;
            }
            if (val > h->params[i].max) {
                val = h->params[i].max;
            }
            break;
        }
    }
    CtlWrite cw;
    cw.port = port_index;
    cw.value = val;
    ZixRingTransaction tx = zix_ring_begin_write(h->ctl_ring);
    if (zix_ring_amend_write(h->ctl_ring, &tx, &cw, sizeof(cw))) {
        return false;
    }
    zix_ring_commit_write(h->ctl_ring, &tx);
    return true;
}

bool lv2host_set_param(Lv2Host *h, uint32_t param_idx, float val)
{
    if (param_idx >= h->n_params) {
        return false;
    }
    return lv2host_write_control_port(h, h->params[param_idx].port_index, val);
}

float lv2host_get_control(const Lv2Host *h, uint32_t port_index)
{
    if (port_index >= h->n_ports) {
        return 0.0f;
    }
    return h->ctl[port_index];
}

bool lv2host_open(Lv2Host *h, const char *plugin_uri, const char *client_name)
{
    memset(h, 0, sizeof(*h));

    h->world = lilv_world_new();
    if (!h->world) {
        fprintf(stderr, "error: lilv_world_new failed\n");
        return false;
    }
    lilv_world_load_all(h->world);

    LilvNode *uri = lilv_new_uri(h->world, plugin_uri);
    if (!uri) {
        fprintf(stderr, "error: '%s' is not a valid URI\n", plugin_uri);
        lilv_world_free(h->world);
        h->world = NULL;
        return false;
    }
    const LilvPlugins *plugins = lilv_world_get_all_plugins(h->world);
    h->plugin = lilv_plugins_get_by_uri(plugins, uri);
    lilv_node_free(uri);
    if (!h->plugin) {
        fprintf(stderr, "error: plugin <%s> not found (check lv2ls / LV2_PATH)\n", plugin_uri);
        lilv_world_free(h->world);
        h->world = NULL;
        return false;
    }
    if (!check_required_features(h->plugin)) {
        lilv_world_free(h->world);
        h->world = NULL;
        return false;
    }

    h->client = jack_client_open(client_name, JackNullOption, NULL);
    if (!h->client) {
        fprintf(stderr, "error: cannot connect to JACK (is jackd running?)\n");
        lilv_world_free(h->world);
        h->world = NULL;
        return false;
    }
    h->sample_rate = (double)jack_get_sample_rate(h->client);
    h->max_block = jack_get_buffer_size(h->client);

    h->jin[0] = jack_port_register(h->client, "in_l", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    h->jin[1] = jack_port_register(h->client, "in_r", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    h->jout[0] =
        jack_port_register(h->client, "out_l", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    h->jout[1] =
        jack_port_register(h->client, "out_r", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    if (!h->jin[0] || !h->jin[1] || !h->jout[0] || !h->jout[1]) {
        fprintf(stderr, "error: jack_port_register failed\n");
        jack_client_close(h->client);
        h->client = NULL;
        lilv_world_free(h->world);
        h->world = NULL;
        return false;
    }

    if (!host_instantiate(h, h->sample_rate)) {
        host_destroy(h);
        jack_client_close(h->client);
        h->client = NULL;
        lilv_world_free(h->world);
        h->world = NULL;
        return false;
    }

    if (jack_set_process_callback(h->client, process_cb, h) != 0) {
        fprintf(stderr, "error: jack_set_process_callback failed\n");
        lv2host_close(h);
        return false;
    }
    if (jack_set_xrun_callback(h->client, xrun_cb, h) != 0) {
        fprintf(stderr, "warning: jack_set_xrun_callback failed; xruns not counted\n");
    }
    if (jack_activate(h->client) != 0) {
        fprintf(stderr, "error: jack_activate failed\n");
        lv2host_close(h);
        return false;
    }
    return true;
}

void lv2host_close(Lv2Host *h)
{
    if (h->client) {
        jack_deactivate(h->client);
        jack_client_close(h->client);
        h->client = NULL;
    }
    host_destroy(h);
    if (h->world) {
        lilv_world_free(h->world);
        h->world = NULL;
    }
}
