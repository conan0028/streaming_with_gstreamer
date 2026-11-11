#include <gst/gst.h>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

/* Função oficial do GStreamer para linkar dinamicamente arquivos que possuem contêineres (como .webm ou .mp4).
 * Ela captura a saída de vídeo assim que o arquivo é lido e a conecta ao filtro, ignorando o áudio. */
static void pad_added_handler (GstElement *src, GstPad *new_pad, GstElement *convert) {
  GstPad *sink_pad = gst_element_get_static_pad (convert, "sink");
  GstCaps *new_pad_caps = NULL;
  GstStructure *new_pad_struct = NULL;
  const gchar *new_pad_type = NULL;

  if (gst_pad_is_linked (sink_pad)) {
    g_object_unref (sink_pad);
    return;
  }

  new_pad_caps = gst_pad_get_current_caps (new_pad);
  if (new_pad_caps == NULL) {
    g_object_unref (sink_pad);
    return;
  }

  new_pad_struct = gst_caps_get_structure (new_pad_caps, 0);
  new_pad_type = gst_structure_get_name (new_pad_struct);

  /* Se for a trilha de vídeo, nós conectamos ao nosso pipeline. Áudio será ignorado. */
  if (g_str_has_prefix (new_pad_type, "video/x-raw")) {
    gst_pad_link (new_pad, sink_pad);
  }

  gst_caps_unref (new_pad_caps);
  g_object_unref (sink_pad);
}

int
tutorial_main (int argc, char *argv[])
{
  GstElement *pipeline, *source, *convert, *filter, *sink;
  GstBus *bus;
  GstMessage *msg;
  GstStateChangeReturn ret;

  /* Initialize GStreamer */
  gst_init (&argc, &argv);

  /* Criação dos elementos */
  source = gst_element_factory_make ("uridecodebin", "source");
  convert = gst_element_factory_make ("videoconvert", "convert");
  filter = gst_element_factory_make ("videobalance", "filter");
  sink = gst_element_factory_make ("autovideosink", "sink");

  pipeline = gst_pipeline_new ("test-pipeline");

  if (!pipeline || !source || !convert || !filter || !sink) {
    g_printerr ("Not all elements could be created.\n");
    return -1;
  }

  /* caminho absoluto do arquivo local usando file:// */
  g_object_set (source, "uri", "file:///home/conan/Videos/sintel_trailer-480p.webm", NULL);

  /* Configurando saturação para 0.0 (preto e branco / remove a cor) */
  g_object_set (filter, "saturation", 0.0, NULL);

  /* Adicionando os elementos ao bin */
  gst_bin_add_many (GST_BIN (pipeline), source, convert, filter, sink, NULL);

  /* Linkando o pipeline estático: convert -> filter -> sink */
  if (gst_element_link_many (convert, filter, sink, NULL) != TRUE) {
    g_printerr ("Elements could not be linked.\n");
    gst_object_unref (pipeline);
    return -1;
  }

  g_signal_connect (source, "pad-added", G_CALLBACK (pad_added_handler), convert);

  /* Start playing */
  ret = gst_element_set_state (pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    g_printerr ("Unable to set the pipeline to the playing state.\n");
    gst_object_unref (pipeline);
    return -1;
  }

  /* Wait until error or EOS */
  bus = gst_element_get_bus (pipeline);
  msg =
      gst_bus_timed_pop_filtered (bus, GST_CLOCK_TIME_NONE,
      GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

  /* Parse message */
  if (msg != NULL) {
    GError *err;
    gchar *debug_info;

    switch (GST_MESSAGE_TYPE (msg)) {
      case GST_MESSAGE_ERROR:
        gst_message_parse_error (msg, &err, &debug_info);
        g_printerr ("Error received from element %s: %s\n",
            GST_OBJECT_NAME (msg->src), err->message);
        g_printerr ("Debugging information: %s\n",
            debug_info ? debug_info : "none");
        g_clear_error (&err);
        g_free (debug_info);
        break;
      case GST_MESSAGE_EOS:
        g_print ("End-Of-Stream reached.\n");
        break;
      default:
        g_printerr ("Unexpected message received.\n");
        break;
    }
    gst_message_unref (msg);
  }

  /* Free resources */
  gst_object_unref (bus);
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);
  return 0;
}

int
main (int argc, char *argv[])
{
#if defined(__APPLE__) && TARGET_OS_MAC && !TARGET_OS_IPHONE
  return gst_macos_main ((GstMainFunc) tutorial_main, argc, argv, NULL);
#else
  return tutorial_main (argc, argv);
#endif
}