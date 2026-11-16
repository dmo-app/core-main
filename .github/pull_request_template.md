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
- [ ] nenhum

Reversível? Como se tira (flag, módulo fora do processo, um commit):

## Resumo

## Definição de entregue

```
ramo / SHA:
gate: build limpa? sim/não | merge N/N | ai N/N | desktop N/N | duração:
autorizacao: python ci/autorizacao.py --ledger <ESTADO.md> → VERDE/VERMELHO
testes novos: <nomes>
prova de vermelho: <output antes> / <output depois>
contagem vs entrega anterior: N → N (nunca desce)
smoke de 8 cliques: screenshots em <pasta>
decisões tocadas: D-...
```

## Decisões pendentes do Diogo

<!-- O que este PR pressupõe e o Diogo ainda não disse por palavras. Se a lista não estiver vazia, o PR fica em
rascunho até ele responder. -->
