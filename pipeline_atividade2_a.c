#include <gst/gst.h>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

typedef struct _CustomData {
  GstElement *pipeline;
  GstElement *video_convert;
  GstElement *audio_convert;
} CustomData;

static void pad_added_handler (GstElement *src, GstPad *new_pad, CustomData *data) {
  GstPad *sink_pad = NULL;
  GstCaps *new_pad_caps = NULL;
  GstStructure *new_pad_struct = NULL;
  const gchar *new_pad_type = NULL;

  new_pad_caps = gst_pad_get_current_caps (new_pad);
  if (new_pad_caps == NULL) return;

  new_pad_struct = gst_caps_get_structure (new_pad_caps, 0);
  new_pad_type = gst_structure_get_name (new_pad_struct);

  if (g_str_has_prefix (new_pad_type, "video/x-raw")) {
    sink_pad = gst_element_get_static_pad (data->video_convert, "sink");
  } else if (g_str_has_prefix (new_pad_type, "audio/x-raw")) {
    sink_pad = gst_element_get_static_pad (data->audio_convert, "sink");
  } else {
    goto exit;
  }

  if (gst_pad_is_linked (sink_pad)) goto exit;
  gst_pad_link (new_pad, sink_pad);

exit:
  if (new_pad_caps != NULL) gst_caps_unref (new_pad_caps);
  if (sink_pad != NULL) g_object_unref (sink_pad);
}

int
tutorial_main (int argc, char *argv[])
{
  CustomData data;
  GstElement *source, *video_filter, *video_sink;
  GstElement *audio_resample1, *audio_capsfilter, *audio_sink;
  GstCaps *audio_caps;
  GstBus *bus;
  GstMessage *msg;
  GstStateChangeReturn ret;

  gst_init (&argc, &argv);

  source = gst_element_factory_make ("uridecodebin", "source");

  data.video_convert = gst_element_factory_make ("videoconvert", "video_convert");
  video_filter = gst_element_factory_make ("videobalance", "video_filter");
  video_sink = gst_element_factory_make ("autovideosink", "video_sink");

  data.audio_convert = gst_element_factory_make ("audioconvert", "audio_convert1");
  audio_resample1 = gst_element_factory_make ("audioresample", "audio_resample1");
  audio_capsfilter = gst_element_factory_make ("capsfilter", "audio_capsfilter");
  audio_sink = gst_element_factory_make ("autoaudiosink", "audio_sink");

  data.pipeline = gst_pipeline_new ("test-pipeline");

  if (!data.pipeline || !source || !data.video_convert || !video_filter || !video_sink ||
      !data.audio_convert || !audio_resample1 || !audio_capsfilter || !audio_sink) {
    g_printerr ("Nem todos os elementos puderam ser criados.\n");
    return -1;
  }

  g_object_set (source, "uri", "file:///home/conan/Videos/sintel_trailer-480p.webm", NULL);
  g_object_set (video_filter, "saturation", 0.0, NULL);

  audio_caps = gst_caps_new_simple ("audio/x-raw",
      "format", G_TYPE_STRING, "S16LE",
      "rate", G_TYPE_INT, 44100,
      "channels", G_TYPE_INT, 2,
      NULL);

  g_object_set (audio_capsfilter, "caps", audio_caps, NULL);
  gst_caps_unref (audio_caps);

  gst_bin_add_many (GST_BIN (data.pipeline), source, data.video_convert, video_filter, video_sink,
                    data.audio_convert, audio_resample1, audio_capsfilter, audio_sink, NULL);

  gst_element_link_many (data.video_convert, video_filter, video_sink, NULL);

  if (gst_element_link_many (data.audio_convert, audio_resample1, audio_capsfilter, audio_sink, NULL) != TRUE) {
    g_printerr ("Elementos de áudio não puderam ser linkados.\n");
    gst_object_unref (data.pipeline);
    return -1;
  }

  g_signal_connect (source, "pad-added", G_CALLBACK (pad_added_handler), &data);

  ret = gst_element_set_state (data.pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    g_printerr ("Não foi possível iniciar a reprodução.\n");
    gst_object_unref (data.pipeline);
    return -1;
  }

  bus = gst_element_get_bus (data.pipeline);
  msg = gst_bus_timed_pop_filtered (bus, GST_CLOCK_TIME_NONE, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

  if (msg != NULL) {
    GError *err;
    gchar *debug_info;
    switch (GST_MESSAGE_TYPE (msg)) {
      case GST_MESSAGE_ERROR:
        gst_message_parse_error (msg, &err, &debug_info);
        g_printerr ("Erro: %s\n", err->message);
        g_clear_error (&err);
        g_free (debug_info);
        break;
      case GST_MESSAGE_EOS:
        g_print ("Fim do fluxo (EOS).\n");
        break;
      default:
        break;
    }
    gst_message_unref (msg);
  }

  gst_object_unref (bus);
  gst_element_set_state (data.pipeline, GST_STATE_NULL);
  gst_object_unref (data.pipeline);
  return 0;
}

int main (int argc, char *argv[]) {
  return tutorial_main (argc, argv);
}