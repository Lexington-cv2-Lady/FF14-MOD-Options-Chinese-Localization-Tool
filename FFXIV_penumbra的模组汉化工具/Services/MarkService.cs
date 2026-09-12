using System;
using System.IO;

namespace FFXIVPenumbraHanhua.Services;

/// <summary> 已翻译标记：模组目录下创建无后缀「已翻译」文件，提取/翻译时自动跳过该模组。 </summary>
public sealed class MarkService
{
    private readonly Func<string> _modRootGetter;

    public MarkService(Func<string> modRootGetter)
    {
        _modRootGetter = modRootGetter;
    }

    public const string MarkName = "已翻译";

    private string Root => _modRootGetter() ?? "";

    /// <summary> 模组目录的完整路径（兼容 mod.Directory 为相对或绝对路径）。 </summary>
    private static string ModFullPath(string root, string modDirectory)
    {
        if (string.IsNullOrEmpty(root)) return modDirectory;
        return Path.IsPathRooted(modDirectory) ? modDirectory : Path.Combine(root, modDirectory);
    }

    /// <summary> 模组目录是否已有「已翻译」标记。 </summary>
    public bool HasMark(string modDirectory)
    {
        if (string.IsNullOrEmpty(Root)) return false;
        return File.Exists(Path.Combine(ModFullPath(Root, modDirectory), MarkName));
    }

    /// <summary> 创建标记。返回是否成功。 </summary>
    public bool Create(string modDirectory)
    {
        try
        {
            var dir = ModFullPath(Root, modDirectory);
            if (!Directory.Exists(dir)) return false;
            var p = Path.Combine(dir, MarkName);
            if (!File.Exists(p)) File.WriteAllText(p, "此模组已完成汉化，提取英文时将自动跳过。如需重新提取，请删除本文件。\n");
            return true;
        }
        catch (Exception)
        {
            return false;
        }
    }

    /// <summary> 删除标记。返回是否成功。 </summary>
    public bool Remove(string modDirectory)
    {
        try
        {
            var p = Path.Combine(ModFullPath(Root, modDirectory), MarkName);
            if (File.Exists(p))
            {
                File.Delete(p);
                return true;
            }
            return false;
        }
        catch (Exception)
        {
            return false;
        }
    }
}
