# templates/hitech-plugins.core-template.json

Feito de `hitech-ref` com `doc::keepOnlyMidi` e os plugins da lista do Diogo, identificados pelo scan do Core no PC
dele (`core-engine scan` / `params`, 16/11/2026): `scan-hitech-pc.json` (o sfizz já com o estado do kit FreePats GM,
de `sfizz_state.py`).

    g++ -std=c++20 -I app/domain/src -I engine/third_party tools/template-gen/hitech_plugins.cpp \
        <build>/app/domain/libcore_domain.a <build>/engine/libcore_engine_lib.a -o hitech_plugins
    ./hitech_plugins templates tools/template-gen/scan-hitech-pc.json <pasta de saída>

O teste `template_midi_tests` confere o resultado (MIDI, plugins, buses, master, guardar/reabrir).
