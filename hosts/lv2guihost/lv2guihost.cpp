/*
 * lv2guihost — native-GUI LV2 test host for Haiku: runs one plugin through
 * JACK (shared hosting core, see lv2host_core.h) and shows its native UI.
 *
 * Two UI paths:
 *  - HaikuUI (<https://github.com/rations/LV2-haiku/ns/ui-haiku#HaikuUI>):
 *    the UI's LV2UI_Widget is a detached BView*; this host attaches it to
 *    its own BWindow. port_event()/cleanup() run on that window's looper
 *    with the looper locked.
 *  - ui:showInterface (any UI type): the UI owns its own window; the host
 *    calls show()/hide() and drives idle() until it returns non-zero.
 *
 * UI -> RT: the UI's write_function marshals control floats into the core's
 * pre-allocated lock-free ring. RT -> UI: a ~30 Hz tick compares the control
 * store and delivers changes via port_event(). No UI code ever runs on, or
 * blocks, the RT thread.
 */
#include "../common/lv2host_core.h"
#include "../../ns/ui-haiku.lv2/ui_haiku.h"

#include <lv2/ui/ui.h>

#include <Application.h>
#include <GroupLayout.h>
#include <Message.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <View.h>
#include <Window.h>

#include <dlfcn.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32 kMsgTick = 'tick';
static const bigtime_t kTickUsec = 33000; /* ~30 Hz */

static volatile sig_atomic_t quit_requested = 0;

static void quit_signal_handler(int sig)
{
    (void)sig;
    quit_requested = 1;
}

/* ---- Loaded UI module ---- */

struct UiModule {
    void *dl;
    const LV2UI_Descriptor *desc;
    LV2UI_Handle handle;
    BView *view; /* HaikuUI widget (not owned once attached) */
    const LV2UI_Show_Interface *show_iface;
    const LV2UI_Idle_Interface *idle_iface;
    bool shown;
};

/* write_function handed to the UI: control floats go through the core's
   lock-free ring; every other protocol is ignored (allow-list). */
static void ui_write_cb(LV2UI_Controller controller, uint32_t port_index, uint32_t buffer_size,
                        uint32_t protocol, const void *buffer)
{
    Lv2Host *h = (Lv2Host *)controller;
    if (protocol == 0 && buffer_size == sizeof(float) && buffer) {
        lv2host_write_control_port(h, port_index, *(const float *)buffer);
    }
}

/* UIs may declare lv2:requiredFeature; we only provide urid map/unmap. */
static bool ui_required_features_ok(LilvWorld *world, const LilvUI *ui)
{
    bool ok = true;
    LilvNode *req_pred = lilv_new_uri(world, LV2_CORE__requiredFeature);
    if (!req_pred) {
        return false;
    }
    LilvNodes *req = lilv_world_find_nodes(world, lilv_ui_get_uri(ui), req_pred, NULL);
    if (req) {
        LILV_FOREACH(nodes, it, req)
        {
            const LilvNode *f = lilv_nodes_get(req, it);
            const char *uri = lilv_node_as_uri(f);
            if (!uri || (strcmp(uri, LV2_URID__map) != 0 && strcmp(uri, LV2_URID__unmap) != 0)) {
                fprintf(stderr, "error: UI requires unsupported feature %s\n", uri ? uri : "(?)");
                ok = false;
            }
        }
        lilv_nodes_free(req);
    }
    lilv_node_free(req_pred);
    return ok;
}

/* dlopen the UI binary and find the descriptor whose URI matches. */
static bool ui_module_load(UiModule *m, LilvWorld *world, const LilvUI *ui)
{
    memset(m, 0, sizeof(*m));

    const LilvNode *bin = lilv_ui_get_binary_uri(ui);
    if (!bin) {
        fprintf(stderr, "error: UI has no ui:binary\n");
        return false;
    }
    char *path = lilv_file_uri_parse(lilv_node_as_uri(bin), NULL);
    if (!path) {
        fprintf(stderr, "error: cannot parse UI binary URI\n");
        return false;
    }
    m->dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!m->dl) {
        fprintf(stderr, "error: dlopen %s failed: %s\n", path, dlerror());
        lilv_free(path);
        return false;
    }
    lilv_free(path);

    LV2UI_DescriptorFunction df = (LV2UI_DescriptorFunction)dlsym(m->dl, "lv2ui_descriptor");
    if (!df) {
        fprintf(stderr, "error: no lv2ui_descriptor symbol: %s\n", dlerror());
        dlclose(m->dl);
        m->dl = NULL;
        return false;
    }
    const char *want = lilv_node_as_uri(lilv_ui_get_uri(ui));
    for (uint32_t i = 0;; i++) {
        const LV2UI_Descriptor *d = df(i);
        if (!d) {
            break;
        }
        if (d->URI && want && !strcmp(d->URI, want)) {
            m->desc = d;
            break;
        }
    }
    if (!m->desc) {
        fprintf(stderr, "error: descriptor <%s> not found in UI binary\n", want ? want : "(?)");
        dlclose(m->dl);
        m->dl = NULL;
        return false;
    }
    (void)world;
    return true;
}

