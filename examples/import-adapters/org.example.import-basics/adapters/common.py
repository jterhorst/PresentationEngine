"""Shared pieces for the reference import adapters: stdio framing, a lyrics-aware
text-to-slides splitter, and an RTF writer for the supported subset.

Reference adapters are Python for readability; they need the generated
Python protobuf modules on PYTHONPATH. Production first-party adapters are
native executables speaking the same protocol.
"""
import re
import struct
import sys

from importer.v1 import importer_pb2 as pb

PROTOCOL_VERSION = 1

# --------------------------------------------------------------------------
# Framing: 4-byte big-endian length, then one message.
# --------------------------------------------------------------------------

def read_request(stream=sys.stdin.buffer):
    header = stream.read(4)
    if len(header) < 4:
        return None  # stdin closed: cancelled or done
    (length,) = struct.unpack(">I", header)
    msg = pb.AdapterRequest()
    msg.ParseFromString(stream.read(length))
    return msg


def send(response, stream=sys.stdout.buffer):
    data = response.SerializeToString()
    stream.write(struct.pack(">I", len(data)) + data)
    stream.flush()


def serve(info, do_import):
    """Run one adapter session: answer describe, run one import, exit."""
    while True:
        req = read_request()
        if req is None:
            return
        kind = req.WhichOneof("body")
        if kind == "describe":
            send(pb.AdapterResponse(info=info))
        elif kind == "import_request":
            try:
                result = do_import(req.import_request)
                send(pb.AdapterResponse(result=result))
            except AdapterFailure as f:
                send(pb.AdapterResponse(error=pb.AdapterError(code=f.code, message=f.message)))
            except Exception as e:  # never crash without telling the UI why
                print(f"internal error: {e!r}", file=sys.stderr)
                send(pb.AdapterResponse(error=pb.AdapterError(
                    code=pb.AdapterError.CODE_INTERNAL, message="The importer hit an unexpected error.")))
            return


