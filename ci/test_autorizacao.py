#!/usr/bin/env python3
"""Testes do gate de autorização (ci/autorizacao.py). Correm sem rede, num repositório git temporário.
  python ci/test_autorizacao.py
Cada caso tem de dar o resultado esperado; um caso que não corre conta como falha."""
import os
import subprocess
import sys
import tempfile

AQUI = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(AQUI, "autorizacao.py")

LEDGER = """# ESTADO
## Decisões
| D10 | antiga |
## Proveniência
| # | Origem | Estado | Citação | Corrige / invalida |
|---|---|---|---|---|
| D10 | Diogo | aprovada | "x" | |
| D11 | proposta da equipa | proposta | sem citação | |
| D12 | Diogo | invalidada por D13 | "y" | |
## Outra secção
| D99 | Diogo | aprovada | fora da secção | |
"""

ANTES = "2026-11-01T10:00:00+00:00"
DEPOIS = "2026-11-17T10:00:00+00:00"


def sh(cwd, *args, env=None):
    r = subprocess.run(args, cwd=cwd, capture_output=True, text=True, env=env)
    if r.returncode != 0:
        raise RuntimeError(" ".join(args) + "\n" + r.stdout + r.stderr)
    return r.stdout


def repo(tmp):
    sh(tmp, "git", "init", "-q", "-b", "main")
    sh(tmp, "git", "config", "user.email", "t@t")
    sh(tmp, "git", "config", "user.name", "t")
    escrever(tmp, "README.md", "x\n")
    commit(tmp, "base", ANTES)
    return sh(tmp, "git", "rev-parse", "HEAD").strip()


def escrever(tmp, nome, texto):
    p = os.path.join(tmp, nome)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        f.write(texto)


def commit(tmp, msg, data):
    env = dict(os.environ, GIT_AUTHOR_DATE=data, GIT_COMMITTER_DATE=data)
    sh(tmp, "git", "add", "-A", env=env)
    sh(tmp, "git", "commit", "-q", "-m", msg, env=env)


def correr(tmp, base, *extra):
    ledger = os.path.join(tmp, "..", os.path.basename(tmp) + "-ESTADO.md")
    with open(ledger, "w", encoding="utf-8") as f:
        f.write(LEDGER)
    r = subprocess.run([sys.executable, SCRIPT, "--ledger", ledger, "--base", base, "--head", "HEAD",
                        "--repo", tmp, *extra], capture_output=True, text=True)
    return r.returncode, r.stdout


CASOS = []


def caso(f):
    CASOS.append(f)
    return f


@caso
def sem_trailer_vermelho(tmp, base):
    escrever(tmp, "app/x.txt", "a\n")
    commit(tmp, "muda x", DEPOIS)
    rc, out = correr(tmp, base)
    return rc == 1 and "falta 'Autorizado-por" in out


@caso
def decisao_aprovada_verde(tmp, base):
    escrever(tmp, "app/x.txt", "a\n")
    commit(tmp, "muda x\n\nAutorizado-por: D10", DEPOIS)
    rc, out = correr(tmp, base)
    return rc == 0 and "VERDE" in out


@caso
def proposta_da_equipa_vermelho(tmp, base):
    escrever(tmp, "app/x.txt", "a\n")
    commit(tmp, "muda x\n\nAutorizado-por: D11", DEPOIS)
    rc, out = correr(tmp, base)
    return rc == 1 and "só uma decisão do Diogo aprovada" in out


@caso
def decisao_invalidada_vermelho(tmp, base):
    escrever(tmp, "app/x.txt", "a\n")
    commit(tmp, "muda x\n\nAutorizado-por: D12", DEPOIS)
    rc, out = correr(tmp, base)
    return rc == 1 and "D12" in out


@caso
def decisao_inexistente_ou_fora_da_secao_vermelho(tmp, base):
    escrever(tmp, "app/x.txt", "a\n")
    commit(tmp, "muda x\n\nAutorizado-por: D99", DEPOIS)
    rc, out = correr(tmp, base)
    return rc == 1 and "D99 não existe" in out


@caso
def anterior_a_regra_so_nota(tmp, base):
    escrever(tmp, "app/x.txt", "a\n")
    commit(tmp, "antigo", ANTES)
    rc, out = correr(tmp, base)
    return rc == 0 and "anterior à regra" in out


@caso
def sqlite_sem_gatilho_vermelho(tmp, base):
    escrever(tmp, "app/ai/mem.cpp", '#include <sqlite3.h>\nint f(){ return sqlite3_open(0,0); }\n')
    commit(tmp, "memoria\n\nAutorizado-por: D10", DEPOIS)
    rc, out = correr(tmp, base)
    return rc == 1 and "'dependencia'" in out and "'persistencia'" in out


