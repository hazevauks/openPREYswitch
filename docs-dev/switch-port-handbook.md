# Manual do port para Nintendo Switch (OpenPrey)

Guia de trabalho para continuar o port do OpenPrey para o Switch e para
começar ports parecidos. Ele reúne o que aprendemos na prática: regras, ambiente,
ciclo de teste, armadilhas e medições.

Documentação técnica do port (em inglês): [switch-port.md](switch-port.md).

**Numa sessão nova com o Claude, comece com:**

> Leia `docs-dev/switch-port-handbook.md` e `docs-dev/switch-port.md` antes de começar.

---

## 1. Regras que não mudam

- **Nunca mexer** em `C:\Users\Usuario\Downloads\trabalho\openPREYwindows (NÃO MEXER)`.
- **Push só para o seu fork** (`hazevauks/openPREYswitch`), branch `switch-port`.
  PRs para o repositório original (`themuffinator/OpenPrey`) só se você pedir.
- **Nunca colocar no repositório arquivos do jogo** (`.pk4`, texturas, áudio). O
  jogador usa a própria cópia do Prey.
- `src/game` é um espelho do repositório OpenPrey-GameLibs (regra do `AGENTS.md`).
  Mudanças no código do jogo precisam ser levadas para lá também.
  **Pendente:** a correção de `long` para `int` em `src/game/Pvs.cpp`.
- Arquivos temporários ficam em `.tmp/`, que o git ignora.
- Meson é o sistema de build oficial. Mudou algo no fluxo de trabalho? Atualize
  `docs-dev/switch-port.md` no mesmo commit.
- O Claude não digita senhas nem credenciais. Login é com você.

## 2. Ambiente (Windows)

| Item | Onde / como |
|---|---|
| devkitPro | `C:\Users\Usuario\Downloads\devkitPro` |
| Shell de build | `devkitPro\msys2\usr\bin\bash.exe`, **sempre com `MSYSTEM=MSYS`** (no modo MinGW, o `meson`/`python` do msys não rodam) |
| Caminho `/opt/devkitpro` | Mapeado em `devkitPro\msys2\etc\fstab`. Cópia de segurança: `fstab.bak-antes-openprey`. Se mover a pasta do devkitPro, corrija essa linha. |
| Python | O do Windows não está instalado. Use `devkitPro/msys2/usr/bin/python3.exe`. |
| Cross file | `tools/switch/meson/switch-cross.ini` (`host_machine.system = 'horizon'`, `-D__SWITCH__`) |
| Pastas de build | `builddir-switch/` (principal) e `builddir-switch-mesa26/` (experimento abandonado) |

### Compilar

```bash
MSYSTEM=MSYS /c/Users/Usuario/Downloads/devkitPro/msys2/usr/bin/bash.exe -lc "cd /c/Users/Usuario/Downloads/trabalho/openPREYswitch && export MESON_RSP_THRESHOLD=2147483647 && meson compile -C builddir-switch"
```

- `MESON_RSP_THRESHOLD=2147483647` é **obrigatório**. Sem ele, linhas de link
  longas vão para um "response file", o MSYS2 não converte os caminhos, e o link falha.
- Uma build completa leva uns 15 minutos. Mexer em cabeçalhos comuns (por exemplo,
  `RenderSystem.h`) recompila tudo; mexer só em `src/sys/switch/*.cpp` é rápido.
- Configurar do zero:
  `meson setup builddir-switch --cross-file tools/switch/meson/switch-cross.ini -Dbuildtype=release`
- Resultado: `builddir-switch/OpenPrey.nro`. Com símbolos:
  `builddir-switch/OpenPrey-client_arm64.elf`.

### Publicar cada versão (sempre nesta ordem)

1. Compilar.
2. **Arquivar o ELF:** copie para `.tmp/elf-builds/OpenPrey-<commit>.elf`. Sem o
   ELF da mesma build, não dá para ler os crashes.
3. Copiar o NRO para `.tmp/cartao-sd/switch/openprey/OpenPrey.nro`.
4. Fazer commit com mensagem descritiva e `git push origin switch-port`.
5. Dar ao testador um plano de teste curto e numerado, com os comandos de console
   exatos.

## 3. Teste no console

- Rodar por **title override**: segure R ao abrir um jogo instalado, para ter
  memória completa. Precisa de Atmosphère, hbmenu/sphaira e Status Monitor
  (mostra CPU, GPU, RAM e fps).
- Pasta no cartão: `sdmc:/switch/openprey/`
  - `OpenPrey.nro`
  - `base/`: os `.pk4` do Prey original
  - `basepr/`: overlay do OpenPrey (inclui `materials/renderscale_openprey.mtr`,
    necessário para a resolução dinâmica)
- **Arquivos para mandar depois de cada teste:**
  - `base/logs/openprey.log`: gravado linha a linha, então a última linha é o
    último evento antes de um travamento.
  - `openprey_error.txt`: mensagem de erro fatal.
  - `openprey_crash.txt`: exceção de CPU (PC, LR, backtrace).
  - Relatórios do Atmosphère (`atmosphere/crash_reports/`), quando o sistema
    mostra uma tela de erro.
