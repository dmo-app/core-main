# Plano: Core como DAW básica com plugins

Atualizado a 2026-11-16. Base: o `main` em `d4c483c` (PR #3, depois do PR #2 `a8c0dcc`).

## A ideia

O Core passa a ser uma DAW simples, feita à maneira do Diogo. As notas, a estrutura e as tools são do Core; o som vem todo de plugins (VST3 agora, VST2 depois). O Bitwig deixa de ser obrigatório e fica só como opção, por export `.mid`.

## Feito

### 1. Os 9 bugs graves (levantamento de 2026-11-14), commit `8e816c1`

| Id | Bug |
|---|---|
| D1 | Remover o loop deixava a música impossível de gravar (e o autosave falhava) |
| D2 | Trocar para Sampler gravava a versão 3 e o Save recusava |
| D4/D6 | Nomes cortados a meio de um carácter (bounce, secção): UTF-8 inválido, Save falhava |
| ED1 | Send to Arrange lia memória já libertada |
| P1 | Randomize (ghost kick) punha notas fora do padrão: a música não voltava a abrir |
| PL1 | F-Core em modo nota passava os 16 moduladores: a música não voltava a abrir |
| PL2 | Conversão Core FX com fxOnly=1: pista muda fora das regiões |
| PL3 | F-Core sobrevivia à troca de instrumento: o motor recusava o projeto |
| E1 | `engine.cpu` usava o endereço do medidor como identidade: leitura fora dos limites |

Verificado: compila, self-test 53/53, e verificação à parte de D1, D2, D6, PL1, PL3.

### 2. O bug do knob, explicado (commits `b507478`, `1e5cdc9`)

`core-engine knobcheck` prova o caso: toca a música offline, mexe num knob (por `project.load`, como a app), repõe-o e compara com a mesma música sem toques. Uma linha JSON por knob. Antes disso verifica que duas corridas iguais dão amostras iguais e que recarregar a mesma música não muda o som (e diz em que pista muda). Com a saída do som próprio, os knobs desses instrumentos deixaram de existir.

### 3. Fora o som próprio (PR #2, commits `8769e88`, `67a140c`, `74fbf70`)

- Saíram W-Core, F-Core, R-Core, kick, hats, snare, sampler, Sample Path, Glitch, Core FX/EQ, LFO e moduladores, racks, devices e a cadeia de efeitos própria (cerca de 30 mil linhas a menos).
- O motor regista só os tipos `plugin`, `audio`, `bus` e `none` (silencioso até ter um plugin).

### 4. Anfitrião VST3 (commits `14ead77`, `3b203b4`, `23f75c8`)

- **Plugins > Scan plugins (VST3):** cada ficheiro é lido e testado no seu próprio processo; os que crasham ou dão NaN ficam de lado.
- **+ Track:** instrumentos VST3 por fabricante. Menu da pista: Instrumento > VST3, e Plugin effects (abrir, ligar/desligar, remover, adicionar). Efeitos por pista e no master.
- **Janela do plugin:** duplo clique abre; mexer na janela marca a música como por gravar.
- **Estado guardado na música:** o estado de cada plugin vai para o `.core` ao gravar e ao exportar. Automação `vst:`, e relink de plugins entre PCs (`plugin_refs`).
- Verificado: Serum 2 na pista e ValhallaDelay como efeito carregam e tocam (pico da pista 0.14), sem plugins em falta.

### 5. Templates e músicas antigas abrem

- Uma pista com instrumento próprio passa a `none` com uma nota, e mantém as notas à espera de um plugin. Os efeitos próprios ficam guardados e silenciosos.
- A automação de um instrumento que saiu é largada com nota; a de mistura (`gainDb`, `pan`) e a dos efeitos (`fx:`) ficam (commit `131f1b5`).
- Os 21 templates em `templates/` abrem e o motor toca-os (testado em `tests/domain_tests`).

### 6. Testes e CI (PR #3)

- `tests/engine_tests`, `tests/domain_tests`, `cli_version`, `cli_demo_render`, `cli_measure` (label `merge`) e `desktop_self_test` (label `desktop`). Regras em `tests/README.md`.
- Gate de um comando: `ci/gate.ps1` (PC do Diogo) e `ci/gate.sh` (Linux/WSL).
- Workflow `.github/workflows/ci.yml` (Linux e Windows). **Nota:** as 3 corridas no GitHub falharam em 2 segundos, antes de qualquer passo, nos dois sistemas. Parece um problema da conta/runners do GitHub Actions, não do código; está por ver.

### Formato `.core` (sem `.dawproject`)

O `.core` guarda as pistas, os clips, as secções, o tempo, a automação e o estado de cada plugin. O Core abre e toca isso sozinho.

## Feito a 2026-11-16 (ramo `claude/project-thread-rl6ita`)

### 7. Mapa de papéis (passo 1)

- **Settings > Roles:** um plugin por papel: efeitos (reverb, delay, filter, sidechain, drive) e instrumentos (bass, lead, pad, pluck, drums). Guardado em `%APPDATA%\Core\roles.json`, fora das músicas.
- Ao escolher o plugin, o Core lê os parâmetros dele (`core-engine params <ficheiro> --id <id>`, no seu próprio processo) e adivinha as macros pelo nome (mix, size, cutoff, volume...). "Parameters..." muda qualquer uma.
- Domínio sem JUCE (`app/domain/src/roles.hpp`), para as tools e para a AI: `ensureRoleEffect`, `roleParamKey`, `addRoleTrack`, `setTrackRole`. O efeito fica marcado com `"role"` na música.

### 8. Tools pelos plugins dos papéis (passo 2)

- **Piano roll > Plugin tools:** SUCK para o drop (1, 2 ou 4 compassos), PUM a partir de outra pista (compassos mostrados ou a música toda) e efeitos do build-up (filtro + reverb, + delay, + drive).
- **Painel do build-up:** linha "Effects", no mesmo passo de undo que as notas.
- Tudo é automação normal dos plugins (`app/domain/src/role_tools.hpp`): undo, gravar e exportar funcionam como sempre.

### 9. MIDI (passo 4)

- **File > Import MIDI:** cada pista do ficheiro vira uma pista sem instrumento, com padrões de 4 compassos (compassos iguais são o mesmo padrão).
- **File > Export MIDI:** a música como toca, com os rolls expandidos como no motor, uma pista MIDI por pista e o tempo com as rampas.

### 10. Bugs médios e pequenos (passo 6, 1.º lote)

Da lista revista do sweep (`sweep-qwen-revisto.md`, 45 por corrigir). Corrigidos, com teste em `tests/bug_tests.cpp` ou `tests/engine_tests.cpp`:
`engine.start` recusado já não pára o som; loop convertido duas vezes numa mudança de tempo; `setLoop` com início negativo; `measure` "filter" media silêncio; `readWav` com chunk 0xFFFFFFFF; `isolateRegion` apagava `extra` e cortava o nome a meio de um carácter; STUTTER ignorava as repetições de um clip em loop; FOLLOW não gravava o corte; `addPattern` com comprimento 0; ids enormes (`strtol` + 1); `schemaVersion` 2^32+1; `nextSlotId` com `stoi`; `approval` dos padrões; id de padrão com sufixo > 80; casts do P-Core; `noteTool("random")` sem pista; colar áudio no fim não fazia crescer a música; `copyClips` com o BPM inicial.

## Falta fazer

- **Bugs ainda abertos da lista do sweep:** gesto de undo aberto quando as linhas se refazem a meio de um arrasto; Reverse / Normalize / Cut silence no thread da UI; Align dos hats não idempotente; `swingNote` em notas pares; `setModulator` perde `source`; `sliceAudioClip` e `insertTime` com cortes < 1 ms; `scoreArrange` com secções não alinhadas; `importProjectV2` com pistas de áudio; `placeBounce` parcial; `readFolder` fora do try; nomes `.saving-N`; `applySuggestedSamples`; `bassFollow` no kick seguinte; `stretchPreset`; `humanizeNotes` entre compiladores; motion "random" em leads; duplo clique no Patterns window; estado "a tocar" falso; autosave no fecho; fecho lento; `boxClips_`; `adoptFile`; `liveFallback` (por decidir).
- **VST2:** já não é preciso (o Serum 2 original é VST3).
- **Presets do Serum por nome:** retirado pelo Diogo.

### Por juntar / por confirmar

- **Ramo `feat/ai-core`** (3 commits de 2026-11-16, não estão no `main`): chat da AI do Core (menu AI, "Aplicar" = um passo de undo), módulo `core_ai` e chamadas da música (pista, P-Core, forma, arranjo, tocar) com testes. Saiu do `main` antes do PR #3 e traz a sua própria pasta `tests/`, por isso precisa de ser atualizado e testado antes de entrar.
- **Trabalho local por enviar.** O trabalho foi parado a 2026-11-16 ~02:00. A revisão da lista de bugs médios e pequenos estava a meio no PC do Diogo e pode não ter chegado ao GitHub.
- **CI do GitHub** a falhar antes de correr (ver acima).

## Regras

- Cada passo é um commit que compila, abre sem crash e passa o self-test.
- "Se passa, mete": o que passa no CI e no "abre tudo" é juntado e instalado sem perguntar.
- A prova final é o ouvido do Diogo.

## AICORE (à parte)

App standalone em JUCE (repo `dmo-app/AICORE`). Fala com o Core só por ficheiros `.mid` e `.wav`. Áudio → MIDI com Demucs e Basic Pitch.
