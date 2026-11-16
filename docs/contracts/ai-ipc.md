# Core ↔ AI: o contrato do processo da AI (Fase 4, 2026-11-16)

A AI do Core é um **plugin**: um processo próprio (`core-ai.exe`, pacote "Core AI"), nunca código dentro do `Core.exe`.
Tudo o que vem com a AI é do plugin: o modelo e as chaves, a base de conhecimento, a memória de receitas (SQLite,
Gostei / Não gostei, export JSONL para treino), os chats guardados e o Core AI Gate. O Core guarda só o que é
correção do DAW: o catálogo de ações que expõe, a verificação de cada proposta e o Undo.

## Onde vive cada coisa

| Peça | Onde |
|---|---|
| Catálogo de ações, âmbito fechado, verificação (`admitProposal`), pipes (`ai::Link`) | `app/ai-link` → `core_ai_link` (sem JUCE, sem SQLite, sem rede), linkado no Core.exe |
| Chat com o modelo, provedores, base de conhecimento, memória SQLite, chats, chaves | `app/ai` → `core_ai`, linkado **só** no `core-ai` |
| O processo | `app/ai-process` → `core-ai` (Windows: JUCE `juce_core` só para HTTPS ao modelo) |
| Pacote | target `core-ai-package` → `build/package/Core AI/` (core-ai.exe, ai/core-ai-knowledge.sqlite, vst3/Core AI Gate.vst3) |

`-DCORE_BUILD_AI=OFF` compila o Core sem nada da AI. O Core procura o `core-ai.exe` em `CORE_AI_EXE`, depois em
`%LOCALAPPDATA%\Programs\Core AI\`, depois numa pasta `Core AI` ao lado da pasta do Core. Sem ele: painel "offline",
o Core a 100%.

## O fio

O Core arranca `core-ai --data <%APPDATA%\Core\AI> --pcore <pcore/styles.json>` com pipes no stdin/stdout. Nada de
sockets, portas ou rede do lado do Core. No Windows o processo herda só os seus dois pipes e fica num job que o
termina quando o Core termina (mesmo num crash). Fecha-se o stdin → o processo sai.

Frame = 4 bytes de comprimento (little endian) + JSON UTF-8, um objeto com `"type"`.

| Limite | Valor |
|---|---|
| AI → Core, por frame | 1 MiB |
| Core → AI, por frame | 32 MiB (a música inteira) |
| Profundidade do JSON | 64 (verificada antes do parse) |
| Chamadas por proposta | 64 |
| hello | dentro de 8 s, e é a primeira mensagem |

Qualquer violação (frame vazio, maior que o limite, JSON/UTF-8 inválido, tipo desconhecido, campo com tipo ou tamanho
errado, mensagem antes do hello, hello duas vezes) → o Core regista, **para o processo** e fica offline. Bytes de
uma mensagem a meio quando o processo morre são deitados fora.

## Mensagens

Core → AI

| type | campos |
|---|---|
| `hello` | `protocol` (1), `catalog` (nomes das ações expostas), `scope` (as funções de criação) |
| `ask` | `id`, `text`, `song` (o documento, `doc::toJson`), `zone` (ou null), `settings` {model, effort 0-2, mode, maxUsd, useMemory, keepMemory} |
| `cancel` | `id` |
| `applied` | `id` (a mensagem cuja proposta o Core aplicou) |
| `feedback` | `recipeId`, `liked`, `note`? |
| `memory` | `op` (count, export, exportJsonl, import), `file`? |
| `chats` | `op` (list, open, new, deleteCurrent, delete), `file`? |
| `bye` | — |

AI → Core

| type | campos |
|---|---|
| `hello` | `protocol` (1), `models`, `notices`, `name`?, `recipes`? |
| `status` / `partial` | `id`, `text` (≤ 4 KiB) |
| `answer` | `id`, `reply`? (≤ 64 KiB), `error`?, `log`? (≤ 128 linhas), `proposal`? (objeto ou null), `cost`? {message, chat, requests} |
| `recipe` | `id`, `recipeId` |
| `result` | `op`, `ok`, `text`?, `data`? |
| `error` | `text`, `id`? |

A AI **nunca aplica** nada: não existe mensagem para isso. Um `"auto"` numa proposta é ignorado: aplicar sem Apply só
com o modo "trusted" das definições do Core.

## Âmbito (físico)

Só ações de criação musical são expostas e aceites: `pcore`, `shape`, `arrange`, `plays`, `track`, `roll`, `stutter`,
`voicestutter` (`ai_scope.hpp`). Ficam fora e nem são chamáveis: `mix`, `space`, `fx`, `gate`, `suck`, `pump`,
`buildup`, `open` e a audição kick/bass (análise de mistura, sound design, stems e treino são do AICORE). Uma função
nova do Core fica fora até uma decisão a pôr na lista.

## Verificação de uma proposta (tudo ou nada)

1. forma: objeto; `id`, `summary` texto curto; `calls` lista de 1 a 64 objetos com `fn`;
2. cada `fn` no âmbito;
3. permissões (`ai-settings.json`, lidas pelo Core): só leitura recusa tudo; ação negada recusa;
4. a zona é a que o Core mandou com o pedido, nunca a da proposta;
5. `control::validateProposal` contra a música **atual**: uma chamada que não cabe recusa a proposta inteira.

Admitida → Apply = um passo de Undo. A mesma proposta aplicada duas vezes ao mesmo estado dá o mesmo resultado.
