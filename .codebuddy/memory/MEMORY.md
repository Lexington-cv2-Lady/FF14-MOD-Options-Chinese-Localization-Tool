# 长期记忆

## FFXIV_MOD选项汉化工具

### 版本与构建工作流
- 运行方式（2026-08-30 确认）：VS 打开项目 Ctrl+Shift+B 生成后 F5，运行 exe 与配置在 `x64\Release\`（**非 E 盘部署目录**）。config.default.json 由 PostBuildEvent 自动 xcopy 到 OutDir。config.user.json（AI Key/词典/翻译/penumbra 目录/customSaves/aiPresets）在 x64\Release，程序退出会用内存配置写回覆盖，改它要小心。
- 打包约定（2026-08-30，v2.3.2 修正）：**有功能性修改编译通过后自动打包** zip 到 `D:\Fast folder\Downloads\压缩文件` 并 git 本地提交；纯文字/UI 文案修改不打包不升版本号。**版本号：功能修改升 0.1（"每次 10 进 1"），不得用 `_0` 后缀代替；`_0/_1` 仅同版本重复打包防覆盖**。zip 顶层目录中文固定名「最终幻想14_mod选项汉化工具_AI翻译版」。打包命令：`powershell -NoProfile -Command "& 'D:\Fast folder\Document\C++\*\build_release.ps1' -Zip"`（全 ASCII + 通配符定位中文目录）。git 提交信息写 UTF-8 到 `D:\Fast folder\Document\_scripts\gitmsg.txt` 再 `git commit -F`；git 在 workspace 根直接运行。
- **GitHub 推送必须等用户明确说「推送」/「push」**；推送命令必须带代理：`git -c http.proxy=http://127.0.0.1:7890 push origin main`（直连报 Connection was reset）；仓库 `Lexington-cv2-Lady/FF14-MOD-Options-Chinese-Localization-Tool`（Private）。
- 内置文件与个性翻译模板（v2.3.3 起在 exe 旁「内置模板」子文件夹）：内置wiki_术语对照.json、内置个性翻译.json（预填 FF14 官方种族译法 Hyur=人族/Elezen=精灵族/Lalafell=拉拉菲尔族/Miqo'te=猫魅族/Roegadyn=鲁加族/Au Ra=敖龙族/Hrothgar=硌狮族/Viera=维埃拉族 + 「格式参考」说明 key，中文 key 永不命中无副作用）；EnsureCustomDictFile 首次创建 个性翻译.json 复制模板，**已存在绝不覆盖（含空 `{}`，残留空文件会堵住模板）**；release/ 被 .gitignore 忽略；**zip 打包隐藏文件必须用 .NET `[IO.Compression.ZipFile]::CreateFromDirectory`（先 `Add-Type -AssemblyName System.IO.Compression.FileSystem`）——Compress-Archive 会跳过隐藏文件**；stage 复制须 `Get-ChildItem -Force | Copy-Item`。vcxproj PostBuildEvent：mkdir + xcopy /Y /Q /H 到 OutDir 内置模板（$builtinDirName=0x5185,0x7F6E,0x6A21,0x677F）。
- 部署目录（2026-08-30）：解压即用版部署到 `E:\Program Files\Game-Related\最终幻想14_mod选项汉化工具_AI翻译版`；更新只覆盖 exe、config.default.json、使用说明.txt、内置模板，保留 config.user.json、日志.json。
- 执行器注意（2026-08-30 实测）：write_to_file 写 workspace 外 `D:\Fast folder\Document\_scripts\*.ps1` 在 execute_command 视角内容为空、cz.ps1 -File 流程不生效、execute_command 命令行 `$` 会被剥——凡需脚本处理中文路径/变量的逻辑，写进 workspace 内 build_release.ps1（码点构造中文名）或避免命令行 `$`。

