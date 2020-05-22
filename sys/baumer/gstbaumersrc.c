#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstbaumersrc.h"
#include <gst/gst.h>
#include <gst/video/video.h>
#include <glib.h>

#ifdef HAVE_ORC
#include <orc/orc.h>
#else
#define orc_memcpy memcpy
#endif

#include "common/genicampixelformat.h"

GST_DEBUG_CATEGORY_STATIC (gst_baumer_src_debug);
#define GST_CAT_DEFAULT gst_baumer_src_debug

#define HANDLE_BGAPI2_ERROR(arg)  \
        if (ret != BGAPI2_RESULT_SUCCESS) {   \
                GST_ELEMENT_ERROR(src, LIBRARY, FAILED, \
                                   (arg ": %s", \
                                    gst_baumer_src_get_error_string(src)), \
                                   (NULL)); \
                goto error; \
        }

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

static GstFlowReturn gst_baumer_src_create (GstPushSrc *src, GstBuffer **buf);

/* static guint64 gst_baumer_src_get_payload_size (GstBaumerSrc * src);
 * static GstBuffer * gst_baumer_src_get_buffer (GstBaumerSrc * src);
 * static GstFlowReturn gst_baumer_src_create (GstPushSrc * psrc, GstBuffer ** buf);
 * static gboolean gst_baumer_src_prepare_buffers (GstBaumerSrc * src); */
static gchar *gst_baumer_src_get_error_string (GstBaumerSrc *src);

enum {
        PROP_0,
        PROP_INTERFACE_INDEX,
        PROP_INTERFACE_ID,
        PROP_DEVICE_INDEX,
        PROP_DEVICE_ID,
        PROP_DATASTREAM_INDEX,
        PROP_DATASTREAM_ID,
        PROP_NUM_CAPTURE_BUFFERS,
        PROP_TIMEOUT
};

