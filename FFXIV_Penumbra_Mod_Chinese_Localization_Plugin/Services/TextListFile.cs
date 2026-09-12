using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace FFXIVPenumbraHanhua.Services;

/// <summary> 行式文本清单读取（# 注释、逗号分隔、BOM 兼容）——单词黑名单 / wiki 黑名单等共用同一口径。 </summary>
internal static class TextListFile
{
    public static List<string> Load(string path)
    {
        var words = new List<string>();
        if (!File.Exists(path)) return words;
        try
        {
            var text = File.ReadAllText(path, Encoding.UTF8);
            if (text.Length >= 3 && text[0] == '\uFEFF') text = text[1..];
            foreach (var rawLine in text.Split('\n'))
            {
                var line = rawLine.TrimEnd('\r');
                var trimmed = line.TrimStart(' ', '\t');
                if (trimmed.Length == 0 || trimmed[0] == '#') continue;
                foreach (var rawToken in line.Split(','))
                {
                    var token = rawToken;
                    var hash = token.IndexOf('#');
                    if (hash >= 0) token = token[..hash];
                    token = token.Trim();
                    if (token.Length > 0) words.Add(token);
                }
            }
        }
        catch (Exception)
        {
            // 文件损坏则静默跳过
        }
        return words;
    }
}
