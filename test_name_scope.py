"""文件名检索的作用域验收：当前目录 vs 所有已索引。"""
import os, sys, time, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_scope_ui as T

def run(tag, root, mode, scope_all, query, wait=7.0):
    T.kill(); time.sleep(1.0)
    env = dict(os.environ)
    env["SPACESIFT_ROOT"] = root
    env["SPACESIFT_AUTOSCAN"] = "1"
    env["SPACESIFT_TAB"] = "2"
    env["SPACESIFT_SEARCH"] = query
    if mode == "content":
        env["SPACESIFT_MODE"] = "content"
    else:
        env.pop("SPACESIFT_MODE", None)
    if scope_all: env["SPACESIFT_SCOPE"] = "all"
    else: env.pop("SPACESIFT_SCOPE", None)
    subprocess.Popen([T.EXE], env=env, cwd=T.DIST, stdout=-3, stderr=-3)
    for _ in range(40):
        time.sleep(0.3)
        if T.find_window(): break
    time.sleep(wait)
    subprocess.run([sys.executable, "shot.py", "SpaceSift Native", f"{tag}.png"], cwd=T.ROOT, capture_output=True)
    print("  shot:", tag)
    T.kill(); time.sleep(0.6)

run("name_a_current", r"E:/others", "name", False, "报告")
run("name_b_all", r"E:/others", "name", True, "报告")
