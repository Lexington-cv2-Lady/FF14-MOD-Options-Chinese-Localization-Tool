using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Numerics;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;
using Dalamud.Bindings.ImGui;
using Dalamud.Interface.Utility;
using Dalamud.Interface.Utility.Raii;
using Dalamud.Interface.Windowing;
using FFXIVPenumbraHanhua.Services;

namespace FFXIVPenumbraHanhua.Windows;

public class MainWindow : Window, IDisposable
{
    private readonly Plugin plugin;
    private readonly PenumbraService penumbra;
    private readonly DictionaryService dict;
    private readonly HanhuaService hanhua;

    private int _selected = -1;
    private bool _autoRefresh = true;
    private bool _showMarked;   // 勾选「已翻译」：只看有标记的模组；默认只显示未翻译

    // 多选集合（批量翻译 / 批量备份）
    private readonly HashSet<int> _selectedSet = new();
    // 列表鼠标框选（空白处拖动拉框多选）
    private readonly ListDragSelect _listDrag = new();
    // 框选期间锁定窗口位置：ImGui 默认把「在空白处按下拖动」当成移动窗口，需在框选时把它锁住
    private System.Numerics.Vector2? _dragWinLock;
    private System.Numerics.Vector2 _posWhileIdle;
    private System.Numerics.Vector2 _listRectMin;
    private System.Numerics.Vector2 _listRectMax;
    private const ImGuiWindowFlags BaseFlags = ImGuiWindowFlags.NoScrollbar | ImGuiWindowFlags.NoScrollWithMouse;

    // 左右分栏比例（分隔条可拖动）
    private float _split = 0.34f;
    private bool _draggingSplit;
    // 「已翻译」标记缓存：避免每帧对每个模组 File.Exists（2 秒 TTL 或显式失效）
    private readonly Dictionary<string, bool> _markCache = new();
    private DateTime _markCacheTime = DateTime.MinValue;
    // 断线自动重连节流：未连接时不再每帧发起 IPC 调用
    private DateTime _lastAutoRefresh = DateTime.MinValue;
    // 详情区选项编辑缓冲
    private readonly Dictionary<string, string> _editBufs = new();
    private string _editFileKey = "";
    private bool _showAllOptions;

    // 详情区
    private ModFileInfo? _selectedFile;
    private string _result = "";

    // 一键汉化（智能分流）：①提取 → ②词典预填 → ③AI翻译（无Key自动降级）→ ④汇总 → ⑤写入本模组
    private bool _ocSummary = true; // 提取方式：true=汇总提取（默认），false=按模组提取
    private Task? _ocTask;
    private CancellationTokenSource? _ocCts;
    private string _ocStatus = "";

    /// <summary> 翻译管线「仅提取勾选」用：当前勾选的模组列表。 </summary>
    public IReadOnlyList<ModEntry> SelectedMods
    {
        get
        {
            var list = new List<ModEntry>();
            foreach (var i in _selectedSet)
            {
                if (i >= 0 && i < penumbra.Mods.Count) list.Add(penumbra.Mods[i]);
            }
            return list;
        }
    }

    /// <summary> 当前可见（筛选后）模组是否已全部勾选（供流程窗口的「全选」显示状态）。 </summary>
    public bool AllVisibleSelected
    {
        get
        {
            var visible = BuildVisibleList();
            return visible.Count > 0 && visible.All(i => _selectedSet.Contains(i));
        }
    }

    /// <summary> 勾选 / 取消勾选当前可见（筛选后）的全部模组（供流程窗口的「全选」调用）。 </summary>
    public void SetAllVisibleSelection(bool selected)
    {
        _selectedSet.Clear();
        if (!selected) return;
        foreach (var i in BuildVisibleList()) _selectedSet.Add(i);
    }

    public MainWindow(Plugin plugin, PenumbraService penumbra, DictionaryService dict, HanhuaService hanhua)
        : base("模组汉化###HanhuaMain", BaseFlags)
    {
        SizeConstraints = new WindowSizeConstraints
        {
            MinimumSize = new Vector2(720, 420),
            MaximumSize = new Vector2(float.MaxValue, float.MaxValue)
        };

        this.plugin = plugin;
        this.penumbra = penumbra;
        this.dict = dict;
        this.hanhua = hanhua;
        this.penumbra.PenumbraDisposed += OnPenumbraDisposed;
        this.penumbra.ModsChanged += OnModsChanged;
    }

    private void OnPenumbraDisposed()
    {
        _selected = -1;
        _selectedFile = null;
        penumbra.Status = "Penumbra 已卸载，请重载插件后重试";
    }

    private void OnModsChanged()
    {
        _markCache.Clear();
        // Penumbra 模组增删后下标会漂移：越界的勾选清空，防止误操作其它模组
        if (_selectedSet.Count > 0 && _selectedSet.Any(i => i >= penumbra.Mods.Count))
        {
            _selectedSet.Clear();
        }
        if (_selected >= penumbra.Mods.Count)
        {
            _selected = -1;
            _selectedFile = null;
            _result = "";
        }
    }

    public void Dispose()
    {
        penumbra.PenumbraDisposed -= OnPenumbraDisposed;
        penumbra.ModsChanged -= OnModsChanged;
    }