class AdapterFailure(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code, self.message = code, message


def progress(fraction, message):
    send(pb.AdapterResponse(progress=pb.ImportProgress(fraction=fraction, message=message)))


# --------------------------------------------------------------------------
# Input model shared by every text adapter.
#   Line:    a list of Run(text, bold, italic, underline)
#   Markers: BLANK (stanza break), PAGE_BREAK, Heading(level, text), Title(text)
# --------------------------------------------------------------------------

class Run:
    __slots__ = ("text", "bold", "italic", "underline")

    def __init__(self, text, bold=False, italic=False, underline=False):
        self.text, self.bold, self.italic, self.underline = text, bold, italic, underline


BLANK, PAGE_BREAK = "blank", "page_break"


class Heading:
    def __init__(self, level, text):
        self.level, self.text = level, text


class Title:
    def __init__(self, text):
        self.text = text


SECTION_RE = re.compile(
    r"^\[?\s*(verse|chorus|pre-?chorus|bridge|tag|intro|outro|ending|refrain|interlude|vamp)"
    r"(\s*\d+)?\s*\]?\s*:?\s*$", re.IGNORECASE)
FOOTER_RE = re.compile(r"^\s*(ccli|©|\(c\)|copyright)", re.IGNORECASE)


def line_text(line):
    return "".join(r.text for r in line)


def build_documents(items, options, fallback_name, warnings):
    """Turn the item stream into ImportedDocuments of semantic slides."""
    split_on = options.get("split_on", "none")
    lines_per_slide = int(options.get("lines_per_slide", 0) or 0)
    title_slide = bool(options.get("title_slide", False))
    keep_formatting = bool(options.get("keep_formatting", True))

    docs, doc, label, group, counts, stanza = [], None, "", "", {}, []

    def new_doc(name):
        nonlocal doc, label, group, counts
        doc = pb.ImportedDocument(name=name or fallback_name)
        docs.append(doc)
        label, group, counts = "", "", {}
        if title_slide and name:
            s = doc.slides.add().semantic
            s.label = "Title"
            add_block(s, [[Run(name)]], pb.TEXT_ROLE_TITLE, keep_formatting)

    def flush():
        nonlocal stanza, label, group
        if not stanza:
            return
        if doc is None:
            keep = (label, group)  # a label seen before the first slide still applies
            new_doc(fallback_name)
            label, group = keep
        chunks = [stanza] if lines_per_slide <= 0 else [
            stanza[i:i + lines_per_slide] for i in range(0, len(stanza), lines_per_slide)]
        for n, chunk in enumerate(chunks):
            s = doc.slides.add().semantic
            base = label or ""
            s.label = base if n == 0 or not base else f"{base} ({n + 1})"
            s.group = group
            add_block(s, chunk, pb.TEXT_ROLE_BODY, keep_formatting)
        stanza = []

    for item in items:
        if item == BLANK:
            flush()
        elif item == PAGE_BREAK:
            flush()
            if split_on == "page_break" and doc is not None and len(doc.slides):
                doc = None
        elif isinstance(item, Title):
            flush()
            if doc is None or len(doc.slides) == 0:
                new_doc(item.text.strip())
            elif split_on != "none":
                new_doc(item.text.strip())
        elif isinstance(item, Heading):
            flush()
            text = item.text.strip()
            if item.level == 1 and split_on == "heading1":
                new_doc(text)
            elif text:
                label, group = section_label(text, counts)
        else:  # a line
            text = line_text(item).strip()
            if not text:
                flush()
                continue
            if FOOTER_RE.match(text):
                flush()
                if doc is None:
                    new_doc(fallback_name)
                key = "ccli" if text.lower().startswith("ccli") else "copyright"
                doc.metadata[key] = text
                continue
            if SECTION_RE.match(text):
                flush()
                label, group = section_label(text, counts)
                continue
            stanza.append(item)
    flush()

    # Drop documents with no content, e.g. a file-level Title before the first
    # Heading 1 when splitting one document per song.
    def has_content(d):
        return any(b.role != pb.TEXT_ROLE_TITLE for s in d.slides for b in s.semantic.blocks) \
            or any(s.WhichOneof("content") == "laid_out" for s in d.slides)
    docs = [d for d in docs if has_content(d)]
    if not docs:
        warnings.append(pb.ImportWarning(severity=pb.ImportWarning.SEVERITY_WARNING,
                                         message="No text found to make slides from.", document_index=-1))
    return docs


def section_label(text, counts):
    """'chorus' -> ('Chorus', 'Chorus'); repeated unnumbered sections keep their name."""
    m = SECTION_RE.match(text)
    if not m:
        return text, text
    raw = m.group(1).lower()
    name = "Pre-Chorus" if raw in ("prechorus", "pre-chorus") else raw.title()
    num = (m.group(2) or "").strip()
    counts[name] = counts.get(name, 0) + 1
    full = f"{name} {num}" if num else name
    return full, full


def add_block(slide, lines, role, keep_formatting):
    block = slide.blocks.add(role=role)
    block.plain_text = "\n".join(line_text(l).strip() for l in lines)
    block.rtf = to_rtf(lines, keep_formatting)


# --------------------------------------------------------------------------
# RTF writer (supported subset only).
# --------------------------------------------------------------------------

def _escape(text):
    out = []
    for ch in text:
        code = ord(ch)
        if ch in "\\{}":
            out.append("\\" + ch)
        elif ch == "\t":
            out.append("\\tab ")
        elif code < 128:
            out.append(ch)
        else:
            signed = code if code < 32768 else code - 65536
            if code > 0xFFFF:  # outside the BMP: write a UTF-16 surrogate pair
                v = code - 0x10000
                for unit in (0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF)):
                    out.append(f"\\u{unit - 65536}?")
                continue
            out.append(f"\\u{signed}?")
    return "".join(out)


def to_rtf(lines, keep_formatting=True):
    parts = ["{\\rtf1\\ansi\\ansicpg1252\\uc1\\deff0{\\fonttbl{\\f0 Helvetica;}}\\f0 "]
    for i, line in enumerate(lines):
        if i:
            parts.append("\\par ")
        for r in line:
            text = r.text.strip("\n")
            if not text:
                continue
            if keep_formatting and (r.bold or r.italic or r.underline):
                on = ("\\b" if r.bold else "") + ("\\i" if r.italic else "") + ("\\ul" if r.underline else "")
                parts.append("{" + on + " " + _escape(text) + "}")
            else:
                parts.append(_escape(text))
    parts.append("}")
    return "".join(parts).encode("ascii")
