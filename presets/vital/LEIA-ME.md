# Presets Vital da AI do Core

Biblioteca inicial de 14 presets Vital feitos de raiz para os estilos do Diogo: dark psycore (DPC), hitech (HT) e
slambient (SLA). Seguem as fichas de estilo do core-manual: gate dos baixos, nada de lead nem de growl no slambient,
clique e FM no hitech. Não usam os presets de fábrica do Vital nem os sons W-Core antigos.

| Preset | Estilo | Papel | Som |
|---|---|---|---|
| DPC KBBB Plonk | darkpsycore | bass | baixo rolante seco, nota até 40 ms, plonk de pitch |
| DPC Growl Talk | darkpsycore | growl | camada com vogais para a 3.ª nota do KBBB, HP 150 Hz |
| DPC Acid Squelch | darkpsycore | acid | acid ressonante, slide nas notas ligadas, acento na velocity |
| DPC Abyss Drone | darkpsycore | drone | drone escuro para os breaks |
| DPC Metal Hit | darkpsycore | metal | acerto metálico curto (FM + comb) |
| HT Snap Bass | hitech | bass | baixo com clique e oitavas, nota 40 a 50 ms |
| HT Laser Zap | hitech | fx | zap com queda de pitch, delay ping-pong |
| HT FM Answer | hitech | lead | resposta FM uma oitava acima, com delay |
| HT Glass Bell | hitech | bell | sino FM de vidro |
| HT Static Riser | hitech | riser | ruído e serra a abrir em 8 compassos a 190 BPM |
| SLA Deep Sub | slambient | bass | sub macio, sem clique, raiz B0 a F#1 |
| SLA Abyss Drone | slambient | drone | drone uma oitava acima, LP 1200 Hz, reverb 0,7 |
| SLA Ghost Bed | slambient | texture | cama granulada larga, delay 3/16, reverb grande |
| SLA Low Boom | slambient | impact | boom 90 para 30 Hz, 900 ms |

## Os 4 macros (iguais em todos)

| Macro | O que faz |
|---|---|
| TOM | filtro / brilho |
| SUJO | drive / distorção |
| MOVIMENTO | LFO, profundidade do envelope ou velocidade |
| COMPRIMENTO | decay / release |

Começam todos a 0,5, que é o som desenhado: 0 tira, 1 põe mais. `presets.json` diz, por preset, em que mexe cada macro.
O volume de todos foi acertado para o mesmo pico (-6 dBFS numa nota de referência).

## Como a AI usa

`presets.json` é o índice: `id`, `name`, `file`, `style`, `role`, `bpm`, `tags`, `description`, `macros`, `origin`.
A AI escolhe pelo nome (por estilo e papel) e só mexe nos 4 macros, o que mantém o custo baixo. Um `.vital` é o
JSON do preset do Vital; para o carregar numa instância VST3 do Vital o Core passa o conteúdo do ficheiro ao estado
do plugin.

Variantes do Diogo: um preset alterado guarda-se com o nome `<preset> - <nome>` (ex.: `DPC KBBB Plonk - Diogo 1`)
na pasta de utilizador, com `origin: "user"` e as mesmas etiquetas, e entra no índice ao lado dos originais.

## Refazer e testar

As ferramentas estão em `tools/vital-presets/` e usam o Vita (bindings Python do Vital, GPL-3) **só como ferramenta**;
nada disto é ligado ao Core.

```
pip install vita numpy soundfile
python tools/vital-presets/build_presets.py presets/vital
python tools/vital-presets/test_presets.py presets/vital        # carrega, modulações, 4 macros mexem, gate dos baixos
python tools/vital-presets/render_previews.py presets/vital <pasta>   # MP3 curtos para ouvir (precisa de ffmpeg)
```
