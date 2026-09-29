# Manual do port para Nintendo Switch (OpenPrey)

Guia de trabalho para continuar o port do OpenPrey para o Switch e para
começar ports parecidos. Ele reúne o que aprendemos na prática: regras, ambiente,
ciclo de teste, armadilhas e medições.

Documentação técnica do port (em inglês): [switch-port.md](switch-port.md).

**Numa sessão nova com o Claude, comece com:**

> Leia `docs-dev/switch-port-handbook.md` e `docs-dev/switch-port.md` antes de começar.

---

## 0. Problemas em aberto (leia primeiro)

Atualizado a cada rodada de testes. Quando um problema for resolvido, ele sai
daqui e a lição vai para a seção 5.

### A conversa com o avô (Enisi) no corredor não começa

- **Como deveria ser:** ao sair do banheiro, o avô aparece no corredor e diz
  "Tommy". A conversa (`Conversation1`, em `script/map_roadhouse.script`)
  destrava o resto do bar: Jen, os bêbados e a briga. Sem ela o jogo não avança.
- **Cadeia no mapa:**
  1. o gatilho `trigger_once` `rhGrandfatherHallwayTrigger` (com
     `"isSimpleBox" "0"`: usa o formato exato do brush);
  2. que aciona o `trigger_relay_5`;
  3. que chama `map_roadhouse::Conversation1`.
- **O que funciona:** o espelho do banheiro ("What are you looking at?") e os
  comentários do Tommy ("Doesn't anyone ever clean this place?"). São gatilhos
  `trigger_multiple` de caixa simples. Portanto tocar gatilhos funciona.
- **Causa provável (análise de código, 29/09):**
  - **O plano B já existente:** o OpenPrey já tinha um plano B em
    `idEntity::TouchTriggers` (`src/game/Entity.cpp`). Quando o teste preciso de
    colisão (`ClipContents`) diz que o jogador não está no gatilho, ele confere
    pelas caixas de contorno. É assim que os gatilhos são tocados.
  - **Onde o gatilho do avô falha:** depois do toque, os gatilhos com
    `isSimpleBox 0` confirmam em `hhTrigger::IsEncroaching`
    (`src/Prey/game_trigger.cpp`) usando **só** o teste preciso. Se ele falha, o
    gatilho se desliga sem disparar.
  - **Por que os outros funcionam:** os gatilhos de caixa simples confirmam
    pelas caixas.
- **Correção na versão de 29/09 (a testar):**
  - **Plano B:** `IsEncroaching` ganhou o mesmo plano B para o jogador.
  - **Diagnóstico com `g_debugTriggers 1`:** o log passa a registrar cada
    gatilho tocado, cada recusa ("dropped: ... is not inside it") e cada vez que
    o plano B salvou o toque ("precise test missed ..., accepted by bounds").
- **Teste** (saia do banheiro andando, sem `noclip`):
  1. no console, `g_debugTriggers 1`;
  2. ande pelo corredor até o avô;
  3. se ele não falar, digite `trigger trigger_relay_5` (força a conversa);
  4. mande o log.
- **Se o log mostrar "accepted by bounds" para `rhGrandfatherHallwayTrigger`:**
  a causa está confirmada. O teste preciso de colisão falha, então vale
  investigar por que ele falha, porque também é usado nas zonas de gravidade
  (`game_zone.cpp`) das fases seguintes.
- **Windows não serve para comparar neste PC:**
  - a build de Windows do OpenPrey trava ao iniciar numa função de OpenGL que
    o driver da Intel HD 4000 (de 2016) não tem;
  - o áudio também exige o OpenAL Soft, porque a DLL que vem com a build é só
    o roteador da Creative.

  A cópia de teste está em `.tmp/win-test`, com o OpenAL Soft e o teste
  automático `save/basepr/npctest.cfg`.
- **A correção fica no código do jogo** (`src/Prey`): precisa ser levada para o
  OpenPrey-GameLibs.
- **Nunca use `noclip` para pular essa parte:** no noclip o jogador não toca
  gatilhos (`Player.cpp`: `if ( !noclip ... ) TouchTriggers()`).

### Falhas gráficas no build Mesa 26 (`OpenPrey-mesa-sdk.nro`)

- **Aparência (prints de 29/09, banheiro):**
  - listras verticais pretas sobre paredes, pôsteres e espelho iluminados;
  - áreas bem mais escuras que no Mesa padrão;
  - na cena do espelho, uma faixa preta no topo e um pedaço da imagem
    deslocado para a direita.
