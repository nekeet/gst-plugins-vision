#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstbaumersrc.h"
#include <gst/gst.h>
#include <gst/video/video.h>
#include <glib.h>

#include "common/genicampixelformat.h"

GST_DEBUG_CATEGORY_STATIC (gst_baumer_src_debug);
#define GST_CAT_DEFAULT gst_baumer_src_debug

#define MAX_ARRAY_SIZE 64

/* TODO: Modify to accept additional args e.g. index */
#define HANDLE_BGAPI2_ERROR(arg)  \
        if (ret != BGAPI2_RESULT_SUCCESS) {   \
                GST_ELEMENT_ERROR(src, LIBRARY, FAILED, \
                                   (arg ": %s", \
                                    gst_baumer_src_get_error_string(src)), \
                                   (NULL)); \
                goto error; \
        }

typedef struct
{
        GstBaumerSrc *src;
        BGAPI2_Buffer * image_buffer;
} VideoFrame;

/* Prototypes */
static void gst_baumer_src_set_property (GObject *object, guint property_id,
                                         const GValue *value, GParamSpec *pspec);
static void gst_baumer_src_get_property (GObject *object, guint property_id,
                                         GValue *value, GParamSpec *spec);
static void gst_baumer_src_dispose (GObject *object);
static void gst_baumer_src_finalize (GObject *object);

static gboolean gst_baumer_src_start (GstBaseSrc *src);
static gboolean gst_baumer_src_stop (GstBaseSrc *src);
static GstCaps * gst_baumer_src_get_caps (GstBaseSrc * bsrc, GstCaps * filter);
static gboolean gst_baumer_src_set_caps (GstBaseSrc *bsrc, GstCaps *caps);
static gboolean gst_baumer_src_unlock (GstBaseSrc *src);
static gboolean gst_baumer_src_unlock_stop (GstBaseSrc *src);

static void gst_baumer_src_print_system_info (GstBaumerSrc * src);
static void gst_baumer_src_print_interface_info (GstBaumerSrc * src);
static void gst_baumer_src_print_device_info (GstBaumerSrc * src);

static gboolean gst_baumer_src_connect_device (GstBaumerSrc * src);
static void gst_baumer_src_release_device (GstBaumerSrc * src);

static gboolean gst_baumer_src_set_framerate (GstBaumerSrc * src);
static gboolean gst_baumer_src_set_gain (GstBaumerSrc * src);
static gboolean gst_baumer_src_set_exposure_time (GstBaumerSrc * src);
static gboolean gst_baumer_src_set_trigger_mode (GstBaumerSrc * src);
static gboolean gst_baumer_src_set_resolution (GstBaumerSrc * src);
static gboolean gst_baumer_src_set_pixel_format (GstBaumerSrc * src);

static void delete_format (gpointer data);
static gboolean gst_baumer_src_get_supported_pixel_formats (GstBaumerSrc * src, GPtrArray ** format_list);
static GstCaps * gst_baumer_src_get_supported_caps (GstBaumerSrc * src);

static gboolean gst_baumer_src_acquisition_configure (GstBaumerSrc * src);
static gboolean gst_baumer_src_acquisition_start (GstBaumerSrc * src);
static void gst_baumer_src_acquisition_stop (GstBaumerSrc * src);

static GstFlowReturn gst_baumer_src_create (GstPushSrc *src, GstBuffer **buf);

static guint64 gst_baumer_src_get_payload_size (GstBaumerSrc * src);
static gboolean gst_baumer_src_prepare_buffers (GstBaumerSrc * src);

static gchar * gst_baumer_src_get_error_string (GstBaumerSrc *src);



enum {
        PROP_0,
        PROP_HEIGHT,
        PROP_WIDTH,
        PROP_BINNINGH,
        PROP_BINNINGV,
        PROP_FRAMERATE,
        PROP_EXPOSURE_TIME,
        PROP_GAIN,
        PROP_PIXEL_FORMAT,
        PROP_INTERFACE_INDEX,
        PROP_DEVICE_INDEX,
        PROP_DATASTREAM_INDEX,
        PROP_NUM_CAPTURE_BUFFERS,
        PROP_TIMEOUT
};

#define DEFAULT_PROP_INTERFACE_INDEX 9999
#define DEFAULT_PROP_DEVICE_INDEX 9999
#define DEFAULT_PROP_DATASTREAM_INDEX 9999
#define DEFAULT_PROP_NUM_CAPTURE_BUFFERS 4
#define DEFAULT_PROP_TIMEOUT 1000
#define DEFAULT_PROP_FRAMERATE 0.0
#define DEFAULT_PROP_HEIGHT 0
#define DEFAULT_PROP_WIDTH 0
#define DEFAULT_PROP_EXPOSURE_TIME 0.0
#define DEFAULT_PROP_GAIN 0.0
#define DEFAULT_PROP_BINNINGH 1
#define DEFAULT_PROP_BINNINGV 1
#define DEFAULT_PROP_PIXEL_FORMAT "auto"

static GstStaticPadTemplate gst_baumer_src_template =
        GST_STATIC_PAD_TEMPLATE ("src",
                                 GST_PAD_SRC,
                                 GST_PAD_ALWAYS,
                                 GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE
                                                  ("{ GRAY8, GRAY16_LE, GRAY16_BE, BGRA }"))
                );

G_DEFINE_TYPE (GstBaumerSrc, gst_baumer_src, GST_TYPE_PUSH_SRC);

