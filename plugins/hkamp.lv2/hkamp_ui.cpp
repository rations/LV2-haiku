/*
 * hkamp_ui — native Haiku (Interface Kit) UI for the hkamp stereo amplifier.
 *
 * UI type <https://github.com/rations/LV2-haiku/ns/ui-haiku#HaikuUI>:
 * instantiate() returns a detached BView* as the LV2UI_Widget; the host
 * attaches it to its own BWindow, and calls port_event() and cleanup() with
 * that window's looper locked. The UI never touches plugin or RT state —
 * all writes go through the host-supplied write_function (control floats),
 * and feedback arrives via port_event().
 */
#include <lv2/core/lv2.h>
#include <lv2/ui/ui.h>

#include <GroupLayout.h>
#include <LayoutBuilder.h>
#include <Message.h>
#include <Slider.h>
#include <StringView.h>
#include <View.h>
#include <Window.h>

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HKAMP_URI "https://github.com/rations/LV2-haiku/plugins/hkamp"
#define HKAMP_UI_URI HKAMP_URI "#ui"

enum { HKAMP_PORT_GAIN = 0 };

static const int32 kMsgGainChanged = 'hkGn';

/* Slider is integer-stepped: 0..1140 <-> -90.0..+24.0 dB in 0.1 dB steps. */
static const float kGainMin = -90.0f;
static const float kGainMax = 24.0f;
static const int32 kSliderSteps = 1140;

static int32 gain_to_slider(float db)
{
    if (db < kGainMin) {
        db = kGainMin;
    }
    if (db > kGainMax) {
        db = kGainMax;
    }
    return (int32)((db - kGainMin) * (float)kSliderSteps / (kGainMax - kGainMin) + 0.5f);
}

static float slider_to_gain(int32 v)
{
    return kGainMin + (float)v * (kGainMax - kGainMin) / (float)kSliderSteps;
}

class HkampView : public BView
{
public:
    HkampView(LV2UI_Write_Function write, LV2UI_Controller controller)
        : BView("hkamp_ui", B_WILL_DRAW), fWrite(write), fController(controller), fSlider(NULL),
          fValueLabel(NULL), fFromHost(false)
    {
        SetLayout(new BGroupLayout(B_VERTICAL));
        fSlider = new BSlider("gain", "Gain (dB)", new BMessage(kMsgGainChanged), 0, kSliderSteps,
                              B_HORIZONTAL);
        fSlider->SetLimitLabels("-90", "+24");
        fSlider->SetHashMarks(B_HASH_MARKS_BOTTOM);
        fSlider->SetHashMarkCount(12);
        fSlider->SetModificationMessage(new BMessage(kMsgGainChanged));
        fSlider->SetValue(gain_to_slider(0.0f));
        fValueLabel = new BStringView("value", "0.0 dB");
        /* A BStringView's max width is its text width; left unlimited the
           label would clamp the whole layout (and the host window) to that
           width, making the window impossible to enlarge. */
        fValueLabel->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
        BLayoutBuilder::Group<>(this, B_VERTICAL, 8)
            .SetInsets(12, 12, 12, 12)
            .Add(fSlider)
            .Add(fValueLabel)
            .AddGlue()
            .End();
        SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
    }

    virtual void AttachedToWindow()
    {
        BView::AttachedToWindow();
        AdoptSystemColors();
        fSlider->SetTarget(this);
    }

    virtual void MessageReceived(BMessage *msg)
    {
        if (msg->what == kMsgGainChanged) {
            const float db = slider_to_gain(fSlider->Value());
            UpdateLabel(db);
            if (!fFromHost && fWrite) {
                fWrite(fController, HKAMP_PORT_GAIN, sizeof(float), 0, &db);
            }
            return;
        }
        BView::MessageReceived(msg);
    }

    /* Called by the host (looper locked) when the port value changes. */
    void SetGain(float db)
    {
        fFromHost = true;
        fSlider->SetValue(gain_to_slider(db));
        UpdateLabel(db);
        fFromHost = false;
    }

private:
    void UpdateLabel(float db)
    {
        char text[32];
        snprintf(text, sizeof(text), "%.1f dB", db);
        fValueLabel->SetText(text);
    }

    LV2UI_Write_Function fWrite;
    LV2UI_Controller fController;
    BSlider *fSlider;
    BStringView *fValueLabel;
    bool fFromHost;
};

extern "C" {

static LV2UI_Handle ui_instantiate(const LV2UI_Descriptor *descriptor, const char *plugin_uri,
                                   const char *bundle_path, LV2UI_Write_Function write_function,
                                   LV2UI_Controller controller, LV2UI_Widget *widget,
                                   const LV2_Feature *const *features)
{
    (void)descriptor;
    (void)bundle_path;
    (void)features;
    if (!widget || !plugin_uri || strcmp(plugin_uri, HKAMP_URI) != 0) {
        return NULL;
    }
    HkampView *view = new (std::nothrow) HkampView(write_function, controller);
    if (!view) {
        return NULL;
    }
    *widget = (LV2UI_Widget) static_cast<BView *>(view);
    return (LV2UI_Handle)view;
}

static void ui_cleanup(LV2UI_Handle handle)
{
    HkampView *view = static_cast<HkampView *>(handle);
    if (!view) {
        return;
    }
    /* The host calls this with the owning window's looper locked (or the
       view already detached); RemoveSelf is safe in both cases. */
    view->RemoveSelf();
    delete view;
}

static void ui_port_event(LV2UI_Handle handle, uint32_t port_index, uint32_t buffer_size,
                          uint32_t format, const void *buffer)
{
    HkampView *view = static_cast<HkampView *>(handle);
    if (!view || format != 0 || buffer_size != sizeof(float) || !buffer) {
        return;
    }
    if (port_index == HKAMP_PORT_GAIN) {
        view->SetGain(*(const float *)buffer);
    }
}

static const void *ui_extension_data(const char *uri)
{
    (void)uri;
    return NULL;
}

static const LV2UI_Descriptor hkamp_ui_descriptor = {
    HKAMP_UI_URI, ui_instantiate, ui_cleanup, ui_port_event, ui_extension_data,
};

LV2_SYMBOL_EXPORT const LV2UI_Descriptor *lv2ui_descriptor(uint32_t index)
{
    return (index == 0) ? &hkamp_ui_descriptor : NULL;
}

} /* extern "C" */
