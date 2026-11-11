/*
 * transmissor.c — Etapa 1: Transmissão de áudio via RTP/UDP
 *
 * Fluxo de áudio (transmitido):
 *   fonte -> audioconvert -> audioresample -> capsfilter(S16BE/44100/2)
 *         -> rtpL16pay -> udpsink(host:porta)
 *
 * Fluxo de vídeo (somente pré-visualização local, quando a fonte for um
 * arquivo com vídeo; reaproveitado da atividade anterior):
 *   uridecodebin -> queue -> videoconvert -> videobalance(sat=0) -> autovideosink
 *
 * Uso:
 *   ./transmissor [host] [porta] [fonte]
 *     fonte = "test"            -> audiotestsrc (senoide 440 Hz)
 *             "mic"             -> autoaudiosrc (microfone)
 *             URI ou caminho    -> uridecodebin (arquivo local / remoto)
 */
#include <gst/gst.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdlib.h>

#include "streaming_config.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

typedef struct _CustomData {
  GstElement *pipeline;
  GstElement *audio_convert;   /* entrada do ramo de áudio */
  GstElement *payloader;
  GMainLoop  *loop;
  gboolean    video_linked;
} CustomData;

/* ------------------------------------------------------------------ */
/* Ramo de vídeo (pré-visualização local), criado sob demanda          */
/* ------------------------------------------------------------------ */
static GstPad *
create_video_preview_branch (CustomData *data)
{
  GstElement *queue   = gst_element_factory_make ("queue",         "video_queue");
  GstElement *convert = gst_element_factory_make ("videoconvert",  "video_convert");
  GstElement *filter  = gst_element_factory_make ("videobalance",  "video_filter");
  GstElement *sink    = gst_element_factory_make ("autovideosink", "video_sink");
  GstPad *sink_pad;

  if (!queue || !convert || !filter || !sink) {
    g_printerr ("[TX] Aviso: elementos de vídeo indisponíveis; vídeo ignorado.\n");
    if (queue)   gst_object_unref (queue);
    if (convert) gst_object_unref (convert);
    if (filter)  gst_object_unref (filter);
    if (sink)    gst_object_unref (sink);
    return NULL;
  }

  g_object_set (filter, "saturation", 0.0, NULL);

  gst_bin_add_many (GST_BIN (data->pipeline), queue, convert, filter, sink, NULL);
  if (!gst_element_link_many (queue, convert, filter, sink, NULL)) {
    g_printerr ("[TX] Aviso: falha ao linkar ramo de vídeo.\n");
    return NULL;
  }

  /* Elementos adicionados com o pipeline já rodando precisam alcançar o estado dele */
  gst_element_sync_state_with_parent (sink);
  gst_element_sync_state_with_parent (filter);
  gst_element_sync_state_with_parent (convert);
  gst_element_sync_state_with_parent (queue);

  sink_pad = gst_element_get_static_pad (queue, "sink");
  return sink_pad;
}

/* ------------------------------------------------------------------ */
/* Linkagem dinâmica do uridecodebin                                    */
/* ------------------------------------------------------------------ */
static void
pad_added_handler (GstElement *src, GstPad *new_pad, CustomData *data)
{
  GstPad *sink_pad = NULL;
  GstCaps *new_pad_caps = NULL;
  const gchar *new_pad_type = NULL;

  new_pad_caps = gst_pad_get_current_caps (new_pad);
  if (new_pad_caps == NULL)
    new_pad_caps = gst_pad_query_caps (new_pad, NULL);
  if (new_pad_caps == NULL)
    return;

  new_pad_type = gst_structure_get_name (gst_caps_get_structure (new_pad_caps, 0));

  if (g_str_has_prefix (new_pad_type, "audio/x-raw")) {
    sink_pad = gst_element_get_static_pad (data->audio_convert, "sink");
  } else if (g_str_has_prefix (new_pad_type, "video/x-raw") && !data->video_linked) {
    sink_pad = create_video_preview_branch (data);
  } else {
    goto exit;
  }

  if (sink_pad == NULL || gst_pad_is_linked (sink_pad))
    goto exit;

  if (GST_PAD_LINK_FAILED (gst_pad_link (new_pad, sink_pad))) {
    g_printerr ("[TX] Falha ao linkar pad do tipo '%s'.\n", new_pad_type);
  } else {
    g_print ("[TX] Pad '%s' linkado.\n", new_pad_type);
    if (g_str_has_prefix (new_pad_type, "video/x-raw"))
      data->video_linked = TRUE;
  }

exit:
  if (new_pad_caps != NULL) gst_caps_unref (new_pad_caps);
  if (sink_pad != NULL)     gst_object_unref (sink_pad);
}