- Ler um crash:
  `aarch64-none-elf-addr2line -f -C -e .tmp/elf-builds/OpenPrey-<commit>.elf <offsets>`
- **Console do jogo:** o botão **−** abre e fecha; **A** abre o teclado do sistema.

## 4. Mapa do código do Switch

| Arquivo | Função |
|---|---|
| `src/sys/switch/switch_main.cpp` | `main`, thread do motor (pilha de 16 MB), caminhos, eventos, handler de crash, perfis de clock (`r_switchPerfProfile`), boost durante o carregamento, `com_logPerf`/`com_logHitches`, saída limpa |
| `switch_glimp.cpp` | EGL/Mesa, contexto GL 4.3 compat 1280x720, swap, `r_fpsLock` |
| `switch_input.cpp` | Controles (jogo/menu/console), binds padrão versionados (`in_switchControlScheme`), teclado do sistema, toque |
| `switch_gyro.cpp` | Mira por giroscópio (`in_gyro*`) |
| `switch_threads.cpp` | Threads e locks; `__wrap_pthread_create` coloca cada thread num núcleo |
| `switch_net.cpp` | Rede só em loopback (stub) |
| `tools/switch/make_game_object.py` | "DLL falsa": junta o jogo num objeto só (`ld -r`) e expõe só `GetGameAPI` |
| `tools/switch/gen_gl11_loader.py` | Gera os ponteiros das funções GL 1.1 via `eglGetProcAddress` |
| `tools/switch/gltest/` | Programa de teste das capacidades de GL no hardware |
| `src/renderer/RenderSystem.cpp` | Escala de renderização e resolução dinâmica, timers de desempenho |
| `src/renderer/tr_backend.cpp` | Cache de parâmetros, contadores de desempenho, `r_perfGpuSync` |
| `src/renderer/Image_load.cpp` | Compressão de texturas em DXT (`image_compressTextures`) |
| `src/framework/FileSystem.cpp` | Cache de diretórios, estatísticas de carregamento, parada da thread de download |

## 5. Armadilhas já encontradas (e a solução)

1. **`long` tem 64 bits no Switch (LP64).** O código de 2006 supõe 32 bits.
   Causou crash na colisão (macros de sinal), `RSqrt`, `rvRandom`, `Pvs.cpp`, e o
   `threadHandle` truncado (que virou `intptr_t`). Em qualquer crash estranho,
   procure `long` usado como se tivesse 32 bits.
2. **Estouro de unidades de textura:** `MAX_MULTITEXTURE_UNITS` subiu para 32. O
   driver informa mais unidades do que o array aguentava, e o estouro sobrescrevia
   memória.
3. **Erro fatal sem mensagem:** o motor travava ao desligar e a mensagem se
   perdia. Agora ela é gravada em arquivo **antes** do desligamento.
4. **Crash ao sair do jogo:**
   - Sair de dentro da thread do motor: agora usa `pthread_exit` e retorna do `main`.
   - Uma thread esquecida viva (a de download em segundo plano) fazia o sistema
     reclamar ao fechar: agora é parada. Toda thread criada precisa terminar antes
     do `main` retornar.
   - Serviços do sistema (giroscópio, perfil de clock) precisam ser fechados e
     restaurados na saída.
5. **Crash ao carregar save:** material nulo em `R_GlobalShaderOverride`. É um bug
   do motor original; foi resolvido com uma verificação de nulo.
6. **Build:**
   - A macro `BIT` existe no idlib e na libnx: use `#undef BIT` antes do `<switch.h>`.
   - `_LITTLE_ENDIAN` conflita com os cabeçalhos de rede.
   - O idlib proíbe `snprintf`: use `idStr::snPrintf`.
   - Grupos COMDAT no `ld -r` exigem `--force-group-allocation`.
7. **Edição de arquivos:**
   - Muitos arquivos do motor usam finais de linha CRLF, e substituições com
     perl/sed falham neles em silêncio. Prefira a ferramenta Edit.
   - Ao gerar código C por script (heredoc + Python), o `\n` dentro de strings
     pode virar uma quebra de linha real e quebrar o `printf`. Confira o
     resultado com `grep`.
8. **Perfil de clock não aplicava na hora:** agora compara o valor e alterna o
   boost para o sistema reaplicar.
9. **Giroscópio com o eixo X invertido:** corrigido no código. Quem já tinha
   `in_gyroInvertX 1` salvo precisa voltar para 0.

## 6. Desempenho: o que já sabemos

Medido no console com GPU a 460,8 MHz e memória a 1600 MHz (perfil 3):

- **Evolução:**
  - Original: 10-15 fps.
  - Pós-processamento desligado, resolução dinâmica, texturas DXT e GPU a 460,8 MHz: ~30 fps parado.
  - Carregamento: 118 s → 50 s.
- **O backend (envio de desenhos ao driver) é 75-85% do quadro.** Jogo: ~2-4 ms.
  Preparação do render: ~2-5 ms.