### UI/交互规范
- 二级窗口交互（v2.3.7 更新，覆盖旧 v2.3.5/v2.3.6）：「提取英文」「恢复备份」「翻译缺失」「Wiki 分类」「导入翻译」统一——左 ListView（LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT）：**单击/双击行 = 勾选 toggle**，快速连点也可取消（第二次是 NM_DBLCLK，若在 NM_DBLCLK 强制勾 TRUE 会取消不了，v2.3.7 统一 toggle 修复）；checkbox 区原生处理；LVN_ITEMCHANGED 只刷右侧。右列表同理 toggle。底部「全选」「取消全选」只有一组（v2.3.8 合并）：「全选」先勾左侧（触发右侧刷新）再勾右侧；「取消全选」先取消右侧再取消左侧。主操作按钮对未勾选场景退回高亮选中项。AI 配置列表（单选）例外：NM_DBLCLK=确认套用。模态窗口相对主窗居中（CenterDialogOnOwner，`or` 是 C++ 替代运算符不能作变量名）。新增控件 ID 三处同步：rc 字面量数字（防 IDE 回滚 Resource.h）、cpp 顶部 #ifndef 兜底宏、Resource.h 宏；ID 不得冲突（1348=IDC_BTN_CUSTOM）。ListView_SetCheckState 宏 if/else 必须加大括号（否则 C2181）；GetDlgItem 用 (int)nm->idFrom 防 C4244（要求 0 警告）。
- 模态窗口位置记忆（v2.3.10）：AppConfig.dlgPos（config.user.json "dlgPos" 数组）+ `PositionDialog(hDlg, dlgId)`（有保存位置且左上角在虚拟屏幕内则恢复，否则居中）+ `RememberDlgPos` + 全局 `g_activeDlgId`；7 个模态窗口 WM_INITDIALOG 用 PositionDialog、WM_DESTROY 记位置。恢复备份（v2.3.10）="恢复到备份时状态"：解压前先清空该文件夹所有 group_*.json 再解压。工具从不修改 group_*.json 文件名。
- 使用说明同步约定（v2.3.7 起用户明确要求）：凡改功能/界面文案必须同步①rc 界面文案；②README.md 使用说明+更新日志（build_release.ps1 从 README 首个 ### vX.Y.Z 解析 zip 版本号）；③release\使用说明.txt（随 zip 分发、不入 git、build_release.ps1 不覆盖）；④程序内置提示文案。漏一处=没改完。
- AI Key 套用行为（v2.3.2 修正）：套用 AI 配置时，预设/自定义记录**自带 Key 才填 API_Key 框，无 Key 则清空**（v2.2.6 曾改"保持当前 Key"，用户实测串 Key 要求改回）。

