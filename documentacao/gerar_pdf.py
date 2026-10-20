#!/usr/bin/env python3
"""Gera os guias PDF usando somente a biblioteca padrão do Python."""

from __future__ import annotations

import re
import textwrap
import unicodedata
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DOCS = Path(__file__).resolve().parent
PAGE_W = 595
PAGE_H = 842
LEFT = 52
RIGHT = 543
BOTTOM = 55
TOP = 770


def pdf_text(value: str) -> str:
    replacements = {
        "→": "->", "←": "<-", "►": ">", "◄": "<", "▼": "v",
        "│": "|", "─": "-", "┌": "+", "┐": "+", "└": "+", "┘": "+",
        "├": "+", "┤": "+", "┬": "+", "┴": "+", "┼": "+", "•": "-",
        "×": "x", "≈": "~", "≥": ">=", "≤": "<=", "≠": "!=",
    }
    value = "".join(replacements.get(char, char) for char in value)
    normalized = []
    for char in value:
        try:
            char.encode("cp1252")
            normalized.append(char)
        except UnicodeEncodeError:
            normalized.append(unicodedata.normalize("NFKD", char).encode("ascii", "ignore").decode("ascii"))
    data = "".join(normalized).encode("cp1252", "replace")
    escaped = []
    for byte in data:
        if byte in (40, 41, 92):
            escaped.append("\\" + chr(byte))
        elif byte < 32 or byte > 126:
            escaped.append(f"\\{byte:03o}")
        else:
            escaped.append(chr(byte))
    return "".join(escaped)


def clean_inline(value: str) -> str:
    value = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", value)
    value = value.replace("**", "").replace("__", "").replace("`", "")
    value = value.replace("*", "")
    return value.strip()


def wrap(value: str, width: int, preserve: bool = False) -> list[str]:
    if not value:
        return [""]
    if preserve:
        indent = len(value) - len(value.lstrip(" "))
        available = max(12, width - indent)
        parts = textwrap.wrap(value[indent:], width=available, replace_whitespace=False, drop_whitespace=False, break_long_words=True, break_on_hyphens=False)
        return [" " * indent + part for part in parts] or [""]
    return textwrap.wrap(value, width=width, replace_whitespace=True, drop_whitespace=True, break_long_words=True, break_on_hyphens=False) or [""]


