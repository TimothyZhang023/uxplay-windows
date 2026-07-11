#include <gst/gst.h>
#include <stdio.h>

static int require_property(GstElement *element, const char *name) {
    if (!g_object_class_find_property(G_OBJECT_GET_CLASS(element), name)) {
        fprintf(stderr, "missing property '%s' on %s\n",
                name, GST_ELEMENT_NAME(element));
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);

    guint major = 0, minor = 0, micro = 0, nano = 0;
    gst_version(&major, &minor, &micro, &nano);
    (void)nano;
    if (major < 1 || (major == 1 && minor < 26)) {
        fprintf(stderr,
                "GStreamer 1.26+ is required for native mouse-double-click events; found %u.%u.%u\n",
                major, minor, micro);
        return 5;
    }

    GstElement *d3d11 = gst_element_factory_make("d3d11videosink", "d3d11-test");
    GstElement *d3d12 = gst_element_factory_make("d3d12videosink", "d3d12-test");
    if (!d3d11 || !d3d12) {
        fprintf(stderr, "could not instantiate D3D11/D3D12 video sinks\n");
        return 1;
    }

    if (!require_property(d3d11, "enable-navigation-events") ||
        !require_property(d3d11, "fullscreen-toggle-mode") ||
        !require_property(d3d11, "fullscreen") ||
        !require_property(d3d12, "enable-navigation-events") ||
        !require_property(d3d12, "fullscreen-on-alt-enter") ||
        !require_property(d3d12, "fullscreen")) {
        return 2;
    }

    gst_util_set_object_arg(G_OBJECT(d3d11), "fullscreen-toggle-mode",
                            "property");
    g_object_set(G_OBJECT(d3d11),
                 "enable-navigation-events", TRUE,
                 "fullscreen", TRUE,
                 NULL);
    guint d3d11_mode = 0;
    gboolean d3d11_navigation = FALSE;
    gboolean d3d11_fullscreen = FALSE;
    g_object_get(G_OBJECT(d3d11),
                 "enable-navigation-events", &d3d11_navigation,
                 "fullscreen-toggle-mode", &d3d11_mode,
                 "fullscreen", &d3d11_fullscreen,
                 NULL);
    /* D3D11 flags: Alt+Enter=0x2, PROPERTY=0x4. Require only PROPERTY. */
    if (!d3d11_navigation || d3d11_mode != 0x4 || !d3d11_fullscreen) {
        fprintf(stderr, "D3D11 navigation/property fullscreen was not accepted\n");
        return 3;
    }

    g_object_set(G_OBJECT(d3d12),
                 "enable-navigation-events", TRUE,
                 "fullscreen-on-alt-enter", FALSE,
                 "fullscreen", TRUE,
                 NULL);
    gboolean d3d12_navigation = FALSE;
    gboolean d3d12_alt_enter = TRUE;
    gboolean d3d12_fullscreen = FALSE;
    g_object_get(G_OBJECT(d3d12),
                 "enable-navigation-events", &d3d12_navigation,
                 "fullscreen-on-alt-enter", &d3d12_alt_enter,
                 "fullscreen", &d3d12_fullscreen,
                 NULL);
    if (!d3d12_navigation || d3d12_alt_enter || !d3d12_fullscreen) {
        fprintf(stderr, "D3D12 navigation/property fullscreen was not accepted\n");
        return 4;
    }

    gst_object_unref(d3d11);
    gst_object_unref(d3d12);
    puts("Validated F11/double-click D3D fullscreen properties; Alt+Enter disabled");
    return 0;
}
