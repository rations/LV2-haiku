/*
 * lv2host_core — shared LV2 hosting core for the Haiku hosts: a stereo JACK
 * client that discovers, instantiates and runs one LV2 plugin (dual-mono for
 * 1-in/1-out plugins).
 *
 * Hosting behavior (feature allow-list, connect-every-port-before-run,
 * per-run atom reset, worker threads fed through lock-free rings) mirrors
 * the LV2 backend of the JackDAW Linux DAW, itself derived from jalv.
 *
 * Threading classes:
 *  - opening thread: discovery, instantiation, teardown (non-RT);
 *  - JACK process callback: RT — no allocation, locks, I/O, or logging;
 *  - one non-RT worker thread per plugin instance (worker extension);
 *  - any control-writer thread (REPL, GUI looper): lv2host_set_param() /
 *    lv2host_write_control_port() marshal through a pre-allocated lock-free
 *    ring drained by the RT callback — writers never touch RT state.
 */
#ifndef LV2HOST_CORE_H
#define LV2HOST_CORE_H

#include <lilv/lilv.h>
#include <lv2/core/lv2.h>
#include <lv2/options/options.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#include <zix/ring.h>
#include <zix/sem.h>
#include <zix/thread.h>

#include <jack/jack.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LV2HOST_MAX_PACKET 4096U
#define LV2HOST_RING_BYTES (LV2HOST_MAX_PACKET * 8U)
#define LV2HOST_NAME_MAX 128

typedef struct {
    const LV2_Worker_Interface *iface;
    LV2_Handle handle;
    ZixRing *requests;
    ZixRing *responses;
    ZixSem sem;
    ZixThread thread;
    volatile int quit;
    bool active;
    LV2_Worker_Schedule schedule;
    LV2_Feature feature;
} Lv2hWorker;

typedef struct {
    uint32_t port_index;
    char name[LV2HOST_NAME_MAX];
    float min, max, def;
    bool toggled, integer, enumeration;
    unsigned n_scale_points;
} Lv2hParam;

typedef struct {
    uint32_t index;
    bool is_input;
    uint32_t capacity;
    void *buf[2]; /* per instance (dual-mono) */
} Lv2hAtomPort;

typedef struct {
    LilvWorld *world;
    const LilvPlugin *plugin;
    LilvInstance *inst[2];
    int n_inst;
    bool dual_mono;

    uint32_t n_ports;
    float *ctl; /* per-port control value store (written by RT only) */

    uint32_t *ain, *aout;
    int n_audio_in, n_audio_out;

    Lv2hParam *params;
    uint32_t n_params;

    Lv2hAtomPort *atoms;
    uint32_t n_atoms;
    LV2_URID urid_seq, urid_chunk;

    float *bufL, *bufR;          /* in-place work buffers (max_block) */
    float *outA, *outB;          /* plugin output scratch */
    float *dummy_in, *dummy_out; /* surplus audio / CV ports */
    void *misc;                  /* zeroed buffer for any leftover port type */
    uint32_t max_block;

    int32_t opt_minblk, opt_maxblk, opt_seqsize;
    float opt_srate;
    LV2_Options_Option options[5];
    LV2_Feature feat_opts;
    Lv2hWorker workers[2];
    const LV2_Feature *features[2][10];

    ZixRing *ctl_ring; /* control writes from non-RT threads to RT */

    jack_client_t *client;
    jack_port_t *jin[2], *jout[2];
    double sample_rate;
    volatile int block_too_big; /* flagged from RT if nframes > max_block */
    volatile unsigned n_xruns;  /* incremented from JACK's xrun callback */
} Lv2Host;

/* Load the lilv world, resolve plugin_uri, reject unsupported required
   features, open a stereo JACK client, instantiate + activate the plugin
   with every port connected, and start processing. Non-RT. On failure the
   host is torn down and false is returned. */
bool lv2host_open(Lv2Host *h, const char *plugin_uri, const char *client_name);

/* Deactivate, close the JACK client (never skip this — a killed client
   leaves the server graph wedged), stop workers, free everything. Non-RT. */
void lv2host_close(Lv2Host *h);

/* Best effort: wire our stereo ports to the physical capture/playback. */
void lv2host_autoconnect(Lv2Host *h);

/* Clamp val to params[param_idx]'s range and enqueue it for the RT thread.
   Callable from any non-RT thread. Returns false on bad index / full ring. */
bool lv2host_set_param(Lv2Host *h, uint32_t param_idx, float val);

/* Enqueue a raw control-port write (clamped when the port is a known
   parameter). Callable from any non-RT thread. */
bool lv2host_write_control_port(Lv2Host *h, uint32_t port_index, float val);

/* Snapshot of a control port's current value (single aligned float read). */
float lv2host_get_control(const Lv2Host *h, uint32_t port_index);

/* The process-wide URID map/unmap features (also handed to plugin UIs). */
LV2_URID lv2host_map_uri(const char *uri);
LV2_URID_Map *lv2host_urid_map(void);
LV2_URID_Unmap *lv2host_urid_unmap(void);

#ifdef __cplusplus
}
#endif

#endif /* LV2HOST_CORE_H */
