## Autorização

Autorizado-por: D###

<!-- Obrigatório (D48). O D### tem de estar no ledger (ESTADO.md, secção "Proveniência") com origem Diogo e
estado aprovada, com a citação exata da mensagem dele. Sem número, o PR não se faz: pergunta-se ao Diogo primeiro.
Silêncio não é aprovação. Cada commit leva também a linha "Autorizado-por: D###". -->

## Gatilhos de arquitetura

Muda o que o Core é → decisão do Diogo. Muda como se faz dentro disso → implementação.
Marca cada gatilho que este PR toca e põe a mesma lista na linha `Gatilhos:` dos commits que o tocam.

- [ ] dependencia: nova biblioteca, código de terceiros ou rede
- [ ] processo: novo executável, plugin ou processo filho
- [ ] persistencia: nova forma de guardar dados (base de dados, ficheiros em AppData, registo)
- [ ] ui: nova janela, painel ou menu
- [ ] audio: qualquer coisa no caminho de áudio (motor, callback, render)
- [ ] ambito: qualquer coisa fora da lista de âmbito do Core
- [ ] default: qualquer default que mude comportamento (D50)
- [ ] nenhum

Reversível? Como se tira (flag, módulo fora do processo, um commit):

## Resumo

## Definição de entregue (D55, D56)

```
ramo / SHA:
autorizacao: python ci/autorizacao.py --ledger <ESTADO.md> → VERDE/VERMELHO
gate do Core: build limpa? sim/não | merge N/N | desktop N/N | duração:
gate do pacote da AI: ai N/N | duração:            (ou "não se aplica")
contagem por âmbito: Core N → N | AI N → N   (nunca desce dentro do âmbito; mudar de âmbito exige decisão)
testes novos: <nomes>
prova de vermelho: <output antes> / <output depois>
smoke de 8 cliques: screenshots em <pasta>
decisões tocadas: D-...
```

## Medido / não medido

| Item | Linux | Windows | Não testado |
|---|---|---|---|
|  |  |  |  |

<!-- Um teste só em Linux não valida Windows. Declara sempre o que NÃO foi testado. Nada de números inventados. -->

## Defaults alterados

<!-- Cada default que muda comportamento é um gatilho (D50): diz qual, o antes e o depois, e a decisão que o autoriza. Se não há, escreve "nenhum". -->

## Decisões pendentes do Diogo

<!-- O que este PR pressupõe e o Diogo ainda não disse por palavras. Se a lista não estiver vazia, o PR fica em
rascunho até ele responder. -->