    public override void Draw()
    {
        // 顶部功能导航
        DrawNavBar();

        // 状态条
        DrawStatusBar();

        ImGui.Spacing();
        ImGui.Separator();
        ImGui.Spacing();

        // 双栏：左 = 模组列表（分隔条可拖动），右 = 详情
        var avail = ImGui.GetContentRegionAvail();
        var splitterW = 6f * ImGuiHelpers.GlobalScale;
        var listW = Math.Max(200f, avail.X * _split);
        var y0 = ImGui.GetCursorPosY();

        // 记录「空闲」时的窗口位置：起拖帧窗口尚未被 ImGui 位移，用它作为锁定基准
        if (!_listDrag.Armed && !_listDrag.Active)
            _posWhileIdle = ImGui.GetWindowPos();

        // 记录左侧列表子区域的屏幕矩形（仅框选该区域时锁定窗口，不影响拖标题栏移动窗口）
        _listRectMin = ImGui.GetCursorScreenPos();
        _listRectMax = _listRectMin + new Vector2(listW, avail.Y);
        using (var left = ImRaii.Child("##ModList", new Vector2(listW, avail.Y), true))
        {
            if (left.Success)
            {
                var visible = BuildVisibleList();
                DrawModListHeader(visible);
                ImGui.Spacing();
                if (visible.Count == 0)
                {
                    ImGui.TextDisabled(_showMarked
                        ? "没有「已翻译」标记的模组（取消勾选查看未翻译）"
                        : "没有未翻译的模组（勾选「已翻译」查看已翻译）");
                }
                else
                {
                    // 列表独立滚动区：头部（已翻译筛选）固定置顶
                    using (var scroll = ImRaii.Child("##ModListScroll", new Vector2(-1, -1), false))
                    {
                        if (scroll.Success)
                        {
                            _listDrag.Begin();
                            var interactive = !_listDrag.Active; // 正在框选时屏蔽行点击，避免起拖行被误切换
                            for (var k = 0; k < visible.Count; k++)
                            {
                                var i = visible[k];
                                var mod = penumbra.Mods[i];
                                var isChecked = _selectedSet.Contains(i);
                                var rowTop = ImGui.GetCursorScreenPos().Y;
                                // 紧凑行：小内边距 → 勾选框更小、行更矮，窗口缩小时一屏可见更多
                                ImGui.PushStyleVar(ImGuiStyleVar.FramePadding, new Vector2(3f, 2f) * ImGuiHelpers.GlobalScale);
                                if (ImGui.Checkbox($"##sel{i}", ref isChecked) && interactive)
                                {
                                    if (isChecked) _selectedSet.Add(i);
                                    else _selectedSet.Remove(i);
                                }
                                ImGui.SameLine();
                                var name = mod.Name.Length > 0 ? mod.Name : mod.Directory;
                                if (ImGui.Selectable(Truncate(name, ImGui.GetContentRegionAvail().X) + $"##{i}", _selected == i) && interactive)
                                {
                                    _selected = i;
                                    _selectedFile = null;
                                    _result = "";
                                }
                                ImGui.PopStyleVar();
                                _listDrag.Row(i, rowTop, rowTop + ImGui.GetFrameHeight());
                                if (ImGui.IsItemHovered())
                                {
                                    ImGui.SetTooltip(mod.Directory + "\n按住左键拖动可框选多个（纯点击 = 单选）");
                                }
                            }
                            // 框选命中 → 勾选（只增不减）
                            foreach (var i in _listDrag.End()) _selectedSet.Add(i);
                        }
                    }
                }
            }
        }

        // ── 框选时的窗口移动处理 ──
        // ImGui 默认把「在窗口空白处按下拖动」当作移动窗口，导致框选时窗口跟着跑。
        // 方案：① 鼠标在列表内（或已在框选）时，给窗口临时加 NoMove 标志，从源头阻止 ImGui 开始移动；
        //       ② 兜底：若框选中窗口仍被位移，用空闲时记下的位置强制回位。
        var mouseNow = ImGui.GetMousePos();
        var hoverList = mouseNow.X >= _listRectMin.X && mouseNow.X <= _listRectMax.X
                        && mouseNow.Y >= _listRectMin.Y && mouseNow.Y <= _listRectMax.Y;
        Flags = BaseFlags | ((hoverList || _listDrag.Armed || _listDrag.Active) ? ImGuiWindowFlags.NoMove : 0);

        if (_listDrag.Active)
        {
            _dragWinLock ??= _posWhileIdle;
            ImGui.SetWindowPos(_dragWinLock.Value);
        }
        else
        {
            _dragWinLock = null;
        }

        // 可拖动分隔条：InvisibleButton 消费点击（防止误拖窗口）+ 宽热区 + 手动坐标计算
        // 竖条 Y 直接取左栏 Child 绘制后的实际屏幕矩形，保证与左右栏上下完全对齐
        var hotW = 12f * ImGuiHelpers.GlobalScale;
        var draw = ImGui.GetWindowDrawList();
        var winPos = ImGui.GetWindowPos();
        var rmin = ImGui.GetWindowContentRegionMin();
        var barMin = ImGui.GetItemRectMin();
        var barMax = ImGui.GetItemRectMax();
        var barX = barMin.X + listW;
        var barY = barMin.Y;
        var barBottom = barMax.Y;
        var mouse = ImGui.GetMousePos();
        var io = ImGui.GetIO();

        ImGui.SetCursorPos(new Vector2(listW, y0));
        ImGui.InvisibleButton("##splitter", new Vector2(hotW, avail.Y));
        var hoverBar = ImGui.IsItemHovered();
        var activeBar = ImGui.IsItemActive();
        if (hoverBar || activeBar || _draggingSplit)
        {
            ImGui.SetMouseCursor(ImGuiMouseCursor.ResizeAll);
            if (activeBar) _draggingSplit = true;
            if (_draggingSplit && io.MouseDown[0])
            {
                _split = Math.Clamp((mouse.X - (winPos.X + rmin.X)) / avail.X, 0.18f, 0.78f);
            }
            if (!io.MouseDown[0]) _draggingSplit = false;
        }
        draw.AddRectFilled(new Vector2(barX, barY), new Vector2(barX + splitterW, barBottom),
            ImGui.GetColorU32(new Vector4(0.7f, 0.7f, 0.7f, _draggingSplit || hoverBar ? 0.6f : 0.25f)));

        ImGui.SetCursorPos(new Vector2(listW + hotW, y0));
        using (var right = ImRaii.Child("##ModDetail", new Vector2(Math.Max(100f, avail.X - listW - hotW), avail.Y), true))
        {
            if (!right.Success) return;
            DrawDetail();
        }
    }

