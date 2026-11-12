/*
 * receptor.c — Etapa 3: Diagnóstico de perda de pacotes RTP
 *
 * Fluxo:
 *   udpsrc(porta, caps RTP) -> rtpjitterbuffer(latency) -> rtpL16depay
 *         -> queue -> audioconvert -> audioresample -> autoaudiosink
 *
 * Uso:
 *   ./receptor [porta] [latencia_ms]
 *     porta       padrão: 5000
 *     latencia_ms padrão: 200 (tamanho do buffer de jitter, em ms)
 *
 * Mecanismos de diagnóstico de perdas (Requisito 4):
 *   1. post-drop-messages: mensagem de bus "drop-msg" a cada pacote descartado
 *      (chegou tarde ou buffer cheio), com seqnum, reason, num-too-late,
 *      num-drop-on-latency. Intervalo mínimo configurável por DROP_MSG_INTERVAL_MS.
 *   2. do-lost: evento downstream GstRTPPacketLost a cada pacote considerado
 *      perdido (nunca chegou dentro do prazo); capturado por probe no pad sink
 *      do depayloader e impresso com seqnum e duração em ms.
 *   3. Polling de stats: a cada STATS_INTERVAL_S segundos, lê a GstStructure
 *      "stats" do jitterbuffer e imprime: num-pushed, num-lost, num-late,
 *      num-duplicates e avg-jitter.
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
#define UDP_TIMEOUT_NS          (3 * GST_SECOND)

/* Latência padrão do rtpjitterbuffer e limite aceito pela linha de comando */
#define DEFAULT_JITTER_LATENCY_MS   200
#define MAX_JITTER_LATENCY_MS       10000

/* Intervalo mínimo entre mensagens "drop-msg" do jitterbuffer (ms).
 * 0 = uma mensagem por pacote descartado (verboso); 500 = agrupa por janela. */
#define DROP_MSG_INTERVAL_MS        0

/* Intervalo entre leituras periódicas do bloco "stats" do jitterbuffer (s). */
#define STATS_INTERVAL_S            2

/* ------------------------------------------------------------------ */

typedef struct _CustomData {
  GstElement *pipeline;
  GstElement *jitterbuffer;   /* guardado para o polling de stats */
  GMainLoop  *loop;
  gboolean    receiving;      /* FALSE enquanto não chegam pacotes / após timeout */
} CustomData;

/* ------------------------------------------------------------------ */
/* 1. Probe no pad sink do depayloader: detecta GstRTPPacketLost        */
/* ------------------------------------------------------------------ */
static GstPadProbeReturn
lost_event_probe (GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
  GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);

  if (GST_EVENT_TYPE (event) == GST_EVENT_CUSTOM_DOWNSTREAM) {
    const GstStructure *s = gst_event_get_structure (event);
    if (s && gst_structure_has_name (s, "GstRTPPacketLost")) {
      guint     seqnum   = 0;
      guint64   duration = 0;
      gst_structure_get_uint   (s, "seqnum",   &seqnum);
      gst_structure_get_uint64 (s, "duration", &duration);
      g_printerr ("[LOSS] Pacote PERDIDO: seqnum=%u  duração=%"
                  G_GUINT64_FORMAT " ms\n",
                  seqnum, (guint64) (duration / GST_MSECOND));
    }
  }
  return GST_PAD_PROBE_OK;
}

/* ------------------------------------------------------------------ */
/* 2. Polling periódico de estatísticas do jitterbuffer                 */
/* ------------------------------------------------------------------ */
static gboolean
print_jitterbuffer_stats (gpointer user_data)
{
  CustomData *data = (CustomData *) user_data;
  GstStructure *stats = NULL;

  g_object_get (data->jitterbuffer, "stats", &stats, NULL);
  if (stats) {
    guint64 num_pushed     = 0;
    guint64 num_lost       = 0;
    guint64 num_late       = 0;
    guint64 num_duplicates = 0;
    guint64 avg_jitter_ns  = 0;

    gst_structure_get_uint64 (stats, "num-pushed",     &num_pushed);
    gst_structure_get_uint64 (stats, "num-lost",       &num_lost);
    gst_structure_get_uint64 (stats, "num-late",       &num_late);
    gst_structure_get_uint64 (stats, "num-duplicates", &num_duplicates);
    gst_structure_get_uint64 (stats, "avg-jitter",     &avg_jitter_ns);

    g_print ("[STATS] pushed=%" G_GUINT64_FORMAT
             "  lost=%" G_GUINT64_FORMAT
             "  late=%" G_GUINT64_FORMAT
             "  dup=%" G_GUINT64_FORMAT
             "  jitter=%" G_GUINT64_FORMAT " µs\n",
             num_pushed, num_lost, num_late, num_duplicates,
             (guint64) (avg_jitter_ns / 1000));

    gst_structure_free (stats);
  }
  return G_SOURCE_CONTINUE;   /* mantém o timer ativo */
}

