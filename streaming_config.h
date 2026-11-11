#ifndef STREAMING_CONFIG_H
#define STREAMING_CONFIG_H

/* Parâmetros compartilhados entre transmissor e receptor.
 * O receptor não negocia formato com o transmissor (UDP não tem canal de
 * retorno), então os caps RTP precisam ser idênticos nos dois lados. */

#define DEFAULT_HOST        "127.0.0.1"
#define DEFAULT_PORT        5000
#define DEFAULT_URI         "file:///home/conan/Videos/sintel_trailer-480p.webm"

/* Formato PCM transmitido: L16 (S16BE) 44.1 kHz estéreo — RFC 3551 */
#define AUDIO_RATE          44100
#define AUDIO_CHANNELS      2
#define RTP_PAYLOAD_TYPE    96   /* faixa dinâmica (96-127) */

#endif /* STREAMING_CONFIG_H */
