# -*- coding: utf-8 -*-
import os, shutil

base = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(base, 'release', 'FFXIV_MOD选项汉化工具.exe')
dst = r'E:\Program Files\Game-Related\FFXIV_MOD选项汉化工具_AI翻译_解压即用版\FFXIV_MOD选项汉化工具.exe'
try:
    shutil.copy2(src, dst)
    print('exe updated:', os.path.getsize(dst))
except Exception as e:
    print('FAIL:', e)