/* ------------------------------------------------------------------ */
/* Probe no src do udpsrc: detecta a chegada (ou retomada) do fluxo    */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
/* Consulta a latência total do pipeline                                */
/* ------------------------------------------------------------------ */
static void
report_pipeline_latency (GstElement *pipeline)
{
  GstQuery *query = gst_query_new_latency ();
  if (gst_element_query (pipeline, query)) {
    gboolean live;
    GstClockTime min_lat, max_lat;
    gst_query_parse_latency (query, &live, &min_lat, &max_lat);
    g_print ("[RX] Latência do pipeline: live=%s, mínima=%" G_GUINT64_FORMAT " ms\n",
        live ? "sim" : "não", (guint64) (min_lat / GST_MSECOND));
  }
  gst_query_unref (query);
}

/* ------------------------------------------------------------------ */
/* Bus watch                                                            */
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

    /* ---- 1b. Mensagens "drop-msg" do rtpjitterbuffer ---- */
    case GST_MESSAGE_ELEMENT: {
      const GstStructure *s = gst_message_get_structure (msg);
      if (s && gst_structure_has_name (s, "GstUDPSrcTimeout")) {
        if (data->receiving)
          g_printerr ("[RX] Fluxo interrompido: nenhum pacote há %" G_GUINT64_FORMAT " s.\n",
              (guint64) (UDP_TIMEOUT_NS / GST_SECOND));
        else
          g_printerr ("[RX] Aguardando pacotes... (o transmissor está ativo?)\n");
        data->receiving = FALSE;
      } else if (s && gst_structure_has_name (s, "drop-msg")) {
        /* post-drop-messages: pacote descartado pelo jitterbuffer */
        guint     seqnum           = 0;
        guint64   timestamp        = 0;
        guint     num_too_late     = 0;
        guint     num_drop_on_lat  = 0;
        const gchar *reason        = NULL;

        gst_structure_get_uint   (s, "seqnum",             &seqnum);
        gst_structure_get_uint64 (s, "timestamp",          &timestamp);
        gst_structure_get_uint   (s, "num-too-late",       &num_too_late);
        gst_structure_get_uint   (s, "num-drop-on-latency",&num_drop_on_lat);
        reason = gst_structure_get_string (s, "reason");

        g_printerr ("[DROP] Pacote DESCARTADO: seqnum=%u  ts=%" G_GUINT64_FORMAT
                    " ms  motivo=\"%s\"  num-too-late=%u  num-drop-on-latency=%u\n",
                    seqnum,
                    (guint64) (timestamp / GST_MSECOND),
                    reason ? reason : "?",
                    num_too_late,
                    num_drop_on_lat);
      }
      break;
    }

    case GST_MESSAGE_STATE_CHANGED:
      if (GST_MESSAGE_SRC (msg) == GST_OBJECT (data->pipeline)) {
        GstState old_s, new_s, pending;
        gst_message_parse_state_changed (msg, &old_s, &new_s, &pending);
        g_print ("[RX] Pipeline: %s -> %s\n",
            gst_element_state_get_name (old_s), gst_element_state_get_name (new_s));
        if (new_s == GST_STATE_PLAYING)
          report_pipeline_latency (data->pipeline);
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
  /* Imprime estatísticas finais antes de encerrar */
  g_print ("\n[RX] Interrompido pelo usuário (Ctrl+C). Estatísticas finais:\n");
  print_jitterbuffer_stats (data);
  g_main_loop_quit (data->loop);
  return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------ */
/* Utilitários de parse de argumentos                                   */
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

static gboolean
parse_latency (const gchar *str, guint *out)
{
  gchar *end = NULL;
  guint64 v = g_ascii_strtoull (str, &end, 10);
  if (end == str || *end != '\0' || v > MAX_JITTER_LATENCY_MS)
    return FALSE;
  *out = (guint) v;
  return TRUE;
}

/* ------------------------------------------------------------------ */
int
tutorial_main (int argc, char *argv[])
{
  CustomData data = { 0 };
  GstElement *udp_src, *depayloader, *queue;
  GstElement *audio_convert, *audio_resample, *audio_sink;
  GstCaps *rtp_caps;
  GstPad *src_pad, *depay_sink_pad;
  GstBus *bus;
  GstStateChangeReturn ret;
  gint port = DEFAULT_PORT;
  guint latency_ms = DEFAULT_JITTER_LATENCY_MS;
  int exit_code = 0;

  gst_init (&argc, &argv);

  if (argc > 1 && !parse_port (argv[1], &port)) {
    g_printerr ("Porta inválida: '%s' (use 1-65535).\n", argv[1]);
    return EXIT_FAILURE;
  }
  if (argc > 2 && !parse_latency (argv[2], &latency_ms)) {
    g_printerr ("Latência inválida: '%s' (use 0-%d ms).\n",
        argv[2], MAX_JITTER_LATENCY_MS);
    return EXIT_FAILURE;
  }

  /* ---------- Elementos ---------- */
  udp_src             = gst_element_factory_make ("udpsrc",          "udp_src");
  /* Armazena e reordena pacotes pelo número de sequência RTP e os libera
   * segundo o timestamp, absorvendo variações de atraso (jitter) da rede */
  data.jitterbuffer   = gst_element_factory_make ("rtpjitterbuffer", "rtp_jitterbuffer");
  depayloader         = gst_element_factory_make ("rtpL16depay",     "rtp_depayloader");
  /* Desacopla a thread de rede da thread de reprodução */
  queue               = gst_element_factory_make ("queue",           "rx_queue");
  audio_convert       = gst_element_factory_make ("audioconvert",    "audio_convert");
  audio_resample      = gst_element_factory_make ("audioresample",   "audio_resample");
  audio_sink          = gst_element_factory_make ("autoaudiosink",   "audio_sink");

  data.pipeline = gst_pipeline_new ("receptor-pipeline");

  if (!data.pipeline || !udp_src || !data.jitterbuffer || !depayloader || !queue ||
      !audio_convert || !audio_resample || !audio_sink) {
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

  g_object_set (data.jitterbuffer,
      /* ---- Etapa 2: controle de jitter ---- */
      "latency",              latency_ms,
      /* ---- Etapa 3: diagnóstico de perdas ---- */
      /* 1. Envia evento GstRTPPacketLost para downstream quando um pacote
       *    não chegou dentro do prazo (nunca chegou); capturado por probe. */
      "do-lost",              TRUE,
      /* 2. Posta mensagem "drop-msg" no bus quando um pacote chegou tarde
       *    demais e foi descartado; capturado em bus_call(). */
      "post-drop-messages",   TRUE,
      /* drop-messages-interval=0: uma mensagem por pacote descartado (máx
       *    detalhamento). Use 500 ms para ambientes com alta taxa de perda. */
      "drop-messages-interval", (guint) DROP_MSG_INTERVAL_MS,
      NULL);

  gst_bin_add_many (GST_BIN (data.pipeline), udp_src, data.jitterbuffer,
      depayloader, queue, audio_convert, audio_resample, audio_sink, NULL);

  if (!gst_element_link_many (udp_src, data.jitterbuffer, depayloader, queue,
          audio_convert, audio_resample, audio_sink, NULL)) {
    g_printerr ("Elementos não puderam ser linkados.\n");
    gst_object_unref (data.pipeline);
    return EXIT_FAILURE;
  }

  /* Probe no udpsrc: detecta o primeiro pacote recebido */
  src_pad = gst_element_get_static_pad (udp_src, "src");
  gst_pad_add_probe (src_pad, GST_PAD_PROBE_TYPE_BUFFER, first_packet_probe, &data, NULL);
  gst_object_unref (src_pad);

  /* Probe no depayloader: captura eventos GstRTPPacketLost emitidos por do-lost */
  depay_sink_pad = gst_element_get_static_pad (depayloader, "sink");
  gst_pad_add_probe (depay_sink_pad,
      GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      lost_event_probe, NULL, NULL);
  gst_object_unref (depay_sink_pad);

  /* ---------- Execução ---------- */
  data.loop = g_main_loop_new (NULL, FALSE);
  bus = gst_element_get_bus (data.pipeline);
  gst_bus_add_watch (bus, bus_call, &data);
  g_unix_signal_add (SIGINT, on_sigint, &data);

  /* Timer periódico para leitura das estatísticas do jitterbuffer */
  g_timeout_add_seconds (STATS_INTERVAL_S, print_jitterbuffer_stats, &data);

  g_print ("[RX] Escutando UDP na porta %d (RTP L16 %d Hz, %d canais, PT %d)\n",
      port, AUDIO_RATE, AUDIO_CHANNELS, RTP_PAYLOAD_TYPE);
  g_print ("[RX] rtpjitterbuffer latency = %u ms\n", latency_ms);
  g_print ("[RX] Diagnóstico: do-lost=ON  post-drop-messages=ON  stats a cada %ds\n",
      STATS_INTERVAL_S);

  /* Falha aqui normalmente indica porta em uso ou sem permissão */
  ret = gst_element_set_state (data.pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    g_printerr ("Não foi possível iniciar o pipeline (porta %d em uso?).\n", port);
    GstMessage *err_msg = gst_bus_pop_filtered (bus, GST_MESSAGE_ERROR);
    if (err_msg) {
      bus_call (bus, err_msg, &data);
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
