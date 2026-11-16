"""Extracts the music knowledge of CORE-MANUAL into ai/knowledge/core-manual.jsonl (Diogo, 2026-11-16).

The AI inside Core consults it through a local database (core-ai-knowledge.sqlite, built from this file by
core-ai-knowledge at build time and shipped with the AI package). One line per chunk: a section of a page, cut at its
headings, at most ~350 words: {"id", "source", "title", "style", "text"}.

    python tools/ai-knowledge/extract_manual.py <core-manual checkout> ai/knowledge/core-manual.jsonl
"""
import json
import re
import subprocess
import sys
from pathlib import Path

PAGES = ["styles/*.md", "knowledge/*.md", "guide/0[1-5]-*.md", "guide/08-*.md", "guide/09-*.md", "guide/10-*.md", "guide/14-*.md"]
STYLES = {"darkpsycore": ["dark psycore", "darkpsycore", "psycore"], "hitech": ["hi-tech", "hitech"], "slambient": ["slambient"],
          "psytrance": ["full-on", "fullon", "psytrance", "progressive"], "darkpsy": ["dark psy ", "darkpsy", "forest"]}
MAX_WORDS = 350


def clean(text):
    text = re.sub(r"!\[[^\]]*\]\([^)]*\)", "", text)              # images
    text = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", text)          # links: keep the words
    text = re.sub(r"<[^>]+>", "", text)
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text.strip()


def style_of(path, title, text):
    name = Path(path).stem
    if path.startswith("styles/") and name in STYLES:
        return name
    head = (title + " " + text[:400]).lower()
    found = [s for s, words in STYLES.items() if any(w in head for w in words)]
    return found[0] if len(found) == 1 else ""


def chunks(path, body):
    title = path
    m = re.search(r"^# (.+)$", body, re.M)
    if m:
        title = m.group(1).strip()
    parts = re.split(r"^(#{2,3} .+)$", body, flags=re.M)
    sections = [("", parts[0])] + [(parts[i].lstrip("#").strip(), parts[i + 1]) for i in range(1, len(parts) - 1, 2)]
    for heading, text in sections:
        text = clean(text)
        if len(text.split()) < 12:
            continue
        words = text.split(" ")
        pieces, cur = [], []
        for para in text.split("\n\n"):
            if cur and len(" ".join(cur).split()) + len(para.split()) > MAX_WORDS:
                pieces.append("\n\n".join(cur))
                cur = []
            cur.append(para)
        if cur:
            pieces.append("\n\n".join(cur))
        for n, piece in enumerate(pieces):
            t = title + (" · " + heading if heading else "") + (f" ({n + 1})" if len(pieces) > 1 else "")
            yield t, piece


def main(manual, out):
    manual = Path(manual)
    commit = subprocess.run(["git", "-C", str(manual), "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    rows = []
    for pattern in PAGES:
        for f in sorted(manual.glob(pattern)):
            rel = f.relative_to(manual).as_posix()
            if rel.endswith("README.md") and not rel.startswith("styles/"):
                continue
            for title, text in chunks(rel, f.read_text(encoding="utf-8")):
                rows.append({"id": f"{rel}#{len(rows)}", "source": rel, "title": title, "style": style_of(rel, title, text), "text": text})
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", encoding="utf-8", newline="\n") as w:
        w.write(json.dumps({"format": "core-ai-knowledge", "version": 1, "from": "dmo-app/core-manual@" + commit, "chunks": len(rows)}, ensure_ascii=False) + "\n")
        for r in rows:
            w.write(json.dumps(r, ensure_ascii=False) + "\n")
    print(f"{len(rows)} chunks from core-manual@{commit} -> {out}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
