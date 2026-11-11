/*
 * receptor.c — Etapa 1: Recepção e reprodução de áudio via RTP/UDP
 *
 * Fluxo:
 *   udpsrc(porta, caps RTP) -> rtpL16depay -> queue -> audioconvert
 *                           -> audioresample -> autoaudiosink
 *
 * Uso:
 *   ./receptor [porta]
 */
#include <gst/gst.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdlib.h>

#include "streaming_config.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

/* Tempo sem pacotes até o udpsrc emitir a mensagem "GstUDPSrcTimeout" */
#define UDP_TIMEOUT_NS  (3 * GST_SECOND)

typedef struct _CustomData {
  GstElement *pipeline;
  GMainLoop  *loop;
  gboolean    receiving;   /* FALSE enquanto não chegam pacotes / após timeout */
} CustomData;

/* Probe no src do udpsrc: detecta a chegada (ou retomada) do fluxo */
static GstPadProbeReturn
first_packet_probe (GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
  CustomData *data = (CustomData *) user_data;
  if (!data->receiving) {
    data->receiving = TRUE;
    g_print ("[RX] Recebendo pacotes RTP.\n");
  }
  return GST_PAD_PROBE_OK;
}

static gboolean
bus_call (GstBus *bus, GstMessage *msg, gpointer user_data)
{
  CustomData *data = (CustomData *) user_data;

  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_ERROR: {
      GError *err = NULL;
      gchar *dbg = NULL;
      gst_message_parse_error (msg, &err, &dbg);
      g_printerr ("[RX] Erro em %s: %s\n", GST_OBJECT_NAME (msg->src), err->message);
      g_printerr ("[RX] Debug: %s\n", dbg ? dbg : "nenhum");
      g_clear_error (&err);
      g_free (dbg);
      g_main_loop_quit (data->loop);
      break;
    }
    case GST_MESSAGE_WARNING: {
      GError *err = NULL;
      gchar *dbg = NULL;
      gst_message_parse_warning (msg, &err, &dbg);
      g_printerr ("[RX] Aviso de %s: %s\n", GST_OBJECT_NAME (msg->src), err->message);
      g_clear_error (&err);
      g_free (dbg);
      break;
    }
    case GST_MESSAGE_EOS:
      g_print ("[RX] Fim do fluxo (EOS).\n");
      g_main_loop_quit (data->loop);
      break;
    case GST_MESSAGE_ELEMENT: {
      const GstStructure *s = gst_message_get_structure (msg);
      if (s && gst_structure_has_name (s, "GstUDPSrcTimeout")) {
        if (data->receiving)
          g_printerr ("[RX] Fluxo interrompido: nenhum pacote há %" G_GUINT64_FORMAT " s.\n",
              (guint64) (UDP_TIMEOUT_NS / GST_SECOND));
        else
          g_printerr ("[RX] Aguardando pacotes... (o transmissor está ativo?)\n");
        data->receiving = FALSE;
      }
      break;
    }
    case GST_MESSAGE_STATE_CHANGED:
      if (GST_MESSAGE_SRC (msg) == GST_OBJECT (data->pipeline)) {
        GstState old_s, new_s, pending;
        gst_message_parse_state_changed (msg, &old_s, &new_s, &pending);
        g_print ("[RX] Pipeline: %s -> %s\n",
            gst_element_state_get_name (old_s), gst_element_state_get_name (new_s));
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
  g_print ("\n[RX] Interrompido pelo usuário (Ctrl+C).\n");
  g_main_loop_quit (data->loop);
  return G_SOURCE_REMOVE;
}

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

int
tutorial_main (int argc, char *argv[])
{
  CustomData data = { 0 };
  GstElement *udp_src, *depayloader, *queue, *audio_convert, *audio_resample, *audio_sink;
  GstCaps *rtp_caps;
  GstPad *src_pad;
  GstBus *bus;
  GstStateChangeReturn ret;
  gint port = DEFAULT_PORT;
  int exit_code = 0;

  gst_init (&argc, &argv);

  if (argc > 1 && !parse_port (argv[1], &port)) {
    g_printerr ("Porta inválida: '%s' (use 1-65535).\n", argv[1]);
    return EXIT_FAILURE;
  }

  /* ---------- Elementos ---------- */
  udp_src        = gst_element_factory_make ("udpsrc",        "udp_src");
  depayloader    = gst_element_factory_make ("rtpL16depay",   "rtp_depayloader");
  /* Desacopla a thread de rede da thread de reprodução e absorve a latência
   * do buffer da placa de som (evita aviso "please add queues") */
  queue          = gst_element_factory_make ("queue",         "rx_queue");
  audio_convert  = gst_element_factory_make ("audioconvert",  "audio_convert");
  audio_resample = gst_element_factory_make ("audioresample", "audio_resample");
  audio_sink     = gst_element_factory_make ("autoaudiosink", "audio_sink");

  data.pipeline = gst_pipeline_new ("receptor-pipeline");

  if (!data.pipeline || !udp_src || !depayloader || !queue || !audio_convert ||
      !audio_resample || !audio_sink) {
    g_printerr ("Nem todos os elementos puderam ser criados "
                "(verifique gst-plugins-base/good).\n");
    return EXIT_FAILURE;
  }

  /* UDP não transporta descrição de formato: os caps RTP devem ser
   * informados manualmente e precisam casar com o transmissor. */
  rtp_caps = gst_caps_new_simple ("application/x-rtp",
      "media",           G_TYPE_STRING, "audio",
      "clock-rate",      G_TYPE_INT,    AUDIO_RATE,
      "encoding-name",   G_TYPE_STRING, "L16",
      "encoding-params", G_TYPE_STRING, G_STRINGIFY (AUDIO_CHANNELS),
      "channels",        G_TYPE_INT,    AUDIO_CHANNELS,
      "payload",         G_TYPE_INT,    RTP_PAYLOAD_TYPE,
      NULL);
  g_object_set (udp_src,
      "port",    port,
      "caps",    rtp_caps,
      "timeout", (guint64) UDP_TIMEOUT_NS,
      NULL);
  gst_caps_unref (rtp_caps);

  gst_bin_add_many (GST_BIN (data.pipeline), udp_src, depayloader, queue,
      audio_convert, audio_resample, audio_sink, NULL);

  if (!gst_element_link_many (udp_src, depayloader, queue, audio_convert,
          audio_resample, audio_sink, NULL)) {
    g_printerr ("Elementos não puderam ser linkados.\n");
    gst_object_unref (data.pipeline);
    return EXIT_FAILURE;
  }

  src_pad = gst_element_get_static_pad (udp_src, "src");
  gst_pad_add_probe (src_pad, GST_PAD_PROBE_TYPE_BUFFER, first_packet_probe, &data, NULL);
  gst_object_unref (src_pad);

  /* ---------- Execução ---------- */
  data.loop = g_main_loop_new (NULL, FALSE);
  bus = gst_element_get_bus (data.pipeline);
  gst_bus_add_watch (bus, bus_call, &data);
  g_unix_signal_add (SIGINT, on_sigint, &data);

  g_print ("[RX] Escutando UDP na porta %d (RTP L16 %d Hz, %d canais, PT %d)\n",
      port, AUDIO_RATE, AUDIO_CHANNELS, RTP_PAYLOAD_TYPE);

  /* Falha aqui normalmente indica porta em uso ou sem permissão */
  ret = gst_element_set_state (data.pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    g_printerr ("Não foi possível iniciar o pipeline (porta %d em uso?).\n", port);
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
