# -*- coding: utf-8 -*-
"""逐格式验证内容索引：造样本 → build → 逐格式断言 → 出报告。

用法: python tests/run_fmt_matrix.py [--ocr]
  --ocr  额外测试图片（png/jpg/bmp/tif），需要 tesseract
"""
import os
import shutil
import struct
import subprocess
import sys
import zipfile
import zlib

import time

ROOT = r"C:\Users\ALICE\dev\SpaceSift-Native"
# 每次跑用一份新目录：复用同一路径要先删掉上百个样本文件，会被批量删除保护拦下；
# 换新目录最省事，也避免上一次的索引库混进 SearchAll 的结果里。
_STAMP = time.strftime("%Y%m%d_%H%M%S")
_SAMPLE_ROOT = os.path.join(r"C:\Temp", "ssfmt_" + _STAMP)
A = os.path.join(_SAMPLE_ROOT, "A")
B = os.path.join(_SAMPLE_ROOT, "B")
TOOL = os.path.join(ROOT, "build", "fmt_matrix.exe")
EXTRACT_ONE = os.path.join(ROOT, "build", "extract_one.exe")

# 本机真实样本（不好凭空造的格式）
REAL = {
    ".doc": r"C:\Users\ALICE\Documents\xwechat_files\wxid_eqdih4gieibz12_8497\msg\file\2024-06\2、研究生学位论文中期检查表（2024版）.doc",
    ".xls": r"C:\Users\ALICE\Documents\xwechat_files\wxid_eqdih4gieibz12_8497\msg\file\2024-09\2024级物流管理（专升本）课表.xls",
    ".ppt": r"C:\Users\ALICE\Documents\xwechat_files\wxid_eqdih4gieibz12_8497\msg\file\2025-12\项目开题_南航202503R.ppt",
    ".dwg": r"C:\Users\ALICE\Documents\xwechat_files\wxid_eqdih4gieibz12_8497\msg\file\2026-03\PE垫片带胶.dwg",
    ".sldprt": r"C:\Users\ALICE\Documents\xwechat_files\wxid_eqdih4gieibz12_8497\msg\file\2025-09\3heater_cig_v2.0.SLDPRT",
    ".sldasm": r"D:\1硕士期间文件\1\Assem2.SLDASM",
    ".vsdx": r"C:\Users\ALICE\Documents\xwechat_files\wxid_eqdih4gieibz12_8497\msg\file\2025-05\技术路线图1(1).vsdx",
    ".drawio": r"D:\毕少华\课题研究\小论文\小论文1\research\figures\Fig0_system_diagram.drawio",
    ".igs": r"C:\Users\ALICE\Documents\xwechat_files\wxid_eqdih4gieibz12_8497\msg\file\2025-09\加热针.IGS",
    ".step": r"C:\Users\ALICE\Documents\xwechat_files\wxid_eqdih4gieibz12_8497\msg\file\2025-01\Part1.1.STEP",
    ".stl": r"C:\Users\ALICE\Documents\xwechat_files\wxid_eqdih4gieibz12_8497\msg\file\2024-09\300-426 底盖.STL",
    ".slx": r"C:\Users\ALICE\Documents\xwechat_files\wxid_eqdih4gieibz12_8497\msg\file\2025-05\untitled20x2D1_5(1).slx",
    ".mph": r"C:\Users\ALICE\Desktop\tobac22_8壁面无对流.mph",
    ".mat": r"D:\隔膜压缩机\matlab file\compressor_simulation_results.mat",
    ".rar": r"C:\Users\ALICE\Documents\xwechat_files\wxid_eqdih4gieibz12_8497\msg\file\2024-09\3挡.rar",
    ".mldatx": r"D:\隔膜压缩机\matlab_system_learning\slprj\_sfprj\EMLReport\GJSpmP5cOchiTLK2fW0YjC\report.mldatx",
}

# 纯文本类：直接写关键词
TEXT_EXTS = """txt md markdown csv tsv log json xml yml yaml ini cfg conf toml properties
html htm css scss less vue svg c h cpp cc cxx hpp hh cs java py js mjs cjs ts tsx jsx
rs go sql sh bash ps1 bat cmd lua rb php pl r m gradle cmake make mk dockerfile
iges stl ply ctb stb scr lsp inp cdb anf wbpj dat sta jnl rpy l asv nb fld tmw original
mm opml mmd puml mermaid""".split()

