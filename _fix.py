# -*- coding: utf-8 -*-
import os, shutil, json

base = os.path.dirname(os.path.abspath(__file__))
dst_dir = r'E:\Program Files\Game-Related\FFXIV_MOD选项汉化工具_AI翻译_解压即用版'
cfg = os.path.join(dst_dir, 'config.json')
exe_src = os.path.join(base, 'release', 'FFXIV_MOD选项汉化工具.exe')
exe_dst = os.path.join(dst_dir, 'FFXIV_MOD选项汉化工具.exe')

# 1) 修正 config.json：API 地址改为智谱（key/模型是智谱，DeepSeek 地址必然 401）
with open(cfg, 'r', encoding='utf-8-sig') as f:
    j = json.load(f)
j['aiBaseUrl'] = 'https://open.bigmodel.cn/api/paas/v4'
with open(cfg, 'w', encoding='utf-8') as f:
    json.dump(j, f, ensure_ascii=False, indent=2)
print('config fixed: aiBaseUrl =', j['aiBaseUrl'], '| aiModel =', j['aiModel'])

# 2) 更新 exe
shutil.copy2(exe_src, exe_dst)
print('exe updated:', exe_dst, os.path.getsize(exe_dst))