- **Não acontece** no `OpenPrey.nro` (Mesa 20.1 do devkitPro). É um problema
  do driver Mesa 26 (port danfromtico/StevensND, backend próprio para o
  Horizon), ou da forma como o motor usa algum recurso que ele trata diferente.
- **Desempenho:** com as threads distribuídas pelos núcleos, o Mesa 26 ficou
  parecido com o Mesa 20.1 (~100 ms por quadro com ~2000 desenhos). Com a
  thread de GL (`r_switchGLThread 1`) a impressão foi melhor, mas o log mostrou
  pouca diferença (backend 74-79 ms com a thread, 76-86 ms sem).
- **Teste para isolar** (Mesa 26, parado no banheiro, trocando um item por vez
  e anotando se as listras somem):
  1. `r_shadows 0` (sombras de stencil);
  2. `r_skipSpecular 1`, depois `r_skipBump 1` (partes do cálculo de luz);
  3. `r_dynamicResolution 0` e `r_renderScale 100` (cópia e ampliação da tela);
  4. `r_switchGLThread 0` e `vid_restart` (a thread de GL);
  5. `r_useIndexBuffers 1`.
- **Resultado em 29/09:** as listras continuaram com `r_skipSpecular 1`,
  `r_skipBump 1`, `r_dynamicResolution 0` + `r_renderScale 100`,
  `r_switchGLThread 0` + `vid_restart` e `r_useIndexBuffers 1`. Esses ficam
  descartados. **Falta testar `r_shadows 0`**: no teste foi digitado
  `r_shadow`, sem o "s", e o console respondeu `Unknown command`.
- **Se nada resolver:** atualizar o Mesa 26 para a versão mais nova do port e
  testar de novo. O build padrão continua sendo o Mesa 20.1.

### Clock da CPU (perfis 3 e 4) não fica aplicado

- **Sintoma:** o log diz `CPU clock 1020 -> 1224 MHz: set`, mas o Status Monitor
  mostra ~1015 MHz.
- **Causa provável:** outro serviço desfaz o ajuste. Pode ser o sys-clk, se
  houver uma regra dele para o título usado no title override ou uma regra
  global. Pode ser também o próprio sistema, que aplica a configuração de
  desempenho um instante depois do nosso ajuste.
- **Diagnóstico nesta versão:** um segundo depois de aplicar o perfil (e depois
  de cada carregamento), o jogo ajusta de novo e grava `CPU clock check: N MHz`
  com o valor lido de volta.
  - **Se aparecer 1224:** funcionou.
  - **Se aparecer ~1020:** algo sobrescreve o ajuste. Desative as regras do
    sys-clk para o título e teste de novo, ou use o próprio sys-clk para
    definir o clock da CPU.
- **Resultado em 29/09 (perfil 4):** o log leu de volta `CPU clock check:
  1785 MHz`, no início e depois do carregamento do mapa. Pelo serviço de clock,
  o ajuste ficou.
- **Falta confirmar:** se o Status Monitor mostra ~1785 MHz com esta versão.
  O print anterior mostrava ~1015 MHz, mas era de uma versão sem essa
  checagem. Se ele continuar mostrando ~1015, o valor aceito pelo serviço não
  está chegando ao hardware, e o caminho é deixar o clock da CPU com o sys-clk.

### Zoom (D-pad para cima)

- O código do Prey só aplica o zoom com uma arma na mão (seção 3). Conferir
  quando houver arma.

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
- **O motor grava em `basepr/`** (é o primeiro diretório de busca):
  - `OpenPreyConfig.cfg`: configurações e binds. Apagar volta tudo ao padrão.
  - `generated/images/*.bimage`: cache das texturas já comprimidas em DXT.
    Apagar faz o próximo carregamento de cada mapa recomprimir todas as imagens
    na CPU, o que leva vários minutos e parece um travamento. No log aparecem
    linhas `Writing generated/images/...`. Só é preciso apagar se mudar
    `image_compressTextures`.
  - Saves e logs.
- Ler um crash:
  `aarch64-none-elf-addr2line -f -C -e .tmp/elf-builds/OpenPrey-<commit>.elf <offsets>`