ZIP_SAMPLES = {
    ".docx": {"word/document.xml": "<w:document><w:body><w:p><w:t>{kw} 自检正文</w:t></w:p></w:body></w:document>"},
    ".docm": {"word/document.xml": "<w:document><w:body><w:p><w:t>{kw} 宏文档</w:t></w:p></w:body></w:document>"},
    ".dotx": {"word/document.xml": "<w:document><w:body><w:p><w:t>{kw} 模板</w:t></w:p></w:body></w:document>"},
    ".xlsx": {"xl/sharedStrings.xml": "<sst><si><t>{kw} 单元格</t></si></sst>"},
    ".xlsm": {"xl/sharedStrings.xml": "<sst><si><t>{kw} 宏表</t></si></sst>"},
    ".pptx": {"ppt/slides/slide1.xml": "<p:sld><p:cSld><a:t>{kw} 幻灯片</a:t></p:cSld></p:sld>"},
    ".pptm": {"ppt/slides/slide1.xml": "<p:sld><p:cSld><a:t>{kw} 宏幻灯</a:t></p:cSld></p:sld>"},
    ".odt": {"content.xml": "<office:document-content><text:p>{kw} 文本文档</text:p></office:document-content>"},
    ".ods": {"content.xml": "<office:document-content><text:p>{kw} 表格</text:p></office:document-content>"},
    ".odp": {"content.xml": "<office:document-content><text:p>{kw} 演示</text:p></office:document-content>"},
    ".mlx": {"matlab/document.xml": "<doc><p>{kw} LiveScript</p></doc>"},
    ".vsdx": {"visio/pages/page1.xml": "<PageContents><Text>{kw} 流程图节点</Text></PageContents>"},
    ".slx": {"simulink/systems/system_root.xml": "<System><Block Name=\"{kw}_block\"><P Name=\"MaskDescription\">{kw} 模型说明</P></Block></System>"},
    ".slxc": {"metadata/coreProperties.xml": "<coreProperties><title>{kw}</title></coreProperties>"},
    ".sldd": {"data/dictionary.xml": "<dict><entry name=\"{kw}\">数据字典项</entry></dict>"},
    ".slddc": {"data/dictionary.xml": "<dict><entry name=\"{kw}\">字典缓存</entry></dict>"},
    ".mldatx": {"metadata/coreProperties.xml": "<coreProperties><title>{kw}</title></coreProperties>"},
    ".xmind": {"content.json": '{"title":"{kw} 中心主题","children":[{"title":"分支一"}]}'},
    ".3mf": {"3D/3dmodel.model": "<model><metadata name=\"Title\">{kw} 模型</metadata><resources><object name=\"{kw}_part\"/></resources></model>"},
    ".epub": {"OEBPS/chapter1.xhtml": "<html><body><p>{kw} 章节正文</p></body></html>"},
    ".zip": {"inner.md": "{kw} 压缩包内文本"},
}


def zz(path: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)


def write(path: str, data: bytes) -> None:
    zz(path)
    with open(path, "wb") as f:
        f.write(data)


# ---------- 各格式样本 ----------

def make_pdf(text: str) -> bytes:
    objs = {}
    objs[1] = b"<< /Type /Catalog /Pages 2 0 R >>"
    objs[2] = b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>"
    objs[3] = (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 120] "
               b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>")
    stream = ("BT /F1 14 Tf 20 60 Td (" + text + ") Tj ET").encode("latin-1", "replace")
    objs[4] = b"<< /Length " + str(len(stream)).encode() + b" >>\nstream\n" + stream + b"\nendstream"
    objs[5] = b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"
    out = bytearray(b"%PDF-1.4\n")
    offs = {}
    for i in range(1, 6):
        offs[i] = len(out)
        out += str(i).encode() + b" 0 obj\n" + objs[i] + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 6\n0000000000 65535 f \n"
    for i in range(1, 6):
        out += ("%010d 00000 n \n" % offs[i]).encode()
    out += b"trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n" + str(xref).encode() + b"\n%%EOF\n"
    return bytes(out)