static void
gst_baumer_src_class_init(GstBaumerSrcClass *klass)
{
        GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
        GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
        GstBaseSrcClass *gstbasesrc_class = GST_BASE_SRC_CLASS (klass);
        GstPushSrcClass *gstpushsrc_class = GST_PUSH_SRC_CLASS (klass);

        gobject_class->set_property = gst_baumer_src_set_property;
        gobject_class->get_property = gst_baumer_src_get_property;
        gobject_class->dispose = gst_baumer_src_dispose;
        gobject_class->finalize = gst_baumer_src_finalize;

        gst_element_class_add_pad_template (gstelement_class,
                                           gst_static_pad_template_get(
                                                   &gst_baumer_src_template));

        gst_element_class_set_static_metadata (gstelement_class,
                                               "Baumer USB3 Vision video source",
                                               "Source/Video/Device",
                                               "Use BGAPI to acquire video "
                                               "from Baumer USB3 Vision cameras "
                                               "for GStreamer source",
                                               "Nikita Semakhin "
                                               "<n.semakhin@kb-nt.com>");

        gstbasesrc_class->start = GST_DEBUG_FUNCPTR (gst_baumer_src_start);
        gstbasesrc_class->stop = GST_DEBUG_FUNCPTR (gst_baumer_src_stop);
        gstbasesrc_class->get_caps = GST_DEBUG_FUNCPTR (gst_baumer_src_get_caps);
        gstbasesrc_class->set_caps = GST_DEBUG_FUNCPTR (gst_baumer_src_set_caps);
        gstbasesrc_class->unlock = GST_DEBUG_FUNCPTR (gst_baumer_src_unlock);
        gstbasesrc_class->unlock_stop =
                GST_DEBUG_FUNCPTR (gst_baumer_src_unlock_stop);

        gstpushsrc_class->create = GST_DEBUG_FUNCPTR (gst_baumer_src_create);

  /* Install GObject properties */

        g_object_class_install_property (gobject_class, PROP_INTERFACE_INDEX,
                                         g_param_spec_uint ("interface-index", "Interface index",
                                                            "Interface index number",
                                                            0, G_MAXUINT, DEFAULT_PROP_INTERFACE_INDEX,
                                                            (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                                                                           GST_PARAM_MUTABLE_READY)));
        g_object_class_install_property (gobject_class, PROP_DEVICE_INDEX,
                                         g_param_spec_uint ("device-index", "Device index",
                                                            "Device index number",
                                                            0, G_MAXUINT, DEFAULT_PROP_DEVICE_INDEX,
                                                            (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                                                                           GST_PARAM_MUTABLE_READY)));
        g_object_class_install_property (gobject_class, PROP_DATASTREAM_INDEX,
                                         g_param_spec_uint ("datastream-index", "Datastream index",
                                                            "Datastream index number",
                                                            0, G_MAXUINT, DEFAULT_PROP_DATASTREAM_INDEX,
                                                            (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                                                                           GST_PARAM_MUTABLE_READY)));
        g_object_class_install_property (gobject_class, PROP_NUM_CAPTURE_BUFFERS,
                                         g_param_spec_uint ("num-capture-buffers", "Number of capture buffers",
                                                            "Number of capture buffers", 1, G_MAXUINT,
                                                            DEFAULT_PROP_NUM_CAPTURE_BUFFERS,
                                                            (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
        g_object_class_install_property (gobject_class, PROP_TIMEOUT,
                                         g_param_spec_int ("timeout",
                                                           "Buffer receive timeout (ms)",
                                                           "Buffer receive timeout in ms (0 to use default)", 0, G_MAXINT,
                                                           DEFAULT_PROP_TIMEOUT,
                                                           G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS ));
        g_object_class_install_property (gobject_class, PROP_FRAMERATE,
                                         g_param_spec_double ("framerate", "Acquisition framerate",
                                                              "Camera acquisition framerate (frames per second)",
                                                              0.0, 1024.0, DEFAULT_PROP_FRAMERATE,
                                                              (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
        g_object_class_install_property (gobject_class, PROP_HEIGHT,
                                         g_param_spec_int ("height", "Frame height",
                                                           "Height of the picture frame (pixels)",
                                                           0, G_MAXINT, DEFAULT_PROP_HEIGHT,
                                                           (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
        g_object_class_install_property (gobject_class, PROP_WIDTH,
                                         g_param_spec_int ("width", "Frame width",
                                                           "Width of the picture frame (pixels)",
                                                           0, G_MAXINT, DEFAULT_PROP_WIDTH,
                                                           (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
        g_object_class_install_property (gobject_class, PROP_BINNINGH,
                                         g_param_spec_int ("binningh", "Horizontal binning",
                                                           "Number of pixels to be binned in horizontal direction",
                                                           1, 6, DEFAULT_PROP_BINNINGH,
                                                           (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
        g_object_class_install_property (gobject_class, PROP_BINNINGV,
                                         g_param_spec_int ("binningv", "Vertical binning",
                                                           "Number of pixels to be binned in vertical direction",
                                                           1, 6, DEFAULT_PROP_BINNINGV,
                                                           (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
        g_object_class_install_property (gobject_class, PROP_PIXEL_FORMAT,
                                         g_param_spec_string ("pixel-format", "Pixel format",
                                                              "Image pixel format (e.g. Mono8, case-sensitive)",
                                                              DEFAULT_PROP_PIXEL_FORMAT,
                                                              (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
        g_object_class_install_property (gobject_class, PROP_GAIN,
                                         g_param_spec_double ("gain", "Gain",
                                                              "Camera gain (dbm)",
                                                              0.0, G_MAXDOUBLE,
                                                              DEFAULT_PROP_GAIN,
                                                              (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
        g_object_class_install_property (gobject_class, PROP_EXPOSURE_TIME,
                                         g_param_spec_double ("exposure-time", "Exposure time",
                                                              "Exposure time for the camera (microseconds)",
                                                              0.0, G_MAXDOUBLE,
                                                              DEFAULT_PROP_EXPOSURE_TIME,
                                                              (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

}

static inline gboolean
baumer_feature_implemented (GstBaumerSrc * src, const gchar * feature)
{
        BGAPI2_Node* node = NULL;
        bo_bool find_flag = 0;
        if (BGAPI2_Device_GetRemoteNode(src->device, feature, &node) == BGAPI2_RESULT_SUCCESS) {
                if (BGAPI2_Node_GetImplemented (node, &find_flag) == BGAPI2_RESULT_SUCCESS
                    && find_flag)
                        return TRUE;
                else
                        return FALSE;
        } else
                return FALSE;
}

static inline gboolean
baumer_feature_available (GstBaumerSrc * src, const gchar * feature)
{
        BGAPI2_Node* node = NULL;
        bo_bool find_flag = 0;
        if (BGAPI2_Device_GetRemoteNode(src->device, feature, &node) == BGAPI2_RESULT_SUCCESS) {
                if (BGAPI2_Node_GetAvailable (node, &find_flag) == BGAPI2_RESULT_SUCCESS
                    && find_flag)
                        return TRUE;
                else
                        return FALSE;
        } else
                return FALSE;
}

static inline gboolean
baumer_feature_readable (GstBaumerSrc * src, const gchar * feature)
{
        BGAPI2_Node* node = NULL;
        bo_bool find_flag = 0;
        if (BGAPI2_Device_GetRemoteNode(src->device, feature, &node) == BGAPI2_RESULT_SUCCESS) {
                if (BGAPI2_Node_IsReadable(node, &find_flag) == BGAPI2_RESULT_SUCCESS
                    && find_flag)
                        return TRUE;
                else
                        return FALSE;
        } else
                return FALSE;
}

static inline gboolean
baumer_feature_writeable (GstBaumerSrc * src, const gchar * feature)
{
        BGAPI2_Node* node = NULL;
        bo_bool find_flag = 0;
        if (BGAPI2_Device_GetRemoteNode(src->device, feature, &node) == BGAPI2_RESULT_SUCCESS) {
                if (BGAPI2_Node_IsWriteable(node, &find_flag) == BGAPI2_RESULT_SUCCESS
                    && find_flag)
                        return TRUE;
                else
                        return FALSE;
        } else
                return FALSE;
}


static void
gst_baumer_src_reset (GstBaumerSrc *src)
{
        src->error_string[0] = 0;
        src->last_frame_count = 0;
        src->total_dropped_frames = 0;

        if (src->caps) {
                gst_caps_unref (src->caps);
                src->caps = NULL;
        }
}

static void
gst_baumer_src_init (GstBaumerSrc *src)
{
        /* initialize member variables */
        src->interface_index = DEFAULT_PROP_INTERFACE_INDEX;
        src->device_index = DEFAULT_PROP_DEVICE_INDEX;
        src->datastream_index = DEFAULT_PROP_DATASTREAM_INDEX;
        src->num_capture_buffers = DEFAULT_PROP_NUM_CAPTURE_BUFFERS;
        src->timeout = DEFAULT_PROP_TIMEOUT;
        src->framerate = DEFAULT_PROP_FRAMERATE;
        src->exposure_time = DEFAULT_PROP_EXPOSURE_TIME;
        src->gain = DEFAULT_PROP_GAIN;
        src->height = DEFAULT_PROP_HEIGHT;
        src->width = DEFAULT_PROP_WIDTH;
        src->pixel_format = DEFAULT_PROP_PIXEL_FORMAT;
        src->binningh = DEFAULT_PROP_BINNINGH;
        src->binningv = DEFAULT_PROP_BINNINGV;

        src->device_connected = FALSE;
        src->acquisition_configured = FALSE;
        src->acquisition_started = FALSE;
        src->stop_requested = FALSE;
        src->payload_size = 0;
        src->caps = NULL;

        src->system = NULL;
        src->interface = NULL;
        src->device = NULL;
        src->datastream = NULL;

        gst_base_src_set_live(GST_BASE_SRC(src), TRUE);
        gst_base_src_set_format(GST_BASE_SRC(src), GST_FORMAT_TIME);
        gst_base_src_set_do_timestamp (GST_BASE_SRC (src), TRUE);

        gst_baumer_src_reset (src);

}

void
gst_baumer_src_set_property (GObject * object, guint property_id,
                             const GValue * value, GParamSpec * pspec)
{
        GstBaumerSrc *src = GST_BAUMER_SRC (object);

        switch (property_id) {
        case PROP_INTERFACE_INDEX:
                src->interface_index = g_value_get_uint (value);
                break;
        case PROP_DEVICE_INDEX:
                src->device_index = g_value_get_uint (value);
                break;
        case PROP_DATASTREAM_INDEX:
                src->datastream_index = g_value_get_uint (value);
                break;
        case PROP_NUM_CAPTURE_BUFFERS:
                src->num_capture_buffers = g_value_get_uint (value);
                break;
        case PROP_TIMEOUT:
                src->timeout = g_value_get_int (value);
                break;
        case PROP_HEIGHT:
                src->height = g_value_get_int (value);
                break;
        case PROP_WIDTH:
                src->width = g_value_get_int (value);
                break;
        case PROP_BINNINGH:
                src->binningh = g_value_get_int (value);
                break;
        case PROP_BINNINGV:
                src->binningv = g_value_get_int (value);
                break;
        case PROP_PIXEL_FORMAT:
                g_free (src->pixel_format);
                src->pixel_format = g_value_dup_string (value);
                break;
        case PROP_FRAMERATE:
                src->framerate = g_value_get_double (value);
                break;
        case PROP_EXPOSURE_TIME:
                src->exposure_time = g_value_get_double (value);
                break;
        case PROP_GAIN:
                src->gain = g_value_get_double (value);
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
                break;
        }
}

void
gst_baumer_src_get_property (GObject * object, guint property_id,
                             GValue * value, GParamSpec * pspec)
{
        GstBaumerSrc *src;

        g_return_if_fail (GST_IS_BAUMER_SRC (object));
        src = GST_BAUMER_SRC (object);

        switch (property_id) {
        case PROP_INTERFACE_INDEX:
                g_value_set_uint (value, src->interface_index);
                break;
        case PROP_DEVICE_INDEX:
                g_value_set_uint (value, src->device_index);
                break;
        case PROP_DATASTREAM_INDEX:
                g_value_set_uint (value, src->datastream_index);
                break;
        case PROP_NUM_CAPTURE_BUFFERS:
                g_value_set_uint (value, src->num_capture_buffers);
                break;
        case PROP_TIMEOUT:
                g_value_set_int (value, src->timeout);
                break;
        case PROP_HEIGHT:
                g_value_set_int (value, src->height);
                break;
        case PROP_WIDTH:
                g_value_set_int (value, src->width);
                break;
        case PROP_BINNINGH:
                g_value_set_int (value, src->binningh);
                break;
        case PROP_BINNINGV:
                g_value_set_int (value, src->binningv);
                break;
        case PROP_PIXEL_FORMAT:
                g_value_set_string (value, src->pixel_format);
                break;
        case PROP_FRAMERATE:
                g_value_set_double (value, src->framerate);
                break;
        case PROP_EXPOSURE_TIME:
                g_value_set_double (value, src->exposure_time);
                break;
        case PROP_GAIN:
                g_value_set_double (value, src->gain);
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
                break;
        }
}

void
gst_baumer_src_dispose (GObject * object)
{
        GstBaumerSrc *src;

        g_return_if_fail (GST_IS_BAUMER_SRC (object));
        src = GST_BAUMER_SRC (object);

        /* clean up as possible.  may be called multiple times */

        G_OBJECT_CLASS (gst_baumer_src_parent_class)->dispose (object);
}

void
gst_baumer_src_finalize (GObject * object)
{
        GstBaumerSrc *src;

        g_return_if_fail (GST_IS_BAUMER_SRC (object));
        src = GST_BAUMER_SRC (object);

        /* clean up object here */

        if (src->caps) {
                gst_caps_unref (src->caps);
                src->caps = NULL;
        }

        G_OBJECT_CLASS (gst_baumer_src_parent_class)->finalize (object);
}

static gboolean
gst_baumer_src_connect_device (GstBaumerSrc * src)
{
        BGAPI2_RESULT ret;
        guint32 i;
        guint num_interfaces, num_devices;

        ret = BGAPI2_UpdateSystemList ();
        HANDLE_BGAPI2_ERROR ("Unable to update system list");
        ret = BGAPI2_GetSystem (1, &(src->system));
        HANDLE_BGAPI2_ERROR ("Unable to find USB system");
        ret = BGAPI2_System_Open (src->system);
        HANDLE_BGAPI2_ERROR ("Unable to open USB system");

        gst_baumer_src_print_system_info (src);

        // Create the interface
        bo_bool changed = 0;
        ret = BGAPI2_System_UpdateInterfaceList (src->system, &changed, 100);
        HANDLE_BGAPI2_ERROR ("Unable to update interface list");

        ret = BGAPI2_System_GetNumInterfaces (src->system, &num_interfaces);
        HANDLE_BGAPI2_ERROR ("Unable to evaluate interfaces number");

        if (num_interfaces == 0) {
                GST_ERROR_OBJECT (src, "No interfaces found, cancelling "
                                  "initialisation.");
                GST_ELEMENT_ERROR (src, RESOURCE, FAILED,
                                   ("No interfaces found"),
                                   (NULL));
        } else if (num_interfaces == 1) {
                if (src->interface_index != 9999) {
                        GST_DEBUG_OBJECT (src, "interface-index was set, "
                                          "but beign ignored as only "
                                          "single instance was found.");
                }
                src->interface_index = 0;
        } else if (num_interfaces > 1 && src->interface_index == 9999) {
                GST_DEBUG_OBJECT (src, "Multiple interfaces found, but "
                                  "interface-index property wasn't specified");
                goto error;
        }

        ret = BGAPI2_System_GetInterface (src->system, src->interface_index,
                                          &(src->interface));
        HANDLE_BGAPI2_ERROR ("Unable to get interface");

        ret = BGAPI2_Interface_Open (src->interface);
        HANDLE_BGAPI2_ERROR ("Unable to open interface");

        gst_baumer_src_print_interface_info (src);

        /* Create the device */
        ret = BGAPI2_Interface_UpdateDeviceList(src->interface, &changed, 200);
        HANDLE_BGAPI2_ERROR ("Unable to update device list");

        ret = BGAPI2_Interface_GetNumDevices (src->interface, &num_devices);
        HANDLE_BGAPI2_ERROR ("Unable to evaluate interfaces number");

        if (num_devices == 0) {
                GST_ERROR_OBJECT (src, "No devices found, canceling "
                                  "initialisation.");
                GST_ELEMENT_ERROR (src, RESOURCE, FAILED,
                                   ("No devices found"),
                                   (NULL));
                goto error;
        } else if (num_devices == 1) {
                if (src->device_index != 9999) {
                        GST_DEBUG_OBJECT (src, "device-index was set, "
                                          "but beign ignored as only "
                                          "single instance found.");
                }
                src->device_index = 0;
        } else if (num_devices > 1 && src->device_index == 9999) {
                GST_DEBUG_OBJECT (src, "Multiple devices found, but "
                                  "device-index property wasn't specified");
                goto error;
        }

        ret = BGAPI2_Interface_GetDevice(src->interface, src->device_index,
                                         &(src->device));
        HANDLE_BGAPI2_ERROR ("Unable to get device");

        ret = BGAPI2_Device_Open(src->device);
        HANDLE_BGAPI2_ERROR ("Unable to open interface");
        src->device_connected = TRUE;

        gst_baumer_src_print_device_info (src);

        return TRUE;

error:
        return FALSE;
}

static void
gst_baumer_src_release_device (GstBaumerSrc * src)
{
        if (src->device) {
                BGAPI2_Device_Close (src->device);
                src->device = NULL;
        }

        if (src->interface) {
                BGAPI2_Interface_Close (src->interface);
                src->interface = NULL;
        }

        if (src->system) {
                BGAPI2_System_Close (src->system);
                BGAPI2_ReleaseSystem (src->system);
                src->system = NULL;
        }
        src->device_connected = FALSE;
}

static gboolean
gst_baumer_src_set_framerate (GstBaumerSrc * src)
{
        BGAPI2_RESULT ret;
        BGAPI2_Node * node;

        // Set framerate
        if (baumer_feature_available (src, "AcquisitionFrameRateEnable")) {
                if (src->framerate != 0) {
                        ret = BGAPI2_Device_GetRemoteNode(src->device, "AcquisitionFrameRateEnable", &node);
                        HANDLE_BGAPI2_ERROR ("Unable to get AcquisitionFrameRateEnable node");
                        ret = BGAPI2_Node_SetBool(node, 1);
                        HANDLE_BGAPI2_ERROR ("Unable to set AcquisitionFrameRateEnable node");

                        if (baumer_feature_available(src, SFNC_ACQUISITION_FRAMERATE)) {
                                GST_DEBUG_OBJECT (src, "Capping framerate to %0.2lf.", src->framerate);
                                ret = BGAPI2_Device_GetRemoteNode(src->device, SFNC_ACQUISITION_FRAMERATE, &node);
                                HANDLE_BGAPI2_ERROR ("Unable to get AcquisitionFrameRate node");
                                ret = BGAPI2_Node_SetDouble(node, src->framerate);
                                HANDLE_BGAPI2_ERROR ("Unable to set AcquisitionFrameRate node");
                        } else {
                                GST_WARNING_OBJECT (src, "Setting AcquisitionFrameRate is unavailable");
                                goto error;
                        }
                } else {
                        ret = BGAPI2_Device_GetRemoteNode(src->device, "AcquisitionFrameRateEnable", &node);
                        HANDLE_BGAPI2_ERROR ("Unable to get AcquisitionFrameRateEnable node"); /*  */
                        ret = BGAPI2_Node_SetBool(node, 0);
                        HANDLE_BGAPI2_ERROR ("Unable to set AcquisitionFrameRateEnable node");
                        GST_DEBUG_OBJECT (src, "Disabled custom framerate limiter.");
                }

        } else {
                GST_WARNING_OBJECT (src, "Setting AcquisitionFrameRateEnable is unavailable");
        }

        return TRUE;

error:
        return FALSE;
}

static gboolean
gst_baumer_src_set_exposure_time (GstBaumerSrc * src)
{
        BGAPI2_RESULT ret;
        BGAPI2_Node * node = NULL;

        if (baumer_feature_available (src, SFNC_EXPOSURETIME)) {
                if (src->exposure_time != 0.0) {
                        GST_DEBUG_OBJECT (src, "Setting exposure to %0.2lf", src->exposure_time);
                        ret = BGAPI2_Device_GetRemoteNode(src->device, SFNC_EXPOSURETIME, &node);
                        HANDLE_BGAPI2_ERROR ("Unable to get ExposureTime node");
                        ret = BGAPI2_Node_SetDouble(node, src->exposure_time);
                        HANDLE_BGAPI2_ERROR ("Unable to set ExposureTime node");
                } else {
                        GST_DEBUG_OBJECT (src, "Exposure property not set, using the saved exposure setting.");
                }
        } else {
                GST_WARNING_OBJECT (src, "This camera doesn't support setting manual exposure.");
        }

        return TRUE;

error:
        return FALSE;

}

static gboolean
gst_baumer_src_set_gain (GstBaumerSrc * src)
{
        BGAPI2_RESULT ret;
        BGAPI2_Node * node = NULL;

        if (baumer_feature_available (src, SFNC_GAIN)) {
                if (src->gain != 0.0) {
                        GST_DEBUG_OBJECT (src, "Setting gain to %0.2lf", src->gain);
                        ret = BGAPI2_Device_GetRemoteNode(src->device, SFNC_GAIN, &node);
                        HANDLE_BGAPI2_ERROR ("Unable to get Gain node");
                        ret = BGAPI2_Node_SetDouble(node, src->gain);
                        HANDLE_BGAPI2_ERROR ("Unable to set Gain node");
                } else {
                        GST_DEBUG_OBJECT (src, "Exposure property not set, using the saved exposure setting.");
                }
        } else {
                GST_WARNING_OBJECT (src, "This camera doesn't support setting gain.");
        }

        return TRUE;

error:
        return FALSE;

}

static gboolean
gst_baumer_src_set_trigger_mode (GstBaumerSrc * src)
{
        BGAPI2_RESULT ret;
        BGAPI2_Node * node = NULL;

        if (baumer_feature_implemented (src, SFNC_TRIGGERMODE)) {
                // Set the TriggerMode feature
                ret = BGAPI2_Device_GetRemoteNode(src->device, SFNC_TRIGGERMODE, &node);
                HANDLE_BGAPI2_ERROR ("Unable to get TriggerMode node");
                ret = BGAPI2_Node_SetString(node, "Off");
                HANDLE_BGAPI2_ERROR ("Unable to set TriggerMode node");
        } else {
                GST_WARNING_OBJECT (src, "This camera doesn't support setting TriggerMode");
        }
        return TRUE;

error:
        return FALSE;
}

static void
gst_baumer_src_print_system_info (GstBaumerSrc * src)
{
        guint64 str_size = 0;
        gchar * id;
        gchar * vendor;
        gchar * model;
        gchar * version;
        gchar * tl_type;
        gchar * name;
        gchar * path_name;
        gchar * display_name;

        BGAPI2_System_GetID (src->system, NULL, &str_size);
        id = g_malloc (str_size);
        BGAPI2_System_GetID (src->system, id, &str_size);
        BGAPI2_System_GetVendor (src->system, NULL, &str_size);
        vendor = g_malloc (str_size);
        BGAPI2_System_GetVendor (src->system, vendor, &str_size);
        BGAPI2_System_GetModel (src->system, NULL, &str_size);
        model = g_malloc (str_size);
        BGAPI2_System_GetModel (src->system, model, &str_size);
        BGAPI2_System_GetVersion (src->system, NULL, &str_size);
        version = g_malloc (str_size);
        BGAPI2_System_GetVersion (src->system, version, &str_size);
        BGAPI2_System_GetTLType (src->system, NULL, &str_size);
        tl_type = g_malloc (str_size);
        BGAPI2_System_GetTLType (src->system, tl_type, &str_size);
        BGAPI2_System_GetFileName (src->system, NULL, &str_size);
        name = g_malloc (str_size);
        BGAPI2_System_GetFileName (src->system, name, &str_size);
        BGAPI2_System_GetPathName (src->system, NULL, &str_size);
        path_name = g_malloc (str_size);
        BGAPI2_System_GetPathName (src->system, path_name, &str_size);
        BGAPI2_System_GetDisplayName (src->system, NULL, &str_size);
        display_name = g_malloc (str_size);
        BGAPI2_System_GetDisplayName (src->system, display_name, &str_size);

        GST_DEBUG_OBJECT (src, "System: ID=%s, Vendor=%s, Model=%s, Version=%s,"
                          " TL_Type=%s, Name=%s, Path_Name=%s, Display_Name=%s",
                          id, vendor, model, version, tl_type, name, path_name,
                          display_name);

        g_free(id);
        g_free(vendor);
        g_free(model);
        g_free(version);
        g_free(tl_type);
        g_free(name);
        g_free(path_name);
        g_free(display_name);

}

static void
gst_baumer_src_print_interface_info (GstBaumerSrc * src)
{
        guint64 str_size = 0;
        char * id;
        char * tl_type;
        char * display_name;

        BGAPI2_Interface_GetID (src->interface, NULL, &str_size);
        id = g_malloc (str_size);
        BGAPI2_Interface_GetID (src->interface, id, &str_size);
        BGAPI2_Interface_GetDisplayName (src->interface, NULL, &str_size);
        display_name = g_malloc (str_size);
        BGAPI2_Interface_GetDisplayName (src->interface, display_name, &str_size);
        BGAPI2_Interface_GetTLType (src->interface, NULL, &str_size);
        tl_type = g_malloc (str_size);
        BGAPI2_Interface_GetTLType (src->interface, tl_type, &str_size);

        GST_DEBUG_OBJECT (src, "Interface: ID=%s, TL_Type=%s, Display_Name=%s",
                          id, tl_type, display_name);
        g_free(id);
        g_free(tl_type);
        g_free(display_name);
}

static void
gst_baumer_src_print_device_info (GstBaumerSrc * src)
{
        guint64 str_size = 0;
        gchar * id;
        gchar * vendor;
        gchar * model;
        gchar * serial_num;
        gchar * tl_type;
        gchar * display_name;

        GST_DEBUG_OBJECT (src, "gst_baumer_src_print_device_info");

        BGAPI2_Device_GetID (src->device, NULL, &str_size);
        id = g_malloc (str_size);
        BGAPI2_Device_GetID (src->device, id, &str_size);
        BGAPI2_Device_GetVendor (src->device, NULL, &str_size);
        vendor = g_malloc (str_size);
        BGAPI2_Device_GetVendor (src->device, vendor, &str_size);
        BGAPI2_Device_GetModel (src->device, NULL, &str_size);
        model = g_malloc (str_size);
        BGAPI2_Device_GetModel (src->device, model, &str_size);
        BGAPI2_Device_GetSerialNumber (src->device, NULL, &str_size);
        serial_num = g_malloc (str_size);
        BGAPI2_Device_GetSerialNumber (src->device, serial_num, &str_size);
        BGAPI2_Device_GetTLType (src->device, NULL, &str_size);
        tl_type = g_malloc (str_size);
        BGAPI2_Device_GetTLType (src->device, tl_type, &str_size);
        BGAPI2_Device_GetDisplayName (src->device, NULL, &str_size);
        display_name = g_malloc (str_size);
        BGAPI2_Device_GetDisplayName (src->device, display_name, &str_size);

        GST_DEBUG_OBJECT (src, "Device: ID=%s, Vendor=%s, Model=%s, "
                          "Serial_Number=%s, TL_Type=%s, Display_Name=%s",
                          id, vendor, model, serial_num,
                          tl_type, display_name);

        g_free(id);
        g_free(vendor);
        g_free(model);
        g_free(serial_num);
        g_free(tl_type);
        g_free(display_name);
}

static gboolean
gst_baumer_src_prepare_buffers (GstBaumerSrc * src)
{
        guint i;
        BGAPI2_Buffer * buffer;
        BGAPI2_RESULT ret;

        for (i = 0; i < src->num_capture_buffers; ++i) {
                ret = BGAPI2_CreateBuffer(&buffer);
                HANDLE_BGAPI2_ERROR ("Unable to create buffer");
                ret = BGAPI2_DataStream_AnnounceBuffer(src->datastream, buffer);
                HANDLE_BGAPI2_ERROR ("Unable to announce buffer");
                ret = BGAPI2_DataStream_QueueBuffer(src->datastream, buffer);
                HANDLE_BGAPI2_ERROR ("Unable to queue buffer");
        }

        return TRUE;

error:
        return FALSE;
}

/* TODO: use src->num_capture_buffers if BGAPI2_DataStream_GetNumAnnounced()
 * fails */
static gboolean
gst_baumer_src_discard_buffers (GstBaumerSrc * src)
{
        guint i;
        guint64 num_buffers = 0;
        BGAPI2_RESULT ret;
        ret = BGAPI2_DataStream_DiscardAllBuffers(src->datastream);
        HANDLE_BGAPI2_ERROR ("Unable to discard buffers");

        /* It should be equal to src->num_capture_buffers but who knows... */
        ret = BGAPI2_DataStream_GetNumAnnounced(src->datastream, &num_buffers);
        HANDLE_BGAPI2_ERROR ("Unable to get announced buffer number");

        for (i = 0; i < num_buffers; i++) {
                BGAPI2_Buffer * buffer;
                ret = BGAPI2_DataStream_GetBufferID(src->datastream, 0, &buffer);
                HANDLE_BGAPI2_ERROR ("Unable to get buffer ID");
                ret = BGAPI2_DataStream_RevokeBuffer(src->datastream, buffer, NULL);
                HANDLE_BGAPI2_ERROR ("Unable to revoke buffer");
                ret = BGAPI2_DeleteBuffer(buffer, NULL);
                HANDLE_BGAPI2_ERROR ("Unable to delete buffer");
        }

        return TRUE;

error:
        return FALSE;
}

static guint64
gst_baumer_src_get_payload_size (GstBaumerSrc * src)
{
        BGAPI2_RESULT ret;
        bo_bool size_defined;
        guint64 payload_size = 0;

        if (!src->datastream) {
                GST_ERROR_OBJECT (src, "Failed to get payload size");
                GST_ELEMENT_ERROR (src, RESOURCE, FAILED,
                                   ("Failed to get payload size"),
                                   ("Request for PayloadSize before datastream acquisition"));
        }

        ret = BGAPI2_DataStream_GetDefinesPayloadSize (src->datastream, &size_defined);

        if (size_defined) {
                ret = BGAPI2_DataStream_GetPayloadSize (src->datastream,
                                                                &payload_size);
                HANDLE_BGAPI2_ERROR ("Unable to get payload size");
        } else {
                gint64 node_value = 0;
                BGAPI2_Node* node = NULL;

                ret = BGAPI2_Device_GetRemoteNode (src->device, SFNC_PAYLOADSIZE, &node);
                HANDLE_BGAPI2_ERROR ("Unable to get PayloadSize node");

                ret = BGAPI2_Node_GetInt (node, &node_value);
                HANDLE_BGAPI2_ERROR ("Unable to get PayloadSize node value");
                payload_size = (guint64) node_value;
        }

        return payload_size;

error:
        return 0;
}


static gboolean
gst_baumer_src_set_resolution (GstBaumerSrc * src)
{
        BGAPI2_RESULT ret;
        BGAPI2_Node * node;
        gint64 width = 0, height = 0;

        /* Set binning of camera */
        if (baumer_feature_implemented (src, SFNC_BINNINGHORIZONTAL) &&
            baumer_feature_implemented (src, SFNC_BINNINGVERTICAL)) {
                GST_DEBUG_OBJECT (src, "Setting horizontal binning to %d", src->binningh);
                ret = BGAPI2_Device_GetRemoteNode (src->device, SFNC_BINNINGHORIZONTAL, &node);
                HANDLE_BGAPI2_ERROR ("Unable to get BinningHorizontal node");
                ret = BGAPI2_Node_SetInt (node, src->binningh);
                HANDLE_BGAPI2_ERROR ("Unable to set BinningHorizontal node");

                GST_DEBUG_OBJECT (src, "Setting vertical binning to %d", src->binningv);
                ret = BGAPI2_Device_GetRemoteNode (src->device, SFNC_BINNINGVERTICAL, &node);
                HANDLE_BGAPI2_ERROR ("Unable to get BinningVertical node");
                ret = BGAPI2_Node_SetInt (node, src->binningv);
                HANDLE_BGAPI2_ERROR ("Unable to set BinningVertical node");
        }

        /* Get the camera's resolution */
        if (!baumer_feature_implemented (src, SFNC_WIDTH) || !baumer_feature_implemented (src, SFNC_HEIGHT)) {
                GST_ERROR_OBJECT (src,
                                  "The camera doesn't seem to be reporting it's resolution.");
                GST_ELEMENT_ERROR (src, RESOURCE, FAILED,
                                   ("Failed to initialise the camera"),
                                   ("Camera isn't reporting it's resolution. (Unsupported device?)"));
                goto error;
        }

        /* Default height/width */
        ret = BGAPI2_Device_GetRemoteNode(src->device, SFNC_WIDTH, &node);
        HANDLE_BGAPI2_ERROR ("Unable to get Width node");
        ret = BGAPI2_Node_GetInt(node, &width);
        HANDLE_BGAPI2_ERROR ("Unable to get Width node value");
        ret = BGAPI2_Device_GetRemoteNode(src->device, SFNC_HEIGHT, &node);
        HANDLE_BGAPI2_ERROR ("Unable to get Height node");
        ret = BGAPI2_Node_GetInt(node, &height);
        HANDLE_BGAPI2_ERROR ("Unable to get Height node value");

        // Max Width and Height.
        if (baumer_feature_implemented (src, SFNC_WIDTHMAX) && baumer_feature_implemented (src, SFNC_HEIGHTMAX)) {
                gint64 max_width, max_height;
                ret = BGAPI2_Device_GetRemoteNode(src->device, SFNC_WIDTHMAX, &node);
                HANDLE_BGAPI2_ERROR ("Unable to get WidthMax node");
                ret = BGAPI2_Node_GetInt(node, &max_width);
                HANDLE_BGAPI2_ERROR ("Unable to get WidthMax node value");
                src->max_width = (gint) max_width;

                ret = BGAPI2_Device_GetRemoteNode(src->device, SFNC_HEIGHTMAX, &node);
                HANDLE_BGAPI2_ERROR ("Unable to get HeightMax node");
                ret = BGAPI2_Node_GetInt(node, &max_height);
                HANDLE_BGAPI2_ERROR ("Unable to get HeightMax node value");
                src->max_height = (gint) max_height;
        }
        GST_DEBUG_OBJECT (src, "Max resolution is %dx%d.", src->max_width,
                          src->max_height);

        /* If custom resolution is set, check if it's even possible and set it */
        if (src->height != 0 || src->width != 0) {
                if (src->width > src->max_width) {
                        GST_DEBUG_OBJECT (src, "Set width is above camera's capabilities.");
                        GST_ELEMENT_ERROR (src, RESOURCE, FAILED,
                                           ("Failed to initialise the camera"),
                                           ("Wrong width specified"));
                        goto error;
                } else if (src->width == 0) {
                        src->width = (gint) width;
                }

                if (src->height > src->max_height) {
                        GST_DEBUG_OBJECT (src, "Set height is above camera's capabilities.");
                        GST_ELEMENT_ERROR (src, RESOURCE, FAILED,
                                           ("Failed to initialise the camera"),
                                           ("Wrong height specified"));
                        goto error;
                } else if (src->height == 0) {
                        src->height = (gint) height;
                }
        } else {
                src->height = (gint) height;
                src->width = (gint) width;
        }

  // Set the final resolution
        ret = BGAPI2_Device_GetRemoteNode(src->device, SFNC_WIDTH, &node);
        HANDLE_BGAPI2_ERROR ("Unable to get Width node");
        ret = BGAPI2_Node_SetInt(node, src->width);
        HANDLE_BGAPI2_ERROR ("Unable to set Width node value");
        ret = BGAPI2_Device_GetRemoteNode(src->device, SFNC_HEIGHT, &node);
        HANDLE_BGAPI2_ERROR ("Unable to get Height node");
        ret = BGAPI2_Node_SetInt(node, src->height);
        HANDLE_BGAPI2_ERROR ("Unable to set Height node value");
        GST_DEBUG_OBJECT (src, "Setting resolution to %dx%d.", src->width,
                          src->height);

        return TRUE;

error:
        return FALSE;
}

static gboolean
gst_baumer_src_set_pixel_format (GstBaumerSrc * src)
{
        if (g_ascii_strncasecmp(src->pixel_format, "auto", -1) == 0) {
                GST_DEBUG_OBJECT (src, "Pixel format was set to auto");
                return TRUE;
        }

        BGAPI2_RESULT ret;
        BGAPI2_Node * node;
        GString * format;
        GPtrArray * pixel_formats;
        gint i;
        gboolean find_flag = FALSE;

        pixel_formats = g_ptr_array_new_full (MAX_ARRAY_SIZE, delete_format);
        gst_baumer_src_get_supported_pixel_formats (src, &pixel_formats);

        for (i = 0; i < pixel_formats->len; i++) {
                format = g_ptr_array_index (pixel_formats, i);
                if (g_ascii_strncasecmp (src->pixel_format, format->str, -1) == 0) {
                        find_flag = TRUE;
                        break;
                }
        }

        if (find_flag) {
                GST_DEBUG_OBJECT (src, "Setting PixelFormat to %s.",
                                  src->pixel_format);
                ret = BGAPI2_Device_GetRemoteNode(src->device, SFNC_PIXELFORMAT, &node);
                HANDLE_BGAPI2_ERROR ("Unable to get PixelFormat node");
                ret = BGAPI2_Node_SetString(node, format->str);
                HANDLE_BGAPI2_ERROR ("Unable to set PixelFormat node value");
        } else {
                GST_ERROR_OBJECT (src, "PixelFormat %s isn't supported", src->pixel_format);
                GST_ELEMENT_ERROR (src, RESOURCE, FAILED,
                                   ("Failed to set PixelFormat"),
                                   ("PixelFormat isn't supported"));
                goto error;
        }

        g_ptr_array_free (pixel_formats, TRUE);

        return TRUE;

error:
        if (format) g_string_free (format, TRUE);
        if (pixel_formats) g_ptr_array_free (pixel_formats, TRUE);
        return FALSE;
}

static void
delete_format (gpointer data)
{
        GString * s = (GString *) data;
        g_string_free (s, TRUE);
}

static gboolean
gst_baumer_src_get_supported_pixel_formats (GstBaumerSrc * src, GPtrArray ** format_list)
{
        BGAPI2_RESULT ret;
        guint64 node_list_count = 0;
        gint i;
        BGAPI2_NodeMap * enum_node_list = NULL;
        BGAPI2_Node * node = NULL;
        guint64 str_size = 0;
        gchar * format_string;
        GString * format = NULL;

        ret = BGAPI2_Device_GetRemoteNode(src->device, "PixelFormat", &node);
        HANDLE_BGAPI2_ERROR ("Unable to get PixelFormat node");
        BGAPI2_Node_GetEnumNodeList(node, &enum_node_list);
        HANDLE_BGAPI2_ERROR ("Unable to get PixelFormat list");
        BGAPI2_NodeMap_GetNodeCount(enum_node_list, &node_list_count);
        HANDLE_BGAPI2_ERROR ("Unable to get PixelFormat node list count");

        for (i = 0; i < node_list_count; i++) {
                BGAPI2_Node * enum_entry = NULL;
                bo_bool is_readable = 0;
                BGAPI2_NodeMap_GetNodeByIndex(enum_node_list, i, &enum_entry);
                HANDLE_BGAPI2_ERROR ("Unable to get PixelFormat enum entry");
                BGAPI2_Node_IsReadable(enum_entry, &is_readable);
                HANDLE_BGAPI2_ERROR ("Unable to determine PixelFormat enum entry status");
                if (is_readable) {
                        BGAPI2_Node_GetString(enum_entry, NULL, &str_size);
                        format_string = g_malloc (str_size);
                        BGAPI2_Node_GetString(enum_entry, format_string, &str_size);
                        HANDLE_BGAPI2_ERROR ("Unable to get PixelFormat enum entry value");
                        GST_DEBUG_OBJECT (src, "got value: %s", format_string);
                        format = g_string_new (format_string);
                        GST_DEBUG_OBJECT (src, "inserting %s", format->str);
                        g_ptr_array_add (*format_list, format);
                        g_free(format_string);
                }
        }

        return TRUE;

error:
        return FALSE;
}

static GstCaps *
gst_baumer_src_get_supported_caps (GstBaumerSrc * src)
{
        GstCaps * caps;
        BGAPI2_Node * node;
        GString * format = NULL;
        gboolean auto_format = FALSE;
        GPtrArray * pixel_formats;
        guint i, j;

        pixel_formats = g_ptr_array_new_full (MAX_ARRAY_SIZE, delete_format);
        gst_baumer_src_get_supported_pixel_formats (src, &pixel_formats);

        GST_DEBUG_OBJECT (src, "src->pixel_format is %s", src->pixel_format);
        if (g_ascii_strncasecmp (src->pixel_format, "auto", -1) == 0) {
                auto_format = TRUE;
        }

        caps = gst_caps_new_empty ();

        /* check every pixel format GStreamer supports */
        for (i = 0; i < G_N_ELEMENTS (gst_genicam_pixel_format_infos); i++) {
                const GstGenicamPixelFormatInfo *info = &gst_genicam_pixel_format_infos[i];

                if (!auto_format && g_ascii_strncasecmp (src->pixel_format,
                                                         info->pixel_format,
                                                         -1) != 0) {
                        continue;
                }

                for (j = 0; j < pixel_formats->len; j++) {
                        format = g_ptr_array_index (pixel_formats, j);
                        GST_DEBUG_OBJECT (src, "info->pixel_format is %s", info->pixel_format);
                        GST_DEBUG_OBJECT (src, "format->str is %s", format->str);
                        if (g_ascii_strncasecmp (info->pixel_format, format->str, -1) == 0) {
                                GstCaps *format_caps;
                                GST_DEBUG_OBJECT (src, "PixelFormat %s supported, adding to caps",
                                                  info->pixel_format);
                                format_caps =
                                        gst_genicam_pixel_format_caps_from_pixel_format (info->pixel_format,
                                                                                         G_BYTE_ORDER,
                                                                                         src->width,
                                                                                         src->height,
                                                                                         src->framerate, 1, 1, 1);
                                if (format_caps && !gst_caps_is_subset (format_caps, caps))
                                        gst_caps_append (caps, format_caps);
                        }
                }
        }

        GST_DEBUG_OBJECT (src, "Supported caps are %" GST_PTR_FORMAT, caps);

        g_ptr_array_free (pixel_formats, TRUE);

        return caps;
}

static gboolean
gst_baumer_src_start (GstBaseSrc * bsrc)
{
        GstBaumerSrc *src = GST_BAUMER_SRC (bsrc);

        GST_DEBUG_OBJECT (src, "start");

        if (!gst_baumer_src_connect_device (src) || !gst_baumer_src_set_resolution (src))
                goto error;

        /* create caps */
        if (src->caps) {
                gst_caps_unref (src->caps);
                src->caps = NULL;
        }

        src->caps = gst_baumer_src_get_supported_caps (src);

        GST_DEBUG_OBJECT (src, "starting acquisition");

        gst_baumer_src_acquisition_start (src);

        return TRUE;

error:
        if (src->acquisition_started)
                gst_baumer_src_acquisition_stop (src);
        if (src->device_connected)
                gst_baumer_src_release_device (src);
        return FALSE;
}

static gboolean
gst_baumer_src_acquisition_configure (GstBaumerSrc * src)
{
        if (!gst_baumer_src_set_pixel_format (src) ||
            !gst_baumer_src_set_framerate (src) ||
            !gst_baumer_src_set_exposure_time (src) ||
            !gst_baumer_src_set_gain (src) ||
            !gst_baumer_src_set_trigger_mode (src))
                goto error;
        src->acquisition_configured = TRUE;

        return TRUE;

error:
        return FALSE;
}

static gboolean
gst_baumer_src_acquisition_start (GstBaumerSrc * src)
{
        BGAPI2_Node * node = NULL;
        BGAPI2_RESULT ret;
        guint num_datastreams = 0;

        GST_DEBUG_OBJECT (src, "Configuring acquisition");

        if (!src->acquisition_configured) {
                if (!gst_baumer_src_acquisition_configure (src))
                        goto error;
        }

        ret = BGAPI2_Device_GetNumDataStreams (src->device, &num_datastreams);
        HANDLE_BGAPI2_ERROR ("Unable to evaluate datastream number");

        if (num_datastreams == 0) {
                GST_ERROR_OBJECT (src, "No datastreams found, canceling "
                                  "initialisation.");
                GST_ELEMENT_ERROR (src, RESOURCE, FAILED,
                                   ("No datastreams found"),
                                   (NULL));
        } else if (num_datastreams == 1) {
                if (src->datastream_index != 9999) {
                        GST_DEBUG_OBJECT (src, "datastream-index was set to %d, "
                                          "but being ignored as only "
                                          "single instance found.", src->datastream_index);
                }
                src->datastream_index = 0;
        } else if (num_datastreams > 1 && src->datastream_index == 9999) {
                GST_DEBUG_OBJECT (src, "Multiple datastreams found, but "
                                  "datastream-index property wasn't specified");
                goto error;
        }

        // Open the data stream
        ret = BGAPI2_Device_GetDataStream(src->device, src->datastream_index,
                                          &(src->datastream));
        HANDLE_BGAPI2_ERROR ("Unable to get datastream");

        ret = BGAPI2_DataStream_Open(src->datastream);
        HANDLE_BGAPI2_ERROR ("Unable to open datastream");

        src->payload_size = gst_baumer_src_get_payload_size (src);

        if (!gst_baumer_src_prepare_buffers (src)) {
                GST_ELEMENT_ERROR (src, RESOURCE, TOO_LAZY, ("Failed to prepare"
                                                             " buffers"),
                                   (NULL));
                goto error;
        }

        ret = BGAPI2_DataStream_StartAcquisitionContinuous(src->datastream);
        HANDLE_BGAPI2_ERROR ("Unable to start acquisition");

        node = NULL;
        ret = BGAPI2_Device_GetRemoteNode(src->device, SFNC_ACQUISITION_START, &node);
        HANDLE_BGAPI2_ERROR ("Unable to get AcquisitionStart node");
        ret = BGAPI2_Node_Execute(node);
        HANDLE_BGAPI2_ERROR ("Unable to execute AcquisitionStart node");

        src->acquisition_started = TRUE;
        src->last_frame_count = 0;

        return TRUE;

error:
        return FALSE;
}

static void
gst_baumer_src_acquisition_stop (GstBaumerSrc * src)
{
        BGAPI2_Node * node = NULL;

        GST_DEBUG_OBJECT (src, "Stopping acquisition");

        BGAPI2_Device_GetRemoteNode(src->device, "AcquisitionAbort", &node);
        BGAPI2_Node_Execute(node);
        BGAPI2_Device_GetRemoteNode(src->device, "AcquisitionStop", &node);
        BGAPI2_Node_Execute(node);
        BGAPI2_DataStream_StopAcquisition(src->datastream);

        if (!gst_baumer_src_discard_buffers (src)) {
                GST_ELEMENT_ERROR (src, RESOURCE, TOO_LAZY, ("Failed to discard"
                                                             " buffers"),
                                   (NULL));
        }

        if (src->datastream) {
                BGAPI2_DataStream_Close (src->datastream);
                BGAPI2_DataStream_DiscardAllBuffers(src->datastream);
                src->datastream = NULL;
        }

        src->acquisition_started = FALSE;
        src->last_frame_count = 0;
}

/* TODO: error handling */
static gboolean
gst_baumer_src_stop (GstBaseSrc * bsrc)
{
        GstBaumerSrc *src = GST_BAUMER_SRC (bsrc);

        GST_DEBUG_OBJECT (src, "stop");

        if (src->acquisition_started)
                gst_baumer_src_acquisition_stop (src);

        if (src->caps) {
                gst_caps_unref (src->caps);
                src->caps = NULL;
        }

        if (src->device_connected)
                gst_baumer_src_release_device (src);

        gst_baumer_src_reset (src);

        return TRUE;
}

static void
video_frame_free (void * data)
{
        VideoFrame *frame = (VideoFrame *) data;
        GstBaumerSrc *src = frame->src;
        BGAPI2_RESULT ret;

        ret = BGAPI2_DataStream_QueueBuffer(src->datastream, frame->image_buffer);
        HANDLE_BGAPI2_ERROR ("Failed to queue buffer");
        g_free (frame);

error:
        return;
}

static GstFlowReturn
gst_baumer_src_create (GstPushSrc * psrc, GstBuffer ** buf)
{
        GstBaumerSrc *src = GST_BAUMER_SRC (psrc);
        BGAPI2_RESULT ret;
        BGAPI2_Buffer * buffer_filled = NULL;
        bo_bool buffer_is_incomplete = 0;
        gpointer data_ptr = NULL;
        guint64 timestamp;
        gboolean base_src_does_timestamp;

        base_src_does_timestamp = gst_base_src_get_do_timestamp(GST_BASE_SRC(psrc));

        GST_LOG_OBJECT (src, "create");

        if (!src->acquisition_started) {
                if (!gst_baumer_src_acquisition_start (src))
                        goto error;
        }

        ret = BGAPI2_DataStream_GetFilledBuffer(src->datastream, &buffer_filled,
                                                src->timeout);
        HANDLE_BGAPI2_ERROR ("Failed to get New Buffer event within timeout period");

        ret = BGAPI2_Buffer_GetIsIncomplete(buffer_filled, &buffer_is_incomplete);
        HANDLE_BGAPI2_ERROR ("Failed to get complete find_flag");

        ret = BGAPI2_Buffer_GetMemPtr(buffer_filled, &data_ptr);
        HANDLE_BGAPI2_ERROR ("Failed to get buffer pointer");

        ret = BGAPI2_Buffer_GetTimestamp(buffer_filled, &timestamp);
        HANDLE_BGAPI2_ERROR ("Failed to get buffer timestamp");

        if (buffer_is_incomplete != 1) {
                VideoFrame *vf = (VideoFrame *) g_malloc0 (sizeof (VideoFrame));

                *buf = gst_buffer_new_wrapped_full ((GstMemoryFlags) GST_MEMORY_FLAG_READONLY,
                                                    data_ptr,
                                                    src->payload_size, 0, src->payload_size,
                                                    vf, (GDestroyNotify) video_frame_free);

                vf->image_buffer = buffer_filled;
                vf->src = src;
        } else {
                GST_ERROR_OBJECT (src, "Error in the image processing loop");
                goto error;
        }

        if (!base_src_does_timestamp) {
                if (src->timestamp_offset == 0) {
                        src->timestamp_offset = timestamp;
                        src->last_timestamp = timestamp;
                }

                GST_BUFFER_PTS (*buf) = timestamp - src->timestamp_offset;
                GST_BUFFER_DURATION (*buf) = timestamp - src->last_timestamp;
                src->last_timestamp = timestamp;
        }


          // Set frame offset
        GST_BUFFER_OFFSET (*buf) = src->last_frame_count;
        src->last_frame_count += 1;
        GST_BUFFER_OFFSET_END (*buf) = src->last_frame_count;

        return GST_FLOW_OK;
error:
        return GST_FLOW_ERROR;
}


static GstCaps *
gst_baumer_src_get_caps (GstBaseSrc * bsrc, GstCaps * filter)
{
        GstBaumerSrc *src = GST_BAUMER_SRC (bsrc);

        GST_DEBUG_OBJECT (src, "Received a request for caps.");
        if (!src->device_connected) {
                GST_DEBUG_OBJECT (src, "Could not send caps - no camera connected.");
                return gst_pad_get_pad_template_caps (GST_BASE_SRC_PAD (bsrc));
        } else {
                return src->caps;
        }
}

static gboolean
gst_baumer_src_set_caps (GstBaseSrc *bsrc, GstCaps *caps)
{
        GstBaumerSrc *src = GST_BAUMER_SRC (bsrc);
        BGAPI2_Node * node;
        gint i;
        gchar * format_string;
        guint64 str_size = 0;
        BGAPI2_RESULT ret;

        GST_DEBUG_OBJECT (src, "Setting caps to %" GST_PTR_FORMAT, caps);

        src->pixel_format = NULL;
        for (i = 0; i < G_N_ELEMENTS (gst_genicam_pixel_format_infos); i++) {
                GstCaps *super_caps;
                GstGenicamPixelFormatInfo *info = &gst_genicam_pixel_format_infos[i];
                super_caps = gst_caps_from_string (info->gst_caps_string);
                ret = BGAPI2_Device_GetRemoteNode(src->device, SFNC_PIXELFORMAT, &node);
                /* HANDLE_BGAPI2_ERROR ("Unable to get PixelFormat node"); */
                BGAPI2_Node_GetString(node, NULL, &str_size);
                format_string = g_malloc(str_size);
                BGAPI2_Node_GetString(node, format_string, &str_size);
                GST_DEBUG_OBJECT (src, "Pixel format of camera is %s", format_string);
                /* HANDLE_BGAPI2_ERROR ("Unable to get PixelFormat node value"); */
                if (gst_caps_is_subset (caps, super_caps)
                    && g_ascii_strncasecmp (format_string, info->pixel_format, -1) == 0) {
                        src->pixel_format = g_strdup (info->pixel_format);
                        GST_DEBUG_OBJECT (src, "Set caps match PixelFormat '%s'",
                                          src->pixel_format);
                        break;
                }
                g_free (format_string);
        }

        if (src->pixel_format == NULL)
                goto unsupported_caps;

        src->timestamp_offset = 0;
        src->last_timestamp = 0;

        return TRUE;

unsupported_caps:
        GST_ERROR_OBJECT (bsrc, "Unsupported caps: %" GST_PTR_FORMAT, caps);
        return FALSE;
}

static gboolean
gst_baumer_src_unlock (GstBaseSrc *bsrc)
{
        GstBaumerSrc *src = GST_BAUMER_SRC (bsrc);

        GST_LOG_OBJECT(src, "unlock");

        src->stop_requested = TRUE;

        return TRUE;
}

static gboolean
gst_baumer_src_unlock_stop (GstBaseSrc *bsrc)
{
        GstBaumerSrc *src = GST_BAUMER_SRC(bsrc);

        GST_LOG_OBJECT(src, "unlock_stop");

        src->stop_requested = FALSE;

        return TRUE;
}

static gchar *
gst_baumer_src_get_error_string (GstBaumerSrc *src)
{
        size_t error_string_size = MAX_ERROR_STRING_LEN;
        BGAPI2_RESULT error_code;

        BGAPI2_GetLastError (&error_code, src->error_string, &error_string_size);

        return src->error_string;

}

static gboolean
plugin_init (GstPlugin * plugin)
{
        GST_DEBUG_CATEGORY_INIT(gst_baumer_src_debug, "baumersrc", 0,
                                "debug category for baumersrc element");
        return gst_element_register(plugin, "baumersrc", GST_RANK_NONE,
                             GST_TYPE_BAUMER_SRC);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
                   GST_VERSION_MINOR,
                   baumer,
                   "Baumer GAPI source",
                   plugin_init,
                   GST_PACKAGE_VERSION,
                   GST_PACKAGE_LICENSE,
                   GST_PACKAGE_NAME,
                   GST_PACKAGE_ORIGIN)