static bool ui_module_instantiate(UiModule *m, Lv2Host *h, const char *plugin_uri, const LilvUI *ui)
{
    const LilvNode *bundle = lilv_ui_get_bundle_uri(ui);
    char *bundle_path = bundle ? lilv_file_uri_parse(lilv_node_as_uri(bundle), NULL) : NULL;

    const LV2_Feature feat_map_f = {LV2_URID__map, lv2host_urid_map()};
    const LV2_Feature feat_unmap_f = {LV2_URID__unmap, lv2host_urid_unmap()};
    const LV2_Feature *feats[] = {&feat_map_f, &feat_unmap_f, NULL};

    LV2UI_Widget widget = NULL;
    m->handle = m->desc->instantiate(m->desc, plugin_uri, bundle_path ? bundle_path : "",
                                     ui_write_cb, (LV2UI_Controller)h, &widget, feats);
    if (bundle_path) {
        lilv_free(bundle_path);
    }
    if (!m->handle) {
        fprintf(stderr, "error: UI instantiate failed\n");
        return false;
    }
    m->view = (BView *)widget;
    if (m->desc->extension_data) {
        m->show_iface =
            (const LV2UI_Show_Interface *)m->desc->extension_data(LV2_UI__showInterface);
        m->idle_iface =
            (const LV2UI_Idle_Interface *)m->desc->extension_data(LV2_UI__idleInterface);
    }
    return true;
}

/* ---- Window hosting a HaikuUI BView ---- */

class GuiWindow : public BWindow
{
public:
    GuiWindow(const char *title, Lv2Host *host, UiModule *mod)
        : BWindow(BRect(120, 120, 520, 260), title, B_TITLED_WINDOW,
                  B_AUTO_UPDATE_SIZE_LIMITS | B_NOT_ZOOMABLE),
          fHost(host), fMod(mod), fRunner(NULL), fLast(NULL)
    {
        SetLayout(new BGroupLayout(B_VERTICAL));
        if (fMod->view) {
            AddChild(fMod->view);
        }
        const uint32_t total = fHost->n_params + fHost->n_outs;
        fLast = (float *)calloc(total ? total : 1, sizeof(float));
        if (fLast) {
            for (uint32_t i = 0; i < fHost->n_params; i++) {
                /* Force one initial port_event per parameter. */
                fLast[i] = fHost->params[i].def + 1.0f;
            }
            for (uint32_t i = 0; i < fHost->n_outs; i++) {
                /* NAN compares unequal to everything: one initial event. */
                fLast[fHost->n_params + i] = NAN;
            }
        }
        fRunner = new BMessageRunner(BMessenger(this), new BMessage(kMsgTick), kTickUsec);
    }

    virtual ~GuiWindow()
    {
        delete fRunner;
        free(fLast);
    }

    virtual void MessageReceived(BMessage *msg)
    {
        if (msg->what == kMsgTick) {
            Tick();
            return;
        }
        BWindow::MessageReceived(msg);
    }

