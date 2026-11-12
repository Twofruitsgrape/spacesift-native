"""干净的 A/B：空框打 MVR（不点按钮，应无结果）→ 点搜索按钮（应出结果）。"""
import os
import subprocess
import sys
import time

from winmsg import App

env = dict(os.environ)
env["SPACESIFT_ROOT"] = r"D:\cigarette"
env["SPACESIFT_AUTOSCAN"] = "1"
env["SPACESIFT_MODE"] = "content"
env["SPACESIFT_CONTENT"] = "1"
env["SPACESIFT_TAB"] = "2"

p = subprocess.Popen([os.path.abspath("build/SpaceSift.exe")], env=env)
time.sleep(26)

app = App(pid=p.pid)
print("client:", app.client_size())

# A: 点进搜索框，打字，不按回车、不点按钮
app.click(950, 30)
app.text("MVR")
time.sleep(1.0)
subprocess.run([sys.executable, "shot.py", "SpaceSift", "ab_before.png"], timeout=60)
print("A: typed, no search")

# B: 点「搜索」按钮
app.click(1140, 30, settle=2.5)
subprocess.run([sys.executable, "shot.py", "SpaceSift", "ab_after.png"], timeout=60)
print("B: button clicked")

p.terminate()