def make_mat(name: str, text: str) -> bytes:
    """最小 MAT v5：一个 char 数组（变量名 + UTF-8 字符串值）。"""
    def elem(tag: int, data: bytes) -> bytes:
        pad = (8 - len(data) % 8) % 8
        return struct.pack("<II", tag, len(data)) + data + b"\x00" * pad

    def small(tag: int, data: bytes) -> bytes:
        if len(data) > 4:                     # 小格式只放得下 4 字节，超了必须用大格式
            return elem(tag, data)
        return struct.pack("<I", (len(data) << 16) | tag) + data + b"\x00" * (4 - len(data))

    body = elem(6, struct.pack("<II", 4 << 8, 0))          # array flags：mxCHAR
    body += elem(5, struct.pack("<ii", 1, len(text)))      # dims
    body += small(1, name.encode())                        # 变量名
    body += elem(16, text.encode("utf-8"))                 # miUTF8 数据
    hdr = b"MATLAB 5.0 MAT-file, Platform: PCWIN64, Created by SpaceSift test"
    hdr = hdr + b" " * (116 - len(hdr)) + b"\x00" * 8 + b"\x00\x01IM"
    return hdr + struct.pack("<II", 14, len(body)) + body


def make_mph(kw: str) -> bytes:
    import io
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("smodel.json", '{"label":"%s 模型根","nodes":[{"label":"参数"}]}' % kw)
        z.writestr("modelinfo.xml", "<model><title>%s</title></model>" % kw)
        z.writestr("dmodel.xml", "<model><param name='%s'>desc %s</param></model>" % (kw, kw))
    return buf.getvalue()


def make_eml(kw: str) -> bytes:
    return ("From: test@example.com\r\nTo: a@b.c\r\nSubject: %s\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n\r\n正文 %s\r\n" % (kw, kw)).encode()


def make_msg(kw: str) -> bytes:
    # 真实 .msg 是 OLE2，这里只能测 UTF-16 兜底路径（报告里会标注）
    return ("邮件主题 " + kw).encode("utf-16-le")


def make_dxf(kw: str) -> bytes:
    return ("0\nSECTION\n2\nENTITIES\n0\nTEXT\n8\n0\n10\n0.0\n20\n0.0\n"
            "1\n%s\n0\nENDSEC\n0\nEOF\n" % kw).encode()


def make_step(kw: str) -> bytes:
    return ("ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION(('%s'),'2;1');\nENDSEC;\n"
            "DATA;\n#1=PRODUCT('%s','零件');\nENDSEC;\nEND-ISO-10303-21;\n" % (kw, kw)).encode()


def make_obj(kw: str) -> bytes:
    return ("# %s 网格\n g 组_%s\n v 0 0 0\n v 1 0 0\n" % (kw, kw)).encode()


def make_ipynb(kw: str) -> bytes:
    return ('{"cells":[{"cell_type":"markdown","source":["# %s notebook"]}],'
            '"metadata":{},"nbformat":4,"nbformat_minor":5}' % kw).encode()


def make_drawio(kw: str) -> bytes:
    return ('<mxfile><diagram><mxGraphModel><root><mxCell value="%s 节点"/></root>'
            '</mxGraphModel></diagram></mxfile>' % kw).encode()


def make_archive(ext: str, kw: str, path: str) -> bool:
    seven = r"C:\Program Files\7-Zip\7z.exe"
    if not os.path.exists(seven):
        return False
    src = path + ".src.md"
    write(src, ("%s 压缩包内文本\n" % kw).encode())
    fmt = "7z" if ext == ".7z" else "rar"
    r = subprocess.run([seven, "a", "-t" + fmt, path, src], capture_output=True)
    os.remove(src)
    return r.returncode == 0 and os.path.exists(path)


def extracted_text(path: str) -> str:
    pfile = os.path.join(ROOT, "build", "_kw_in.txt")
    ofile = os.path.join(ROOT, "build", "_kw_out.txt")
    write(pfile, path.encode("utf-8"))
    if os.path.exists(ofile):
        os.remove(ofile)
    subprocess.run([EXTRACT_ONE, pfile, ofile], capture_output=True)
    if not os.path.exists(ofile):
        return ""
    return open(ofile, encoding="utf-8", errors="replace").read()