    /// <summary> 可见模组索引：默认只显示未翻译模组；勾选「已翻译」后只看有标记的模组。 </summary>
    private List<int> BuildVisibleList()
    {
        // 标记状态走缓存：每帧对每模组 File.Exists 在模组多时磁盘压力过大
        if ((DateTime.Now - _markCacheTime).TotalSeconds > 2)
        {
            _markCache.Clear();
            _markCacheTime = DateTime.Now;
        }
        var list = new List<int>();
        for (var i = 0; i < penumbra.Mods.Count; i++)
        {
            var dir = penumbra.Mods[i].Directory;
            if (!_markCache.TryGetValue(dir, out var hasMark))
            {
                hasMark = plugin.Mark.HasMark(dir);
                _markCache[dir] = hasMark;
            }
            if (hasMark == _showMarked) list.Add(i);
        }
        return list;
    }

    /// <summary> 模组列表标题行：标题自适应剩余宽度，「已翻译」筛选始终靠右缘（随分隔条同步移动）。 </summary>
    private void DrawModListHeader(IReadOnlyList<int> visible)
    {
        var total = penumbra.Mods.Count;
        var title = _showMarked
            ? $"模组列表（已翻译 {visible.Count}/{total}）"
            : $"模组列表（未翻译 {visible.Count}/{total}）";
        var availW = ImGui.GetContentRegionAvail().X;

        // 右侧「已翻译」勾选框宽度估算（勾选框 ≈ 帧高，加文字和间距）
        var frameH = ImGui.GetFrameHeight();
        var checkMarkW = frameH + ImGui.CalcTextSize("已翻译").X + 10f * ImGuiHelpers.GlobalScale;
        var rightBlock = checkMarkW + 8f * ImGuiHelpers.GlobalScale;

        // 标题占用剩余宽度（超长截断）；勾选框靠右缘，随分隔条拖动同步移动
        ImGui.TextUnformatted(FitTitle(title, Math.Max(40f, availW - rightBlock)));
        ImGui.SameLine(Math.Max(40f, availW - checkMarkW));
        if (ImGui.Checkbox("已翻译", ref _showMarked))
        {
            // 切换筛选时清空勾选/选中，避免误操作被隐藏的模组
            _selectedSet.Clear();
            _selected = -1;
            _selectedFile = null;
            _result = "";
        }
        if (ImGui.IsItemHovered())
        {
            ImGui.SetTooltip("默认：只显示未翻译模组（隐藏已翻译）；勾选后：只显示有「已翻译」标记的模组");
        }
    }

    /// <summary> 标题按宽度截断（超出加省略号），保证行内按钮位置不随文字长度跳动。 </summary>
    private static string FitTitle(string t, float maxW)
    {
        if (ImGui.CalcTextSize(t).X <= maxW) return t;
        var s = "";
        for (var i = 0; i < t.Length; i++)
        {
            var c = t[i];
            if (ImGui.CalcTextSize(s + c + "…").X > maxW) break;
            s += c;
        }
        return s + "…";
    }

    /// <summary> 顶部功能导航：各功能独立窗口。 </summary>
    private void DrawNavBar()
    {
        if (ImGui.Button("汉化流程"))
        {
            plugin.TogglePipelineUi();
        }
        if (ImGui.IsItemHovered())
        {
            ImGui.SetTooltip("① 提取英文 → ② 预翻译 → ③ AI 翻译 → ④ 汇总已翻译内容 → ⑤ 翻译写入MOD\n（⑤ 直写版即本页：勾选模组 → 翻译并写入）");
        }
        ImGui.SameLine();
        if (ImGui.Button("备份管理"))
        {
            plugin.ToggleBackupUi();
        }
        if (ImGui.IsItemHovered())
        {
            ImGui.SetTooltip("创建 / 还原 / 删除备份");
        }
        ImGui.SameLine();
        if (ImGui.Button("目录和词典管理"))
        {
            plugin.ToggleDictionaryUi();
        }
        if (ImGui.IsItemHovered())
        {
            ImGui.SetTooltip("词典目录 / 翻译目录 / 备份份数设置，以及词典加载状态与各来源词条统计");
        }
        ImGui.SameLine();
        if (ImGui.Button("Wiki提取"))
        {
            plugin.ToggleWikiUi();
        }
        if (ImGui.IsItemHovered())
        {
            ImGui.SetTooltip("从灰机 wiki 抓取官方中/英名，按分类写入 词典目录\\wiki_术语对照\\");
        }
        ImGui.SameLine();
        if (ImGui.Button("日志"))
        {
            plugin.ToggleLogUi();
        }
        ImGui.SameLine();
        if (ImGui.Button("AI 设置"))
        {
            plugin.ToggleAiSettingsUi();
        }
        if (ImGui.IsItemHovered())
        {
            ImGui.SetTooltip("供应商 / API Key / 模型 / AI 配置列表（自定义服务商、清空预设配置）/ 测试连接");
        }
        ImGui.Separator();
    }

    private void DrawStatusBar()
    {
        ImGui.TextUnformatted(penumbra.Status);
        ImGui.SameLine();
        ImGui.TextColored(new Vector4(0.6f, 0.85f, 1f, 1f), "|");
        ImGui.SameLine();
        ImGui.TextUnformatted(dict.Status);
        ImGui.SameLine();
        if (ImGui.Button("刷新"))
        {
            penumbra.Refresh();
        }
        ImGui.SameLine();
        if (ImGui.Button("重载词典"))
        {
            plugin.ReloadDictionary();
        }

        if (_autoRefresh && penumbra.Mods.Count == 0 && penumbra.Status.StartsWith("未连接", StringComparison.Ordinal)
            && (DateTime.Now - _lastAutoRefresh).TotalSeconds >= 3)
        {
            _lastAutoRefresh = DateTime.Now;
            penumbra.Refresh();
        }
    }

