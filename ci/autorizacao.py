#!/usr/bin/env python3
"""Gate de autorização do Core (D48, Diogo 16/11/2026).

Todo o trabalho leva "Autorizado-por: D###", e o D### tem de existir no ledger (ESTADO.md, secção
"## Proveniência") com origem Diogo e estado aprovada. Sem isso, o gate fica vermelho.

Além disso, cada commit que toca um gatilho de arquitetura (muda o que o Core é) tem de o declarar
numa linha "Gatilhos: <lista>". Os gatilhos são os da D48: dependencia, processo, persistencia, ui,
audio, ambito, e "default" (D50: qualquer default que mude comportamento). Os cinco primeiros são
detetados no diff; "ambito" e "default" não se detetam e declaram-se à mão.

Uso:
  python ci/autorizacao.py --ledger <ESTADO.md> [--base origin/main] [--head HEAD] [--desde ISO]
  python ci/autorizacao.py --ledger <ESTADO.md> --pr-body <ficheiro>   (texto do PR)

Os commits anteriores a --desde (por defeito, a hora da regra: 2026-11-16T13:18:27Z) são listados
como "anterior à regra" e tratados pela auditoria de 16/11, não por este script.
Sai com 0 (verde) ou 1 (vermelho). Sem ledger legível = vermelho: nada se salta em silêncio.
"""
import argparse
import datetime as dt
import re
import subprocess
import sys

REGRA_DESDE = "2026-11-16T13:18:27+00:00"
GATILHOS = ("dependencia", "processo", "persistencia", "ui", "audio", "ambito", "default")

# Linhas acrescentadas que mudam o que o Core é. Pastas de testes, ci e docs não contam.
PADROES = {
    "dependencia": [
        (r"\.cmake$|CMakeLists\.txt$", r"\b(FetchContent_Declare|ExternalProject_Add|find_package|CPMAddPackage)\b"),
        (r"\.(c|cc|cpp|h|hpp)$", r"#\s*include\s*[<\"](sqlite3|curl/|httplib|winhttp|wininet|onnx|torch/|llama|boost/)"),
        (r"\.(c|cc|cpp|h|hpp)$", r"\b(WSAStartup|WinHttpOpen|InternetOpen|juce::URL|StreamingSocket|DatagramSocket|curl_easy_)\b"),
    ],
    "processo": [
        (r"\.cmake$|CMakeLists\.txt$", r"\b(add_executable|juce_add_gui_app|juce_add_console_app|juce_add_plugin)\s*\("),
        (r"\.(c|cc|cpp|h|hpp)$", r"\b(ChildProcess|CreateProcess[AW]?|ShellExecute[AW]?|popen|std::system)\b"),
    ],
    "persistencia": [
        (r"\.(c|cc|cpp|h|hpp)$", r"(sqlite|leveldb|PropertiesFile|ApplicationProperties|userApplicationDataDirectory|"
                                 r"getSpecialLocation|APPDATA|RegSetValue|RegCreateKey)"),
        (r"\.cmake$|CMakeLists\.txt$", r"sqlite"),
    ],
    "ui": [
        (r"^app/desktop/.*\.(c|cc|cpp|h|hpp)$", r"\b(DocumentWindow|DialogWindow|CallOutBox|ResizableWindow|PopupMenu)\b"),
    ],
}
FORA = re.compile(r"^(tests|ci|docs|\.github)/")


def git(*args, cwd=None):
    r = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise RuntimeError("git " + " ".join(args) + ": " + r.stderr.strip())
    return r.stdout


def ler_ledger(caminho):
    """{ 'D47': {'origem':..., 'estado':..., 'valida': bool} } a partir da secção '## Proveniência'."""
    with open(caminho, encoding="utf-8-sig") as f:
        texto = f.read()
    m = re.search(r"^##\s+Proveni[eê]ncia.*?$(.*?)(?=^##\s|\Z)", texto, re.S | re.M)
    if not m:
        raise ValueError("o ledger não tem a secção '## Proveniência'")
    decisoes = {}
    for linha in m.group(1).splitlines():
        cel = [c.strip() for c in linha.strip().strip("|").split("|")]
        if len(cel) < 3 or not re.fullmatch(r"D\d+", cel[0]):
            continue
        origem, estado = cel[1].lower(), cel[2].lower()
        decisoes[cel[0]] = {
            "origem": cel[1], "estado": cel[2],
            "valida": origem.startswith("diogo") and estado.startswith("aprovada"),
        }
    if not decisoes:
        raise ValueError("a secção '## Proveniência' não tem linhas de decisões")
    return decisoes


def refs(texto, chave):
    """Valores das linhas 'Chave: a, b' (trailers de commit ou linhas do PR)."""
    out = []
    for m in re.finditer(r"^\s*" + chave + r"\s*:\s*(.+)$", texto, re.M | re.I):
        out += [v.strip() for v in re.split(r"[,\s]+", m.group(1)) if v.strip()]
    return out


