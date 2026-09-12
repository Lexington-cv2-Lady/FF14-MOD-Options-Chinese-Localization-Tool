using System;
using System.Numerics;
using Dalamud.Bindings.ImGui;
using Dalamud.Interface.Utility;
using Dalamud.Interface.Utility.Raii;
using Dalamud.Interface.Windowing;
using FFXIVPenumbraHanhua.Services;

namespace FFXIVPenumbraHanhua.Windows;

/// <summary> 日志窗口：环形操作日志（新→旧），底部常驻最近报错。 </summary>
public class LogWindow : Window, IDisposable
{
    private readonly AppLog _log;
    private bool _autoScroll = true;

    public LogWindow(AppLog log) : base("日志###HanhuaLog")
    {
        Size = new Vector2(640, 480);
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
        ImGui.Checkbox("自动滚动", ref _autoScroll);
        ImGui.SameLine();
        if (ImGui.Button("清空日志"))
        {
            _log.Clear();
        }
        ImGui.Spacing();
        ImGui.Separator();
        ImGui.Spacing();

        var entries = _log.Snapshot();
        var avail = ImGui.GetContentRegionAvail();
        using var child = ImRaii.Child("##LogList", new Vector2(avail.X, avail.Y - 64), true);
        if (child.Success)
        {
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
                ImGui.SetScrollHereY(0f);
            }
        }

        // 底部报错条（常驻显示最近一条错误）
        ImGui.Separator();
        if (string.IsNullOrEmpty(_log.LastError))
        {
            ImGui.TextDisabled("无报错");
        }
        else
        {
            ImGui.TextColored(new Vector4(1f, 0.45f, 0.45f, 1f), _log.LastError);
        }
    }
}
