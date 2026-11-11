# Atividade Prática: GStreamer - Manipulação de Vídeo H.264 e Áudio PCM (CMP1103)

Este repositório contém a solução desenvolvida em C utilizando a biblioteca GStreamer para a disciplina CMP1103. A aplicação constrói uma pipeline multimédia capaz de reproduzir vídeo codificado em H.264/MPEG-4 e processar simultaneamente áudio em formato bruto (PCM).

## Objetivos e Requisitos Atendidos

- **Fonte de Mídia:** Leitura de ficheiro `.webm` local contendo vídeo comprimido em H.264 e áudio nativo.
- **Vídeo (H.264):** Decodificação dinâmica, conversão do espaço de cores (`videoconvert`), aplicação de efeitos de saturação (`videobalance`) e exibição sincronizada no ecrã (`autovideosink`).
- **Áudio (PCM - `audio/x-raw`):** Separação do fluxo, dupla conversão (`audioconvert`), dupla reamostragem (`audioresample`) e aplicação de filtros limitadores estruturais para alteração profunda nas propriedades da onda sonora.
- **Saída:** Reprodução multimédia funcional em tempo real no sistema operativo, com tratamento integrado de erros de barramento (bus) e paragem de fluxo (EOS).

## Configurações PCM Implementadas

Para demonstrar o impacto das características do áudio digital, a solução foi dividida em dois ficheiros de execução, aplicando restrições diretamente na pipeline através de objetos `GstCaps` no elemento `capsfilter`:

*   **Configuração A (Alta Qualidade - Padrão CD):**
    *   **Ficheiro:** `pipeline_atividade2_a.c`
    *   **Taxa de Amostragem (Rate):** 44.100 Hz
    *   **Formato/Profundidade:** S16LE (16 bits)
    *   **Canais:** 2 (Estéreo)
    *   *Resultado:* Áudio cristalino e direcional, mantendo a integridade da fonte original.

*   **Configuração B (Baixa Qualidade - Efeito Telefone):**
    *   **Ficheiro:** `pipeline_atividade2_b.c`
    *   **Taxa de Amostragem (Rate):** 8.000 Hz
    *   **Formato/Profundidade:** S8 (8 bits)
    *   **Canais:** 1 (Mono)
    *   *Resultado:* Perda intencional de agudos, introdução de ruído (chiado por redução da resolução de amplitude) e perda de perceção espacial (mixagem de canais).

*Nota sobre a arquitetura:* Para que a placa de som nativa reproduzisse o áudio severamente degradado da *Configuração B*, foi implementada uma "arquitetura de gargalo". O áudio é convertido e reamostrado para um limite inferior rígido (destruindo os dados originais) e, de seguida, reformatado por um segundo conversor para respeitar as exigências de entrada da placa de som.

## Arquitetura e Fluxo de Dados

O diagrama completo, modelado em formato de fluxograma, encontra-se no ficheiro **`diagrama.pdf`**.
Ele detalha a taxonomia da pipeline, as duas ramificações independentes de processamento, a representação gráfica do "gargalo" PCM implementado para assegurar a compatibilidade das saídas e o estado transitório dos dados (`video/x-raw` e `audio/x-raw`) entre os nós.

## Requisitos e Execução

### Dependências
- SO: Linux (Testado em Fedora Workstation)
- Bibliotecas: `gstreamer-1.0`, `glib-2.0`
- Ferramentas: CMake, Compilador C (GCC/Clang)

### Como Compilar
A compilação do projeto é gerida via CMake. Na raiz do projeto:

```bash
mkdir build
cd build
cmake ..
make
```
### Como Executar
Antes de iniciar, certifique-se de que a constante do caminho do ficheiro WebM no código-fonte aponta para um diretório local válido.

```Bash
# Para testar o áudio limpo (44.1kHz / 16-bit / Estéreo)
./pipeline_atividade2_a
```

# Para testar o áudio degradado (8kHz / 8-bit / Mono)
```
./pipeline_atividade2_b
```
# streaming_with_gstreamer
