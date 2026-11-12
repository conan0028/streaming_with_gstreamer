# Streaming de Áudio RTP/UDP com GStreamer (C)

Atividade prática da disciplina **CMP1103** — transmissão e recepção de áudio em tempo real com **GStreamer 1.0** em **C**, construída com **CMake**.

## Sumário

- [A. Visão Geral](#a-visão-geral)
- [B. Linux (Fedora) + CLion](#b-linux-fedora--clion)
- [C. Windows + Visual Studio](#c-windows--visual-studio)
- [Solução de problemas](#solução-de-problemas)

---

## A. Visão Geral

A aplicação envia áudio PCM encapsulado em **RTP (L16)** sobre **UDP** entre dois executáveis independentes:

| Executável | Arquivo | Função |
|---|---|---|
| `transmissor` | `transmissor.c` | Captura/gera áudio, converte para S16BE/44100 Hz/estéreo, empacota em RTP e envia por UDP |
| `receptor` | `receptor.c` | Recebe UDP, gerencia jitter, desempacota RTP e reproduz; imprime diagnóstico de perdas |

**Pipelines**

```
Transmissor: fonte -> audioconvert -> audioresample -> capsfilter(S16BE, 44100, 2ch)
             -> rtpL16pay (PT=96) -> udpsink (host:porta)

Receptor:    udpsrc (porta, caps RTP) -> rtpjitterbuffer (latency=N ms) -> rtpL16depay
             -> queue -> audioconvert -> audioresample -> autoaudiosink
```

**Etapas implementadas**

1. **Streaming básico** — transmissão RTP/UDP de `test` (senoide 440 Hz), `mic` (microfone) ou arquivo/URI.
2. **Latência e jitter** — `rtpjitterbuffer` com `latency` configurável pela linha de comando.
3. **Diagnóstico de perda de pacotes** — logs `[LOSS]`, `[DROP]` e `[STATS]`.

**Argumentos**

```text
./transmissor [host=127.0.0.1] [porta=5000] [fonte=test|mic|<arquivo/URI>]
./receptor    [porta=5000]     [latency_ms=200]
```

> Sempre inicie o **receptor primeiro**, depois o transmissor.

**Estrutura do repositório**

```text
.
├── CMakeLists.txt        # alvos: transmissor, receptor
├── streaming_config.h    # constantes (porta, payload type, taxa, canais)
├── transmissor.c
├── receptor.c
├── flashback_pipeline.c, pipeline_atividade2_a.c, pipeline_atividade2_b.c   # atividades anteriores (não compilados pelo CMake)
└── Atividade_Streaming_GStreamer_Fluxo_Audio.pdf
```

---

## B. Linux (Fedora) + CLion

### B.1 Dependências

```bash
sudo dnf install gcc cmake gstreamer1-devel gstreamer1-plugins-base-devel
```

Para rodar as etapas é necessário também (normalmente já instalados no Fedora Workstation):

```bash
sudo dnf install gstreamer1-plugins-good pkgconf-pkg-config iproute-tc
```

- `gstreamer1-plugins-good`: `rtpjitterbuffer`, `rtpL16pay/depay`, `udpsrc/udpsink`.
- `iproute-tc`: fornece o comando `tc` (usado nas etapas 2 e 3).

Verificação rápida:

```bash
gst-inspect-1.0 rtpjitterbuffer | head -n 3
```

### B.2 Clonar e configurar no CLion

```bash
git clone <URL-DO-REPOSITORIO> streaming_with_gstreamer
```

1. **File → Open** e selecione a pasta do projeto (a que contém `CMakeLists.txt`).
2. Se aparecer *Trust Project*, clique em **Trust Project**.
3. O CLion executa o CMake automaticamente (aba **CMake** na parte inferior). Deve aparecer `GStreamer encontrado: 1.x.x` e `[Finished]`. Se falhar, veja [Solução de problemas](#solução-de-problemas). Para recarregar: botão **Reload CMake Project**.
4. No seletor no canto superior direito, devem existir dois alvos: **transmissor** e **receptor**.

### B.3 Argumentos de execução (Run/Debug Configurations)

1. Clique no seletor de alvos → **Edit Configurations…**
2. Selecione **receptor** → campo **Program arguments**, por exemplo: `5000 200`
3. Selecione **transmissor** → **Program arguments**, por exemplo: `127.0.0.1 5000 test`
4. **Apply / OK**.

Para executar os dois ao mesmo tempo, rode o receptor (▶), troque o seletor para o transmissor e rode (▶). O CLion mantém ambos ativos em abas separadas da janela **Run**. Alternativa: crie uma configuração *Compound* contendo as duas, com o receptor primeiro.

> Configuração dedicada para cada teste: use **Copy Configuration** (Ctrl+D) e altere apenas os argumentos (ex.: `receptor-0ms`, `receptor-200ms`, `receptor-1000ms`).

### B.4 Etapa 1 — Streaming básico

| Executável | Program arguments |
|---|---|
| `receptor` | `5000 200` |
| `transmissor` (tom de teste) | `127.0.0.1 5000 test` |
| `transmissor` (microfone) | `127.0.0.1 5000 mic` |

Resultado esperado: um tom contínuo de 440 Hz (ou o áudio do microfone — use fones para evitar microfonia). O transmissor imprime `[TX] Caps RTP: ...`.

### B.5 Etapa 2 — Latência e jitter

**a) Três latências diferentes** (o transmissor permanece `127.0.0.1 5000 test`). Pare o receptor entre os testes e altere somente o 2º argumento:

| Teste | Program arguments do `receptor` |
|---|---|
| Latência 0 ms | `5000 0` |
| Latência 200 ms | `5000 200` |
| Latência 1000 ms | `5000 1000` |

Observe: com **0 ms** o áudio é entregue sem buffer de reordenação (mais sensível a jitter); com **1000 ms** há atraso perceptível, mas maior tolerância.

**b) Simular jitter** (atraso médio de 50 ms ± 40 ms) no terminal, na interface de loopback:

```bash
sudo tc qdisc add dev lo root netem delay 50ms 40ms
```

Repita os três testes acima com o jitter ativo e compare a qualidade do áudio. Confirme que a regra está ativa:

```bash
tc qdisc show dev lo
```

> **Importante:** remova a regra ao terminar (veja [Limpeza de rede](#b7-limpeza-de-rede)). Se já existir uma regra, o comando `add` falha com `File exists`; apague antes com `del`.

### B.6 Etapa 3 — Perda de pacotes

Aplique perda de 30% com o mesmo atraso/jitter:

```bash
sudo tc qdisc add dev lo root netem loss 30% delay 50ms 40ms
```

Rode o receptor (por exemplo `5000 50`, depois `5000 1000` para comparação) e o transmissor (`127.0.0.1 5000 test`). No console **Run** do CLion, observe:

| Prefixo | Significado |
|---|---|
| `[LOSS]` | Pacote RTP considerado **perdido** (nunca chegou dentro da janela de latência). Vem do evento `GstRTPPacketLost` (`do-lost=TRUE`). |
| `[DROP]` | Pacote **descartado** pelo jitterbuffer (chegou atrasado ou buffer cheio). Vem das mensagens de bus `drop-msg` (`post-drop-messages=TRUE`). |
| `[STATS]` | Resumo periódico (a cada 2 s) da propriedade `stats`: `num-pushed`, `num-lost`, `num-late`, `num-duplicates`, `avg-jitter`. |

Análise sugerida para o relatório:

- **Latência baixa (ex.: 50 ms):** muitos `[DROP]` (pacotes atrasados não cabem na janela) e `num-late` alto.
- **Latência alta (ex.: 1000 ms):** quase todos os atrasados são absorvidos; os `[LOSS]` restantes correspondem à perda real (~30%).

### B.7 Limpeza de rede

> [!CAUTION]
> **Sempre remova a regra de rede após os testes.** Enquanto ela existir, todo o tráfego em `lo` (inclusive serviços locais, bancos de dados e o próprio CLion/IDE em portas locais) sofre atraso e perda.

```bash
sudo tc qdisc del dev lo root
```

Verificação (deve mostrar apenas `noqueue`):

```bash
tc qdisc show dev lo
```

### B.8 Compilar e executar pelo terminal (opcional)

```bash
cmake -S . -B build
cmake --build build
./build/receptor 5000 200            # terminal 1
./build/transmissor 127.0.0.1 5000 test   # terminal 2
```

---

## C. Windows + Visual Studio

> [!WARNING]
> O `transmissor.c` e o `receptor.c` usam `<glib-unix.h>` e `g_unix_signal_add` (tratamento de Ctrl+C), que são **exclusivos de Unix** e não existem no GStreamer para Windows. Para compilar no Windows é preciso proteger esse trecho com `#ifdef G_OS_UNIX` (ou substituí-lo por `g_win32`/`signal()`), ou executar o projeto no WSL2/Linux. Este tutorial assume que o código foi adaptado dessa forma.

### C.1 Dependências

1. Instale o **Visual Studio 2022** com a carga de trabalho **Desenvolvimento para desktop com C++** (inclui o suporte a CMake).
2. Baixe em <https://gstreamer.freedesktop.org/download/> os instaladores **MSVC 64-bit** (não use o MinGW):
   - **Runtime installer** (`gstreamer-1.0-msvc-x86_64-<versão>.msi`)
   - **Development installer** (`gstreamer-1.0-devel-msvc-x86_64-<versão>.msi`)
3. Instale **ambos** na mesma pasta (padrão: `C:\gstreamer\1.0\msvc_x86_64`), escolhendo a instalação **Complete**.
4. Instale o **CMake** pelo instalador do Visual Studio ou de <https://cmake.org/download/>. O `pkg-config` é necessário para o `CMakeLists.txt` atual; o instalador de desenvolvimento do GStreamer já inclui `bin\pkg-config.exe`.

### C.2 Variáveis de ambiente

Abra *Editar as variáveis de ambiente do sistema* e configure:

| Variável | Valor |
|---|---|
| `GSTREAMER_1_0_ROOT_MSVC_X86_64` | `C:\gstreamer\1.0\msvc_x86_64` (o instalador costuma criá-la) |
| `PATH` (adicionar) | `C:\gstreamer\1.0\msvc_x86_64\bin` |
| `PKG_CONFIG_PATH` | `C:\gstreamer\1.0\msvc_x86_64\lib\pkgconfig` |

> A pasta `bin` no `PATH` é **obrigatória** para localizar as DLLs do GStreamer em tempo de execução. **Reinicie o Visual Studio** (ou faça logoff) após alterar as variáveis.

Teste em um novo terminal:

```powershell
gst-inspect-1.0 --version
gst-inspect-1.0 rtpjitterbuffer
```

### C.3 Abrir o projeto (Open Folder)

Não crie `.sln`. Use o suporte nativo a CMake:

1. Abra o Visual Studio → **Open a local folder** (ou **File → Open → Folder…**) e selecione a pasta do projeto.
2. O VS detecta `CMakeLists.txt` e gera o cache automaticamente (veja a janela **Output → CMake**).
3. Selecione a configuração **x64-Debug** na barra superior e, no seletor **Startup Item**, escolha `transmissor.exe` ou `receptor.exe`.

Se o cache não for gerado: **Project → Delete Cache and Reconfigure**.

### C.4 Argumentos de execução (`launch.vs.json`)

1. No **Solution Explorer → Folder View**, clique com o botão direito em `CMakeLists.txt` → **Add Debug Configuration** → *Default*. O arquivo `.vs\launch.vs.json` é criado.
2. Defina uma configuração para cada executável:

```json
{
  "version": "0.2.1",
  "defaults": {},
  "configurations": [
    {
      "type": "default",
      "project": "CMakeLists.txt",
      "projectTarget": "receptor.exe",
      "name": "receptor (5000, 200ms)",
      "args": [ "5000", "200" ]
    },
    {
      "type": "default",
      "project": "CMakeLists.txt",
      "projectTarget": "transmissor.exe",
      "name": "transmissor (test)",
      "args": [ "127.0.0.1", "5000", "test" ]
    }
  ]
}
```

3. Selecione `receptor (5000, 200ms)` em **Startup Item** e pressione **Ctrl+F5**; depois selecione `transmissor (test)` e execute novamente (os dois ficam em execução simultânea).
4. Para testar latências diferentes, altere o 2º argumento do receptor (`"0"`, `"200"`, `"1000"`).

> `.vs/` está no `.gitignore`. Se quiser versionar o `launch.vs.json`, remova-o da lista de ignorados ou copie-o para a raiz do projeto.

### C.5 Simulação de perda e jitter no Windows

O `tc`/`netem` é **exclusivo do Linux**. No Windows use uma ferramenta de manipulação de pacotes, por exemplo:

- **[Clumsy](https://jagt.github.io/clumsy/)** (recomendado, gratuito): usa WinDivert para aplicar *Lag* (atraso/jitter), *Drop* (perda %), *Out of order* e *Duplicate*.
- **NetLimiter / Network Emulator for Windows Toolkit (NEWT)**: alternativas.

Uso básico do Clumsy (execute como **Administrador**):

1. Em **Filter**, use `udp and (udp.DstPort == 5000 or udp.SrcPort == 5000)`.
2. Marque **Lag** (ex.: 50 ms) e **Drop** (ex.: 30%) → **Start**.
3. Observe os logs `[LOSS]`, `[DROP]`, `[STATS]` no console do receptor.

Observação: o Clumsy captura tráfego de loopback (`127.0.0.1`) de forma limitada em algumas versões; se não funcionar, rode transmissor e receptor em duas máquinas ou use o WSL2/Linux com `tc`.

---

## Solução de problemas

| Sintoma | Causa provável / solução |
|---|---|
| `Package 'gstreamer-1.0' not found` | Instale `gstreamer1-devel` (Fedora) ou configure `PKG_CONFIG_PATH` (Windows). |
| `no element "rtpjitterbuffer"` / `rtpL16pay` | Falta o pacote `gstreamer1-plugins-good`. |
| Sem áudio, sem erros | Receptor iniciado depois do transmissor, porta diferente, ou dispositivo de áudio mudo. |
| `Address already in use` | Outro receptor ainda aberto na mesma porta; encerre-o ou use outra porta. |
| `tc: command not found` | `sudo dnf install iproute-tc`. |
| Áudio com atraso/falhas mesmo sem teste | Há uma regra `netem` ativa: `sudo tc qdisc del dev lo root`. |
| Windows: `.exe` não abre / DLL ausente | `C:\gstreamer\1.0\msvc_x86_64\bin` não está no `PATH`; reinicie o VS. |
