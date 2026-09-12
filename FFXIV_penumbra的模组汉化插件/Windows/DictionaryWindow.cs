using System;
using System.Diagnostics;
using System.IO;
using System.Numerics;
using Dalamud.Bindings.ImGui;
using Dalamud.Interface.Utility;
using Dalamud.Interface.Utility.Raii;
using Dalamud.Interface.Windowing;

namespace FFXIVPenumbraHanhua.Windows;

/// <summary> 词典管理窗口：路径配置、加载状态、各来源条数。 </summary>
public class DictionaryWindow : Window
{
    private readonly Plugin plugin;

    public DictionaryWindow(Plugin plugin)
        : base("词典管理###HanhuaDict", ImGuiWindowFlags.NoScrollbar | ImGuiWindowFlags.NoScrollWithMouse)
    {
        Size = new Vector2(640, 480);
        SizeCondition = ImGuiCond.FirstUseEver;
        SizeConstraints = new WindowSizeConstraints
        {
            MinimumSize = new Vector2(480, 360),
            MaximumSize = new Vector2(float.MaxValue, float.MaxValue)
        };
        this.plugin = plugin;
    }

    public override void Draw()
    {
        var configuration = plugin.Configuration;
        var dict = plugin.Dict;

        ImGui.TextUnformatted("词典目录（复用独立版词典资产）：");
        ImGui.Spacing();

        var dictPath = configuration.DictionaryPath;
        var pasteW = 64f * ImGuiHelpers.GlobalScale;
        ImGui.SetNextItemWidth(Math.Max(120f, ImGui.GetContentRegionAvail().X - 110f * ImGuiHelpers.GlobalScale - pasteW - 8f * ImGuiHelpers.GlobalScale));
        if (ImGui.InputText("##DictPath", ref dictPath, 512))
        {
            configuration.DictionaryPath = dictPath;
        }
        ImGui.SameLine();
        if (ImGui.Button("粘贴##DictPaste", new Vector2(pasteW, 0)))
        {
            var clip = ImGui.GetClipboardText();
            if (!string.IsNullOrWhiteSpace(clip))
            {
                configuration.DictionaryPath = clip.Trim();
            }
        }
        ImGui.SameLine();
        if (ImGui.Button("应用并重载", new Vector2(100 * ImGuiHelpers.GlobalScale, 0)))
        {
            configuration.Save();
            plugin.ReloadDictionary();
        }

        ImGui.Spacing();
        ImGui.Separator();
        ImGui.Spacing();

        // 加载状态
        ImGui.TextUnformatted("加载状态");
        ImGui.Spacing();
        ImGui.TextWrapped(dict.Status);

        ImGui.Spacing();
        ImGui.Separator();
        ImGui.Spacing();

        // 各来源条数（右侧「打开」按钮用系统默认程序打开）
        ImGui.TextUnformatted("各来源词条统计");
        ImGui.Spacing();
        DrawStatRow("我的翻译.json（mods + terms）", Path.Combine(dictPath, "我的翻译.json"), dict.MyCount);
        DrawStatRow("个性翻译.json（词级覆盖层）", Path.Combine(dictPath, "个性翻译.json"), dict.CustomCount);
        DrawStatRow("wiki_术语对照\\ 分类文件夹", Path.Combine(dictPath, "wiki_术语对照"), dict.WikiCount);
        DrawStatRow("AI知识库\\AI知识库.json（只读底料）", Path.Combine(dictPath, "AI知识库", "AI知识库.json"), dict.AiCount);
        DrawStatRow("单词黑名单.json（保留英文专名）", Path.Combine(dictPath, "单词黑名单.json"), dict.BlacklistCount);

        ImGui.Spacing();
        ImGui.Separator();
        ImGui.Spacing();

        // 词典结构说明
        ImGui.TextDisabled("词典目录结构：");
        ImGui.TextDisabled("  ├ 我的翻译.json      —— 主词典（mods 双层 + terms 数组），优先命中");
        ImGui.TextDisabled("  ├ 个性翻译.json      —— 词级覆盖层，覆盖 我的翻译");
        ImGui.TextDisabled("  ├ 单词黑名单.json    —— 命中词保留英文，不翻译");
        ImGui.TextDisabled("  ├ wiki_术语对照\\    —— 分类术语只读兜底（不覆盖用户词条）");
        ImGui.TextDisabled("  │   └ wiki_术语对照_黑名单.json —— 剔除污染词");
        ImGui.TextDisabled("  └ AI知识库\\AI知识库.json —— 只读底料，优先级最低");
    }

    private void DrawStatRow(string name, string path, int count)
    {
        var w = ImGui.GetContentRegionAvail().X;
        var btnW = 56f * ImGuiHelpers.GlobalScale;
        var countW = 80f * ImGuiHelpers.GlobalScale;
        var isDir = Directory.Exists(path);
        var exists = isDir || File.Exists(path);

        // 最左：标准「打开」按钮（与其他按钮风格一致）
        if (exists)
        {
            if (ImGui.Button("打开", new Vector2(btnW, 0)))
            {
                OpenPath(path);
            }
            if (ImGui.IsItemHovered())
            {
                ImGui.SetTooltip((isDir ? "打开文件夹" : "用系统默认程序打开") + "：" + path);
            }
        }
        else
        {
            ImGui.InvisibleButton("##openMissing", new Vector2(btnW, 0)); // 占位保持对齐
        }

        // 文件名（超长截断省略号，悬停显示完整路径）
        ImGui.SameLine();
        var nameW = Math.Max(40f, w - btnW - countW - 20f);
        var text = name;
        if (exists && ImGui.CalcTextSize(text).X > nameW)
        {
            while (text.Length > 2 && ImGui.CalcTextSize(text + "…").X > nameW)
            {
                text = text[..^1];
            }
            text += "…";
        }
        ImGui.TextUnformatted(text);
        if (ImGui.IsItemHovered())
        {
            ImGui.SetTooltip((exists ? (isDir ? "文件夹：" : "文件：") : "文件不存在：") + path);
        }

        // 最右：条数
        ImGui.SameLine(w - countW);
        ImGui.TextColored(new Vector4(0.8f, 0.9f, 1f, 1f), count.ToString("N0") + " 条");
    }

    /// <summary> 用系统默认程序打开文件 / 资源管理器打开文件夹。 </summary>
    private static void OpenPath(string path)
    {
        try
        {
            if (Directory.Exists(path))
            {
                Process.Start("explorer.exe", "\"" + path + "\"");
            }
            else if (File.Exists(path))
            {
                Process.Start(new ProcessStartInfo { FileName = path, UseShellExecute = true });
            }
        }
        catch (Exception)
        {
            /* 打开失败静默（系统环境限制） */
        }
    }
}