    private void DrawDetail()
    {
        if (_selected < 0 || _selected >= penumbra.Mods.Count)
        {
            // 新用户引导：词典/翻译目录未设置时，详情区先引导配置，设置完后自动隐藏
            var cfg = plugin.Configuration;
            var dictMissing = string.IsNullOrWhiteSpace(cfg.DictionaryPath) || !Directory.Exists(cfg.DictionaryPath);
            var transMissing = string.IsNullOrWhiteSpace(cfg.TranslationPath) || !Directory.Exists(cfg.TranslationPath);
            if (dictMissing || transMissing)
            {
                ImGui.TextUnformatted("欢迎使用模组汉化插件！开始前需要先设置两个目录：");
                ImGui.Spacing();
                if (dictMissing)
                {
                    ImGui.TextColored(new Vector4(1f, 0.75f, 0.3f, 1f), "⚠ 词典目录未设置（存放 我的翻译 / 个性翻译 / wiki 术语 / AI知识库）");
                }
                else
                {
                    ImGui.TextColored(new Vector4(0.55f, 0.9f, 0.55f, 1f), "✓ 词典目录已设置");
                }
                if (transMissing)
                {
                    ImGui.TextColored(new Vector4(1f, 0.75f, 0.3f, 1f), "⚠ 翻译目录未设置（提取英文 / AI翻译 的输入输出目录）");
                }
                else
                {
                    ImGui.TextColored(new Vector4(0.55f, 0.9f, 0.55f, 1f), "✓ 翻译目录已设置");
                }
                ImGui.Spacing();
                if (ImGui.Button("打开目录和词典管理，配置目录"))
                {
                    plugin.ToggleDictionaryUi();
                }
                if (ImGui.IsItemHovered())
                {
                    ImGui.SetTooltip("在「目录和词典管理」窗口中填写词典目录与翻译目录并点击「保存设置」");
                }
                ImGui.Spacing();
                ImGui.TextDisabled("目录设置完成后，本提示自动消失，可正常开始汉化。");
                return;
            }
            ImGui.TextDisabled("← 左侧选择一个模组查看详情");
            return;
        }

        var mod = penumbra.Mods[_selected];
        var modRoot2 = penumbra.GetModRoot();
        var modFullPath = Path.Combine(modRoot2 ?? "", mod.Directory);

        // 模组行：最左「打开」按钮（带阴影） + 模组名（可点击打开文件夹）
        var openW = 56f * ImGuiHelpers.GlobalScale;
        ButtonWithShadow("打开", new Vector2(openW, 0), () => OpenModFolder(modFullPath),
            "打开模组文件夹\n" + modFullPath);
        ImGui.SameLine();
        ImGui.TextUnformatted("模组：");
        ImGui.SameLine();
        if (ImGui.Selectable(mod.Name + "##openModFolder"))
        {
            OpenModFolder(modFullPath);
        }
        if (ImGui.IsItemHovered())
        {
            ImGui.SetTooltip("点击打开模组文件夹\n" + modFullPath);
        }
        ImGui.TextDisabled($"目录：{mod.Directory}");
        ImGui.Spacing();

        // 文件列表
        var modRoot = penumbra.GetModRoot();
        var files = new List<ModFileInfo>();
        if (!string.IsNullOrEmpty(modRoot))
        {
            files = plugin.ModFiles.ReadModFiles(System.IO.Path.Combine(modRoot, mod.Directory));
        }

        if (files.Count == 0)
        {
            // 有 meta.json 但无 Groups（纯文件替换模组，如动画/武器替换）与完全无选项文件，都走这里
            var metaPath = string.IsNullOrEmpty(modRoot)
                ? ""
                : System.IO.Path.Combine(modRoot, mod.Directory, "meta.json");
            if (metaPath.Length > 0 && File.Exists(metaPath))
            {
                ImGui.TextWrapped("该模组没有可汉化的选项：meta.json 中没有 Groups（属纯文件替换模组，如动画/武器替换），无需翻译。");
            }
            else
            {
                ImGui.TextWrapped("该模组没有可汉化的选项：未找到 meta.json / group_*.json。");
            }
            ImGui.Spacing();
            ImGui.TextDisabled("可创建「已翻译」标记，将其从主列表「未翻译」筛选中隐藏：");
            DrawMarkButton(mod); // 无选项模组同样允许手动标记
            ImGui.Spacing();
            Plugin.ResultBox("##MainResult", _result, "操作结果将显示在这里");
            return;
        }

        if (_selectedFile == null) _selectedFile = files[0];

        ImGui.TextUnformatted("文件（点击查看选项）:");
        ImGui.Spacing();
        foreach (var f in files)
        {
            if (ImGui.Selectable($"{(f.IsMeta ? "[新] " : "")}{f.FileName}##file", ReferenceEquals(_selectedFile, f)))
            {
                _selectedFile = f;
            }
            if (ImGui.IsItemHovered())
            {
                ImGui.SetTooltip($"文件：{f.FileName}\n完整路径：{f.Path}\n选项组：{f.Groups.Count} 个 / 选项：{CountOptions(f)} 项");
            }
        }

        ImGui.Spacing();
        ImGui.Separator();
        ImGui.Spacing();

        // 选项编辑（可直接修改中英文）
        var file = _selectedFile!;
        ImGui.TextUnformatted($"选项编辑（{CountOptions(file)} 项，直接改中英文，点保存写回）：");
        ImGui.Spacing();

        // 切换文件时重置编辑缓冲
        if (_editFileKey != file.Path)
        {
            _editFileKey = file.Path;
            _editBufs.Clear();
            _showAllOptions = false;
        }

        var rowW = ImGui.GetContentRegionAvail().X;
        var pasteW = 44f * ImGuiHelpers.GlobalScale;
        var inputW = Math.Max(120f, rowW - 230f * ImGuiHelpers.GlobalScale - pasteW - 8f * ImGuiHelpers.GlobalScale);
        var shown = 0;
        var limit = _showAllOptions ? int.MaxValue : 30;
        var truncated = false;

        foreach (var g in file.Groups)
        {
            // 组名编辑
            if (g.Name.Length > 0 || g.Options.Count > 0)
            {
                if (shown >= limit) { truncated = true; break; }
                var gk = $"{file.Path}|G{g.Index}";
                if (!_editBufs.TryGetValue(gk, out var gv)) _editBufs[gk] = gv = g.Name;
                ImGui.TextDisabled(string.IsNullOrEmpty(g.Description) ? "组名：" : $"组名（{g.Description}）：");
                ImGui.SetNextItemWidth(inputW);
                if (ImGui.InputText($"##g{g.Index}", ref gv, 1024)) _editBufs[gk] = gv;
                ImGui.SameLine();
                if (ImGui.Button($"贴##gp{g.Index}", new Vector2(pasteW, 0)))
                {
                    var clip = ImGui.GetClipboardText();
                    if (!string.IsNullOrWhiteSpace(clip)) _editBufs[gk] = clip.Trim();
                }
                if (ImGui.IsItemHovered())
                {
                    ImGui.SetTooltip("组名（可直接改中英文）\n原文：" + g.Name + "\n「贴」= 读取剪贴板覆盖本框");
                }
                shown++;
            }

            foreach (var o in g.Options)
            {
                if (shown >= limit) { truncated = true; break; }
                var k = $"{file.Path}|{g.Index}|{o.Index}";
                if (!_editBufs.TryGetValue(k, out var v)) _editBufs[k] = v = o.Name;
                ImGui.SetNextItemWidth(inputW);
                if (ImGui.InputText($"##e{shown}", ref v, 1024)) _editBufs[k] = v;
                if (ImGui.IsItemHovered())
                {
                    ImGui.SetTooltip("输入框内可直接改中英文\n原文：" + o.Name + "\n「贴」= 读取剪贴板覆盖本框");
                }
                ImGui.SameLine();
                if (ImGui.Button($"贴##ep{shown}", new Vector2(pasteW, 0)))
                {
                    var clip = ImGui.GetClipboardText();
                    if (!string.IsNullOrWhiteSpace(clip)) _editBufs[k] = clip.Trim();
                }
                ImGui.SameLine();
                var orig = o.Name.Length > 22 ? o.Name.Substring(0, 22) + "…" : o.Name;
                if (orig.Length > 0) ImGui.TextDisabled(orig);
                shown++;
            }
            if (truncated) break;
        }

        if (truncated)
        {
            ImGui.Spacing();
            if (ImGui.Button("显示全部选项"))
            {
                _showAllOptions = true;
            }
        }
        else if (CountOptions(file) > 0)
        {
            ImGui.Spacing();
            ImGui.TextDisabled($"共 {CountOptions(file)} 项，已全部显示");
        }

        ImGui.Spacing();
        if (ImGui.Button("保存修改"))
        {
            SaveEdits(file, mod);
        }
        if (ImGui.IsItemHovered())
        {
            ImGui.SetTooltip("先自动备份原文件，再把输入框内容写回模组，随后触发重载");
        }
        ImGui.SameLine();
        if (ImGui.Button("放弃修改"))
        {
            _result = "已放弃未保存的修改";
            ReloadSelectedFile();
        }
        if (ImGui.IsItemHovered())
        {
            ImGui.SetTooltip("丢弃输入框未保存的修改，重新从文件读取");
        }

        ImGui.Spacing();
        ImGui.Separator();
        ImGui.Spacing();

        // 翻译按钮
        if (ImGui.Button("翻译并写入", new Vector2(140 * ImGuiHelpers.GlobalScale, 0)))
        {
            _result = "";
            var changed = hanhua.TranslateMod(mod.Directory, mod.Name);
            _result = hanhua.LastResult;
            penumbra.Refresh();
            ReloadSelectedFile(); // 刷新详情区与编辑缓冲：旧英文缓冲若被「保存修改」写回会覆盖刚翻译的中文
        }
        if (ImGui.IsItemHovered())
        {
            ImGui.SetTooltip($"翻译并写入：{mod.Name}\n（先自动备份，再写回 meta.json / group_*.json，随后触发 Penumbra 重载）");
        }
        ImGui.SameLine();
        if (ImGui.Button("备份全部文件"))
        {
            var modPath = Path.Combine(modRoot ?? "", mod.Directory);
            var zip = plugin.Backup.CreateModZip(modPath, plugin.Configuration.BackupCount);
            _result = zip != null
                ? $"已备份模组全部文件：{Path.GetFileName(zip)}（zip 轮转保留 {plugin.Configuration.BackupCount} 份）"
                : "备份失败（无 meta.json / group_*.json 或打包异常）";
        }
        if (ImGui.IsItemHovered())
        {
            ImGui.SetTooltip("手动备份当前模组的全部文件为 zip（meta.json / group_*.json）");
        }

        // ── 一键汉化（智能分流：有 Key 全自动；无 Key 停在词典预填，等外部 AI）──
        ImGui.Spacing();
        ImGui.Separator();
        ImGui.Spacing();
        ImGui.TextUnformatted("一键汉化（提取 → 词典预填 → AI翻译 → 汇总 → 写入本模组）");
        if (ImGui.RadioButton("汇总提取（默认）", _ocSummary)) _ocSummary = true;
        ImGui.SameLine();
        if (ImGui.RadioButton("按模组提取", !_ocSummary)) _ocSummary = false;
        if (ImGui.IsItemHovered())
        {
            ImGui.SetTooltip("汇总提取：本模组条目合并进 全部模组_未翻译.json\n按模组提取：单独生成 <模组名>_未翻译.json\n两种写回效果相同，只影响文件组织方式");
        }

        if (_ocTask != null && !_ocTask.IsCompleted)
        {
            ImGui.TextWrapped(_ocStatus);
            if (ImGui.Button("取消一键汉化"))
            {
                _ocCts?.Cancel();
                _ocStatus += "\n正在取消…（AI 请求会被中断，已翻译部分写盘保留）";
            }
        }
        else
        {
            if (ImGui.Button("一键汉化本模组", new Vector2(150 * ImGuiHelpers.GlobalScale, 0)))
            {
                StartOneClick(mod);
            }
            if (ImGui.IsItemHovered())
            {
                ImGui.SetTooltip("自动完成：提取本模组英文 → 词典预填 → AI 翻译（已配 Key 时）→ 汇总进词典 → 写回本模组并重载。\n未配置 Key 时自动停在词典预填，把生成的 _未翻译.json 交给外部 AI 即可。");
            }
            ImGui.SameLine();
            if (ImGui.Button("汇总并写入"))
            {
                SumupAndWrite(mod);
            }
            if (ImGui.IsItemHovered())
            {
                ImGui.SetTooltip("外部 AI 翻完后点这个：把翻译目录里的 _已翻译.json 汇总进词典，再写回本模组并重载。");
            }
            // 轮询任务完成：清任务状态 + UI 线程收尾
            if (_ocTask != null && _ocTask.IsCompleted)
            {
                _ocTask = null;
                _ocCts?.Dispose();
                _ocCts = null;
                penumbra.Refresh();
                ReloadSelectedFile();
            }
        }

        // 已翻译标记 + 查漏补缺
        ImGui.Spacing();
        ImGui.Separator();
        ImGui.Spacing();

        DrawMarkButton(mod);
        ImGui.SameLine();
        if (ImGui.Button("查漏补缺"))
        {
            _result = CheckGaps(files);
        }
        if (ImGui.IsItemHovered())
        {
            ImGui.SetTooltip("扫描当前模组未翻译的选项/描述（不受「已翻译」标记影响）");
        }
        ImGui.SameLine();
        if (ImGui.Button("并入我的翻译"))
        {
            plugin.Sumup.SumupFromMod(modRoot ?? "", mod.Directory, plugin.Configuration.DictionaryPath);
            _result = plugin.Sumup.LastResult;
        }
        if (ImGui.IsItemHovered())
        {
            ImGui.SetTooltip("把当前模组中已改中文的条目（英文原文来自英文快照/双语格式）沉淀进 我的翻译.json，其他模组翻译时可直接命中");
        }

        // 详情区操作结果：带边框统一风格
        ImGui.Spacing();
        Plugin.ResultBox("##MainResult", _result, "操作结果将显示在这里（如：已保存 N 项修改…）");
    }