    virtual bool QuitRequested()
    {
        /* Looper is locked here: safe to clean the UI up per the HaikuUI
           threading contract, then quit the app. */
        if (fMod->handle && fMod->desc->cleanup) {
            fMod->desc->cleanup(fMod->handle);
            fMod->handle = NULL;
            fMod->view = NULL;
        }
        be_app->PostMessage(B_QUIT_REQUESTED);
        return true;
    }

private:
    void Tick()
    {
        if (quit_requested) {
            PostMessage(B_QUIT_REQUESTED);
            return;
        }
        if (!fMod->handle || !fLast) {
            return;
        }
        /* Deliver control changes to the UI (we are the looper thread). */
        if (fMod->desc->port_event) {
            for (uint32_t i = 0; i < fHost->n_params; i++) {
                const uint32_t port = fHost->params[i].port_index;
                const float v = lv2host_get_control(fHost, port);
                if (v != fLast[i]) {
                    fLast[i] = v;
                    fMod->desc->port_event(fMod->handle, port, sizeof(float), 0, &v);
                }
            }
            for (uint32_t i = 0; i < fHost->n_outs; i++) {
                const uint32_t port = fHost->outs[i].port_index;
                const float v = lv2host_get_control(fHost, port);
                if (v != fLast[fHost->n_params + i]) {
                    fLast[fHost->n_params + i] = v;
                    fMod->desc->port_event(fMod->handle, port, sizeof(float), 0, &v);
                }
            }
        }
        if (fMod->idle_iface && fMod->idle_iface->idle) {
            if (fMod->idle_iface->idle(fMod->handle) != 0) {
                PostMessage(B_QUIT_REQUESTED);
            }
        }
    }

    Lv2Host *fHost;
    UiModule *fMod;
    BMessageRunner *fRunner;
    float *fLast;
};

/* ---- App shell for the showInterface path (no window of ours) ---- */

class ShowIfaceApp : public BApplication
{
public:
    ShowIfaceApp(Lv2Host *host, UiModule *mod)
        : BApplication("application/x-vnd.rations-lv2guihost"), fHost(host), fMod(mod),
          fRunner(NULL), fLast(NULL)
    {
    }

    virtual void ReadyToRun()
    {
        const uint32_t total = fHost->n_params + fHost->n_outs;
        fLast = (float *)calloc(total ? total : 1, sizeof(float));
        if (fLast) {
            for (uint32_t i = 0; i < fHost->n_params; i++) {
                fLast[i] = fHost->params[i].def + 1.0f;
            }
            for (uint32_t i = 0; i < fHost->n_outs; i++) {
                fLast[fHost->n_params + i] = NAN;
            }
        }
        fRunner = new BMessageRunner(BMessenger(this), new BMessage(kMsgTick), kTickUsec);
    }

    virtual void MessageReceived(BMessage *msg)
    {
        if (msg->what == kMsgTick) {
            if (quit_requested) {
                PostMessage(B_QUIT_REQUESTED);
                return;
            }
            if (fMod->desc->port_event && fLast) {
                for (uint32_t i = 0; i < fHost->n_params; i++) {
                    const uint32_t port = fHost->params[i].port_index;
                    const float v = lv2host_get_control(fHost, port);
                    if (v != fLast[i]) {
                        fLast[i] = v;
                        fMod->desc->port_event(fMod->handle, port, sizeof(float), 0, &v);
                    }
                }
                for (uint32_t i = 0; i < fHost->n_outs; i++) {
                    const uint32_t port = fHost->outs[i].port_index;
                    const float v = lv2host_get_control(fHost, port);
                    if (v != fLast[fHost->n_params + i]) {
                        fLast[fHost->n_params + i] = v;
                        fMod->desc->port_event(fMod->handle, port, sizeof(float), 0, &v);
                    }
                }
            }
            /* The spec requires the host to drive idle() and to call hide()
               once idle() returns non-zero (the user closed the UI). */
            if (fMod->idle_iface && fMod->idle_iface->idle &&
                fMod->idle_iface->idle(fMod->handle) != 0) {
                if (fMod->shown && fMod->show_iface && fMod->show_iface->hide) {
                    fMod->show_iface->hide(fMod->handle);
                    fMod->shown = false;
                }
                PostMessage(B_QUIT_REQUESTED);
            }
            return;
        }
        BApplication::MessageReceived(msg);
    }

