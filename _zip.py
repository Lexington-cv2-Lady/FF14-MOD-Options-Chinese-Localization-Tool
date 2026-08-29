# -*- coding: utf-8 -*-
import os, zipfile, sys

base = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(base, 'release')
dst = r'D:\Fast folder\Downloads\压缩文件\FFXIV_MOD_Options_Chinese_AI-Translated_v2.1.6.zip'

if not os.path.isdir(os.path.dirname(dst)):
    print('目标目录不存在:', os.path.dirname(dst))
    sys.exit(1)

if os.path.exists(dst):
    os.remove(dst)

with zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED) as z:
    for name in sorted(os.listdir(src)):
        p = os.path.join(src, name)
        if os.path.isfile(p):
            z.write(p, name)

print('Created:', dst)
print('Size:', os.path.getsize(dst))