    /// <summary>
    /// 一键汉化本模组（智能分流）：① 提取（默认汇总提取，可选按模组）→ ② 词典预填 →
    /// 有 Key：③ AI 翻译 → ④ 汇总 → ⑤ 写回本模组；无 Key：停在 ②，引导走外部 AI 后用「汇总并写入」。
    /// </summary>
    private void StartOneClick(ModEntry mod)
    {
        var modRoot = penumbra.GetModRoot();
        if (string.IsNullOrEmpty(modRoot) || !Directory.Exists(Path.Combine(modRoot, mod.Directory)))
        {
            _result = "无法获取 Penumbra 模组根目录（或模组目录不存在）";
            return;
        }
        var cfg = plugin.Configuration;
        if (string.IsNullOrWhiteSpace(cfg.DictionaryPath) || !Directory.Exists(cfg.DictionaryPath))
        {
            _result = "请先在「目录和词典管理」设置词典目录";
            return;
        }
        var transDir = cfg.TranslationPath;
        try
        {
            if (!Directory.Exists(transDir)) Directory.CreateDirectory(transDir);
        }
        catch (Exception ex)
        {
            _result = "翻译目录不可用：" + ex.Message;
            return;
        }

        var hasKey = !string.IsNullOrWhiteSpace(AiTranslateService.GetApiKey(cfg));
        var summary = _ocSummary;
        var mods = new List<ModEntry> { mod };
        _result = "";
        _ocStatus = "① 提取英文…";
        _ocCts = new CancellationTokenSource();
        var ct = _ocCts.Token;

        _ocTask = Task.Run(() =>
        {
            var log = new StringBuilder();
            try
            {
                // ① 提取（只针对本模组；用户显式点按钮，不受「已翻译」标记影响）
                var n = summary
                    ? plugin.Extract.Extract(mods, skipMarked: false, transDir, modRoot)
                    : plugin.Extract.ExtractPerMod(mods, skipMarked: false, transDir, modRoot);
                if (n < 0)
                {
                    _ocStatus = "① 提取失败：" + plugin.Extract.LastResult;
                    _result = _ocStatus;
                    return;
                }
                var outputs = plugin.Extract.LastOutputPaths.ToList();
                log.Append(plugin.Extract.LastResult);
                _ocStatus = $"① 提取完成（{n} 项）→ ② 词典预填…";

                // ② 词典预填（能翻的先翻上，交给 AI 的就少了）
                var hit = 0;
                foreach (var f in outputs) hit += Math.Max(0, plugin.Extract.PrefillFile(f));
                log.Append($"；词典预填 {hit} 项");

                if (!hasKey)
                {
                    _ocStatus = "未配置 API Key：已按词典预填完成 ✓";
                    _result = log +
                              $"\n把翻译目录里的 {Path.GetFileName(outputs[0])} 交给外部 AI（连同 翻译规则.json），" +
                              "翻好后改名为 _已翻译.json 放回翻译目录，再点「汇总并写入」。";
                    return;
                }

                // ③ AI 翻译
                foreach (var input in outputs)
                {
                    if (ct.IsCancellationRequested) break;
                    _ocStatus = $"③ AI 翻译：{Path.GetFileName(input)}…";
                    var output = Path.ChangeExtension(input, null) + "_已翻译.json";
                    plugin.AiTranslate.TranslateAsync(input, output, cfg, ct).GetAwaiter().GetResult();
                    log.Append('\n').Append(plugin.AiTranslate.LastResult);
                }
                if (ct.IsCancellationRequested)
                {
                    _ocStatus = "已取消（已翻译部分写盘保留）";
                    _result = log + "\n稍后可点「汇总并写入」继续。";
                    return;
                }

                // ④ 汇总 + 重载词典 → ⑤ 写回本模组
                _ocStatus = "④ 汇总已翻译内容…";
                SumupCore(transDir, log);
                _ocStatus = "⑤ 翻译写入MOD…";
                plugin.Import.ApplyDictionary(modRoot, plugin.Dict, mods);
                log.Append('\n').Append(plugin.Import.LastResult);
                _ocStatus = "完成 ✓";
                _result = log.ToString();
            }
            catch (Exception ex)
            {
                _ocStatus = "出错：" + ex.Message;
                _result = "一键汉化出错：" + ex.Message;
            }
        });
    }