def pick_keyword(text: str) -> str:
    import re
    for m in re.finditer(r"[A-Za-z][A-Za-z0-9_\-]{5,}", text):
        return m.group(0)
    for m in re.finditer(r"[\u4e00-\u9fff]{4,8}", text):
        return m.group(0)
    return ""


def main() -> int:
    ocr = "--ocr" in sys.argv
    for d in (A, B):
        os.makedirs(d, exist_ok=True)
    print("样本目录:", _SAMPLE_ROOT)

    cases_a = []      # (ext, keyword, path)
    notes = []        # 无法测试的格式与原因
    idx = 0

    def add(ext: str, kw: str, path: str) -> None:
        cases_a.append((ext, kw, path))

    # 1) 纯文本类
    for ext in TEXT_EXTS:
        idx += 1
        kw = "zzkw%03d" % idx
        p = os.path.join(A, "t%03d.%s" % (idx, ext))
        if ext == "dxf":
            write(p, make_dxf(kw))
        elif ext in ("step", "stp"):
            write(p, make_step(kw))
        elif ext == "obj":
            write(p, make_obj(kw))
        elif ext == "ipynb":
            write(p, make_ipynb(kw))
        elif ext == "drawio":
            write(p, make_drawio(kw))
        elif ext in ("json", "xml", "svg"):
            write(p, ('{"k":"%s"}' % kw if ext == "json"
                      else "<r><v>%s</v></r>" % kw).encode())
        else:
            write(p, ("%s 样例文本 for %s\n" % (kw, ext)).encode())
        add(ext, kw, p)

    # 2) zip 家族
    for ext, entries in ZIP_SAMPLES.items():
        idx += 1
        kw = "zzkw%03d" % idx
        p = os.path.join(A, "z%03d%s" % (idx, ext))
        with zipfile.ZipFile(p, "w", zipfile.ZIP_DEFLATED) as z:
            for name, body in entries.items():
                z.writestr(name, body.replace("{kw}", kw))
        add(ext, kw, p)

    # 3) 特殊构造
    idx += 1; kw = "zzkw%03d" % idx
    p = os.path.join(A, "s%03d.pdf" % idx)
    write(p, make_pdf(kw + " PDF body")); add(".pdf", kw, p)

    idx += 1; kw = "zzkw%03d" % idx
    p = os.path.join(A, "s%03d.mat" % idx)
    write(p, make_mat("kwvar%03d" % idx, kw + " 矩阵字符串")); add(".mat", kw, p)

    idx += 1; kw = "zzkw%03d" % idx
    p = os.path.join(A, "s%03d.mph" % idx)
    write(p, make_mph(kw)); add(".mph", kw, p)

    idx += 1; kw = "zzkw%03d" % idx
    p = os.path.join(A, "s%03d.eml" % idx)
    write(p, make_eml(kw)); add(".eml", kw, p)

    idx += 1; kw = "zzkw%03d" % idx
    p = os.path.join(A, "s%03d.msg" % idx)
    write(p, make_msg(kw)); add(".msg", kw, p)

    # 7-Zip 只能建 .7z，不能建 .rar —— rar 用真实文件测（见 REAL 表）
    for ext in (".7z",):
        idx += 1; kw = "zzkw%03d" % idx
        p = os.path.join(A, "a%03d%s" % (idx, ext))
        if make_archive(ext, kw, p):
            add(ext, kw, p)
        else:
            notes.append((ext, "7-Zip 不可用，未测"))

    # 4) 真实文件（关键词从抽取结果里取）
    for ext, src in REAL.items():
        if not os.path.exists(src):
            notes.append((ext, "本机没有样本"))
            continue
        dst = os.path.join(A, "r%s" % ext)
        shutil.copy2(src, dst)
        txt = extracted_text(dst)
        kw = pick_keyword(txt)
        if not kw:
            notes.append((ext, "文件在，但抽取为 0 字节"))
            continue
        add(ext, kw, dst)
        if ext == ".msg":
            notes.append((ext, "无真实 .msg，用自造 UTF-16 样本（仅测兜底路径）"))

    # 5) 图片（可选）
    if ocr:
        for ext in (".png", ".jpg", ".bmp"):
            idx += 1; kw = "zzkw%03d" % idx
            p = os.path.join(A, "i%03d%s" % (idx, ext))
            if make_image(p, kw):
                add(ext, kw, p)
            else:
                notes.append((ext, "图片生成失败"))

    # B 目录：两条对照（验证"换个文件夹"）
    cases_b = []
    for i, ext in [(1, ".txt"), (2, ".docx")]:
        kw = "zzkb%03d" % i
        p = os.path.join(B, "b%03d%s" % (i, ext))
        if ext == ".txt":
            write(p, ("%s B目录文本\n" % kw).encode())
        else:
            with zipfile.ZipFile(p, "w") as z:
                z.writestr("word/document.xml", "<w:document><w:t>%s B目录 docx</w:t></w:document>" % kw)
        cases_b.append((ext, kw, p))

    ma = os.path.join(ROOT, "build", "_manifest_a.tsv")
    mb = os.path.join(ROOT, "build", "_manifest_b.tsv")
    with open(ma, "w", encoding="utf-8") as f:
        for e, k, p in cases_a:
            f.write("%s\t%s\t%s\n" % (e, k, p))
    with open(mb, "w", encoding="utf-8") as f:
        for e, k, p in cases_b:
            f.write("%s\t%s\t%s\n" % (e, k, p))

    print("样本 %d 个（A）+ %d 个（B）" % (len(cases_a), len(cases_b)))
    # 索引隔离到临时目录：否则 SearchAll 会把用户真实的全盘索引也查一遍
    env = dict(os.environ)
    env["SPACESIFT_DATA_DIR"] = os.path.join(os.path.dirname(A), "data")
    if ocr:
        env["SPACESIFT_OCR"] = "1"
    r = subprocess.run([TOOL, A, B, ma, mb], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", env=env)
    out = r.stdout or ""
    err = r.stderr or ""
    report(out, err, notes)
    return 0


def make_image(path: str, kw: str) -> bool:
    ps = (
        "Add-Type -AssemblyName System.Drawing;"
        "$bmp = New-Object System.Drawing.Bitmap 600,120;"
        "$g = [System.Drawing.Graphics]::FromImage($bmp);"
        "$g.Clear([System.Drawing.Color]::White);"
        "$f = New-Object System.Drawing.Font 'Arial',32;"
        "$g.DrawString('%s', $f, [System.Drawing.Brushes]::Black, 10, 40);"
        "$bmp.Save('%s');" % (kw, path)
    )
    r = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True)
    return os.path.exists(path)