/* Disparado quando o uridecodebin terminou de expor os pads */
static void
no_more_pads_handler (GstElement *src, CustomData *data)
{
  GstPad *audio_sink = gst_element_get_static_pad (data->audio_convert, "sink");

  if (!gst_pad_is_linked (audio_sink)) {
    g_printerr ("[TX] Erro: a fonte não possui trilha de áudio.\n");
    g_main_loop_quit (data->loop);
  }
  gst_object_unref (audio_sink);
}

/* ------------------------------------------------------------------ */
/* Mensagens do bus                                                     */
/* ------------------------------------------------------------------ */
static gboolean
bus_call (GstBus *bus, GstMessage *msg, gpointer user_data)
{
  CustomData *data = (CustomData *) user_data;

  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_ERROR: {
      GError *err = NULL;
      gchar *dbg = NULL;
      gst_message_parse_error (msg, &err, &dbg);
      g_printerr ("[TX] Erro em %s: %s\n", GST_OBJECT_NAME (msg->src), err->message);
      g_printerr ("[TX] Debug: %s\n", dbg ? dbg : "nenhum");
      g_clear_error (&err);
      g_free (dbg);
      g_main_loop_quit (data->loop);
      break;
    }
    case GST_MESSAGE_WARNING: {
      GError *err = NULL;
      gchar *dbg = NULL;
      gst_message_parse_warning (msg, &err, &dbg);
      g_printerr ("[TX] Aviso de %s: %s\n", GST_OBJECT_NAME (msg->src), err->message);
      g_clear_error (&err);
      g_free (dbg);
      break;
    }
    case GST_MESSAGE_EOS:
      g_print ("[TX] Fim do fluxo (EOS).\n");
      g_main_loop_quit (data->loop);
      break;
    case GST_MESSAGE_STATE_CHANGED:
      if (GST_MESSAGE_SRC (msg) == GST_OBJECT (data->pipeline)) {
        GstState old_s, new_s, pending;
        gst_message_parse_state_changed (msg, &old_s, &new_s, &pending);
        g_print ("[TX] Pipeline: %s -> %s\n",
            gst_element_state_get_name (old_s), gst_element_state_get_name (new_s));

        /* Mostra os caps RTP efetivamente negociados (úteis para o receptor) */
        if (new_s == GST_STATE_PLAYING) {
          GstPad *pad = gst_element_get_static_pad (data->payloader, "src");
          GstCaps *caps = gst_pad_get_current_caps (pad);
          if (caps) {
            gchar *s = gst_caps_to_string (caps);
            g_print ("[TX] Caps RTP: %s\n", s);
            g_free (s);
            gst_caps_unref (caps);
          }
          gst_object_unref (pad);
        }
      }
      break;
    default:
      break;
  }
  return TRUE;
}