    /// <summary> 外部 AI 流程收尾：把翻译目录里的 _已翻译.json 汇总进词典，再写回本模组并重载。 </summary>
    private void SumupAndWrite(ModEntry mod)
    {
        var modRoot = penumbra.GetModRoot();
        if (string.IsNullOrEmpty(modRoot))
        {
            _result = "无法获取 Penumbra 模组根目录";
            return;
        }
        var transDir = plugin.Configuration.TranslationPath;
        var log = new StringBuilder();
        if (!SumupCore(transDir, log))
        {
            _result = "未找到 _已翻译.json：请先把外部 AI 翻好的文件改名为 <名称>_已翻译.json 放回翻译目录。";
            return;
        }
        plugin.Import.ApplyDictionary(modRoot, plugin.Dict, new List<ModEntry> { mod });
        log.Append('\n').Append(plugin.Import.LastResult);
        penumbra.Refresh();
        ReloadSelectedFile();
        _result = log.ToString();
    }

    /// <summary> ④ 汇总翻译目录下所有 _已翻译.json 进词典（有新增才重载词典）。返回是否找到并处理了文件。 </summary>
    private bool SumupCore(string transDir, StringBuilder log)
    {
        var files = Directory.Exists(transDir)
            ? Directory.GetFiles(transDir, "*_已翻译.json", SearchOption.TopDirectoryOnly).ToList()
            : new List<string>();
        if (files.Count == 0) return false;
        var added = 0;
        foreach (var f in files) added += Math.Max(0, plugin.Sumup.Sumup(f, plugin.Configuration.DictionaryPath));
        log.Append($"④ 汇总：{files.Count} 个文件，新增 {added} 条");
        if (added > 0) plugin.ReloadDictionary();
        return true;
    }

