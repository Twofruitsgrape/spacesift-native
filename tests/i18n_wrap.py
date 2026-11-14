# -*- coding: utf-8 -*-
"""把源码里的中文 UI 文案包上 TR("中","英")。

规则：
- 只处理代码行里的窄字符串字面量（跳过注释行、跳过 L"..." 宽字符串）
- 精确匹配翻译表里的整条字符串，避免子串误伤
- 调试串（[reveal]/[skip]）与 selftest 不在表内，天然跳过
"""
import io, re, sys

FILES = ['src/main.cpp', 'src/rules.cpp', 'src/content.cpp', 'src/db.cpp']

T = {
# ---- main.cpp: 通用 ----
'(未知)': '(unknown)',
'选择要扫描的文件夹': 'Choose a folder to scan',
'路径为空': 'Path is empty',
'已在资源管理器中定位': 'Revealed in Explorer',
'已打开所在目录（未能选中文件：': 'Opened containing folder (could not select file: ',
' 管理员模式': ' (admin)',
'失败诊断:': 'Diagnostics: ',
'权限被拦(0x80070005)：': 'Access denied (0x80070005): ',
'以管理员运行时打不开，试试普通权限启动': 'Cannot open while elevated; try launching without admin',
'右键这条 → 以管理员身份打开所在位置': 'Right-click this entry → "Open containing folder (as admin)"',
'已打开目录': 'Folder opened',
'打开目录失败(': 'Failed to open folder (',
'文件已不存在，已打开所在目录': 'File no longer exists; opened its folder',
'打开资源管理器失败(': 'Failed to open Explorer (',
'找不到该文件，所在目录也不存在': 'File not found and its folder is gone',
'打开所在位置': 'Open containing folder',
'以管理员身份打开所在位置': 'Open containing folder (as admin)',
'已请求管理员权限，确认后会自动定位': 'Admin permission requested; it will reveal after you confirm',
'复制路径': 'Copy path',
'路径已复制': 'Path copied',
'复制失败': 'Copy failed',
'从这里开始扫描': 'Scan from here',
# ---- 扫描与索引状态 ----
'索引随扫描建立中…': 'Building file index during scan…',
'索引建立失败（不影响扫描）': 'File index failed (scan unaffected)',
'索引只建了一部分，下次会重来': 'File index partially built; it will retry next time',
'索引已就绪 · ': 'File index ready · ',
' 目录 + ': ' folders + ',
' 文件': ' files',
'索引换名失败（可能有另一个窗口在用）': 'Failed to rename index (possibly in use by another window)',
'正在检查可清理项…': 'Checking cleanable items…',
'检查失败': 'Check failed',
'选中项都不在当前扫描范围内，已全部跳过（先扫描它所在的目录）': 'None of the selected items are in the scanned range; all skipped (scan their folder first)',
'没有可删除的项（可能都不在当前扫描范围内）': 'Nothing to delete (items may be outside the scanned range)',
'已跳过 ': 'Skipped ',
' 项范围外的路径，只删当前扫描范围内的': ' item(s) outside the scanned range; only in-range items are deleted',
'（其余 ': ' (plus ',
' 项）': ' more)',
'%s\\n共 %s\\n点击展开并查看列表': '%s\\n%s total\\nClick to expand and view the list',
'%s\\n%s\\n点击在资源管理器中显示': '%s\\n%s\\nClick to show in Explorer',
# ---- 首屏 ----
'正在扫描…': 'Scanning…',
'先选一个位置': 'Pick a location first',
'上面选磁盘、粘贴路径，或点「浏览...」挑文件夹，然后点「扫描」': 'Pick a disk or paste a path above (or click Browse...), then click Scan',
'开始扫描': 'Start a scan',
'这个目录下没有可显示的子项': 'No displayable items in this folder',
# ---- 检索消息 ----
'还没有任何内容索引：先在右栏点「建立内容索引」': 'No content index yet: click "Build content index" in the right panel',
'」还没有内容索引：右栏点「建立内容索引」，': '" has no content index: click "Build content index" in the right panel, ',
'或把范围切到「所有已索引」': 'or switch the scope to "All indexed"',
'所有已建索引的目录里都没有匹配「': 'No content matches "',
'」的内容': '" in any indexed folder',
'已在全部索引里搜到 ': 'Found ',
' 条': ' result(s) across all indexes',
'所有已建索引的目录里都没有文件名包含「': 'No file name contains "',
'」的项': '" in any indexed folder',
'」还没扫描过，没有文件名索引': '" has never been scanned; no file-name index',
'扫描进行中，索引正在重建，等扫描结束后再搜索': 'Scan in progress; index rebuilding — search again after it finishes',
# ---- 范围与索引管理 ----
'当前目录': 'Current folder',
'所有已索引': 'All indexed',
'范围': 'Scope',
'索引管理 · %d 份 · %s': 'Index manager · %d database(s) · %s',
'还没有任何内容索引。': 'No content index yet.',
'  %lld 个文档 · %s%s%s': '  %lld docs · %s%s%s',
' · 未建完': ' · partial',
' · 需重建': ' · rebuild needed',
'删除': 'Delete',
'已删除索引：': 'Deleted index: ',
# ---- 建索引进度 ----
'① 清点文件': '① Counting files',
'② 抽取正文': '② Extracting text',
'约剩 -- 分 -- 秒': '~--m --s left',
'约剩 %d 时 %02d 分': '~%dh %02dm left',
'约剩 %d 分 %02d 秒': '~%dm %02ds left',
'统计可抽正文的文件': 'Counting extractable files',
'已处理 %d / %d     新抽取 %d · 复用 %d': 'Processed %d / %d     new %d · reused %d',
'已清点 %d 个候选     已抽正文 %d 个': 'Counted %d candidates     extracted %d',
'最近抽取': 'Recently extracted',
'停止': 'Stop',
'停下的部分会保留，下次点「增量更新」接着做': 'Finished parts are kept; "Update" resumes next time',
# ---- 内容页签 ----
'「%s」还没有内容索引，所以刚才那次搜索什么也没返回。': '"%s" has no content index, so the search returned nothing.',
'当前目录还没有内容索引。': 'The current folder has no content index yet.',
'只抽文本/代码、Office、PDF、图纸、三维模型；': 'Extracts text/code, Office, PDF, drawings, 3D models; ',
'建一次，之后按大小和时间增量更新。': 'build once, then incremental updates by size and mtime.',
'建立内容索引': 'Build content index',
'扫描进行中，等扫描结束后再建。': 'Scan in progress; wait for it to finish.',
'当前缺 %d 项可选依赖：PDF/DWG/OCR 里对应格式搜不到，': 'Missing %d optional dependency(ies): the matching formats in PDF/DWG/OCR are not searchable, ',
'其余格式不受影响（详见 USAGE.md）。': 'other formats unaffected (see USAGE.md).',
'内容索引就绪。在上面搜索框输入关键词即可搜正文。': 'Content index ready. Type keywords above to search inside files.',
'扫描 %d 个文件 → 可抽正文 %d 个（其余是依赖目录、不支持的格式、未开 OCR 的图片）': 'Scanned %d files → %d extractable (rest: dependency folders, unsupported formats, images without OCR)',
'语法：词A 词B（同时含）· A OR B · -排除 · ext:pdf · name:报告 · size:>10m · after:2024-06': 'Syntax: wordA wordB (AND) · A OR B · -exclude · ext:pdf · name:report · size:>10m · after:2024-06',
'本次：新抽取 %d 个，复用上次结果 %d 个。': 'This run: %d newly extracted, %d reused.',
'增量更新': 'Update',
'提示：同目录放一个 pdfium.dll 才能搜 PDF 正文。': 'Tip: put pdfium.dll next to the exe to search PDF text.',
'提示：要搜 .dwg 需要 dwg2dxf.exe + libredwg-0.dll 或 ODA File Converter。': 'Tip: searching .dwg needs dwg2dxf.exe + libredwg-0.dll or ODA File Converter.',
'提示：.doc/.ppt/.xls 走原生解析；装 catdoc/LibreOffice 对复杂文件更稳。': 'Tip: .doc/.ppt/.xls use built-in parsing; catdoc/LibreOffice helps with complex files.',
'提示：.7z/.rar 需要安装 7-Zip 或 WinRAR。': 'Tip: searching .7z/.rar requires 7-Zip or WinRAR.',
'OCR 图片：%s': 'OCR images: %s',
'开': 'On',
'关': 'Off',
'设置已改变 —— 点下面的「增量更新」重新索引。': 'Setting changed — click "Update" below to re-index.',
'（扫描件 PDF 需装 Tesseract-OCR 才会自动识别）': '(scanned PDFs need Tesseract-OCR for auto recognition)',
'扫描件 PDF 自动识别（每次最多 120 页）；这个开关只管图片。': 'Scanned PDFs auto-recognize (up to 120 pages per run); this toggle is for images only.',
# ---- 检索中/结果 ----
'正在搜索…': 'Searching…',
'逐个查所有已建索引的目录，先出结果先算完': 'Querying every indexed folder; results stream in as each finishes',
'正在查当前目录的内容索引': 'Querying content index of the current folder',
'逐个查所有已建索引的目录': 'Querying every indexed folder',
'正在查当前目录的文件名索引': 'Querying file-name index of the current folder',
'没有文件内容匹配「%s」。': 'No file content matches "%s".',
'想连之前扫过的其他目录一起搜，点上面的「所有已索引」。': 'To also search previously scanned folders, switch to "All indexed" above.',
'内容命中 %d 项（按相关度排序，勾选可加入待清理）': '%d content hits (by relevance; check items to add to cleanup)',
'%d 项在扫描范围外（带锁，不可勾选），扫它所在目录后可清理': '%d item(s) outside scanned range (locked, not selectable); scan their folder to clean them',
'范围外': 'Out of scope',
'\\n不在当前扫描范围内，勾选与清理会被跳过': '\\nOutside the scanned range; selection and cleanup are skipped',
# ---- 页签与右栏 ----
'推荐清理': 'Recommended',
'深度清理': 'Deep clean',
'检索结果': 'Search results',
'全选安全项': 'Select safe items',
'取消全选': 'Deselect all',
'还没有检查': 'Not checked yet',
'扫描一个目录后，这里会列出当前目录下可清理的缓存项。': 'After scanning a folder, cleanable cache items are listed here.',
'缓存规则只覆盖系统和应用的缓存目录。当前目录下还有 %d 个直接文件，': 'Cache rules only cover system/app cache folders. This folder also has %d loose files; ',
'可以在「深度清理」里看。': 'see them under "Deep clean".',
'缓存规则只覆盖系统和应用的缓存目录。': 'Cache rules only cover system/app cache folders.',
'这里没有可清理的缓存项': 'No cleanable cache items here',
'超时': 'Timed out',
'需确认': 'Confirm needed',
'正在扫描': 'Scanning',
'扫完这里会列出当前目录下的每一项。': 'After the scan, every item in this folder is listed here.',
'还没有内容': 'Nothing here yet',
'先在上面选一个目录扫描，这里会按大小列出子项。': 'Scan a folder above first; its children will be listed by size here.',
'%d 个子目录 · %d 个文件 · 合计 %s（勾选可加入待清理）': '%d folders · %d files · %s total (check items to add to cleanup)',
'…文件较多，仅列出最大的 %d 个（共 %d 个）': '…Too many files; showing the largest %d of %d',
'还没有搜索': 'No search yet',
'在上面搜索框输入文件名，回车或点搜索。Ctrl+F 可以直接跳过去。': 'Type a file name above, press Enter or click Search. Ctrl+F jumps to the box.',
'没有命中': 'No matches',
'所有已建索引的目录里都没有匹配的文件名。': 'No file name matches in any indexed folder.',
'换个关键词试试，或者把上面的范围切成「所有已索引」。': 'Try another keyword, or switch the scope above to "All indexed".',
'命中 %d 项（按大小排序，勾选可加入待清理）': '%d hits (by size; check items to add to cleanup)',
'%d 项': '%d items',
# ---- 清理确认 ----
'清理中': 'Cleaning…',
'清理选中项': 'Clean selected items',
'确认删除这些内容': 'Confirm deletion',
'推荐清理 %d 项 · 深度勾选 %d 项 · 检索勾选 %d 项 · 合计 %s': 'Recommended %d · deep %d · search %d · total %s',
'还有 %d 项未列出': '%d more not listed',
'直接删除，不进回收站，无法撤销。': 'Files are deleted directly — not to Recycle Bin, unrecoverable.',
'取消': 'Cancel',
'确认删除': 'Delete',
# ---- 状态栏 ----
'开始扫描，内容索引已停止（已完成的部分保留，稍后可「增量更新」）': 'Scan started; content indexing stopped (finished parts kept; "Update" resumes later)',
'磁盘空间分析  v': 'Disk Space Analyzer  v',
'已扫 %s · %lld 文件': 'Scanned %s · %lld files',
'%lld 文件 · %s · %.1f 秒': '%lld files · %s · %.1f s',
'就绪 · 选一个位置开始': 'Ready · pick a location to start',
'正在清理 ': 'Cleaning ',
'扫描中 · ': 'Scanning · ',
' 目录 · ': ' folders · ',
'建立内容索引 · 已写入 ': 'Building content index · ',
' 个': ' written',
'扫描完成': 'Scan complete',
# ---- 顶栏控件 ----
'文件名': 'File name',
'内容': 'Content',
'亮色': 'Light',
'暗色': 'Dark',
'选择磁盘': 'Select disk',
'浏览': 'Browse',
'继续': 'Resume',
'暂停': 'Pause',
'结束中': 'Stopping',
'结束': 'Stop',
'扫描': 'Scan',
'搜索文件内容': 'Search file content',
'搜索文件名': 'Search file names',
'搜索': 'Search',
# ---- 清理结果 ----
'已清理 ': 'Cleaned ',
' 个文件被占用，已跳过）': ' file(s) in use, skipped)',
'；另有 ': '; plus ',
' 项在扫描范围外，未处理': ' item(s) out of scope, untouched',
' · 重新检测中，方块图下次扫描后更新': ' · re-checking; treemap updates on next scan',
# ---- 工具条/右键菜单 ----
'筛选': 'Filter',
'全部': 'All',
'上一层': 'Up one level',
'回到根': 'Back to root',
'Direct3D 初始化失败': 'Direct3D initialization failed',
# ---- rules.cpp ----
'用户临时文件': 'User temp files',
'程序和安装包留下的临时文件，重启后大多已经没用。': 'Temp files left by apps and installers; mostly useless after a reboot.',
'缩略图缓存': 'Thumbnail cache',
'资源管理器的缩略图数据库，会自动重建。': "Explorer's thumbnail database; rebuilt automatically.",
'崩溃转储': 'Crash dumps',
'程序崩溃时写下的转储文件，排查完就没用了。': 'Dump files written on crashes; useless once investigated.',
'pip 缓存': 'pip cache',
'下载过的 wheel 包缓存，重装时会重新下载。': 'Downloaded wheel cache; re-downloaded when reinstalling.',
'uv 缓存': 'uv cache',
'uv 的包缓存，可用 uv cache clean 重建。': 'uv package cache; rebuild with "uv cache clean".',
'npm 缓存': 'npm cache',
'npm 的本地包缓存。': 'npm local package cache.',
'Chrome 缓存': 'Chrome cache',
'网页缓存，删除后首次访问会稍慢，不影响登录状态。': 'Web cache; first visits get slower, logins unaffected.',
'Edge 缓存': 'Edge cache',
'网页缓存，删除后首次访问会稍慢。': 'Web cache; first visits get slower.',
'Electron 应用缓存': 'Electron app caches',
'VS Code、Cursor、Discord 等应用的缓存与日志。': 'Caches and logs of VS Code, Cursor, Discord, etc.',
'回收站': 'Recycle Bin',
'清空回收站。删掉的文件就找不回来了。': 'Empty the Recycle Bin. Deleted files cannot be recovered.',
'Windows 临时目录': 'Windows temp folder',
'系统与安装程序共用的临时目录，需要管理员权限。': 'Shared system temp folder; requires admin rights.',
'Windows 更新下载缓存': 'Windows Update download cache',
'已安装更新的安装包缓存，需要管理员权限。': 'Installer cache of installed updates; requires admin rights.',
'Windows 错误报告': 'Windows Error Reporting',
'错误报告队列，需要管理员权限。': 'Error report queue; requires admin rights.',
'Chrome 端侧 AI 模型': 'Chrome on-device AI models',
'Chrome 内置的端侧 AI 模型，每份约 4 GB，多个浏览器副本各存一份。删了下次用到会重新下载。': 'Chrome built-in on-device AI models, ~4 GB each, one copy per browser profile. Re-downloaded when needed.',
'pnpm 全局存储': 'pnpm global store',
'pnpm 用硬链接共享这个存储，删了已装项目的依赖会一起坏。': 'pnpm hard-links this store; deleting it breaks installed projects.',
'组件存储备份 (WinSxS)': 'Component store backup (WinSxS)',
'微软建议用 DISM 而不是手删；手删会破坏组件存储。': 'Microsoft recommends DISM; manual deletion corrupts the component store.',
'Windows 日志': 'Windows logs',
'系统日志文件，排查问题时会用到。': 'System log files; useful when troubleshooting.',
# ---- content.cpp ----
'抽取参数已升级，旧正文索引作废，本次重新抽取': 'Extractor upgraded: old text index invalidated, rebuilding',
'无法创建内容索引库': 'Failed to create content index database',
'索引已停止，已完成的部分保留': 'Indexing stopped; finished parts are kept',
'索引已停止，已完成的部分保留，下次点「增量更新」接着做': 'Indexing stopped; finished parts kept. "Update" resumes next time',
'索引未建立': 'Index not built',
}

