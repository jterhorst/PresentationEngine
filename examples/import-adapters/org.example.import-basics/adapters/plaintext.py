#!/usr/bin/env python3
"""Reference import adapter: plain-text lyrics (.txt).

Blank lines separate slides. Lines such as "Verse 1", "[Chorus]" or "Bridge:"
label the slides that follow. Lines starting with "CCLI" or "©" go to metadata.
"""
import pathlib

from common import (BLANK, AdapterFailure, Run, Title, build_documents, pb, progress, serve,
                    PROTOCOL_VERSION)

INFO = pb.AdapterInfo(
    protocol_version=PROTOCOL_VERSION,
    adapter_id="org.example.import-basics/plaintext",
    version="1.0.0",
    file_types=[pb.FileType(uttype="public.plain-text", extensions=["txt"], mime_types=["text/plain"])],
    supports_multiple_documents=False,
)


def read_text(path):
    raw = pathlib.Path(path).read_bytes()
    if b"\x00" in raw[:4096]:
        raise AdapterFailure(pb.AdapterError.CODE_UNSUPPORTED_FORMAT, "This doesn't look like a text file.")
    for enc in ("utf-8-sig", "cp1252"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("latin-1")


def do_import(req):
    from google.protobuf.json_format import MessageToDict
    options = MessageToDict(req.options)
    progress(-1, "Reading text")
    text = read_text(req.input_path)
    lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")

    items = []
    if options.get("first_line_is_title", True):
        while lines and not lines[0].strip():
            lines.pop(0)
        if lines:
            items.append(Title(lines.pop(0)))
    for line in lines:
        items.append([Run(line)] if line.strip() else BLANK)

    warnings = []
    stem = pathlib.Path(req.original_filename or req.input_path).stem
    docs = build_documents(items, options, stem, warnings)
    return pb.ImportResult(documents=docs, warnings=warnings)


if __name__ == "__main__":
    serve(INFO, do_import)