static gboolean
on_sigint (gpointer user_data)
{
  CustomData *data = (CustomData *) user_data;
  g_print ("\n[TX] Interrompido pelo usuário (Ctrl+C).\n");
  g_main_loop_quit (data->loop);
  return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------ */
/* Utilitários                                                          */
/* ------------------------------------------------------------------ */
static gboolean
parse_port (const gchar *str, gint *out)
{
  gchar *end = NULL;
  guint64 v = g_ascii_strtoull (str, &end, 10);
  if (end == str || *end != '\0' || v == 0 || v > 65535)
    return FALSE;
  *out = (gint) v;
  return TRUE;
}

/* ------------------------------------------------------------------ */
int
tutorial_main (int argc, char *argv[])
{
  CustomData data = { 0 };
  GstElement *source, *audio_resample, *audio_capsfilter, *udp_sink;
  GstCaps *audio_caps;
  GstBus *bus;
  GstStateChangeReturn ret;
  const gchar *host = DEFAULT_HOST;
  const gchar *src_arg = DEFAULT_URI;
  gint port = DEFAULT_PORT;
  gboolean dynamic_source = FALSE;
  int exit_code = 0;

  gst_init (&argc, &argv);

  /* ---------- Argumentos ---------- */
  if (argc > 1) host = argv[1];
  if (argc > 2 && !parse_port (argv[2], &port)) {
    g_printerr ("Porta inválida: '%s' (use 1-65535).\n", argv[2]);
    return EXIT_FAILURE;
  }
  if (argc > 3) src_arg = argv[3];

  /* ---------- Fonte ---------- */
  if (g_strcmp0 (src_arg, "test") == 0) {
    source = gst_element_factory_make ("audiotestsrc", "source");
    if (source)
      g_object_set (source, "is-live", TRUE, "freq", 440.0, NULL);
  } else if (g_strcmp0 (src_arg, "mic") == 0) {
    source = gst_element_factory_make ("autoaudiosrc", "source");
  } else {
    gchar *uri = NULL;
    source = gst_element_factory_make ("uridecodebin", "source");
    dynamic_source = TRUE;

    if (gst_uri_is_valid (src_arg)) {
      uri = g_strdup (src_arg);
    } else {
      GError *err = NULL;
      uri = gst_filename_to_uri (src_arg, &err);
      if (!uri) {
        g_printerr ("Fonte inválida '%s': %s\n", src_arg, err->message);
        g_clear_error (&err);
        if (source) gst_object_unref (source);
        return EXIT_FAILURE;
      }
    }
    if (source)
      g_object_set (source, "uri", uri, NULL);
    g_free (uri);
  }

  /* ---------- Ramo de áudio ---------- */
  data.audio_convert = gst_element_factory_make ("audioconvert",  "audio_convert");
  audio_resample     = gst_element_factory_make ("audioresample", "audio_resample");
  audio_capsfilter   = gst_element_factory_make ("capsfilter",    "audio_capsfilter");
  data.payloader     = gst_element_factory_make ("rtpL16pay",     "rtp_payloader");
  udp_sink           = gst_element_factory_make ("udpsink",       "udp_sink");

  data.pipeline = gst_pipeline_new ("transmissor-pipeline");

  if (!data.pipeline || !source || !data.audio_convert || !audio_resample ||
      !audio_capsfilter || !data.payloader || !udp_sink) {
    g_printerr ("Nem todos os elementos puderam ser criados "
                "(verifique gst-plugins-base/good).\n");
    return EXIT_FAILURE;
  }

  /* PCM 16 bits big-endian (ordem de rede), exigido pelo payload L16 */
  audio_caps = gst_caps_new_simple ("audio/x-raw",
      "format",   G_TYPE_STRING, "S16BE",
      "layout",   G_TYPE_STRING, "interleaved",
      "rate",     G_TYPE_INT,    AUDIO_RATE,
      "channels", G_TYPE_INT,    AUDIO_CHANNELS,
      NULL);
  g_object_set (audio_capsfilter, "caps", audio_caps, NULL);
  gst_caps_unref (audio_caps);

  g_object_set (data.payloader, "pt", RTP_PAYLOAD_TYPE, NULL);
  g_object_set (udp_sink, "host", host, "port", port, NULL);

  gst_bin_add_many (GST_BIN (data.pipeline), source, data.audio_convert,
      audio_resample, audio_capsfilter, data.payloader, udp_sink, NULL);

  if (!gst_element_link_many (data.audio_convert, audio_resample,
          audio_capsfilter, data.payloader, udp_sink, NULL)) {
    g_printerr ("Elementos de áudio não puderam ser linkados.\n");
    gst_object_unref (data.pipeline);
    return EXIT_FAILURE;
  }

  if (dynamic_source) {
    g_signal_connect (source, "pad-added",    G_CALLBACK (pad_added_handler),    &data);
    g_signal_connect (source, "no-more-pads", G_CALLBACK (no_more_pads_handler), &data);
  } else if (!gst_element_link (source, data.audio_convert)) {
    g_printerr ("Não foi possível linkar a fonte ao ramo de áudio.\n");
    gst_object_unref (data.pipeline);
    return EXIT_FAILURE;
  }

  /* ---------- Execução ---------- */
  data.loop = g_main_loop_new (NULL, FALSE);
  bus = gst_element_get_bus (data.pipeline);
  gst_bus_add_watch (bus, bus_call, &data);
  g_unix_signal_add (SIGINT, on_sigint, &data);

  g_print ("[TX] Fonte: %s\n[TX] Destino: %s:%d (RTP L16 %d Hz, %d canais, PT %d)\n",
      src_arg, host, port, AUDIO_RATE, AUDIO_CHANNELS, RTP_PAYLOAD_TYPE);

  ret = gst_element_set_state (data.pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    g_printerr ("Não foi possível iniciar o pipeline.\n");
    GstMessage *err_msg = gst_bus_pop_filtered (bus, GST_MESSAGE_ERROR);
    if (err_msg) {
      bus_call (bus, err_msg, &data);   /* imprime a causa real */
      gst_message_unref (err_msg);
    }
    exit_code = EXIT_FAILURE;
  } else {
    g_main_loop_run (data.loop);
  }

  /* ---------- Liberação ---------- */
  gst_element_set_state (data.pipeline, GST_STATE_NULL);
  gst_bus_remove_watch (bus);
  gst_object_unref (bus);
  gst_object_unref (data.pipeline);
  g_main_loop_unref (data.loop);
  return exit_code;
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
