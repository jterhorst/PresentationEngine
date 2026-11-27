"""End-to-end import adapter tests: start each reference adapter as the UI
would, speak the framed protocol over stdin/stdout, and check the documents.

Usage: python3 tests/test_import.py <generated-python-dir>
"""
import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import zipfile

from jsonschema import Draft202012Validator

GEN = pathlib.Path(sys.argv[1]).resolve()
sys.path.insert(0, str(GEN))
from google.protobuf.struct_pb2 import Struct  # noqa: E402
from importer.v1 import importer_pb2 as pb  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
PLUGIN = ROOT / "examples/import-adapters/org.example.import-basics"
failures = 0


def check(name, ok, detail=""):
    global failures
    print(("PASS " if ok else "FAIL ") + name + (f" ({detail})" if detail and not ok else ""))
    failures += 0 if ok else 1


# --- Minimal UI-side host --------------------------------------------------

def run_adapter(command, path, original_name, options=None, timeout=30):
    env = dict(os.environ, PYTHONPATH=f"{GEN}{os.pathsep}{PLUGIN / 'adapters'}")
    proc = subprocess.Popen([sys.executable, str(PLUGIN / command)], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)

    def send(req):
        data = req.SerializeToString()
        proc.stdin.write(struct.pack(">I", len(data)) + data)
        proc.stdin.flush()

    def recv():
        header = proc.stdout.read(4)
        if len(header) < 4:
            return None
        resp = pb.AdapterResponse()
        resp.ParseFromString(proc.stdout.read(struct.unpack(">I", header)[0]))
        return resp

    send(pb.AdapterRequest(describe=pb.Describe(protocol_version=1)))
    info = recv().info
    opts = Struct()
    opts.update(options or {})
    with tempfile.TemporaryDirectory() as out:
        req = pb.AdapterRequest()
        req.import_request.input_path = str(path)
        req.import_request.original_filename = original_name
        req.import_request.output_dir = out
        req.import_request.options.CopyFrom(opts)
        req.import_request.canvas.width, req.import_request.canvas.height = 1920, 1080
        send(req)
        progress_seen, final = 0, None
        while True:
            resp = recv()
            if resp is None:
                break
            kind = resp.WhichOneof("body")
            if kind == "progress":
                progress_seen += 1
            else:
                final = resp
                break
    proc.stdin.close()
    proc.wait(timeout=timeout)
    return info, final, progress_seen, proc.stderr.read().decode()


# --- Fixtures ---------------------------------------------------------------

def make_docx(path, paragraphs, heading_style_id="Heading1"):
    """paragraphs: list of (style_id or None, [runs]) where a run is
    (text, bold) or the string 'BR' (soft break) or 'PAGE' (page break)."""
    w = "http://schemas.openxmlformats.org/wordprocessingml/2006/main"
    body = []
    for style, runs in paragraphs:
        ppr = f'<w:pPr><w:pStyle w:val="{style}"/></w:pPr>' if style else ""
        rs = []
        for run in runs:
            if run == "BR":
                rs.append("<w:r><w:br/></w:r>")
            elif run == "PAGE":
                rs.append('<w:r><w:br w:type="page"/></w:r>')
            else:
                text, bold = run
                rpr = "<w:rPr><w:b/></w:rPr>" if bold else ""
                rs.append(f'<w:r>{rpr}<w:t xml:space="preserve">{text}</w:t></w:r>')
        body.append(f"<w:p>{ppr}{''.join(rs)}</w:p>")
    document = f'<?xml version="1.0" encoding="UTF-8"?><w:document xmlns:w="{w}"><w:body>{"".join(body)}</w:body></w:document>'
    styles = (f'<?xml version="1.0" encoding="UTF-8"?><w:styles xmlns:w="{w}">'
              f'<w:style w:type="paragraph" w:styleId="Title"><w:name w:val="Title"/></w:style>'
              f'<w:style w:type="paragraph" w:styleId="{heading_style_id}"><w:name w:val="heading 1"/></w:style>'
              f'<w:style w:type="paragraph" w:styleId="Heading2"><w:name w:val="heading 2"/></w:style></w:styles>')
    content_types = ('<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
                     '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
                     '<Default Extension="xml" ContentType="application/xml"/>'
                     '<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/></Types>')
    rels = ('<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
            '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>')
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("[Content_Types].xml", content_types)
        z.writestr("_rels/.rels", rels)
        z.writestr("word/document.xml", document)
        z.writestr("word/styles.xml", styles)


