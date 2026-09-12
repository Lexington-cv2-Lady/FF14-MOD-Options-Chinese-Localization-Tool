using System;
using System.Diagnostics;
using System.IO;
using System.Numerics;
using System.Text;
using Dalamud.Bindings.ImGui;
using Dalamud.Interface.Utility;
using Dalamud.Interface.Utility.Raii;
using Dalamud.Interface.Windowing;
using FFXIVPenumbraHanhua.Services;

namespace FFXIVPenumbraHanhua.Windows;

/// <summary> 日志窗口：环形操作日志（新→旧）占主区，底部固定一条「最近报错」，顶部可打开日志文件。 </summary>
public class LogWindow : Window, IDisposable
{
    private readonly AppLog _log;
    private bool _autoScroll = true;
    private string _openMsg = "";

    public LogWindow(AppLog log) : base("日志###HanhuaLog")
    {
        Size = new Vector2(680, 520);
        SizeCondition = ImGuiCond.FirstUseEver;
        SizeConstraints = new WindowSizeConstraints
        {
            MinimumSize = new Vector2(480, 360),
            MaximumSize = new Vector2(float.MaxValue, float.MaxValue)
        };
        _log = log;
    }

    public void Dispose() { }

    public override void Draw()
    {
        // ── 顶部工具条 ──
        ImGui.Checkbox("自动滚动", ref _autoScroll);
        ImGui.SameLine();
        if (ImGui.Button("清空日志"))
        {
            _log.Clear();
            _openMsg = "";
        }
        ImGui.SameLine();
        if (ImGui.Button("打开日志文件"))
        {
            OpenLogFile();
        }
        if (ImGui.IsItemHovered())
        {
            ImGui.SetTooltip(_log.FilePath != null
                ? "用系统默认程序打开日志文件：\n" + _log.FilePath
                : "日志文件不可用（落盘失败，仅内存日志）");
        }
        if (_openMsg.Length > 0)
        {
            ImGui.SameLine();
            ImGui.TextColored(new Vector4(1f, 0.5f, 0.2f, 1f), _openMsg);
        }
        ImGui.Spacing();

        // ── 日志列表（可滚动，占主区） ──
        // 注意：必须用块级 using 让 Child 在此处结束；若用 using 声明，EndChild 会推迟到方法末尾，
        // 底部报错条就会被画进滚动列表内部（导致列表里日志后面全是报错、窗口底部却空一块）。
        var avail = ImGui.GetContentRegionAvail();
        var bottomH = 76f * ImGuiHelpers.GlobalScale;
        var listH = Math.Max(80f, avail.Y - bottomH - ImGui.GetStyle().ItemSpacing.Y * 2f);
        using (var list = ImRaii.Child("##LogList", new Vector2(0, listH), true))
        {
            if (list.Success)
            {
                var entries = _log.Snapshot();
                foreach (var e in entries)
                {
                    var color = e.Lv switch
                    {
                        AppLog.Level.Error => new Vector4(1f, 0.45f, 0.45f, 1f),
                        AppLog.Level.Warn => new Vector4(1f, 0.8f, 0.4f, 1f),
                        _ => new Vector4(0.85f, 0.9f, 0.95f, 1f)
                    };
                    var tag = e.Lv switch
                    {
                        AppLog.Level.Error => "[错误]",
                        AppLog.Level.Warn => "[警告]",
                        _ => "[信息]"
                    };
                    ImGui.TextColored(color, $"{e.Time:HH:mm:ss} {tag} {e.Text}");
                }
                if (_autoScroll && entries.Count > 0)
                {
                    ImGui.SetScrollY(0f); // 列表新→旧：顶部即最新一条
                }
            }
        }

        // ── 底部报错条（固定在列表之外，长文本自动换行） ──
        ImGui.Spacing();
        using (var err = ImRaii.Child("##LogLastError", new Vector2(0, -1), true))
        {
            if (err.Success)
            {
                if (string.IsNullOrEmpty(_log.LastError))
                {
                    ImGui.TextDisabled("无报错");
                }
                else
                {
                    ImGui.PushStyleColor(ImGuiCol.Text, new Vector4(1f, 0.45f, 0.45f, 1f));
                    ImGui.TextWrapped(_log.LastError);
                    ImGui.PopStyleColor();
                }
            }
        }
    }

    /// <summary> 用系统默认程序打开日志文件（不存在则先建空文件）。 </summary>
    private void OpenLogFile()
    {
        var path = _log.FilePath;
        if (string.IsNullOrEmpty(path))
        {
            _openMsg = "日志文件不可用";
            return;
        }
        try
        {
            if (!File.Exists(path))
            {
                var dir = Path.GetDirectoryName(path);
                if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
                File.WriteAllText(path, "", Encoding.UTF8);
            }
            Process.Start(new ProcessStartInfo { FileName = path, UseShellExecute = true });
            _openMsg = "";
        }
        catch (Exception ex)
        {
            _openMsg = "打开失败：" + ex.Message;
        }
    }
}
