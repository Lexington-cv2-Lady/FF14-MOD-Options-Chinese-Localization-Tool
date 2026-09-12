using System.Collections.Generic;
using System.Numerics;
using Dalamud.Bindings.ImGui;

namespace FFXIVPenumbraHanhua.Windows;

/// <summary>
/// 列表鼠标框选：在列表空白处按下并拖动即可拉出选框，框到的条目自动被勾选（只增不减，不会取消已勾选项，
/// 因此不会误清空选择）。用法：进入列表滚动 Child 后调用 <see cref="Begin"/>；每绘制一行调用
/// <see cref="Row"/>（传入该行的屏幕上下边界 Y）；行循环结束后调用 <see cref="End"/>，返回本帧被框中的行索引。
/// 起点落在条目上时不触发框选，避免与「点击条目」冲突；滚动条区域也不触发。
/// </summary>
internal sealed class ListDragSelect
{
    private bool _dragging;
    private Vector2 _start;
    private readonly List<(int Index, float Top, float Bottom)> _rows = new();

    /// <summary> 当前是否正在框选（用于提示或联动）。 </summary>
    public bool Active => _dragging;

    /// <summary> 每帧进入列表后调用，清空上一帧收集的行矩形。 </summary>
    public void Begin() => _rows.Clear();

    /// <summary> 记录一行的屏幕上下边界 Y（行坐标收集，用于与选框求交）。 </summary>
    public void Row(int index, float top, float bottom) => _rows.Add((index, top, bottom));

    /// <summary> 行绘制结束后调用；返回本帧被选框覆盖、需要勾选的行索引集合。 </summary>
    public IReadOnlyList<int> End()
    {
        var hits = new List<int>();
        var io = ImGui.GetIO();
        var mouse = io.MousePos;

        // 当前列表可视区（Child 的屏幕矩形；右侧扣除滚动条，避免拖滚动条被误判为框选）
        var wPos = ImGui.GetWindowPos();
        var wSize = ImGui.GetWindowSize();
        var min = wPos;
        var max = new Vector2(wPos.X + wSize.X - ImGui.GetStyle().ScrollbarSize, wPos.Y + wSize.Y);
        var inList = mouse.X >= min.X && mouse.X <= max.X && mouse.Y >= min.Y && mouse.Y <= max.Y;

        // 空白处按下即为框选起点（落在条目上不触发）
        if (ImGui.IsMouseClicked(0) && inList && !ImGui.IsAnyItemHovered())
        {
            _dragging = true;
            _start = mouse;
        }
        if (!_dragging) return hits;

        var cMin = Vector2.Max(Vector2.Min(_start, mouse), min);
        var cMax = Vector2.Min(Vector2.Max(_start, mouse), max);
        // 有实际拖动面积才算框选：零面积单击（未拖动）不选中任何行
        if (cMax.X > cMin.X && cMax.Y > cMin.Y)
        {
            var dl = ImGui.GetWindowDrawList();
            dl.AddRectFilled(cMin, cMax, ImGui.GetColorU32(new Vector4(0.30f, 0.62f, 1f, 0.25f)));
            dl.AddRect(cMin, cMax, ImGui.GetColorU32(new Vector4(0.50f, 0.78f, 1f, 0.95f)));

            foreach (var r in _rows)
            {
                if (r.Bottom >= cMin.Y && r.Top <= cMax.Y) hits.Add(r.Index);
            }
        }

        if (!io.MouseDown[0]) _dragging = false;
        return hits;
    }
}