    /// <summary> 「创建 / 删除已翻译标记」按钮（带缓存失效）。有选项与无选项模组共用。 </summary>
    private void DrawMarkButton(ModEntry mod)
    {
        var mark = plugin.Mark;
        var marked = mark.HasMark(mod.Directory);
        if (ImGui.Button(marked ? "删除「已翻译」标记" : "创建「已翻译」标记"))
        {
            if (marked)
            {
                _result = mark.Remove(mod.Directory)
                    ? "已删除标记，提取英文时将重新处理该模组"
                    : "删除标记失败：无法写入模组目录";
            }
            else
            {
                _result = mark.Create(mod.Directory)
                    ? "已创建标记，提取英文时将自动跳过该模组"
                    : "创建标记失败：无法写入模组目录（请确认模组目录存在且可写）";
            }
            _markCache.Remove(mod.Directory); // 立即失效标记缓存，列表筛选即时更新
        }
        if (ImGui.IsItemHovered())
        {
            ImGui.SetTooltip("模组目录创建无后缀「已翻译」文件：提取英文/翻译时自动跳过；查漏补缺不受影响；备份还原时自动删除");
        }
    }

    /// <summary> 查漏补缺：列出模组文件中仍为英文的选项/组名/描述。 </summary>
    private string CheckGaps(List<ModFileInfo> files)
    {
        var gaps = new List<string>();
        foreach (var f in files)
        {
            foreach (var g in f.Groups)
            {
                if (g.Name.Length > 0 && !dict.ContainsChinese(g.Name)) gaps.Add(g.Name);
                foreach (var o in g.Options)
                {
                    if (o.Name.Length > 0 && !dict.ContainsChinese(o.Name)) gaps.Add(o.Name);
                    if (!string.IsNullOrWhiteSpace(o.Description) && !dict.ContainsChinese(o.Description)) gaps.Add(o.Description);
                }
            }
        }
        if (gaps.Count == 0) return "查漏补缺：未发现未翻译条目（全部已中文）";
        var shown = gaps.Distinct().Take(20).ToList();
        return $"查漏补缺：发现 {gaps.Count} 条未翻译\n" + string.Join("\n", shown) +
               (gaps.Count > 20 ? $"\n… 其余 {gaps.Count - 20} 条" : "");
    }