    virtual bool QuitRequested()
    {
        if (fMod->shown && fMod->show_iface && fMod->show_iface->hide) {
            fMod->show_iface->hide(fMod->handle);
            fMod->shown = false;
        }
        if (fMod->handle && fMod->desc->cleanup) {
            fMod->desc->cleanup(fMod->handle);
            fMod->handle = NULL;
        }
        delete fRunner;
        fRunner = NULL;
        free(fLast);
        fLast = NULL;
        return BApplication::QuitRequested();
    }

private:
    Lv2Host *fHost;
    UiModule *fMod;
    BMessageRunner *fRunner;
    float *fLast;
};

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: lv2guihost <plugin-uri>\n");
        fprintf(stderr, "       (list installed plugins with lv2ls)\n");
        return 1;
    }

    static Lv2Host h;
    if (!lv2host_open(&h, argv[1], "lv2guihost")) {
        return 1;
    }
    lv2host_autoconnect(&h);

    LilvNode *name = lilv_plugin_get_name(h.plugin);
    printf("loaded: %s <%s>\n", name ? lilv_node_as_string(name) : "?", argv[1]);
    printf("audio: %d in, %d out%s; sr %.0f Hz, block %u\n", h.n_audio_in, h.n_audio_out,
           h.dual_mono ? " (dual-mono x2)" : "", h.sample_rate, h.max_block);

    /* Pick a UI: prefer HaikuUI (embeddable BView); otherwise any UI that
       declares ui:showInterface (owns its own window). */
    LilvNode *haiku_ui_class = lilv_new_uri(h.world, LV2_UI_HAIKU__HaikuUI);
    LilvNode *ext_data_pred = lilv_new_uri(h.world, LV2_CORE__extensionData);
    LilvNode *show_iface_node = lilv_new_uri(h.world, LV2_UI__showInterface);
    const LilvUI *chosen = NULL;
    bool use_show = false;

    LilvUIs *uis = lilv_plugin_get_uis(h.plugin);
    if (uis && haiku_ui_class && ext_data_pred && show_iface_node) {
        LILV_FOREACH(uis, it, uis)
        {
            const LilvUI *ui = lilv_uis_get(uis, it);
            if (lilv_ui_is_a(ui, haiku_ui_class)) {
                chosen = ui;
                use_show = false;
                break;
            }
            if (!chosen &&
                lilv_world_ask(h.world, lilv_ui_get_uri(ui), ext_data_pred, show_iface_node)) {
                chosen = ui;
                use_show = true;
            }
        }
    }
    lilv_node_free(haiku_ui_class);
    lilv_node_free(ext_data_pred);
    lilv_node_free(show_iface_node);

    int rc = 1;
    UiModule mod;
    memset(&mod, 0, sizeof(mod));

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = quit_signal_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGTERM, &sa, NULL) != 0 || sigaction(SIGINT, &sa, NULL) != 0 ||
        sigaction(SIGHUP, &sa, NULL) != 0) {
        fprintf(stderr, "warning: failed to install signal handlers\n");
    }

    if (!chosen) {
        fprintf(stderr, "error: plugin has no HaikuUI and no ui:showInterface UI\n");
    } else if (!ui_required_features_ok(h.world, chosen)) {
        /* message already printed */
    } else if (ui_module_load(&mod, h.world, chosen)) {
        if (use_show) {
            printf("UI: ui:showInterface (UI owns its window)\n");
            ShowIfaceApp app(&h, &mod);
            if (ui_module_instantiate(&mod, &h, argv[1], chosen)) {
                if (mod.show_iface && mod.show_iface->show &&
                    mod.show_iface->show(mod.handle) == 0) {
                    mod.shown = true;
                    app.Run();
                    rc = 0;
                } else {
                    fprintf(stderr, "error: UI show() failed\n");
                }
                if (mod.handle && mod.desc->cleanup) {
                    mod.desc->cleanup(mod.handle);
                    mod.handle = NULL;
                }
            }
        } else {
            printf("UI: HaikuUI (embedded BView)\n");
            BApplication app("application/x-vnd.rations-lv2guihost");
            if (ui_module_instantiate(&mod, &h, argv[1], chosen)) {
                if (mod.view) {
                    GuiWindow *win =
                        new GuiWindow(name ? lilv_node_as_string(name) : "LV2 plugin", &h, &mod);
                    win->Show();
                    app.Run();
                    rc = 0;
                } else {
                    fprintf(stderr, "error: HaikuUI returned no BView widget\n");
                    if (mod.desc->cleanup) {
                        mod.desc->cleanup(mod.handle);
                        mod.handle = NULL;
                    }
                }
            }
        }
        if (mod.dl) {
            dlclose(mod.dl);
        }
    }

    if (uis) {
        lilv_uis_free(uis);
    }
    if (name) {
        lilv_node_free(name);
    }
    unsigned xruns = h.n_xruns;
    lv2host_close(&h);
    printf("xruns while running: %u\n", xruns);
    return rc;
}
