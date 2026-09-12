# FFXIV_penumbra的模组汉化插件

> 卫月（Dalamud）插件：把 Penumbra 模组的英文选项、描述批量汉化成简体中文并写回模组文件，游戏内即时生效。

**AI 编写声明**：本插件代码由 AI 辅助编写，作者（Lexington-cv2-Lady）负责需求设计、逐项测试与验收，对应卫月官方 [AI Usage Policy](https://dalamud.dev/plugin-publishing/ai-policy) 的 Copilot 级 AI 使用。插件不采集任何遥测或用户数据，所有翻译仅在本地完成。

## 功能

- 读取 Penumbra 模组选项（`meta.json` / `group_*.json`，兼容新旧双格式）
- 词典汉化：我的翻译 / 个性翻译 / wiki 术语对照 / AI 知识库 / 单词黑名单
- AI 翻译：国内服务商优先，按平台自动拆批与输出上限
- 应用前自动备份（zip 轮转保留），一键恢复并保留 Penumbra 选项状态
- 纯中文输出、黑名单保留专名、「已翻译」标记自动跳过

## 安装

**自定义仓库安装（推荐）**

1. 打开卫月设置 → 「实验性」→ 自定义插件仓库
2. 添加仓库地址（任选其一）：
   - `https://raw.githubusercontent.com/Lexington-cv2-Lady/FFXIV_Penumbra_Mod_Chinese_Localization_Plugin/main/plugin_repo.json`
   - 镜像：`https://cdn.jsdelivr.net/gh/Lexington-cv2-Lady/FFXIV_Penumbra_Mod_Chinese_Localization_Plugin@main/plugin_repo.json`
3. 在插件安装器中搜索「模组汉化」安装

**手动安装**

1. 从 [Releases](https://github.com/Lexington-cv2-Lady/FFXIV_Penumbra_Mod_Chinese_Localization_Plugin/releases) 下载 `FFXIV-Penumbra-Mod-Localization.zip`
2. 解压到 `XIVLauncherCN\plugins\FFXIV_penumbra的模组汉化插件\`
3. 重启游戏或重新加载卫月

## 使用流程

① 提取英文 → ② 预翻译 → ③ AI 翻译 → ④ 汇总已翻译内容 → ⑤ 翻译写入MOD

## 数据目录

| 目录 | 用途 |
|---|---|
| 词典目录（默认 `E:\<MOD_ROOT>\词典目录`） | 我的翻译 / 个性翻译 / 单词黑名单 / wiki_术语对照 / AI知识库 |
| 翻译目录（默认 `E:\<MOD_ROOT>\翻译目录`） | AI 翻译管线的 `_未翻译.json` / `_已翻译.json` |
| 模组根目录（Penumbra 数据目录） | `E:\<MOD_ROOT>\penumbra` |

## 构建与打包

```powershell
$env:DALAMUD_HOME = "$env:APPDATA\XIVLauncherCN\addon\Hooks\dev"
dotnet build "FFXIV_penumbra的模组汉化插件\FFXIV_penumbra的模组汉化插件.csproj" -c Debug
```

打包：将 `bin\Debug` 中的 dll、json（manifest）与依赖一并压缩为 zip 后发布到 Releases。