def report(out: str, err: str, notes) -> None:
    rows = []
    leaks = []
    mode = None
    for line in out.splitlines():
        if line == "===ROWS===":
            mode = "rows"; continue
        if line == "===CROSS===":
            mode = "cross"; continue
        if line == "===DONE===":
            break
        if line.startswith("==") or line.startswith("!!"):
            print(line)
            continue
        if mode == "cross" and line.startswith("LEAK"):
            leaks.append(line); continue
        if mode == "rows":
            p = line.split("\t")
            if len(p) >= 8:
                rows.append(p)

    print("\n%-8s %-6s %-6s %-8s %-8s %s" % ("格式", "入库", "正文", "当前目录", "跨库", "关键词"))
    bad = []
    for tag, ext, db, tlen, cur, allh, kw, path in rows:
        ok_db = db != "0"
        ok_txt = int(tlen) > 0
        ok_cur = cur == "1"
        ok_all = allh == "1"
        flag = "OK " if (ok_db and ok_txt and ok_cur and ok_all) else "!! "
        if not (ok_db and ok_txt and ok_cur and ok_all):
            bad.append((ext, db, tlen, cur, allh, kw, path))
        print("%s%-8s %-6s %-6s %-8s %-8s %s" % (flag, ext, "有" if ok_db else "无",
                                                 "有" if ok_txt else "无",
                                                 "yes" if ok_cur else "NO",
                                                 "yes" if ok_all else "NO", kw))

    print("\n合计 %d 项，失败 %d 项" % (len(rows), len(bad)))
    if leaks:
        print("\n【当前目录串库（不该命中却命中）】")
        for l in leaks:
            print(" ", l)
    if notes:
        print("\n【未能测试 / 备注】")
        for ext, why in notes:
            print("  %-8s %s" % (ext, why))
    if err.strip():
        print("\n[stderr]\n" + err[:1500])


if __name__ == "__main__":
    sys.exit(main())
