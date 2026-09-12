using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace FFXIVPenumbraHanhua.Services;

/// <summary> 翻译写入MOD：按已加载的词典（我的翻译/个性翻译/wiki/AI知识库）直接写回模组文件。 </summary>
public sealed class ImportService
{
    private readonly ModFileService _files;
    private readonly PenumbraService _penumbra;
    private readonly AppLog _log;

    public string LastResult { get; private set; } = "";

    public ImportService(ModFileService files, PenumbraService penumbra, AppLog log)
    {
        _files = files;
        _penumbra = penumbra;
        _log = log;
    }

    /// <summary>
    /// 词典直写回（翻译写入MOD）：直接读取已加载的词典译文（我的翻译/个性翻译/wiki/AI知识库），
    /// 应用到指定模组的组名 / 选项名 / 描述，写回模组文件并重载。已含中文的原文不重复覆盖。
    /// </summary>
    public int ApplyDictionary(string modRoot, DictionaryService dict, IReadOnlyList<ModEntry> mods)
    {
        if (string.IsNullOrEmpty(modRoot) || !Directory.Exists(modRoot))
        {
            LastResult = "无法获取 Penumbra 模组根目录";
            return -1;
        }

        var totalWritten = 0;
        var totalBackups = 0;
        var errors = new List<string>();
        var reloaded = new HashSet<string>();
        var modBackedUp = new HashSet<string>();

        foreach (var mod in mods)
        {
            var modDirPath = Path.Combine(modRoot, mod.Directory);
            if (!Directory.Exists(modDirPath))
            {
                errors.Add(mod.Directory + "：目录不存在");
                continue;
            }

            var files = _files.ReadModFiles(modDirPath);
            if (files.Count == 0) continue; // 无 meta.json / group_*.json 的模组（本身没有选项）

            var wroteAny = false;
            foreach (var fileInfo in files)
            {
                var groupNames = new Dictionary<int, string>();
                var optionNames = new Dictionary<(int, int), string>();
                var optionDescs = new Dictionary<(int, int), string>();
                var fileName = Path.GetFileName(fileInfo.Path);

                foreach (var g in fileInfo.Groups)
                {
                    var gName = ApplyLookup(dict, fileName, "Name", g.Name);
                    if (gName != null) groupNames[g.Index] = gName;

                    foreach (var o in g.Options)
                    {
                        var oName = ApplyLookup(dict, fileName, "Opt", o.Name);
                        if (oName != null) optionNames[(g.Index, o.Index)] = oName;
                        var oDesc = ApplyLookup(dict, fileName, "Description", o.Description);
                        if (oDesc != null) optionDescs[(g.Index, o.Index)] = oDesc;
                    }
                }

                if (groupNames.Count == 0 && optionNames.Count == 0 && optionDescs.Count == 0)
                    continue;

                // 整个模组打 zip 备份一次（同一模组多文件只备一次）
                if (!modBackedUp.Contains(mod.Directory))
                {
                    var zip = _files.CreateModZip(modDirPath, _files.MaxBackups);
                    if (zip == null)
                    {
                        errors.Add(mod.Directory + "：备份失败，跳过写回");
                        continue;
                    }
                    modBackedUp.Add(mod.Directory);
                    totalBackups++;
                }

                if (_files.WriteTranslation(fileInfo.Path, fileInfo, groupNames, optionNames, optionDescs))
                {
                    totalWritten += groupNames.Count + optionNames.Count + optionDescs.Count;
                    wroteAny = true;
                }
                else
                {
                    errors.Add(fileName + "：写入失败");
                }
            }
            if (wroteAny) reloaded.Add(mod.Directory);
        }

        foreach (var modDir in reloaded)
        {
            _penumbra.Reload(modDir);
        }

        var sb = new StringBuilder();
        sb.Append($"翻译写入完成：写入 {totalWritten} 项 / 备份 {totalBackups} 个模组（zip）/ 重载 {reloaded.Count} 个模组");
        if (errors.Count > 0)
            sb.Append("；问题：" + string.Join("；", errors.Take(3)) + (errors.Count > 3 ? $" 等 {errors.Count} 条" : ""));
        LastResult = sb.ToString();
        _log.Info(LastResult);
        return totalWritten;
    }

    /// <summary> 查词典译文：原文为空 / 已含中文（不重复覆盖）/ 黑名单 → 不写回。mods 层精确键优先，再 terms 层。 </summary>
    private static string? ApplyLookup(DictionaryService dict, string fileName, string field, string english)
    {
        if (string.IsNullOrWhiteSpace(english)) return null;
        if (dict.ContainsChinese(english)) return null;
        if (dict.IsBlacklisted(english)) return null;

        var zh = dict.LookupMod($"{fileName}||{field}||{english}");
        if (zh == null) zh = dict.LookupTerm(english);
        if (string.IsNullOrWhiteSpace(zh)) return null;
        return zh == english ? null : zh;
    }
}