- **Parado, o limite é a GPU:** metade da resolução dobra o fps; GPU a 99%; RAM
  ~15 GB/s.
- **Ao girar a câmera, o backend fica até 4x mais lento** com quase o mesmo número
  de desenhos, e há vários quadros de ~100 ms sem leitura de arquivo. Causa ainda
  não confirmada. Suspeitos:
  - buffers de vértices recriados a cada quadro para modelos animados (veja
    `buffers` no `com_logPerf`);
  - sincronização da CPU com a GPU (veja `r_perfGpuSync`).
- **Descartado:**
  - Cache de parâmetros ARB: nenhum ganho.
  - Mesa 26: mais lento e com falhas gráficas.
  - SaltyNX: trocado pelo monitor nativo do jogo.
- **Decisão:** trava em 30 fps (`r_fpsLock 30`), com a resolução dinâmica
  segurando os 30.
- **Expectativa realista:** 60 fps estáveis exigiriam um renderer novo, escrito
  direto na API nativa (deko3d), como o do Doom 3 oficial. São meses de trabalho.
- **Próximo passo estrutural:** rodar o backend numa thread própria, no núcleo 2
  (hoje jogo, preparação e envio rodam em série no núcleo 0).

### Cvars úteis

| Cvar | Padrão | Uso |
|---|---|---|
| `com_showFPS 1` | 0 | fps e resolução 3D na tela |
| `com_logPerf 1` | 0 | Uma linha de desempenho por segundo no log |
| `com_logHitches` | 100 | Registra quadros acima de N ms, com o detalhamento |
| `r_perfGpuSync 1` | 0 | Diagnóstico: separa tempo de CPU e de GPU (baixa o fps) |
| `r_fpsLock` | 30 | 30 = travado; 0 = destravado |
| `r_dynamicResolution` | 1 | Resolução dinâmica |
| `r_dynamicResolutionMin` | 50 | Resolução mínima, em % |
| `r_renderScale` | 100 | Resolução máxima, em % |
| `r_switchPerfProfile` | 3 | 0 = padrão; 1 = GPU 384 MHz; 2 = GPU 460,8; 3 = GPU 460,8 + RAM 1600 |
| `image_compressTextures` | 1 | Texturas DXT (2 inclui normal maps) |
| `r_cacheProgramParms` | 1 | Cache de parâmetros (sem efeito medido) |
| `in_gyro` | 1 | 0 = desligado; 1 = sempre; 2 = só mirando com ZL |

**Como medir:** peça ao testador `com_logPerf 1`, `r_fpsLock 0` e
`r_dynamicResolution 0`, sempre nos mesmos lugares (parado no banheiro, girando
a câmera no corredor), trocando só uma cvar por vez. Os prints do Status Monitor
completam os dados.

## 7. Roteiro para portar outro jogo (id Tech 4 ou parecido)

A ordem que funcionou aqui:

1. **Estudar o motor:** sistema de build, camada de plataforma (`src/sys/*`), como
   o jogo é carregado (DLL?), qual API gráfica usa.
2. **Testar o hardware antes do jogo:** um programa pequeno (como o
   `tools/switch/gltest`) que confirma versão de GL, extensões e stencil no console.
3. **Cross file do Meson** com `host_machine.system = 'horizon'` e `__SWITCH__`.
   Compile as partes em ordem: idlib → jogo → motor.
4. **Camada de plataforma própria** (`src/sys/switch/`), em vez de encher o
   código de Linux de `#ifdef`s: arquivos, tempo, threads, entrada, vídeo, rede
   em stub.
5. **DLLs viram objeto estático** (a técnica do `make_game_object.py`).
6. **GL sem libGL:** tudo passa por `eglGetProcAddress`.
7. **Diagnóstico antes de otimizar:**
   - handler de crash com backtrace;
   - arquivo de erro gravado cedo;
   - log gravado linha a linha;
   - ELF guardado de cada build.
8. **Procurar bugs de 64 bits** (`long`, ponteiros guardados em `int`).
9. **Jogável primeiro, rápido depois:**
   - perfis de clock;
   - threads distribuídas nos núcleos;
   - texturas comprimidas;
   - resolução dinâmica;
   - pós-processamento desligado;
   - só então otimizações mais profundas, **sempre medidas** e atrás de uma cvar
     que dá para desligar.
10. **Controles:** esquema moderno de FPS, binds versionados e giroscópio opcional.
11. **Saída limpa:** parar todas as threads e restaurar os serviços do sistema.

## 8. Pendências

- Analisar o próximo log (`buffers`, `gpu wait`, trava de 30) e corrigir a queda
  ao girar a câmera.
- Backend numa thread própria (núcleo 2), atrás de uma cvar.
- Levar a correção de `Pvs.cpp` para o OpenPrey-GameLibs.
- Tempo de carregamento: ~28 s ainda são imagens.
- Áudio: confirmar qual backend o OpenAL Soft usa. Multiplayer: sockets de verdade.
