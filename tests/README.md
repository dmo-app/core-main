# Testes do Core

Desde 2026-11-16. Substituem as 11 suites antigas (removidas a 2026-11-15) com outras regras:

- **Sinais gerados** (seno, impulso, ruído com semente fixa) em `support/signals.hpp`. Nenhum WAV de teste no repo.
- **Sem goldens bit-a-bit presos a um compilador.** O determinismo testa-se dentro da mesma build (duas vezes igual,
  qualquer tamanho de bloco igual), não contra um hash guardado.
- **Um teste que não pode correr falha.** Nada é saltado em silêncio; cada falha imprime o valor medido e o esperado.
- **Um bug corrigido traz o seu teste**, que falha sem a correção.

## Regra de ferro (plano de testes, 2026-11-16)

1. **Prova de vermelho.** Cada correção ou feature entrega o output do teste novo a falhar no commit anterior e a
   passar no novo.
2. **Nada se apaga nem se enfraquece.** Mudar assertions, `support/Check.hpp`, timeouts ou desativar testes exige uma
   decisão numerada no `ESTADO.md`.
3. **Sem saltos silenciosos.** Um teste que não pode correr falha (plugins e modelo incluídos).
4. **A contagem nunca desce.** `ci/testes.txt` é o inventário: o gate falha se um teste da lista não existe, se tem
   outras labels, se o ctest tem um teste fora da lista, ou se um teste da `main` desapareceu.
5. **Flaky é bug.** Proibido subir timeouts para ficar verde.

## Testes e labels

Cada teste tem um nível (`merge` ou `desktop`) e, quando se aplica, uma área (`ai`, `ui`, `plugins`).

| Teste | Labels | O que cobre |
|---|---|---|
| `engine_tests` | merge | WAV, tempo, JSON do projeto, render (blocos, determinismo, ficheiros em falta), loop |
| `domain_tests` | merge | documento, templates, guardar/reabrir, undo/redo, "modificado" |
| `ai_tests` | merge, ai | a AI dentro do Core: chamadas da música, chat (respostas fixas, sem rede), base de conhecimento, provedores |
| `ai_contract_tests` | merge, ai | rede de segurança da AI (T05–T08): modelo falso sem rede, um contrato por ação do catálogo (meta-teste), undo único exato, âmbito de cada ação; cada verificador com um caso negativo |
| `aifx_tests` | merge, ai | Core AI Gate: som, papel, toolGate e a chamada "gate" da AI |
| `role_tests` | merge, ai | papéis, macros, SUCK, PUM, build-up, pistas mudas, bus partilhado, F-Core, ROLL, catálogo das tools |
| `midi_tests` | merge | MIDI exportar e importar |
| `bitwig_tests` | merge | plano do Export for Bitwig, nomes de ficheiro |
| `bug_tests` | merge | bugs do sweep revisto |
| `audit_tests` | merge | auditoria externa 1 (B01–B06) |
| `audit2_tests` | merge | auditoria externa 2 |
| `hunt_tests` | merge | caça profunda (P01–P08, IPC, traduções) |
| `round3_tests` | merge, ai | ronda 3 (R3-A–R3-G): tools e cartões |
| `cli_version`, `cli_demo_render`, `cli_measure`, `cli_unicode` | merge | a linha de comandos do `core-engine` |
| `desktop_self_test` | desktop, ui | `Core.exe --self-test` no null device (abre a janela ~20 s); só onde a app é compilada |

`plugins` fica vazio até ao T19 (Vital e sfizz reais, à mão, fora do gate). As labels do ctest são expressões
regulares: para uma só, `-L "^ai$"`.

## Correr

```
ctest --test-dir <build> -L merge --output-on-failure      # rápido (~7 s)
ctest --test-dir <build> -L "^ai$" -N                      # lista os testes da AI
ctest --test-dir <build> --output-on-failure               # tudo
<build>/tests/engine_tests render                          # só os casos com "render" no nome
```

Gate de um comando, com resumo em `<build>/gate-resumo.txt` (`ci/resumo.cmake`):

```
powershell -ExecutionPolicy Bypass -File ci\gate.ps1 -Clean -Desktop    # Windows, o de uma entrega
CLEAN=1 ci/gate.sh <build>                                              # Linux/WSL, sem a app
```

O `gate.ps1` usa sempre um JUCE 9.0.5 local e não vai à rede (`-Online` para descarregar). CI no GitHub:
`.github/workflows/ci.yml`, o mesmo resumo.

## Onde pôr um teste novo

- Motor (projecto, render, WAV, tempo): `engine_tests.cpp`.
- Documento da app (guardar, undo, templates, projecção): `domain_tests.cpp`.
- Linha de comandos do `core-engine`: um `cli_*.cmake` e um `add_test` em `CMakeLists.txt`.
- Uma ação nova da AI: o seu contrato em `ai_contract_tests.cpp` (chamada, âmbito); sem ele o meta-teste fica vermelho.
- Sempre: uma linha em `ci/testes.txt`, no mesmo commit.
