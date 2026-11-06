# 从 labwc 的 Xwayland root tree 里抽出真实顶层窗口，生成 TSV 喂给新桥。
# 格式: xid \t title \t width \t height
import re
import subprocess

out = subprocess.check_output(["xwininfo", "-root", "-tree"], text=True, errors="replace")
for line in out.splitlines():
    m = re.match(r'\s*(0x[0-9a-fA-F]+)\s+"([^"]*)".*?\s(\d+)x(\d+)[+-]', line)
    if not m:
        continue
    xid, title, w, h = m.group(1), m.group(2), int(m.group(3)), int(m.group(4))
    if not title or w < 100 or h < 100:
        continue
    print("\t".join([xid, title, str(w), str(h)]))