- **Interagir** (apertar botões, usar telas) é o botão de ataque (ZR), como no
  Prey original: chegue perto e mire no alvo.
  - O zoom (D-pad para cima) só funciona com uma arma (`hhPlayer`, código do jogo).
  - **O Prey não tem botão para falar com NPCs.** As conversas do bar
    (`script/map_roadhouse.script`) disparam sozinhas:
    - ao passar por gatilhos invisíveis (`trigger_once`, por exemplo o
      `Conversation2` no corredor);
    - ao ficar a menos de 312 unidades do balcão (`WaitPlayerNearBar`).

    Os comentários dos bêbados e da Jen também são gatilhos de toque.
  - **Não use `noclip` em áreas com roteiro.** No noclip o jogador não toca
    gatilhos (`if ( !noclip ... ) TouchTriggers()` em `Player.cpp`), então a
    cena não avança. Carregue um save anterior e ande normalmente.
- **Console do jogo:** o botão **−** abre e fecha; **A** abre o teclado do sistema.

## 4. Mapa do código do Switch

| Arquivo | Função |
|---|---|
| `src/sys/switch/switch_main.cpp` | `main`, thread do motor (pilha de 16 MB), caminhos, eventos, handler de crash, clocks (perfis `r_switchPerfProfile`, CPU via clkrst, boost no carregamento), `com_logPerf`/`com_logHitches`, saída limpa |
| `switch_glimp.cpp` | EGL/Mesa, contexto GL 4.3 compat 1280x720, swap, trava de fps (`r_fpsLock`, `Switch_PaceFrame`) |
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

10. **Trava de fps que parecia travamento no carregamento.** Durante o
    carregamento, o jogo redesenha a tela de loading depois de quase cada arquivo
    lido (`PacifierUpdate`). Uma espera de 33 ms na troca de quadro virava minutos
    de carregamento, com a CPU a ~15%. Swap interval 2 congelava a tela. A trava
    agora espera **entre** quadros do laço principal (`Switch_PaceFrame`), nunca
    dentro de um quadro. Regra geral: nada de dormir dentro do render.
11. **Nem todo problema de controle é do port.**
    - Interagir pelo botão de ataque e o zoom que exige arma são regras do
      jogo.
    - As conversas com NPCs são gatilhos de roteiro, que o `noclip` desliga.

    Antes de "consertar" um controle, leia o script do mapa (dentro dos pk4) e
    confira na versão de Windows se o comportamento é o mesmo.
12. **Brigar com o sys-clk.** Reaplicar o clock da CPU a cada segundo fazia o
    valor oscilar no Status Monitor. O jogo agora só define o clock ao aplicar o
    perfil e depois de cada carregamento. Um ajuste do sys-clk para este título
    vence.

## 6. Desempenho: o que já sabemos

A referência completa, em inglês, está na seção Performance de
[switch-port.md](switch-port.md). Resumo:

- **Evolução:**
  - Original: 10-15 fps.
  - Hoje, cenas leves: ~30 fps.
  - Cenas pesadas (o bar cheio de NPCs, ~2000 desenhos): ~9-10 fps.
  - Carregamento: 118 s → ~50 s.
- **O backend (envio de desenhos ao driver) é 75-85% do quadro.**
- **Há dois tipos de cena:**
  - **Presas na GPU** (banheiro): metade da resolução dobra o fps. A resolução
    dinâmica resolve.
  - **Presas na CPU/driver** (bar): ~40 µs por desenho, e a resolução não muda
    nada. A resolução dinâmica agora percebe isso e desfaz a redução.
- **Descartado:**
  - cache de parâmetros ARB (nenhum ganho);
  - recriação de buffers de vértices (pequena);
  - Mesa 26 (mais lento e com falhas gráficas);
  - SaltyNX (trocado pelo monitor nativo).
- **Medido com `r_perfGpuSync 1`, no bar:** ~25 ms de CPU e mais ~19-28 ms de
  GPU por quadro. CPU e GPU estão, os dois, perto do limite de 33 ms.
- **Clock da CPU ajuda pouco:** mesmo a ~2,4 GHz o fps quase não subiu. Os
  núcleos 0 e 3 ficam ocupados e os núcleos 1-2 ficam ociosos. O núcleo 3 é do
  sistema, onde roda o serviço do driver da GPU.
- **Também descartados:** `r_useIndexBuffers 1` e swap interval 0/1.
- **A pesquisa aponta o driver.** O nouveau (base do Mesa no Switch) não usa
  Zcull, render targets comprimidos nem o cache em tiles da GPU, e seu OpenGL
  pesa bem mais na CPU que o deko3d. Fontes na seção Performance de
  switch-port.md.
