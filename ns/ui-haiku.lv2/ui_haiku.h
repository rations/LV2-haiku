/*
 * HaikuUI — a native Haiku LV2 UI type.
 *
 * A UI of type <https://github.com/rations/LV2-haiku/ns/ui-haiku#HaikuUI>
 * sets *widget in LV2UI_Descriptor::instantiate() to a BView* that is not
 * yet attached to any window (exactly parallel to ui:CocoaUI and NSView*).
 * The host attaches it to a BWindow of its own; from that point on the view
 * runs on that window's looper thread, and the host calls port_event() and
 * cleanup() with that looper locked. The UI touches plugin state only via
 * the write_function given at instantiate time.
 *
 * If ui:parent is supplied it is the host's parent BView*; a UI may add
 * itself instead of returning a detached view, but returning the view and
 * letting the host attach it is the recommended, order-safe path.
 */
#ifndef LV2_UI_HAIKU_H
#define LV2_UI_HAIKU_H

#define LV2_UI_HAIKU_URI "https://github.com/rations/LV2-haiku/ns/ui-haiku"
#define LV2_UI_HAIKU_PREFIX LV2_UI_HAIKU_URI "#"
#define LV2_UI_HAIKU__HaikuUI LV2_UI_HAIKU_PREFIX "HaikuUI"

#endif /* LV2_UI_HAIKU_H */
