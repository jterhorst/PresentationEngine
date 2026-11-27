#!/usr/bin/env python3
"""Reference import adapter: Word documents (.docx), standard library only.

Reads word/document.xml and word/styles.xml from the OOXML package:
- Heading / Title paragraph styles (matched by style *name*, so localized
  style ids still work) label slides or start new documents.
- Empty paragraphs end a slide. If the document uses soft line breaks
  (Shift-Return) inside paragraphs, each paragraph is one slide instead.
- Bold, italic and underline runs survive as RTF when keep_formatting is on.
- Page breaks can start a new document (split_on = "page_break").
- Tables are flattened to text with a warning; images are reported, not imported.

The production adapter on macOS can use NSAttributedString's Office Open XML
reader instead; this file documents the expected behavior and feeds tests.
"""
import pathlib
import re
import zipfile
import xml.etree.ElementTree as ET

from common import (BLANK, PAGE_BREAK, AdapterFailure, Heading, Run, Title, build_documents, pb,
                    progress, serve, PROTOCOL_VERSION)

W = "{http://schemas.openxmlformats.org/wordprocessingml/2006/main}"

INFO = pb.AdapterInfo(
    protocol_version=PROTOCOL_VERSION,
    adapter_id="org.example.import-basics/docx",
    version="1.0.0",
    file_types=[pb.FileType(
        uttype="org.openxmlformats.wordprocessingml.document", extensions=["docx"],
        mime_types=["application/vnd.openxmlformats-officedocument.wordprocessingml.document"])],
    supports_multiple_documents=True,
)


def on(el):
    """OOXML toggle property: present and not explicitly false."""
    if el is None:
        return False
    return el.get(W + "val", "true").lower() not in ("0", "false", "none")


def style_names(z):
    try:
        root = ET.fromstring(z.read("word/styles.xml"))
    except KeyError:
        return {}
    names = {}
    for s in root.iter(W + "style"):
        sid, name = s.get(W + "styleId"), s.find(W + "name")
        if sid and name is not None:
            names[sid] = name.get(W + "val", "").lower()
    return names


def paragraph_kind(p, names):
    ppr = p.find(W + "pPr")
    sid = ppr.find(W + "pStyle").get(W + "val") if ppr is not None and ppr.find(W + "pStyle") is not None else ""
    name = names.get(sid, sid.lower())
    if name == "title":
        return ("title", 0)
    m = re.match(r"heading\s*(\d)", name)
    if m:
        return ("heading", int(m.group(1)))
    return ("body", 0)


def paragraph_lines(p, warnings_state):
    """Runs of a paragraph split into lines at soft breaks; reports page breaks."""
    lines, current, page_break, soft = [], [], False, False
    for r in p.iter(W + "r"):
        rpr = r.find(W + "rPr")
        b = on(rpr.find(W + "b")) if rpr is not None else False
        i = on(rpr.find(W + "i")) if rpr is not None else False
        u = rpr is not None and rpr.find(W + "u") is not None and \
            rpr.find(W + "u").get(W + "val", "single") != "none"
        for child in r:
            tag = child.tag
            if tag == W + "t":
                current.append(Run(child.text or "", b, i, u))
            elif tag == W + "tab":
                current.append(Run("\t", b, i, u))
            elif tag in (W + "br", W + "cr"):
                if child.get(W + "type") == "page":
                    page_break = True
                else:
                    lines.append(current)
                    current, soft = [], True
            elif tag == W + "drawing":
                warnings_state["images"] += 1
        if r.find(W + "lastRenderedPageBreak") is not None:
            pass  # layout hint only, not a real break
    lines.append(current)
    return lines, page_break, soft


def do_import(req):
    from google.protobuf.json_format import MessageToDict
    options = MessageToDict(req.options)
    progress(0.0, "Opening document")
    try:
        z = zipfile.ZipFile(req.input_path)
        body_xml = z.read("word/document.xml")
    except zipfile.BadZipFile:
        raise AdapterFailure(pb.AdapterError.CODE_CORRUPT_FILE,
                             "This file isn't a valid Word document (.docx).")
    except KeyError:
        if "EncryptedPackage" in "".join(getattr(z, "namelist", lambda: [])()):
            raise AdapterFailure(pb.AdapterError.CODE_PASSWORD_PROTECTED, "This document is password protected.")
        raise AdapterFailure(pb.AdapterError.CODE_CORRUPT_FILE, "The document has no main text part.")

    names = style_names(z)
    body = ET.fromstring(body_xml).find(W + "body")
    state = {"images": 0, "tables": 0}

    # First pass: does the author use soft breaks? Then a paragraph is a slide.
    paragraphs = []
    for el in body:
        if el.tag == W + "p":
            paragraphs.append(("p", el))
        elif el.tag == W + "tbl":
            state["tables"] += 1
            for p in el.iter(W + "p"):
                paragraphs.append(("p", p))
    parsed = []
    for _, p in paragraphs:
        kind, level = paragraph_kind(p, names)
        lines, page_break, soft = paragraph_lines(p, state)
        parsed.append((kind, level, lines, page_break, soft))
    paragraph_is_slide = any(soft for *_, soft in parsed)

    progress(0.5, "Making slides")
    items = []
    for kind, level, lines, page_break, _ in parsed:
        text = "".join(r.text for line in lines for r in line).strip()
        if page_break:
            items.append(PAGE_BREAK)
        if kind == "title" and text:
            items.append(Title(text))
        elif kind == "heading" and text:
            items.append(Heading(level, text))
        elif not text:
            items.append(BLANK)
        else:
            items.extend(lines)
            if paragraph_is_slide:
                items.append(BLANK)

    warnings = []
    if paragraph_is_slide:
        warnings.append(pb.ImportWarning(severity=pb.ImportWarning.SEVERITY_INFO, document_index=-1,
                                         message="The document uses line breaks within paragraphs, "
                                                 "so each paragraph became one slide."))
    if state["tables"]:
        warnings.append(pb.ImportWarning(severity=pb.ImportWarning.SEVERITY_WARNING, document_index=-1,
                                         message=f"{state['tables']} table(s) were flattened to text."))
    if state["images"]:
        warnings.append(pb.ImportWarning(severity=pb.ImportWarning.SEVERITY_WARNING, document_index=-1,
                                         message=f"{state['images']} image(s) were not imported."))

    stem = pathlib.Path(req.original_filename or req.input_path).stem
    docs = build_documents(items, options, stem, warnings)
    progress(1.0, "Done")
    return pb.ImportResult(documents=docs, warnings=warnings)


if __name__ == "__main__":
    serve(INFO, do_import)
