#ifndef _GST_BAUMER_SRC_H_
#define _GST_BAUMER_SRC_H_

#include <gst/base/gstpushsrc.h>
#include "bgapi2_genicam/bgapi2_genicam.h"

#define MAX_ERROR_STRING_LEN 256

G_BEGIN_DECLS

#define GST_TYPE_BAUMER_SRC (gst_baumer_src_get_type())
#define GST_BAUMER_SRC(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_BAUMER_SRC, GstBaumerSrc))
#define GST_BAUMER_SRC_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_BAUMER_SRC, GstBaumerSrcClass))
#define GST_IS_BAUMER_SRC(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_BAUMER_SRC))
#define GST_IS_BAUMER_SRC_CLASS(obj) (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_BAUMER_SRC))

typedef struct _GstBaumerSrc GstBaumerSrc;
typedef struct _GstBaumerSrcClass GstBaumerSrcClass;

struct _GstBaumerSrc {
        GstPushSrc base_baumer_src;

        BGAPI2_System *system;
        BGAPI2_Interface *interface;
        BGAPI2_Device *device;
        BGAPI2_DataStream *datastream;
        char error_string[MAX_ERROR_STRING_LEN];
        gboolean device_connected;
        gboolean acquisition_configured, acquisition_started;

        /* properties */
        guint interface_index, device_index, datastream_index;
        guint num_capture_buffers;
        gint timeout;

        GstClockTime acq_start_time;
        guint32 last_frame_count, total_dropped_frames;
        guint64 payload_size;

        GstCaps *caps;
        gint height, width, max_width, max_height, binningh, binningv;
        gboolean stop_requested;
        gdouble framerate, exposure_time, gain;
        gchar *pixel_format;
};

struct _GstBaumerSrcClass {
        GstPushSrcClass base_baumersrc_class;
};

GType gst_baumer_src_get_type (void);

G_END_DECLS

#endif // _GST_BAUMER_SRC_H_