@caso
def sqlite_com_gatilhos_verde(tmp, base):
    escrever(tmp, "app/ai/mem.cpp", '#include <sqlite3.h>\nint f(){ return sqlite3_open(0,0); }\n')
    commit(tmp, "memoria\n\nAutorizado-por: D10\nGatilhos: dependencia, persistencia", DEPOIS)
    rc, out = correr(tmp, base)
    return rc == 0


@caso
def processo_novo_vermelho(tmp, base):
    escrever(tmp, "app/ai/CMakeLists.txt", "add_executable(core-ai main.cpp)\n")
    commit(tmp, "processo\n\nAutorizado-por: D10", DEPOIS)
    rc, out = correr(tmp, base)
    return rc == 1 and "'processo'" in out


@caso
def motor_de_audio_vermelho(tmp, base):
    escrever(tmp, "engine/src/core/mix.cpp", "int x = 1;\n")
    commit(tmp, "mix\n\nAutorizado-por: D10", DEPOIS)
    rc, out = correr(tmp, base)
    return rc == 1 and "'audio'" in out


@caso
def ficheiro_novo_na_app_e_ui(tmp, base):
    escrever(tmp, "app/desktop/src/AiPanel.cpp", "int painel = 0;\n")
    commit(tmp, "painel\n\nAutorizado-por: D10", DEPOIS)
    rc, out = correr(tmp, base)
    return rc == 1 and "'ui'" in out


@caso
def rede_e_dependencia(tmp, base):
    escrever(tmp, "app/control/net.cpp", "void f(){ juce::URL u(\"http://x\"); }\n")
    commit(tmp, "rede\n\nAutorizado-por: D10", DEPOIS)
    rc, out = correr(tmp, base)
    return rc == 1 and "'dependencia'" in out


@caso
def testes_nao_contam_como_gatilho(tmp, base):
    escrever(tmp, "tests/t.cpp", '#include <sqlite3.h>\nint main(){ std::system("x"); }\n')
    commit(tmp, "teste\n\nAutorizado-por: D10", DEPOIS)
    rc, out = correr(tmp, base)
    return rc == 0


@caso
def gatilho_desconhecido_vermelho(tmp, base):
    escrever(tmp, "app/x.txt", "a\n")
    commit(tmp, "x\n\nAutorizado-por: D10\nGatilhos: magia", DEPOIS)
    rc, out = correr(tmp, base)
    return rc == 1 and "desconhecido 'magia'" in out


@caso
def gatilho_default_aceite(tmp, base):
    escrever(tmp, "app/x.txt", "a\n")
    commit(tmp, "x\n\nAutorizado-por: D10\nGatilhos: default", DEPOIS)
    rc, out = correr(tmp, base)
    return rc == 0


@caso
def corpo_do_pr(tmp, base):
    ledger = os.path.join(tmp, "E.md")
    with open(ledger, "w", encoding="utf-8") as f:
        f.write(LEDGER)
    sem = os.path.join(tmp, "sem.txt")
    com = os.path.join(tmp, "com.txt")
    with open(sem, "w", encoding="utf-8") as f:
        f.write("## Resumo\nfaz coisas\n")
    with open(com, "w", encoding="utf-8") as f:
        f.write("## Autorização\nAutorizado-por: D10\n")
    r1 = subprocess.run([sys.executable, SCRIPT, "--ledger", ledger, "--pr-body", sem], capture_output=True, text=True)
    r2 = subprocess.run([sys.executable, SCRIPT, "--ledger", ledger, "--pr-body", com], capture_output=True, text=True)
    return r1.returncode == 1 and r2.returncode == 0


@caso
def ledger_ilegivel_vermelho(tmp, base):
    r = subprocess.run([sys.executable, SCRIPT, "--ledger", os.path.join(tmp, "nao-existe.md"),
                        "--repo", tmp, "--base", base], capture_output=True, text=True)
    return r.returncode == 1 and "ledger ilegível" in r.stdout


def main():
    falhas = 0
    for f in CASOS:
        with tempfile.TemporaryDirectory() as tmp:
            try:
                ok = f(tmp, repo(tmp))
            except Exception as e:  # um caso que não corre é uma falha
                ok = False
                print(f"  {f.__name__}: exceção {e}")
        print(f"{'ok   ' if ok else 'FALHA'} {f.__name__}")
        falhas += not ok
    print(f"test_autorizacao: {len(CASOS) - falhas}/{len(CASOS)} casos")
    return 1 if falhas else 0


if __name__ == "__main__":
    sys.exit(main())
