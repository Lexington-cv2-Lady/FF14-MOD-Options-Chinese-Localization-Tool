using System;
using System.IO;
using System.Linq;

namespace FFXIVPenumbraHanhua.Services;

/// <summary>
/// 英文快照：在翻译写入 / 详情区保存 / 备份还原时，若模组文件内容为纯英文，
/// 把原文存到 词典目录\.英文快照\&lt;模组&gt;\&lt;文件名&gt;（放在词典目录，避免妨碍翻译目录里的文件整理）。
/// 用途：选项已被改成中文后，「翻译并写入」可用快照里的英文原文重新查词典，
/// 词典更新后即可覆盖旧译法（等价独立版 .bak 英文名写回物）。
/// </summary>
public sealed class EnglishSnapshotService
{
    private readonly Func<string> _dictionaryDirGetter;

    public EnglishSnapshotService(Func<string> dictionaryDirGetter)
    {
        _dictionaryDirGetter = dictionaryDirGetter;
    }

    private string Root => Path.Combine(_dictionaryDirGetter() ?? "", ".英文快照");

    /// <summary> 确保快照根目录存在（设置好词典目录后调用，立即创建 .英文快照 空文件夹）。 </summary>
    public void EnsureRoot()
    {
        try
        {
            var dictDir = _dictionaryDirGetter() ?? "";
            if (string.IsNullOrEmpty(dictDir)) return;
            Directory.CreateDirectory(dictDir);
            Directory.CreateDirectory(Root);
        }
        catch (Exception)
        {
            /* 创建失败不阻断启动 */
        }
    }

    /// <summary> 词典目录变更时迁移 .英文快照（目标已存在时不覆盖）。返回是否迁移。 </summary>
    public bool MigrateFrom(string oldDictionaryDir)
    {
        try
        {
            var current = _dictionaryDirGetter() ?? "";
            if (string.IsNullOrEmpty(oldDictionaryDir) || string.IsNullOrEmpty(current) ||
                oldDictionaryDir == current) return false;
            var src = Path.Combine(oldDictionaryDir, ".英文快照");
            if (!Directory.Exists(src)) return false;
            var dst = Root;
            if (Directory.Exists(dst)) return false; // 目标已存在，不覆盖
            Directory.Move(src, dst);
            return true;
        }
        catch (Exception)
        {
            return false;
        }
    }

    /// <summary> 内容为纯英文（不含中文）时保存快照。返回是否已保存。 </summary>
    public bool SaveIfEnglish(string modDirName, string fileName, string content)
    {
        if (string.IsNullOrEmpty(content) || string.IsNullOrEmpty(modDirName)) return false;
        if (ContainsChinese(content)) return false;
        try
        {
            var dir = Path.Combine(Root, modDirName);
            Directory.CreateDirectory(dir);
            File.WriteAllText(Path.Combine(dir, fileName), content);
            return true;
        }
        catch (Exception)
        {
            return false;
        }
    }

    /// <summary> 取模组文件的英文快照（解析为 ModFileInfo），无则 null。 </summary>
    public ModFileInfo? GetEnglish(string modDirName, string fileName)
    {
        try
        {
            var p = Path.Combine(Root, modDirName, fileName);
            if (!File.Exists(p)) return null;
            var text = File.ReadAllText(p);
            if (text.Length == 0) return null;
            return fileName.Equals("meta.json", StringComparison.OrdinalIgnoreCase)
                ? ModFileService.ParseMeta(p)
                : ModFileService.ParseGroup(p);
        }
        catch (Exception)
        {
            return null;
        }
    }

    /// <summary> 快速判断是否含中文（与词典口径一致：CJK 基本区）。 </summary>
    public static bool ContainsChinese(string s)
    {
        foreach (var c in s)
            if (c >= 0x4E00 && c <= 0x9FFF) return true;
        return false;
    }
}