tmp = pathlib.Path(tempfile.mkdtemp())

# Song set: localized heading style id ("berschrift1") named "heading 1".
songs = tmp / "songs.docx"
make_docx(songs, [
    ("Title", [("Sunday Set", False)]),
    ("berschrift1", [("Amazing Grace", False)]),
    ("Heading2", [("Verse 1", False)]),
    (None, [("Amazing grace, how sweet the sound", False)]),
    (None, [("That saved a wretch like me", False)]),
    (None, []),
    (None, [("Chorus", False)]),
    (None, [("My chains are gone, ", False), ("I've been set free", True)]),
    (None, []),
    (None, [("CCLI Song # 4768151", False)]),
    ("berschrift1", [("How Great Thou Art", False)]),
    (None, [("[Verse 1]", False)]),
    (None, [("O Lord my God", False)]),
    (None, [("When I in awesome wonder", False)]),
], heading_style_id="berschrift1")

info, res, prog, err = run_adapter("adapters/docx_reference.py", songs, "songs.docx", {"split_on": "heading1"})
check("docx adapter describes itself", info.adapter_id == "org.example.import-basics/docx"
      and "docx" in info.file_types[0].extensions and info.supports_multiple_documents)
check("docx adapter reports progress", prog >= 2, f"progress messages: {prog}")
docs = res.result.documents if res and res.WhichOneof("body") == "result" else []
check("split_on=heading1 yields one document per song (localized style id)",
      [d.name for d in docs] == ["Amazing Grace", "How Great Thou Art"], str([d.name for d in docs]) + err)
if len(docs) == 2:
    s = [sl.semantic for sl in docs[0].slides]
    check("headings and label lines become slide labels",
          [x.label for x in s] == ["Verse 1", "Chorus"], str([x.label for x in s]))
    check("stanza lines stay together in one text block",
          s[0].blocks[0].plain_text == "Amazing grace, how sweet the sound\nThat saved a wretch like me")
    check("bold run is kept in the RTF", b"{\\b I've been set free}" in s[1].blocks[0].rtf, s[1].blocks[0].rtf[:120])
    check("CCLI line goes to metadata, not a slide", docs[0].metadata.get("ccli") == "CCLI Song # 4768151")
    check("bracketed [Verse 1] labels the next slide", docs[1].slides[0].semantic.label == "Verse 1")

info, res, _, _ = run_adapter("adapters/docx_reference.py", songs, "songs.docx",
                              {"split_on": "none", "lines_per_slide": 1, "title_slide": True})
docs = res.result.documents
check("split_on=none keeps one document named by the Title style", [d.name for d in docs] == ["Sunday Set"])
labels = [sl.semantic.label for sl in docs[0].slides]
check("lines_per_slide=1 splits stanzas and numbers the parts; title slide first",
      labels[:4] == ["Title", "Verse 1", "Verse 1 (2)", "Chorus"], str(labels))

soft = tmp / "soft.docx"
make_docx(soft, [
    (None, [("Line one", False), "BR", ("Line two", False)]),
    (None, [("Line three", False), "BR", ("Line four", False)]),
])
_, res, _, _ = run_adapter("adapters/docx_reference.py", soft, "Blessed Be.docx")
docs = res.result.documents
check("soft line breaks: each paragraph becomes one slide",
      len(docs) == 1 and len(docs[0].slides) == 2 and docs[0].name == "Blessed Be"
      and docs[0].slides[1].semantic.blocks[0].plain_text == "Line three\nLine four")