PAT = re.compile(r'"((?:[^"\\]|\\.)*)"')
LIT_RE = re.compile(r'(?:^|[^\\w"FLuU])"((?:[^"\\]|\\.)*)"', re.I)

def wrap(match, prev_char):
    if prev_char in 'FLUu':  # 宽字符串/原始串，跳过
        return match.group(0)
    txt = match.group(1)
    if txt in T:
        return 'TR("%s", "%s")' % (txt, T[txt])
    return match.group(0)

total = 0
for f in FILES:
    lines = io.open(f, encoding='utf-8').read().split('\n')
    n = 0
    for i, line in enumerate(lines):
        stripped = line.lstrip()
        if stripped.startswith('//'): continue
        out, last = [], 0
        for m in re.finditer(r'(?<![\\wFLuU"])"((?:[^"\\]|\\.)*)"', line):
            txt = m.group(1)
            if txt in T:
                out.append((m.start(), m.end()))
        if out:
            new, prev = [], 0
            for a, b in out:
                new.append(line[prev:a]); prev = b
                zh = line[a+1:b-1]
                new.append('TR("%s", "%s")' % (zh, T[zh]))
            new.append(line[prev:])
            lines[i] = ''.join(new)
            n += 1
    if n:
        io.open(f, 'w', encoding='utf-8', newline='').write('\n'.join(lines))
    print(f, n, 'lines changed')
    total += n
print('total lines:', total)

# 校验：还有多少未覆盖的中文字符串（排除注释/调试）
import json
miss = {}
for f in FILES:
    for i, line in enumerate(io.open(f, encoding='utf-8').read().split('\n'), 1):
        s = line.strip()
        if s.startswith('//') or s.startswith('*'): continue
        code = line.split('//')[0]
        if '[reveal]' in code or '[skip]' in code: continue
        for m in re.finditer(r'(?<![\\wFLuU"])"([^"\n]*[一-龥][^"\n]*)"', code):
            miss.setdefault(m.group(1), []).append('%s:%d' % (f, i))
print('未覆盖:', len(miss))
for k in list(miss)[:20]: print('  ', repr(k), miss[k][:3])
