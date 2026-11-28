"""docs/User_Manual.md -> docs/User_Manual.pdf (Python markdown + headless Edge)."""
import os, re, subprocess, sys
import markdown

ROOT = r"M:\ratspeak-handheld-for-tdeck-pro"
SRC = os.path.join(ROOT, "docs", "User_Manual.md")
OUT = os.path.join(ROOT, "docs", "User_Manual.pdf")
TMP = os.path.join(os.environ.get("TEMP", "."), "User_Manual.html")
EDGE = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"

md = open(SRC, encoding="utf-8").read()
body = markdown.markdown(md, extensions=["tables", "fenced_code", "toc"])
docs = "file:///" + os.path.join(ROOT, "docs").replace("\\", "/") + "/"
body = re.sub(r'src="(?!https?:|file:)([^"]+)"', lambda m: 'src="%s%s"' % (docs, m.group(1)), body)
body = re.sub(r'href="(?!https?:|file:|#)([^"]+)"', lambda m: 'href="%s%s"' % (docs, m.group(1)), body)
css = """
@page { size: A4; margin: 16mm 15mm 18mm 15mm; }
body { font-family: 'Segoe UI', Arial, sans-serif; font-size: 10.5pt; line-height: 1.45; color: #1d1b18; }
h1 { font-size: 22pt; border-bottom: 2px solid #1d1b18; padding-bottom: 4px; }
h2 { font-size: 16pt; margin-top: 22px; border-bottom: 1px solid #999; page-break-before: always; }
h1 + p + p + h2, hr + h2 { }
h3 { font-size: 12.5pt; margin-top: 16px; page-break-after: avoid; }
table { border-collapse: collapse; width: 100%; margin: 8px 0; font-size: 9.5pt; page-break-inside: auto; }
tr { page-break-inside: avoid; }
th, td { border: 1px solid #aaa; padding: 3px 6px; vertical-align: top; text-align: left; }
th { background: #eee; }
code, pre { font-family: Consolas, monospace; font-size: 9.5pt; background: #f3f1ec; }
pre { padding: 6px 8px; white-space: pre-wrap; }
img { max-width: 60%; display: block; margin: 8px auto; }
blockquote { border-left: 3px solid #bbb; margin: 8px 0; padding: 2px 10px; color: #444; }
hr { border: 0; }
"""
html = "<!doctype html><html><head><meta charset='utf-8'><title>RatSpeak T-Deck Pro User Manual</title><style>%s</style></head><body>%s</body></html>" % (css, body)
open(TMP, "w", encoding="utf-8").write(html)
r = subprocess.run([EDGE, "--headless=new", "--disable-gpu", "--no-pdf-header-footer", "--allow-file-access-from-files",
                    "--print-to-pdf=" + OUT, "file:///" + TMP.replace("\\", "/")], capture_output=True, text=True, timeout=180)
print(r.returncode, r.stderr[-400:])
from pypdf import PdfReader
print("pages", len(PdfReader(OUT).pages), "bytes", os.path.getsize(OUT))