check("soft-break heuristic is reported as an info warning",
      any(w.severity == pb.ImportWarning.SEVERITY_INFO for w in res.result.warnings))

pages = tmp / "paged.docx"
make_docx(pages, [(None, [("Song A line", False)]), (None, ["PAGE", ("Song B line", False)])])
_, res, _, _ = run_adapter("adapters/docx_reference.py", pages, "paged.docx", {"split_on": "page_break"})
check("split_on=page_break starts a new document at a page break", len(res.result.documents) == 2)

corrupt = tmp / "broken.docx"
corrupt.write_bytes(b"this is not a zip file")
_, res, _, _ = run_adapter("adapters/docx_reference.py", corrupt, "broken.docx")
check("corrupt .docx returns CORRUPT_FILE instead of crashing",
      res.WhichOneof("body") == "error" and res.error.code == pb.AdapterError.CODE_CORRUPT_FILE)

txt = tmp / "lyrics.txt"
txt.write_text("10,000 Reasons\n\nVerse 1\nThe sun comes up\nIt's a new day dawning\n\n"
               "Chorus:\nBless the Lord, O my soul\nO my soul — worship\n\n© 2011 Thankyou Music\n",
               encoding="utf-8")
info, res, _, _ = run_adapter("adapters/plaintext.py", txt, "lyrics.txt")
docs = res.result.documents
check("plain-text adapter: title from first line, labeled stanzas",
      docs[0].name == "10,000 Reasons" and [s.semantic.label for s in docs[0].slides] == ["Verse 1", "Chorus"])
check("non-ASCII text is escaped in RTF and kept in plain text",
      b"\\u8212?" in docs[0].slides[1].semantic.blocks[0].rtf
      and "—" in docs[0].slides[1].semantic.blocks[0].plain_text)
check("copyright line goes to metadata", docs[0].metadata.get("copyright", "").startswith("© 2011"))

untitled = tmp / "untitled.txt"
untitled.write_text("Verse 1\nLine one\nLine two\n", encoding="utf-8")
_, res, _, _ = run_adapter("adapters/plaintext.py", untitled, "untitled.txt", {"first_line_is_title": False})
check("a label before the first slide survives lazy document creation",
      [s.semantic.label for s in res.result.documents[0].slides] == ["Verse 1"])

binary = tmp / "image.txt"
binary.write_bytes(b"\x89PNG\x00\x00\x00binary")
_, res, _, _ = run_adapter("adapters/plaintext.py", binary, "image.txt")
check("binary input to the text adapter returns UNSUPPORTED_FORMAT",
      res.WhichOneof("body") == "error" and res.error.code == pb.AdapterError.CODE_UNSUPPORTED_FORMAT)

# --- Manifest and option forms validate --------------------------------------
manifest = json.loads((PLUGIN / "plugin.json").read_text())
plugin_schema = json.loads((ROOT / "schemas/logic-plugin.schema.json").read_text())
ui_schema = json.loads((ROOT / "schemas/ui-schema.schema.json").read_text())
errs = list(Draft202012Validator(plugin_schema).iter_errors(manifest))
check("importer-only plugin manifest validates (no 'run')", not errs, "; ".join(e.message for e in errs[:2]))
bad = dict(manifest)
bad.pop("importers")
check("a plugin with neither 'run' nor 'importers' is rejected",
      bool(list(Draft202012Validator(plugin_schema).iter_errors(bad))))
for imp in manifest["importers"]:
    form = json.loads((PLUGIN / imp["options"]).read_text())
    errs = list(Draft202012Validator(ui_schema).iter_errors(form))
    check(f"options form for '{imp['id']}' validates", not errs, "; ".join(e.message for e in errs[:2]))

print("ALL PASSED" if failures == 0 else f"{failures} FAILED")
sys.exit(1 if failures else 0)
