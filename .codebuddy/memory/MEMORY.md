# 长期记忆

## FFXIV_MOD选项汉化工具

- 翻译规则同步约定（2026-08-30，用户明确要求）：凡修改 AI 翻译规则，必须同时同步两处——(1) 程序内置的 system prompt（`AITranslateBatch` 里的 `sysMsg`，约 2700 行附近）；(2) 写入 `_未翻译.json` 的「翻译规则」字段（`out["翻译规则"]`，约 648-659 行，这是给外部 AI 看的规则）。两处都要改，不能只改一处。
- 翻译规则相关：短名称用「中文（英文）」格式；无法翻译的专有名词/缩写/品牌名（如 EXQB、Uranus）原样保留英文，禁止输出「英文（英文）」重复格式；`fixRepeatParen()` 在 AI 译文写入前做兜底修正。
- 运行方式（2026-08-30 确认）：用户用 **Visual Studio 打开项目，Ctrl+Shift+B 重新生成后按 F5 运行**，实际运行的 exe 和配置在 `x64\Release\`（也可能 `x64\Debug\`）目录，**不是 E 盘部署目录**。因此：以后源码修改后无需往 E 盘部署 exe；改 `config.default.json` 后需同步到 `x64\Release\`（VS 项目已加 PostBuildEvent：`xcopy /Y /Q "$(ProjectDir)config.default.json" "$(OutDir)"`，用户每次重新生成会自动同步，无需手动）。`config.user.json`（用户 AI Key、词典/翻译/penumbra 目录、customSaves、aiPresets）在 x64\Release 目录，改它要小心程序退出时会用内存配置写回覆盖。
- 黑名单机制（2026-08-30）：来源 = 内置 `AppConfig.blacklist` 默认值（当前 `{"Uranus","EXQB","Yanilla","Rue","Lavabod"}`，即体型 mod 专名）+ config 各层 + 词典目录 `单词黑名单.json`（用户数据）。`LoadConfigFrom` 用户层黑名单**仅非空数组覆盖**（空=未设置，保留默认）。命中黑名单的英文：提取时跳过（子串匹配）、应用时跳过翻译并还原英文。**子串匹配有误伤风险**：`rue` 会命中 `true`/`screw` 等——用户 2026-08-30 明确要求把 Rue、Lavabod 写入默认黑名单（Rue 被 AI 误翻"芸香"的根源是词典 terms 里 `Rue: 芸香`，已改 `Rue: Rue`），用户已知情并接受子串误伤。**提取英文时会自动创建/导出 单词黑名单.json 到词典目录**（文件不存在时）。FFXIV 常见体型 mod 专名：Uranus Redux（Uranus）、EXQB、Yanilla、Rue、Lavabod（Lava-*：Cupcake/Teardrop/Omoi/Frazetta/Sugar）、YAB、YAMasc（Masc-Flat/Pecs，Masc=masculine 缩写但作为 YAMasc 简称有歧义，按专名保留英文），全是专名应保留英文。
- 打包约定（2026-08-30 更新）：**有功能性修改的新版本编译通过后自动打包** zip 到 `D:\Fast folder\Downloads\压缩文件`（重名追加 _0/_1 递增后缀，zip 内顶层目录为中文固定名「最终幻想14_mod选项汉化工具_AI翻译版」），并 git 本地提交；纯文字/UI 文案修改不打包不升版本号。GitHub 推送必须等用户明确说「推送」才执行。打包命令：`powershell -NoProfile -Command "& 'D:\Fast folder\Document\C++\*\build_release.ps1' -Zip"`（全 ASCII + 通配符定位中文目录）；git 提交信息走 `write_to_file` 写 UTF-8 到 `D:\Fast folder\Document\_scripts\gitmsg.txt` 再 `git commit -F`；git 在 workspace 根目录直接运行即可（无需 -C 中文路径）。