- **Mesa 26 retestado (29/09):**
  - com as threads nos núcleos certos, ficou parecido com o Mesa 20.1;
  - a thread de GL ajudou pouco nos números;
  - as falhas gráficas continuam (seção 0).
- **Decisão:** trava em 30 fps, com CPU a 1224 MHz no perfil 3 para segurar um
  mínimo nas cenas pesadas.
- **Expectativa realista:**
  - 30 fps cravados nas cenas pesadas exigem cortar o custo por desenho
    (índices, backend numa thread própria);
  - 60 fps estáveis exigiriam um renderer novo, na API nativa (deko3d), como o
    do Doom 3 oficial. São meses de trabalho.

### Cvars úteis

| Cvar | Padrão | Uso |
|---|---|---|
| `com_showFPS 1` | 0 | fps e resolução 3D na tela |
| `com_logPerf 1` | 0 | Uma linha de desempenho por segundo no log |
| `com_logHitches` | 100 | Registra quadros acima de N ms, com o detalhamento |
| `r_perfGpuSync 1` | 0 | Diagnóstico: separa tempo de CPU e de GPU (baixa o fps) |
| `r_fpsLock` | 30 | 30 = travado; 0 = destravado (de 1 a 19 vale 30) |
| `r_dynamicResolution` | 1 | Resolução dinâmica; testa cada redução e desfaz se não ajudou |
| `r_dynamicResolutionMin` | 50 | Resolução mínima, em % |
| `r_renderScale` | 100 | Resolução máxima, em % |
| `r_switchPerfProfile` | 3 | 0 = padrão; 1 = GPU 384 MHz; 2 = GPU 460,8; 3 = GPU 460,8 + RAM 1600 + CPU 1224; 4 = como 3 com CPU 1785 (experimental) |
| `r_switchGLThread` | 1 | Só no build Mesa 26: driver de GL numa thread própria (núcleo 2); vale após `vid_restart` |
| `r_useIndexBuffers` | 0 | Testado: sem ganho no Switch |
| `image_compressTextures` | 1 | Texturas DXT (2 inclui normal maps) |
| `r_cacheProgramParms` | 1 | Cache de parâmetros (sem efeito medido) |
| `in_gyro` | 1 | 0 = desligado; 1 = sempre; 2 = só mirando com ZL |

**Como medir:**

1. Peça ao testador `com_logPerf 1`, `r_fpsLock 0` e `r_dynamicResolution 0`.
2. Meça sempre nos mesmos lugares: parado no banheiro, no bar cheio de NPCs e
   girando a câmera no corredor.
3. Troque só uma cvar por vez.

Os prints do Status Monitor completam os dados.

### Driver Mesa 20.1 compilado por nós

O `OpenPrey.nro` usa o Mesa 20.1 do devkitPro (pacote `switch-mesa` 20.1.0-5).
O pacman só entrega esse driver já compilado. Para poder modificá-lo, a
receita oficial foi reproduzida em `tools/switch/mesa20/build.sh`, com
instruções no `README.md` da pasta.

- **O que o script faz:** baixa o código do Mesa e os patches do devkitPro,
  confere os checksums, aplica dois patches nossos só de compatibilidade (sem
  mudar o driver) e compila.
- **Resultado:** fica em `.tmp/mesa20-build/mesa20-install/`. O Mesa instalado
  no devkitPro não é tocado.
- **Uso no OpenPrey:** uma pasta de build separada com
  `-Dswitch_mesa_sdk=<essa pasta>/opt/devkitpro/portlibs/switch`.
- **Uso em outro port do devkitPro:** apontar os `include/` e `lib/` dessa
  pasta antes dos portlibs.
- **Armadilha:** não exporte `MESON_RSP_THRESHOLD` ao compilar o Mesa. Sem os
  "arquivos de resposta" do meson, juntar o `libEGL.a` estoura o limite de
  linha de comando do Windows.

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

- Os itens da seção 0: conversa do avô, falhas do Mesa 26 e clock da CPU.
- Medir perfil 3 contra perfil 4 (CPU 1224 contra 1785 MHz) quando o clock
  estiver aplicando (`CPU clock check` no log).
- Backend numa thread própria (núcleo 2), atrás de uma cvar.
- Levar a correção de `Pvs.cpp` para o OpenPrey-GameLibs.
- Tempo de carregamento: ~28 s ainda são imagens.
- Áudio: confirmar qual backend o OpenAL Soft usa. Multiplayer: sockets de verdade.