#define DEFAULT_PROP_SYSTEM_INDEX 0
#define DEFAULT_PROP_SYSTEM_ID ""
#define DEFAULT_PROP_INTERFACE_INDEX 0
#define DEFAULT_PROP_INTERFACE_ID ""
#define DEFAULT_PROP_DEVICE_INDEX 0
#define DEFAULT_PROP_DEVICE_ID ""
#define DEFAULT_PROP_DATASTREAM_INDEX 0
#define DEFAULT_PROP_DATASTREAM_ID ""
#define DEFAULT_PROP_NUM_CAPTURE_BUFFERS 4
#define DEFAULT_PROP_TIMEOUT 1000

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
          "Interface index number, zero-based, overridden by interface-id",
          0, G_MAXUINT, DEFAULT_PROP_INTERFACE_INDEX,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              GST_PARAM_MUTABLE_READY)));
  g_object_class_install_property (gobject_class, PROP_INTERFACE_ID,
      g_param_spec_string ("interface-id", "Interface ID",
          "Interface ID, overrides interface-index if not empty string",
          DEFAULT_PROP_INTERFACE_ID,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              GST_PARAM_MUTABLE_READY)));
  g_object_class_install_property (gobject_class, PROP_DEVICE_INDEX,
      g_param_spec_uint ("device-index", "Device index",
          "Device index number, zero-based, overridden by device-id",
          0, G_MAXUINT, DEFAULT_PROP_DEVICE_INDEX,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              GST_PARAM_MUTABLE_READY)));
  g_object_class_install_property (gobject_class, PROP_DEVICE_ID,
      g_param_spec_string ("device-id", "Device ID",
          "Device ID, overrides device-index if not empty string",
          DEFAULT_PROP_DEVICE_ID,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              GST_PARAM_MUTABLE_READY)));
  g_object_class_install_property (gobject_class, PROP_DATASTREAM_INDEX,
      g_param_spec_uint ("datastream-index", "Datastream index",
          "Datastream index number, zero-based, overridden by datastream-id",
          0, G_MAXUINT, DEFAULT_PROP_DATASTREAM_INDEX,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              GST_PARAM_MUTABLE_READY)));
  g_object_class_install_property (gobject_class, PROP_DATASTREAM_ID,
      g_param_spec_string ("datastream-id", "Datastream ID",
          "Datastream ID, overrides datastream-index if not empty string",
          DEFAULT_PROP_DATASTREAM_ID,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
               GST_PARAM_MUTABLE_READY)));
  g_object_class_install_property (gobject_class, PROP_NUM_CAPTURE_BUFFERS,
      g_param_spec_uint ("num-capture-buffers", "Number of capture buffers",
          "Number of capture buffers", 1, G_MAXUINT,
          DEFAULT_PROP_NUM_CAPTURE_BUFFERS,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (G_OBJECT_CLASS (klass),
      PROP_TIMEOUT, g_param_spec_int ("timeout",
          "Buffer receive imeout (ms)",
          "Buffer receive timeout in ms (0 to use default)", 0, G_MAXINT,
          DEFAULT_PROP_TIMEOUT, G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE));
}

static void
gst_baumer_src_reset(GstBaumerSrc *src)
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
gst_baumer_src_init(GstBaumerSrc *src)
{
        /* initialize member variables */
        src->interface_index = DEFAULT_PROP_INTERFACE_INDEX;
        src->interface_id = g_strdup (DEFAULT_PROP_INTERFACE_ID);
        src->device_index = DEFAULT_PROP_DEVICE_INDEX;
        src->device_id = g_strdup (DEFAULT_PROP_DEVICE_ID);
        src->datastream_index = DEFAULT_PROP_DATASTREAM_INDEX;
        src->datastream_id = g_strdup (DEFAULT_PROP_DATASTREAM_ID);
        src->num_capture_buffers = DEFAULT_PROP_NUM_CAPTURE_BUFFERS;
        src->timeout = DEFAULT_PROP_TIMEOUT;

        src->stop_requested = FALSE;
        src->caps = NULL;

        src->system = NULL;
        src->interface = NULL;
        src->device = NULL;
        src->datastream = NULL;

        /* set source as live (no preroll) */
        gst_base_src_set_live(GST_BASE_SRC(src), TRUE);

        /* override default of BYTES to operate in time mode */
        gst_base_src_set_format(GST_BASE_SRC(src), GST_FORMAT_TIME);

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
        case PROP_INTERFACE_ID:
                g_free (src->interface_id);
                src->interface_id = g_strdup (g_value_get_string (value));
                break;
        case PROP_DEVICE_INDEX:
                src->device_index = g_value_get_uint (value);
                break;
        case PROP_DEVICE_ID:
                g_free (src->device_id);
                src->device_id = g_strdup (g_value_get_string (value));
                break;
        case PROP_DATASTREAM_INDEX:
                src->datastream_index = g_value_get_uint (value);
                break;
        case PROP_DATASTREAM_ID:
                g_free (src->datastream_id);
                src->datastream_id = g_strdup (g_value_get_string (value));
                break;
        case PROP_NUM_CAPTURE_BUFFERS:
                src->num_capture_buffers = g_value_get_uint (value);
                break;
        case PROP_TIMEOUT:
                src->timeout = g_value_get_int (value);
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
        case PROP_INTERFACE_ID:
                g_value_set_string (value, src->interface_id);
                break;
        case PROP_DEVICE_INDEX:
                g_value_set_uint (value, src->device_index);
                break;
        case PROP_DEVICE_ID:
                g_value_set_string (value, src->device_id);
                break;
        case PROP_DATASTREAM_INDEX:
                g_value_set_uint (value, src->datastream_index);
                break;
        case PROP_DATASTREAM_ID:
                g_value_set_string (value, src->datastream_id);
                break;
        case PROP_NUM_CAPTURE_BUFFERS:
                g_value_set_uint (value, src->num_capture_buffers);
                break;
        case PROP_TIMEOUT:
                g_value_set_int (value, src->timeout);
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

#define BGAPI2_MAX_STR_SIZE 128

void
gst_baumer_print_system_info (GstBaumerSrc * src)
{
        guint64 str_size = BGAPI2_MAX_STR_SIZE;
        char id[BGAPI2_MAX_STR_SIZE];
        char vendor[BGAPI2_MAX_STR_SIZE];
        char model[BGAPI2_MAX_STR_SIZE];
        char version[BGAPI2_MAX_STR_SIZE];
        char tl_type[BGAPI2_MAX_STR_SIZE];
        char name[BGAPI2_MAX_STR_SIZE];
        char path_name[BGAPI2_MAX_STR_SIZE];
        char display_name[BGAPI2_MAX_STR_SIZE];

        BGAPI2_System_GetID (src->system, id, &str_size);
        BGAPI2_System_GetVendor (src->system, vendor, &str_size);
        BGAPI2_System_GetModel (src->system, model, &str_size);
        BGAPI2_System_GetVersion (src->system, version, &str_size);
        BGAPI2_System_GetTLType (src->system, tl_type, &str_size);
        BGAPI2_System_GetFileName (src->system, name, &str_size);
        BGAPI2_System_GetPathName (src->system, path_name, &str_size);
        BGAPI2_System_GetDisplayName (src->system, display_name, &str_size);

        GST_DEBUG_OBJECT (src, "System: ID=%s, Vendor=%s, Model=%s, Version=%s,"
                          " TL_Type=%s, Name=%s, Path_Name=%s, Display_Name=%s",
                          id, vendor, model, version, tl_type, name, path_name,
                          display_name);
}

void
gst_baumer_print_interface_info (GstBaumerSrc * src)
{
        guint64 str_size = BGAPI2_MAX_STR_SIZE;
        char iface_id[BGAPI2_MAX_STR_SIZE];
        char id[BGAPI2_MAX_STR_SIZE];
        char tl_type[BGAPI2_MAX_STR_SIZE];
        char display_name[BGAPI2_MAX_STR_SIZE];

        BGAPI2_Interface_GetID (src->interface, id, &str_size);
        BGAPI2_Interface_GetDisplayName (src->interface, display_name, &str_size);
        BGAPI2_Interface_GetTLType (src->interface, tl_type, &str_size);

        GST_DEBUG_OBJECT (src, "Interface: ID=%s, TL_Type=%s, Display_Name=%s",
                          id, tl_type, display_name);
}

void
gst_baumer_print_device_info (GstBaumerSrc * src)
{
        guint64 str_size = BGAPI2_MAX_STR_SIZE;
        gchar dev_id[BGAPI2_MAX_STR_SIZE];
        gchar id[BGAPI2_MAX_STR_SIZE];
        gchar vendor[BGAPI2_MAX_STR_SIZE];
        gchar model[BGAPI2_MAX_STR_SIZE];
        gchar serial_num[BGAPI2_MAX_STR_SIZE];
        gchar tl_type[BGAPI2_MAX_STR_SIZE];
        gchar display_name[BGAPI2_MAX_STR_SIZE];
        gchar access_status[BGAPI2_MAX_STR_SIZE];


        BGAPI2_Device_GetID (src->device, id, &str_size);
        BGAPI2_Device_GetVendor (src->device, vendor, &str_size);
        BGAPI2_Device_GetModel (src->device, model, &str_size);
        BGAPI2_Device_GetSerialNumber (src->device, serial_num, &str_size);
        BGAPI2_Device_GetTLType (src->device, tl_type, &str_size);
        BGAPI2_Device_GetDisplayName (src->device, display_name, &str_size);
        BGAPI2_Device_GetAccessStatus (src->device, access_status, &str_size);

        GST_DEBUG_OBJECT (src, "Device %d: ID=%s, Vendor=%s, Model=%s, "
                          "Serial_Number=%s, TL_Type=%s, Display_Name=%s, "
                          "Access_Status=%d", id, vendor, model, serial_num,
                          tl_type, display_name, access_status);
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

static guint64
gst_baumer_src_get_payload_size (GstBaumerSrc * src)
{
        BGAPI2_RESULT ret;
        bo_bool size_defined;
        guint64 payload_size = 0;

        ret = BGAPI2_DataStream_GetDefinesPayloadSize (src->datastream, &size_defined);

        if (size_defined) {
                ret = BGAPI2_DataStream_GetPayloadSize (src->datastream,
                                                                &payload_size);
                HANDLE_BGAPI2_ERROR ("Unable to get payload size");
        } else {
                gint64 node_value = 0;
                guint64 tmp_val = 0;
                BGAPI2_Node* node = NULL;

                ret = BGAPI2_Device_GetRemoteNode (src->device, "PayloadSize", &node);
                HANDLE_BGAPI2_ERROR ("Unable to get PayloadSize node");

                ret = BGAPI2_Node_GetInt (node, &node_value);
                HANDLE_BGAPI2_ERROR ("Unable to get PayloadSize node value");
                tmp_val = (guint64)node_value;
                payload_size = GUINT32_FROM_BE (tmp_val);
        }

        return payload_size;

error:
        return 0;
}

static gboolean
gst_baumer_src_start (GstBaseSrc * bsrc)
{
        GstBaumerSrc *src = GST_BAUMER_SRC (bsrc);
        BGAPI2_RESULT ret;
        guint32 i;
        gint64 width = 0;
        gint64 height = 0;
        guint32 bpp, stride;
        GstVideoInfo vinfo;

        GST_DEBUG_OBJECT (src, "start");

        // Create the system
        ret = BGAPI2_UpdateSystemList ();
        HANDLE_BGAPI2_ERROR ("Unable to update system list");

        ret = BGAPI2_GetSystem (1, &(src->system));
        HANDLE_BGAPI2_ERROR ("Unable to find USB system");

        /* gst_baumer_print_system_info (src); */

        ret = BGAPI2_System_Open (src->system);
        HANDLE_BGAPI2_ERROR ("Unable to open USB system");

        // Create the interface
        bo_bool changed = 0;
        ret = BGAPI2_System_UpdateInterfaceList (src->system, &changed, 100);
        HANDLE_BGAPI2_ERROR ("Unable to update interface list");

        ret = BGAPI2_System_GetInterface (src->system, 0, &(src->interface));
        HANDLE_BGAPI2_ERROR ("Unable to get interface");

        /* gst_baumer_print_interface_info (src); */

        ret = BGAPI2_Interface_Open (src->interface);
        HANDLE_BGAPI2_ERROR ("Unable to open interface");


        // Create the device
        ret = BGAPI2_Interface_UpdateDeviceList(src->interface, &changed, 200);
        HANDLE_BGAPI2_ERROR ("Unable to update device list");

        ret = BGAPI2_Interface_GetDevice(src->interface, 0, &(src->device));
        HANDLE_BGAPI2_ERROR ("Unable to get device");

        /* gst_baumer_print_device_info (src); */

        ret = BGAPI2_Device_Open(src->device);
        HANDLE_BGAPI2_ERROR ("Unable to open interface");

        // Set the ExposureTime feature
        BGAPI2_Node * node;
        ret = BGAPI2_Device_GetRemoteNode(src->device, SFNC_TRIGGERMODE, &node);
        HANDLE_BGAPI2_ERROR ("Unable to get TriggerMode node");
        ret = BGAPI2_Node_SetString(node, "Off");
        HANDLE_BGAPI2_ERROR ("Unable to set TriggerMode node");

        // Set the ExposureTime feature
        node = NULL;
        ret = BGAPI2_Device_GetRemoteNode(src->device, SFNC_EXPOSURETIME, &node);
        HANDLE_BGAPI2_ERROR ("Unable to get ExposureTime node");
        ret = BGAPI2_Node_SetDouble(node, 10000.0);
        HANDLE_BGAPI2_ERROR ("Unable to set ExposureTime node");

        // Set the Gain
        node = NULL;
        ret = BGAPI2_Device_GetRemoteNode(src->device, SFNC_GAIN, &node);
        HANDLE_BGAPI2_ERROR ("Unable to get Gain node");
        ret = BGAPI2_Node_SetDouble(node, 18.0);
        HANDLE_BGAPI2_ERROR ("Unable to set Gain node");

        // Open the data stream
        ret = BGAPI2_Device_GetDataStream(src->device, 0, &(src->datastream));
        HANDLE_BGAPI2_ERROR ("Unable to get datastream");

        ret = BGAPI2_DataStream_Open(src->datastream);
        HANDLE_BGAPI2_ERROR ("Unable to open datastream");

        {
                ret = BGAPI2_Device_GetRemoteNode(src->device, SFNC_WIDTH, &node);
                HANDLE_BGAPI2_ERROR ("Unable to get Width node");
                ret = BGAPI2_Node_GetInt(node, &width);
                HANDLE_BGAPI2_ERROR ("Unable to get Width node value");
                ret = BGAPI2_Device_GetRemoteNode(src->device, SFNC_HEIGHT, &node);
                HANDLE_BGAPI2_ERROR ("Unable to get Height node");
                ret = BGAPI2_Node_GetInt(node, &height);
                HANDLE_BGAPI2_ERROR ("Unable to get Height node value");

                bpp = 8;
        }

        if (!gst_baumer_src_prepare_buffers (src)) {
                GST_ELEMENT_ERROR (src, RESOURCE, TOO_LAZY, ("Failed to prepare buffers"),
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

        /* create caps */
        if (src->caps) {
                gst_caps_unref (src->caps);
                src->caps = NULL;
        }

        gst_video_info_init (&vinfo);

        if (bpp <= 8) {
                gst_video_info_set_format (&vinfo, GST_VIDEO_FORMAT_GRAY8, width, height);
                src->caps = gst_video_info_to_caps (&vinfo);
        } else if (bpp > 8 && bpp <= 16) {
                GValue val = G_VALUE_INIT;
                GstStructure *s;

                if (G_BYTE_ORDER == G_LITTLE_ENDIAN) {
                        gst_video_info_set_format (&vinfo, GST_VIDEO_FORMAT_GRAY16_LE, width,
                                                   height);
                } else if (G_BYTE_ORDER == G_BIG_ENDIAN) {
                        gst_video_info_set_format (&vinfo, GST_VIDEO_FORMAT_GRAY16_BE, width,
                                                   height);
                }
                src->caps = gst_video_info_to_caps (&vinfo);

                /* set bpp, extra info for GRAY16 so elements can scale properly */
                s = gst_caps_get_structure (src->caps, 0);
                g_value_init (&val, G_TYPE_INT);
                g_value_set_int (&val, bpp);
                gst_structure_set_value (s, "bpp", &val);
                g_value_unset (&val);
        } else {
                GST_ELEMENT_ERROR (src, STREAM, WRONG_TYPE,
                                   ("Unknown or unsupported bit depth (%d).", bpp), (NULL));
                return FALSE;
        }

        src->height = vinfo.height;
        src->gst_stride = GST_VIDEO_INFO_COMP_STRIDE (&vinfo, 0);

        GST_DEBUG_OBJECT (src, "starting acquisition");
//TODO: start acquisition engine

        /* TODO: check timestamps on buffers vs start time */
        src->acq_start_time =
                gst_clock_get_time (gst_element_get_clock (GST_ELEMENT (src)));

        return TRUE;

error:
        if (src->datastream) {
                BGAPI2_DataStream_Close (src->datastream);
                BGAPI2_DataStream_DiscardAllBuffers(src->datastream);
                src->datastream = NULL;
        }

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

        return FALSE;
}

static gboolean
gst_baumer_src_stop (GstBaseSrc * bsrc)
{
        GstBaumerSrc *src = GST_BAUMER_SRC (bsrc);

        GST_DEBUG_OBJECT (src, "stop");

        if (src->datastream) {
                BGAPI2_DataStream_Close (src->datastream);
                BGAPI2_DataStream_DiscardAllBuffers(src->datastream);
                src->datastream = NULL;
        }

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

        gst_baumer_src_reset (src);

        return TRUE;
}

static GstBuffer *
gst_baumer_src_get_buffer (GstBaumerSrc * src)
{
        BGAPI2_RESULT ret;
        BGAPI2_Buffer * buffer_filled;
        GstBuffer *buf = NULL;
        guint64 str_size = BGAPI2_MAX_STR_SIZE;
        gchar payload_type[BGAPI2_MAX_STR_SIZE] = "";
        guint64 frame_id, buffer_size;
        bo_bool buffer_is_incomplete;
        gpointer data_ptr;
        GstMapInfo minfo;

        ret = BGAPI2_DataStream_GetFilledBuffer(src->datastream, &buffer_filled,
                                                src->timeout);
        HANDLE_BGAPI2_ERROR ("Failed to get New Buffer event within timeout period");

        ret = BGAPI2_Buffer_GetPayloadType (buffer_filled, payload_type, &str_size);
        HANDLE_BGAPI2_ERROR ("Failed to get payload type");

        ret = BGAPI2_Buffer_GetFrameID(buffer_filled, &frame_id);
        HANDLE_BGAPI2_ERROR ("Failed to get frame id");

        ret = BGAPI2_Buffer_GetIsIncomplete(buffer_filled, &buffer_is_incomplete);
        HANDLE_BGAPI2_ERROR ("Failed to get complete flag");

        ret = BGAPI2_Buffer_GetMemSize(buffer_filled, &buffer_size);
        HANDLE_BGAPI2_ERROR ("Failed to get buffer size");

        ret = BGAPI2_Buffer_GetMemPtr(buffer_filled, &data_ptr);
        HANDLE_BGAPI2_ERROR ("Failed to get buffer pointer");

        if (g_strcmp0 (payload_type, BGAPI2_PAYLOADTYPE_IMAGE) != 0) {
                GST_ELEMENT_ERROR (src, STREAM, TOO_LAZY,
                                   ("Unsupported payload type: %s", payload_type), (NULL));
                goto error;
        }

        buf = gst_buffer_new_allocate (NULL, buffer_size, NULL);
        if (!buf) {
                GST_ELEMENT_ERROR (src, STREAM, TOO_LAZY,
                                   ("Failed to allocate buffer"), (NULL));
                goto error;
        }

        gst_buffer_map (buf, &minfo, GST_MAP_WRITE);
        orc_memcpy (minfo.data, (void *) data_ptr, minfo.size);
        gst_buffer_unmap (buf, &minfo);

        ret = BGAPI2_DataStream_QueueBuffer(src->datastream, buffer_filled);
        HANDLE_BGAPI2_ERROR ("Failed to queue buffer");

        return buf;

error:
        if (buf) {
                gst_buffer_unref (buf);
        }
        return NULL;
}

static GstFlowReturn
gst_baumer_src_create (GstPushSrc * psrc, GstBuffer ** buf)
{
        GstBaumerSrc *src = GST_BAUMER_SRC (psrc);
        guint32 dropped_frames = 0;
        GstClock *clock;
        GstClockTime clock_time;

        GST_LOG_OBJECT (src, "create");

        *buf = gst_baumer_src_get_buffer (src);
        if (!*buf) {
                return GST_FLOW_ERROR;
        }

        clock = gst_element_get_clock (GST_ELEMENT (src));
        clock_time = gst_clock_get_time (clock);
        gst_object_unref (clock);

        if (dropped_frames > 0) {
                src->total_dropped_frames += dropped_frames;
                GST_WARNING_OBJECT (src, "Dropped %d frames (%d total)", dropped_frames,
                                    src->total_dropped_frames);
        } else if (dropped_frames < 0) {
                GST_WARNING_OBJECT (src, "Frame count non-monotonic, signal disrupted?");
        }

        GST_BUFFER_TIMESTAMP (*buf) =
                GST_CLOCK_DIFF (gst_element_get_base_time (GST_ELEMENT (src)),
                                clock_time);

        if (src->stop_requested) {
                if (*buf != NULL) {
                        gst_buffer_unref (*buf);
                        *buf = NULL;
                }
                return GST_FLOW_FLUSHING;
        }

        return GST_FLOW_OK;

error:
        return GST_FLOW_ERROR;
}


static GstCaps *
gst_baumer_src_get_caps (GstBaseSrc * bsrc, GstCaps * filter)
{
        GstBaumerSrc *src = GST_BAUMER_SRC(bsrc);
        GstCaps *caps;

        if (src->datastream == NULL) {
                caps = gst_pad_get_pad_template_caps (GST_BASE_SRC_PAD (src));
        } else {
                caps = gst_caps_copy (src->caps);
        }

        GST_DEBUG_OBJECT (src, "The caps before filtering are %" GST_PTR_FORMAT,
                          caps);

        if (filter && caps) {
                GstCaps *tmp = gst_caps_intersect (caps, filter);
                gst_caps_unref (caps);
                caps = tmp;
        }

        GST_DEBUG_OBJECT (src, "The caps after filtering are %" GST_PTR_FORMAT, caps);

        return caps;
}

static gboolean
gst_baumer_src_set_caps (GstBaseSrc *bsrc, GstCaps *caps)
{
        GstBaumerSrc *src = GST_BAUMER_SRC (bsrc);
        GstVideoInfo vinfo;
        GstStructure *s = gst_caps_get_structure (caps, 0);

        GST_DEBUG_OBJECT (src, "The caps being set are %" GST_PTR_FORMAT, caps);

        gst_video_info_from_caps(&vinfo, caps);

        if (GST_VIDEO_INFO_FORMAT(&vinfo) != GST_VIDEO_FORMAT_UNKNOWN) {
                src->gst_stride = GST_VIDEO_INFO_COMP_STRIDE (&vinfo, 0);
        } else {
                goto unsupported_caps;
        }

        return TRUE;

unsupported_caps:
        GST_ERROR_OBJECT (src, "Unsupported caps: %" GST_PTR_FORMAT, caps);
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

gchar *
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