class PDFDocument:
    def __init__(self, title: str):
        self.title = title
        self.pages: list[list[str]] = []
        self.y = TOP
        self.cover = True
        self._new_page()

    def _new_page(self) -> None:
        self.pages.append([])
        self.y = TOP
        if len(self.pages) > 1:
            self.cover = False
            self._line(LEFT, 779, RIGHT, 779, (0.78, 0.83, 0.88), 0.7)
            self._text(LEFT, 790, "GUIA TÉCNICO | PROGRAMAÇÃO DISTRIBUÍDA", 7.5, "F2", (0.20, 0.31, 0.42))
            self.y = 756

    def _emit(self, command: str) -> None:
        self.pages[-1].append(command)

    def _text(self, x: float, y: float, value: str, size: float, font: str = "F1", color: tuple[float, float, float] = (0.15, 0.18, 0.22)) -> None:
        self._emit(f"{color[0]:.3f} {color[1]:.3f} {color[2]:.3f} rg BT /{font} {size:.1f} Tf 1 0 0 1 {x:.1f} {y:.1f} Tm ({pdf_text(value)}) Tj ET")

    def _line(self, x1: float, y1: float, x2: float, y2: float, color: tuple[float, float, float], width: float = 1.0) -> None:
        self._emit(f"{color[0]:.3f} {color[1]:.3f} {color[2]:.3f} RG {width:.1f} w {x1:.1f} {y1:.1f} m {x2:.1f} {y2:.1f} l S")

    def _rect(self, x: float, y: float, w: float, h: float, color: tuple[float, float, float]) -> None:
        self._emit(f"{color[0]:.3f} {color[1]:.3f} {color[2]:.3f} rg {x:.1f} {y:.1f} {w:.1f} {h:.1f} re f")

    def cover_title(self, title: str, subtitle: str) -> None:
        self._rect(0, 0, PAGE_W, PAGE_H, (1.0, 1.0, 1.0))
        self._rect(0, 0, 16, PAGE_H, (0.10, 0.28, 0.44))
        self._text(LEFT, 790, "TRABALHO DE PROGRAMACAO DISTRIBUIDA", 8.5, "F2", (0.23, 0.36, 0.49))
        self._line(LEFT, 770, RIGHT, 770, (0.10, 0.42, 0.62), 2.0)
        title_lines = wrap(clean_inline(title), 34)
        y = 672
        for line in title_lines:
            self._text(LEFT, y, line, 25, "F2", (0.08, 0.20, 0.32))
            y -= 33
        self._line(LEFT, y - 16, LEFT + 78, y - 16, (0.08, 0.49, 0.68), 4.0)
        for line in wrap(clean_inline(subtitle), 72):
            y -= 45
            self._text(LEFT, y, line, 12, "F1", (0.23, 0.31, 0.39))
        self._rect(LEFT, 184, RIGHT - LEFT, 75, (0.94, 0.96, 0.98))
        self._text(LEFT + 16, 235, "GUIA EXPLICATIVO", 9, "F2", (0.08, 0.37, 0.55))
        self._text(LEFT + 16, 216, "Fluxos, módulos, funções, decisões, evidências e pendências", 9, "F1", (0.20, 0.26, 0.32))
        self._text(LEFT + 16, 198, "Leitura baseada no código e na documentação do repositório", 8, "F1", (0.35, 0.40, 0.45))
        self._text(LEFT, 72, "Revisão: 20/10/2026", 9, "F2", (0.25, 0.32, 0.40))
        self._text(LEFT, 52, "Uso didático: implementação atual separada de requisitos e trabalho futuro", 8, "F1", (0.35, 0.40, 0.45))

    def ensure(self, height: float) -> None:
        if self.y - height < BOTTOM:
            self._new_page()

    def heading(self, text: str, level: int) -> None:
        size = {2: 16, 3: 12, 4: 10.5}.get(level, 10.5)
        gap = {2: 20, 3: 14, 4: 10}.get(level, 10)
        self.ensure(size + 24)
        self.y -= gap
        color = (0.08, 0.24, 0.37) if level == 2 else (0.10, 0.39, 0.55)
        for line in wrap(clean_inline(text), 70 if level == 2 else 82):
            self._text(LEFT, self.y, line, size, "F2", color)
            self.y -= size + 4
        self.y -= 3
        if level == 2:
            self._line(LEFT, self.y + 1, LEFT + 36, self.y + 1, (0.12, 0.52, 0.69), 1.5)
            self.y -= 5

    def paragraph(self, text: str, bullet: bool = False, quote: bool = False) -> None:
        value = clean_inline(text)
        if bullet:
            prefix = "- "
            lines = wrap(value, 88)
            lines[0] = prefix + lines[0]
            indent = LEFT + 13
        else:
            lines = wrap(value, 100)
            indent = LEFT + (12 if quote else 0)
        line_height = 13.3
        extra = 5
        self.ensure(len(lines) * line_height + extra)
        if quote:
            self._rect(LEFT, self.y - len(lines) * line_height + 5, 3, len(lines) * line_height + 2, (0.12, 0.52, 0.69))
        for line in lines:
            self._text(indent, self.y, line, 9.3, "F1", (0.16, 0.19, 0.23))
            self.y -= line_height
        self.y -= extra

    def code_block(self, lines: list[str]) -> None:
        wrapped = []
        for line in lines:
            wrapped.extend(wrap(line.expandtabs(4), 101, preserve=True))
        if not wrapped:
            return
        index = 0
        while index < len(wrapped):
            available = max(1, int((self.y - BOTTOM - 14) / 10.2))
            batch = wrapped[index:index + available]
            height = len(batch) * 10.2 + 14
            self.ensure(height)
            bottom = self.y - height + 5
            self._rect(LEFT, bottom, RIGHT - LEFT, height, (0.95, 0.96, 0.97))
            for line in batch:
                self._text(LEFT + 9, self.y - 9, line, 7.2, "F3", (0.17, 0.22, 0.27))
                self.y -= 10.2
            self.y -= 9
            index += len(batch)
            if index < len(wrapped):
                self._new_page()

    def finish(self) -> bytes:
        total = len(self.pages)
        for number, page in enumerate(self.pages, 1):
            if number > 1:
                self._line(LEFT, 43, RIGHT, 43, (0.82, 0.85, 0.88), 0.5)
                self._text(LEFT, 29, self.title, 7.3, "F1", (0.38, 0.43, 0.48))
                self._text(RIGHT - 60, 29, f"Pagina {number} de {total}", 7.3, "F1", (0.38, 0.43, 0.48))
        objects: list[bytes] = []

        def add_object(data: bytes) -> int:
            objects.append(data)
            return len(objects)

        catalog_id = add_object(b"")
        pages_id = add_object(b"")
        regular_id = add_object(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>")
        bold_id = add_object(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>")
        mono_id = add_object(b"<< /Type /Font /Subtype /Type1 /BaseFont /Courier /Encoding /WinAnsiEncoding >>")
        page_ids = []
        for page in self.pages:
            stream = ("\n".join(page) + "\n").encode("ascii")
            stream_id = add_object(f"<< /Length {len(stream)} >>\nstream\n".encode("ascii") + stream + b"endstream")
            page_id = add_object(
                f"<< /Type /Page /Parent {pages_id} 0 R /MediaBox [0 0 {PAGE_W} {PAGE_H}] /Resources << /Font << /F1 {regular_id} 0 R /F2 {bold_id} 0 R /F3 {mono_id} 0 R >> >> /Contents {stream_id} 0 R >>".encode("ascii")
            )
            page_ids.append(page_id)
        objects[catalog_id - 1] = f"<< /Type /Catalog /Pages {pages_id} 0 R >>".encode("ascii")
        kids = " ".join(f"{page_id} 0 R" for page_id in page_ids)
        objects[pages_id - 1] = f"<< /Type /Pages /Kids [{kids}] /Count {len(page_ids)} >>".encode("ascii")

        output = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
        offsets = [0]
        for number, obj in enumerate(objects, 1):
            offsets.append(len(output))
            output.extend(f"{number} 0 obj\n".encode("ascii"))
            output.extend(obj)
            output.extend(b"\nendobj\n")
        xref = len(output)
        output.extend(f"xref\n0 {len(objects) + 1}\n0000000000 65535 f \n".encode("ascii"))
        for offset in offsets[1:]:
            output.extend(f"{offset:010d} 00000 n \n".encode("ascii"))
        output.extend(f"trailer\n<< /Size {len(objects) + 1} /Root {catalog_id} 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode("ascii"))
        return bytes(output)


def convert(source: Path, destination: Path) -> int:
    lines = source.read_text(encoding="utf-8").splitlines()
    title = next((line[2:].strip() for line in lines if line.startswith("# ")), source.stem)
    subtitle = ""
    title_index = lines.index(f"# {title}") if f"# {title}" in lines else -1
    for line in lines[title_index + 1:]:
        if line.strip():
            subtitle = line.strip()
            break
    doc = PDFDocument(title)
    doc.cover_title(title, subtitle)
    in_code = False
    code_lines: list[str] = []
    started = False
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("# "):
            continue
        if stripped.startswith("## ") and not started:
            doc._new_page()
            started = True
        if stripped.startswith("```"):
            if in_code:
                doc.code_block(code_lines)
                code_lines = []
                in_code = False
            else:
                in_code = True
            continue
        if in_code:
            code_lines.append(line)
            continue
        if not stripped:
            continue
        if stripped.startswith("### "):
            doc.heading(stripped[4:], 3)
        elif stripped.startswith("#### "):
            doc.heading(stripped[5:], 4)
        elif stripped.startswith("## "):
            doc.heading(stripped[3:], 2)
        elif stripped.startswith("> "):
            doc.paragraph(stripped[2:], quote=True)
        elif stripped.startswith("- ") or re.match(r"\d+\.\s", stripped):
            bullet_text = re.sub(r"^\d+\.\s+", "", stripped) if re.match(r"\d+\.\s", stripped) else stripped[2:]
            doc.paragraph(bullet_text, bullet=True)
        else:
            doc.paragraph(stripped)
    if in_code:
        doc.code_block(code_lines)
    destination.write_bytes(doc.finish())
    return len(doc.pages)


def main() -> None:
    documents = [
        (DOCS / "guia_aluno_1.md", DOCS / "guia_aluno_1.pdf"),
        (DOCS / "guia_aluno_2.md", DOCS / "guia_aluno_2.pdf"),
    ]
    for source, destination in documents:
        pages = convert(source, destination)
        print(f"{destination.relative_to(ROOT)}: {pages} paginas")


if __name__ == "__main__":
    main()
