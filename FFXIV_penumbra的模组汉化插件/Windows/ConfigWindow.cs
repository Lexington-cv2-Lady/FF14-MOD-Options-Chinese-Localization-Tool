using System;
using System.Numerics;
using Dalamud.Bindings.ImGui;
using Dalamud.Interface.ImGuiFileDialog;
using Dalamud.Interface.Utility;
using Dalamud.Interface.Windowing;

namespace FFXIVPenumbraHanhua.Windows;

/// <summary> 设置窗口：通用配置项。 </summary>
public class ConfigWindow : Window, IDisposable
{
    private readonly Plugin plugin;
    private readonly Configuration configuration;
    private readonly FileDialogManager _fileDialog = new();

    public ConfigWindow(Plugin plugin) : base("设置###HanhuaConfig")
    {
        Size = new Vector2(640, 480);
        SizeCondition = ImGuiCond.FirstUseEver;
        SizeConstraints = new WindowSizeConstraints
        {
            MinimumSize = new Vector2(480, 360),
            MaximumSize = new Vector2(float.MaxValue, float.MaxValue)
        };

        this.plugin = plugin;
        configuration = plugin.Configuration;
    }

    public void Dispose() { }

    public override void Draw()
    {
        // 词典目录（带浏览/粘贴）
        ImGui.TextUnformatted("词典目录（我的翻译 / 个性翻译 / wiki / AI知识库）：");
        var dictPath = configuration.DictionaryPath;
        var btnW = 60f * ImGuiHelpers.GlobalScale;
        ImGui.SetNextItemWidth(Math.Max(120f, ImGui.GetContentRegionAvail().X - btnW * 2 - 16f * ImGuiHelpers.GlobalScale));
        if (ImGui.InputText("##DictPath", ref dictPath, 512))
        {
            configuration.DictionaryPath = dictPath;
        }
        ImGui.SameLine();
        if (ImGui.Button("浏览##DictBrowse", new Vector2(btnW, 0)))
        {
            _fileDialog.OpenFolderDialog("选择词典目录", (ok, path) =>
            {
                if (ok && !string.IsNullOrWhiteSpace(path))
                {
                    configuration.DictionaryPath = path.Trim();
                }
            });
        }
        if (ImGui.IsItemHovered()) ImGui.SetTooltip("弹出文件夹选择框，选择词典目录");
        ImGui.SameLine();
        if (ImGui.Button("粘贴##DictPaste", new Vector2(btnW, 0)))
        {
            var clip = ImGui.GetClipboardText();
            if (!string.IsNullOrWhiteSpace(clip))
            {
                configuration.DictionaryPath = clip.Trim();
            }
        }
        if (ImGui.IsItemHovered()) ImGui.SetTooltip("直接读取剪贴板中的路径填入（无需 Ctrl+V）");

        ImGui.Spacing();
        ImGui.Separator();
        ImGui.Spacing();

        // 翻译目录（带浏览/粘贴）
        ImGui.TextUnformatted("翻译目录（AI 翻译管线的输入输出目录）：");
        var transPath = configuration.TranslationPath;
        ImGui.SetNextItemWidth(Math.Max(120f, ImGui.GetContentRegionAvail().X - btnW * 2 - 16f * ImGuiHelpers.GlobalScale));
        if (ImGui.InputText("##TransPath", ref transPath, 512))
        {
            configuration.TranslationPath = transPath;
        }
        ImGui.SameLine();
        if (ImGui.Button("浏览##TransBrowse", new Vector2(btnW, 0)))
        {
            _fileDialog.OpenFolderDialog("选择翻译目录", (ok, path) =>
            {
                if (ok && !string.IsNullOrWhiteSpace(path))
                {
                    configuration.TranslationPath = path.Trim();
                }
            });
        }
        if (ImGui.IsItemHovered()) ImGui.SetTooltip("弹出文件夹选择框，选择翻译目录");
        ImGui.SameLine();
        if (ImGui.Button("粘贴##TransPaste", new Vector2(btnW, 0)))
        {
            var clip = ImGui.GetClipboardText();
            if (!string.IsNullOrWhiteSpace(clip))
            {
                configuration.TranslationPath = clip.Trim();
            }
        }
        if (ImGui.IsItemHovered()) ImGui.SetTooltip("直接读取剪贴板中的路径填入（无需 Ctrl+V）");
        ImGui.TextDisabled($"当前状态：{(System.IO.Directory.Exists(configuration.TranslationPath) ? "目录存在" : "目录不存在（创建后生效）")}");

        ImGui.Spacing();
        ImGui.Separator();
        ImGui.Spacing();

        // 备份保留份数
        ImGui.TextUnformatted("备份保留份数（写回前自动备份，轮转保留）：");
        ImGui.Spacing();
        var backupCount = configuration.BackupCount;
        ImGui.SetNextItemWidth(160 * ImGuiHelpers.GlobalScale);
        if (ImGui.InputInt("##BackupCount", ref backupCount, 1, 5))
        {
            configuration.BackupCount = Math.Clamp(backupCount, 1, 20);
            plugin.ModFiles.MaxBackups = configuration.BackupCount;
        }
        ImGui.SameLine();
        ImGui.TextDisabled("（1 - 20）");

        ImGui.Spacing();
        ImGui.Separator();
        ImGui.Spacing();

        ImGui.SameLine(ImGui.GetContentRegionAvail().X - 110 * ImGuiHelpers.GlobalScale);
        if (ImGui.Button("保存设置", new Vector2(100 * ImGuiHelpers.GlobalScale, 0)))
        {
            configuration.Save();
            plugin.MigrateDirectories();
            plugin.Snapshot.EnsureRoot();
            plugin.ReloadDictionary(); // 目录可能已变更：立即重载词典，避免旧内存词典不生效
        }

        // 文件选择对话框（浏览文件夹用）
        _fileDialog.Draw();
    }
}