    /// <summary> 带投影阴影的按钮：先画右下偏移阴影，再画按钮，增加层次感（消除扁平突兀感）。 </summary>
    private static void ButtonWithShadow(string label, Vector2 size, Action onClick, string? tooltip = null)
    {
        var draw = ImGui.GetWindowDrawList();
        var pos = ImGui.GetCursorScreenPos();
        var rounding = ImGui.GetStyle().FrameRounding;
        var shadowColor = ImGui.GetColorU32(new Vector4(0f, 0f, 0f, 0.38f));
        // 阴影：右下偏移 2px，圆角与按钮一致
        draw.AddRectFilled(new Vector2(pos.X + 2f, pos.Y + 3f),
            new Vector2(pos.X + size.X + 2f, pos.Y + size.Y + 3f),
            shadowColor, rounding);
        draw.AddRectFilled(new Vector2(pos.X + 1.5f, pos.Y + 2.5f),
            new Vector2(pos.X + size.X + 1.5f, pos.Y + size.Y + 2.5f),
            ImGui.GetColorU32(new Vector4(0f, 0f, 0f, 0.18f)), rounding);

        if (ImGui.Button(label, size))
        {
            onClick();
        }
        if (tooltip != null && ImGui.IsItemHovered())
        {
            ImGui.SetTooltip(tooltip);
        }
    }

    /// <summary> 超长文本截断省略号（防止列表项溢出窗口边界）。 </summary>
    private static string Truncate(string text, float maxWidth)
    {
        if (maxWidth <= 10f || ImGui.CalcTextSize(text).X <= maxWidth) return text;
        var result = text;
        while (result.Length > 1 && ImGui.CalcTextSize(result + "…").X > maxWidth)
        {
            result = result[..^1];
        }
        return result + "…";
    }

    /// <summary> 打开模组文件夹（explorer）。 </summary>
    private void OpenModFolder(string modFullPath)    {
        if (Directory.Exists(modFullPath))
        {
            try
            {
                Process.Start(new ProcessStartInfo("explorer.exe", $"\"{modFullPath}\"") { UseShellExecute = true });
            }
            catch (Exception ex)
            {
                _result = "打开模组文件夹失败：" + ex.Message;
            }
        }
        else
        {
            _result = "模组目录不存在：" + modFullPath;
        }
    }

    /// <summary> 保存详情区编辑：备份（zip）→ 写回输入框内容 → 重读文件 → 触发 Penumbra 重载。 </summary>
    private void SaveEdits(ModFileInfo file, ModEntry mod)
    {
        try
        {
            // 先自动备份整个模组（zip），防翻车
            var modDirPath = Path.GetDirectoryName(file.Path) ?? "";
            var modDirName = Path.GetFileName(modDirPath);
            plugin.Backup.CreateModZip(modDirPath, plugin.Configuration.BackupCount);

            // 保存前若文件仍为纯英文：存英文快照（改中文后仍可用原文覆写）
            try
            {
                var before = File.ReadAllText(file.Path);
                plugin.Snapshot.SaveIfEnglish(modDirName, file.FileName, before);
            }
            catch (Exception)
            {
                /* 快照失败不影响保存 */
            }

            var node = JsonNode.Parse(File.ReadAllText(file.Path)) as JsonObject;
            if (node == null)
            {
                _result = "保存失败：无法解析文件";
                return;
            }

            var changed = 0;
            // 定位 Groups：顶层（新版 meta）或 Mod.Groups 包装（旧版），与解析/写回同规则
            JsonArray? groupsArr = node["Groups"] as JsonArray;
            if (groupsArr == null && node["Mod"] is JsonObject modWrap)
                groupsArr = modWrap["Groups"] as JsonArray;

            foreach (var kv in _editBufs)
            {
                var parts = kv.Key.Split('|');
                if (parts.Length < 2) continue;

                if (parts.Length == 2 && parts[1].StartsWith("G")) // 组名（key: 路径|G组索引）
                {
                    var gi = int.Parse(parts[1].Substring(1));
                    if (file.IsMeta && groupsArr != null && gi < groupsArr.Count &&
                        groupsArr[gi] is JsonObject gObj)
                    {
                        gObj["Name"] = kv.Value;
                        changed++;
                    }
                    else if (!file.IsMeta && gi == 0)
                    {
                        node["Name"] = kv.Value;
                        changed++;
                    }
                }
                else if (parts.Length >= 3) // 选项名（key: 路径|组索引|选项索引）
                {
                    var gi = int.Parse(parts[1]);
                    var oi = int.Parse(parts[2]);
                    JsonArray? opts = null;
                    if (file.IsMeta)
                    {
                        if (groupsArr != null && gi < groupsArr.Count && groupsArr[gi] is JsonObject gObj2)
                            opts = gObj2["Options"] as JsonArray;
                    }
                    else
                    {
                        opts = node["Options"] as JsonArray;
                    }
                    if (opts != null && oi < opts.Count && opts[oi] is JsonObject oObj)
                    {
                        oObj["Name"] = kv.Value;
                        changed++;
                    }
                }
            }

            File.WriteAllText(file.Path, node.ToJsonString(JsonFile.Indented));
            _result = $"已保存 {changed} 项修改（原文件已自动备份）";

            ReloadSelectedFile();
            penumbra.Reload(mod.Directory, mod.Name); // 触发 Penumbra 重新加载该模组（按目录+名称匹配），游戏内立即生效
        }
        catch (Exception ex)
        {
            _result = "保存失败：" + ex.Message;
        }
    }

    /// <summary> 重新读取当前模组的文件，刷新编辑缓冲。 </summary>
    private void ReloadSelectedFile()
    {
        var modRoot = penumbra.GetModRoot();
        if (_selected < 0 || _selected >= penumbra.Mods.Count || string.IsNullOrEmpty(modRoot))
        {
            _selectedFile = null;
            return;
        }
        var mod = penumbra.Mods[_selected];
        var files = plugin.ModFiles.ReadModFiles(Path.Combine(modRoot, mod.Directory));
        _selectedFile = files.FirstOrDefault(x => x.Path == _selectedFile?.Path) ?? files.FirstOrDefault();
        _editFileKey = _selectedFile?.Path ?? "";
        _editBufs.Clear();
        _showAllOptions = false;
    }

    private static int CountOptions(ModFileInfo f)
    {
        var n = 0;
        foreach (var g in f.Groups) n += g.Options.Count;
        return n;
    }
}