def verificar_autorizacao(texto, decisoes, onde):
    erros = []
    ds = [d.upper() for d in refs(texto, "Autorizado-por")]
    if not ds:
        return [f"{onde}: falta 'Autorizado-por: D###'"]
    for d in ds:
        if not re.fullmatch(r"D\d+", d):
            erros.append(f"{onde}: '{d}' não é um número de decisão (D###)")
        elif d not in decisoes:
            erros.append(f"{onde}: {d} não existe na Proveniência do ledger")
        elif not decisoes[d]["valida"]:
            erros.append(f"{onde}: {d} é '{decisoes[d]['origem']} / {decisoes[d]['estado']}': "
                         "só uma decisão do Diogo aprovada autoriza")
    return erros


def gatilhos_do_diff(diff):
    achados = {}
    ficheiro = None
    for linha in diff.splitlines():
        if linha.startswith("+++ "):
            ficheiro = linha[6:] if linha.startswith("+++ b/") else None
            continue
        if linha.startswith("diff --git") or ficheiro is None or FORA.match(ficheiro):
            continue
        if not linha.startswith("+") or linha.startswith("+++"):
            continue
        if ficheiro.startswith("engine/src/"):
            achados.setdefault("audio", f"{ficheiro} (motor de áudio)")
        for g, regras in PADROES.items():
            for padrao_fich, padrao_linha in regras:
                if re.search(padrao_fich, ficheiro) and re.search(padrao_linha, linha):
                    achados.setdefault(g, f"{ficheiro}: {linha[1:].strip()[:90]}")
    return achados


def ficheiros_novos(sha, cwd):
    out = git("show", "--no-renames", "--diff-filter=A", "--name-only", "--format=", sha, cwd=cwd)
    return [f for f in out.splitlines() if f.strip()]


def verificar_commits(base, head, desde, decisoes, cwd=None):
    erros, notas = [], []
    shas = git("rev-list", "--no-merges", "--reverse", f"{base}..{head}", cwd=cwd).split()
    for sha in shas:
        data = dt.datetime.fromisoformat(git("show", "-s", "--format=%aI", sha, cwd=cwd).strip())
        titulo = git("show", "-s", "--format=%s", sha, cwd=cwd).strip()
        onde = f"{sha[:7]} \"{titulo[:60]}\""
        if data < desde:
            notas.append(f"{onde}: anterior à regra (auditoria de 16/11)")
            continue
        msg = git("show", "-s", "--format=%B", sha, cwd=cwd)
        erros += verificar_autorizacao(msg, decisoes, onde)
        achados = gatilhos_do_diff(git("show", "--no-renames", "--format=", "-U0", sha, cwd=cwd))
        for f in ficheiros_novos(sha, cwd):
            if f.startswith("app/desktop/") and not FORA.match(f):
                achados.setdefault("ui", f"{f} (ficheiro novo na app)")
            if "/third_party/" in f or f.startswith("third_party/"):
                achados.setdefault("dependencia", f"{f} (código de terceiros)")
        declarados = {g.lower() for g in refs(msg, "Gatilhos")}
        desconhecidos = declarados - set(GATILHOS) - {"nenhum"}
        for g in sorted(desconhecidos):
            erros.append(f"{onde}: gatilho desconhecido '{g}' (válidos: {', '.join(GATILHOS)})")
        for g, prova in sorted(achados.items()):
            if g not in declarados:
                erros.append(f"{onde}: toca o gatilho '{g}' sem o declarar (Gatilhos: {g}) — {prova}")
            else:
                notas.append(f"{onde}: gatilho '{g}' declarado — {prova}")
    return shas, erros, notas


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--ledger", required=True, help="ESTADO.md com a secção '## Proveniência'")
    p.add_argument("--base", default="origin/main")
    p.add_argument("--head", default="HEAD")
    p.add_argument("--desde", default=REGRA_DESDE, help="só os commits a partir desta hora (ISO 8601)")
    p.add_argument("--pr-body", help="ficheiro com o texto do PR: verifica só o Autorizado-por do PR")
    p.add_argument("--repo", default=None, help="pasta do repositório (por defeito, a atual)")
    a = p.parse_args(argv)

    try:
        decisoes = ler_ledger(a.ledger)
    except (OSError, ValueError) as e:
        print(f"autorizacao: VERMELHO — ledger ilegível ({a.ledger}): {e}")
        return 1

    if a.pr_body:
        with open(a.pr_body, encoding="utf-8-sig") as f:
            erros = verificar_autorizacao(f.read(), decisoes, "PR")
        notas, shas = [], []
    else:
        try:
            shas, erros, notas = verificar_commits(a.base, a.head, dt.datetime.fromisoformat(a.desde), decisoes, a.repo)
        except RuntimeError as e:
            print(f"autorizacao: VERMELHO — {e}")
            return 1

    for n in notas:
        print("  nota:", n)
    for e in erros:
        print("  ERRO:", e)
    alvo = "PR" if a.pr_body else f"{len(shas)} commit(s) {a.base}..{a.head}"
    print(f"autorizacao: {'VERMELHO' if erros else 'VERDE'} — {alvo}, {len(erros)} erro(s), "
          f"ledger {a.ledger} ({len(decisoes)} decisões, {sum(d['valida'] for d in decisoes.values())} válidas)")
    return 1 if erros else 0


if __name__ == "__main__":
    sys.exit(main())