### 翻译与词典
- 翻译规则同步约定（2026-08-30 用户明确要求）：改 AI 翻译规则必须同步两处——①内置 system prompt（`AITranslateBatch` 的 `sysMsg`，约 2700 行）；②`_未翻译.json` 的「翻译规则」字段（约 648-659 行，给外部 AI 看）。短名称用「中文（英文）」；无法翻译的专名/缩写/品牌名（EXQB、Uranus 等）原样保留英文，禁止「英文（英文）」重复格式；`fixRepeatParen()` 写入前兜底。
- 黑名单机制（v2.2.10）：来源 = 内置默认 `{"Uranus","EXQB","Yanilla","Rue","Lavabod"}`（体型 mod 专名）+ config 各层 + 词典目录 单词黑名单.json；LoadConfigFrom 用户层**仅非空数组覆盖**。v2.2.9 起提取英文**不跳过黑名单词**；v2.2.10 起**命中 = 整词完全匹配**（trim 相等、忽略大小写）：单独 Lavabod/Rue 保留英文；组合词条不再整条被拦（专名由 AI 保留）；消除 rue→true 子串误伤。生效位置：AI 翻译整词命中原样保留英文、应用（词典写入Mod）时跳过并还原。**Masc 从不在黑名单**（靠词典 terms `"Masc":"Masc"`）。**AI 翻译预填链路优先级（v2.2.9）：黑名单 > 个性翻译.json > 汇总词典 > wiki 原典**。FFXIV 体型 mod 专名：Uranus Redux、EXQB、Yanilla、Rue、Lavabod（Lava-*：Cupcake/Teardrop/Omoi/Frazetta/Sugar）、YAB、YAMasc（Masc-Flat/Pecs），全保留英文。
- 查漏补缺黑名单联动（v2.3.16 确认）：CollectMissingEntries 用 is_blacklisted（整词匹配）过滤，整词命中黑名单不算缺失；含黑名单词的**组合**会列出属设计行为；不与翻译规则（system prompt/规则字段）联动；恢复备份后英文原文回归导致条目重现属正常。
- 目录确定即建文件+自动迁移（v2.2.8）：确定词典/翻译目录即建 汇总词典.json、个性翻译.json、单词黑名单.json、wiki_术语对照.json（翻译目录则建目录）；改目录自动迁移旧目录词典文件（4 个）与翻译产物，新目录已有同名不覆盖。实现：EnsureBlacklistFile()/MigrateDirFiles()/OnDictionaryDirChanged()。
- 「翻译缺失」窗口（IDD_MISSING_DIALOG=1270 系列）对接：CollectMissingEntries() 扫描 MOD，key 组装与 ExtractEnglish 一致（rel||Name||原文 / rel||Desc||原文 / rel||Opt||原文）；选中后生成 `查漏补缺_未翻译.json`（v2.3.15 起固定名，覆盖不积累）并指向 g_aiTargetFile；RunAITranslateThread 开头检测非空直接翻译（failPath 须在 if/else 外声明）。
- 个性翻译值格式偏好（2026-08-30）：词条值统一「中文(英文)」（英文半角括号紧贴，如 `"Hyur": "人族(Hyur)"`）。写入已存在的隐藏文件 `[IO.File]::WriteAllText` 可能 Access denied（本机实测），改用 `Copy-Item -Force` 覆盖后再设 Hidden。
- 恢复备份日志（v2.3.16）：IDC_RESTORE_BTN 处理输出 Log——开始（共 N 个文件夹）→ 每文件夹（清空 N 个 group_*.json、从 M 个备份包恢复、每个 zip 成功/失败，用 wstring_to_utf8 转中文名）→ 完成（成功/跳过/失败统计）。

## 系统问题排查备忘

### MSI Afterburner 窗口不可见（2026-08-31）
- 根因：Afterburner 是 High integrity（S-1-16-12288）+ WS_EX_LAYERED 窗口，Wallpaper Engine 运行时 GPU 反复 `DXGI device lost in render loop`（WE log.txt 可证），DWM 丢弃其分层窗口画面——窗口状态正常但内容不可见。WE apprules（stop 规则）格式被识别但不执行；普通权限对高完整性窗口操作被 UIPI 拒绝（LastError=5），管理员提权操作有效但 ABwake 唤醒方案用户实测无效，最终放弃并撤销全部修改、恢复默认。
- **关键新发现（用户实测）**：右键 WE 托盘「暂停」→ 小飞机窗口立即恢复可见。据此实现自动联动脚本 `C:\Tools\ABwallpaper_sync.ps1` + 计划任务 `ABwallpaperSync`（登录自启）：检测小飞机主窗口，非最小化（用户想显示）时自动 `wallpaper64.exe -control pause`，最小化/进程退出时 `-control play`。
- 保留的用户自启计划任务 `MSIAfterburner`：Action=`C:\Program Files (x86)\MSI Afterburner\MSIAfterburner.exe`，Args=`/s`（启动最小化到托盘），RunLevel=Highest——该任务本身是"开机窗口隐藏"的嫌疑点之一。
- 经验：进程正常但窗口不可见，优先怀疑 DWM/GPU 冲突（DXGI device lost、分层窗口）+ UIPI 完整性级别。
- 诊断技巧：完整性级别 `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION=0x1000)` + `GetTokenInformation(TokenIntegrityLevel=25)`，SID 在 buffer offset 0（offset 8 是 Attributes）；`Start-Process -Verb RunAs` 不能配 `-RedirectStandardOutput`（参数集冲突），提权脚本自己写结果文件再读回；WE 配置在安装目录顶层 config.json（.bak 为修改前备份）。
