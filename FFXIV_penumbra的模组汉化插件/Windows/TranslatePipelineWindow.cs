using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Numerics;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading.Tasks;
using Dalamud.Bindings.ImGui;
using Dalamud.Interface.Utility;
using Dalamud.Interface.Windowing;
using FFXIVPenumbraHanhua.Services;

namespace FFXIVPenumbraHanhua.Windows;

/// <summary> 汉化流程窗口：① 提取英文 → ② 预翻译 → ③ AI 翻译 → ④ 汇总已翻译内容 → ⑤ 翻译写入MOD。 </summary>
public class TranslatePipelineWindow : Window, IDisposable
{
    private readonly Plugin _plugin;
    private readonly ExtractService _extract;
    private readonly AiTranslateService _ai;
    private readonly ImportService _import;
    private readonly SumupService _sumup;
    private readonly DictionaryService _dict;
    private readonly AppLog _log;

    private bool _skipMarked = true;
    private string _result = "";
    private Task<int>? _task;
    private string _taskStatus = "";

    public TranslatePipelineWindow(Plugin plugin) : base("汉化流程###HanhuaPipeline")
    {
        Size = new Vector2(640, 480);
        SizeCondition = ImGuiCond.FirstUseEver;
        SizeConstraints = new WindowSizeConstraints
        {
            MinimumSize = new Vector2(480, 360),
            MaximumSize = new Vector2(float.MaxValue, float.MaxValue)
        };

        _plugin = plugin;
        _extract = plugin.Extract;
        _ai = plugin.AiTranslate;
        _import = plugin.Import;
        _sumup = plugin.Sumup;
        _dict = plugin.Dict;
        _log = plugin.AppLog;
    }

    public void Dispose() { }

    public override void Draw()
    {
        var cfg = _plugin.Configuration;
        var transDir = cfg.TranslationPath;
        var modRoot = _plugin.Penumbra.GetModRoot();
        var notFound = string.IsNullOrEmpty(modRoot) || !Directory.Exists(modRoot);

        ImGui.TextWrapped("流程：① 提取英文 → ② 预翻译（词典预填）→ ③ AI 翻译 → ④ 汇总已翻译内容（编入词典）→ ⑤ 翻译写入MOD。");
        ImGui.Spacing();
        ImGui.TextDisabled($"翻译目录：{transDir}（{(Directory.Exists(transDir) ? "存在" : "不存在，提取时会自动创建")}）");
        ImGui.Spacing();
        ImGui.Separator();
        ImGui.Spacing();

        // ── ① 提取英文 ──
        ImGui.TextUnformatted("① 提取英文（只处理主窗口勾选的模组，未勾选请先到主窗口点「全选」）");
        ImGui.Spacing();
        ImGui.Checkbox("跳过已标记「已翻译」的模组", ref _skipMarked);
        ImGui.Spacing();
        if (ImGui.Button("提取英文"))
        {
            if (notFound)
            {
                _result = "无法获取 Penumbra 模组根目录";
            }
            else
            {
                var mods = _plugin.MainWindow.SelectedMods;
                var n = _extract.ExtractPerMod(mods, _skipMarked, transDir, modRoot ?? "");
                _result = _extract.LastResult;
            }
        }
        if (ImGui.IsItemHovered())
        {
            ImGui.SetTooltip($"每个模组单独生成 翻译目录\\<模组名>_未翻译.json\n" +
                             $"键格式：模组目录/文件||字段||原文（含翻译规则段）\n" +
                             $"文件名用简洁模组名，键内仍带完整目录，⑤ 写回不受影响");
        }
        ImGui.SameLine();
        if (ImGui.Button("汇总提取"))
        {
            if (notFound)
            {
                _result = "无法获取 Penumbra 模组根目录";
            }
            else
            {
                var mods = _plugin.MainWindow.SelectedMods;
                var n = _extract.Extract(mods, _skipMarked, transDir, modRoot ?? "");
                _result = _extract.LastResult;
            }
        }
        if (ImGui.IsItemHovered())
        {
            ImGui.SetTooltip($"所有模组合并生成 {Path.Combine(transDir, "全部模组_未翻译.json")}\n适合整批交给外部 AI 翻译");
        }
        ImGui.Spacing();
        ImGui.Separator();
        ImGui.Spacing();

        // ── ② 预翻译 ──
        ImGui.TextUnformatted("② 预翻译（词典预填：我的翻译/个性翻译/wiki/AI知识库 能翻的自动填上）");
        ImGui.Spacing();
        if (ImGui.Button("预翻译"))
        {
            var files = ListUntranslatedFiles(transDir);
            if (files.Count == 0)
            {
                _result = "未找到 _未翻译.json，请先执行 ① 提取英文";
            }
            else
            {
                var hit = 0;
                var done = 0;
                foreach (var path in files)
                {
                    var n = Prefill(path, modRoot ?? "");
                    if (n >= 0)
                    {
                        hit += n;
                        done++;
                    }
                }
                _result = done == 0 ? "预翻译失败：文件解析错误" : $"预翻译完成：{done} 个文件，命中 {hit} 项（仍为英文的交给 ③ AI 翻译）";
            }
        }
        ImGui.Spacing();
        ImGui.Separator();
        ImGui.Spacing();

        // ── ③ AI 翻译 ──
        ImGui.TextUnformatted("③ AI 翻译（把仍未翻译的项交给所选供应商）");
        ImGui.Spacing();
        var providerName = AiTranslateService.CurrentProviderName(cfg);
        ImGui.TextDisabled($"供应商：{providerName}" +
                           $"（{AiTranslateService.ResolveEndpoint(cfg).Model}）");
        if (string.IsNullOrWhiteSpace(AiTranslateService.GetApiKey(cfg)))
        {
            ImGui.SameLine();
            ImGui.TextDisabled("　⚠ 未填写 API Key");
        }
        ImGui.Spacing();

        if (_task != null && !_task.IsCompleted)
        {
            ImGui.TextWrapped(_taskStatus);
            ImGui.TextDisabled("翻译进行中，请等待…（可切到其他窗口，完成后回来查看）");
        }
        else
        {
            if (ImGui.Button("AI 翻译"))
            {
                var files = ListUntranslatedFiles(transDir);
                if (files.Count == 0)
                {
                    _result = "未找到 _未翻译.json，请先执行 ① 提取英文";
                }
                else
                {
                    _result = "";
                    _taskStatus = "AI 翻译进行中…";
                    _task = Task.Run(async () =>
                    {
                        var total = 0;
                        foreach (var input in files)
                        {
                            var output = Path.ChangeExtension(input, null) + "_已翻译.json";
                            _taskStatus = $"AI 翻译中：{Path.GetFileName(input)}…";
                            total += await _ai.TranslateAsync(input, output, cfg);
                        }
                        return total;
                    });
                }
            }
            if (ImGui.IsItemHovered())
            {
                ImGui.SetTooltip($"翻译翻译目录下所有 _未翻译.json\n分别写出对应的 _已翻译.json（外部 AI 翻好的文件也按此命名即可被 ④ 汇总）");
            }
            ImGui.SameLine();
            if (ImGui.Button("取消 AI 翻译"))
            {
                _task = null;
                _taskStatus = "";
                _result = "已取消（当前批次可能仍会完成）";
            }
            if (ImGui.IsItemHovered())
            {
                ImGui.SetTooltip("取消后续批次（当前批次可能仍会完成）");
            }
        }
        ImGui.Spacing();
        ImGui.Separator();
        ImGui.Spacing();

        // ── ④ 汇总已翻译内容 ──
        ImGui.TextUnformatted("④ 汇总已翻译内容（把 _已翻译.json 的译文编入 我的翻译.json，已有译文不覆盖）");
        ImGui.Spacing();
        if (ImGui.Button("汇总已翻译内容"))
        {
            var files = Directory.Exists(transDir)
                ? Directory.GetFiles(transDir, "*_已翻译.json", SearchOption.TopDirectoryOnly).ToList()
                : new List<string>();
            if (files.Count == 0)
            {
                _result = "未找到 _已翻译.json，请先执行 ③ AI 翻译（或把外部 AI 翻好的文件命名为 <模组名>_已翻译.json）";
            }
            else
            {
                var total = 0;
                foreach (var f in files)
                {
                    total += _sumup.Sumup(f, cfg.DictionaryPath);
                }
                _result = $"汇总完成：{files.Count} 个文件，新增 {total} 条 → 我的翻译.json";
                if (total > 0) _plugin.ReloadDictionary();
            }
        }
        if (ImGui.IsItemHovered())
        {
            ImGui.SetTooltip("沉淀翻译到 我的翻译.json，之后翻译更准；没有 Key 走外部 AI 的用户，这一步就是独立版的「4. 汇总已翻译内容」");
        }
        ImGui.Spacing();
        ImGui.Separator();
        ImGui.Spacing();

        // ── ⑤ 翻译写入MOD ──
        ImGui.TextUnformatted("⑤ 翻译写入MOD（读取词典译文：我的翻译/个性翻译/wiki/AI知识库，写回勾选的模组文件并重载）");
        ImGui.Spacing();
        if (ImGui.Button("翻译写入MOD"))
        {
            if (notFound)
            {
                _result = "无法获取 Penumbra 模组根目录";
            }
            else
            {
                var mods = _plugin.MainWindow.SelectedMods;
                if (mods.Count == 0)
                {
                    _result = "未勾选任何模组，请先到主窗口勾选（或点「全选」）";
                }
                else
                {
                    var n = _import.ApplyDictionary(modRoot ?? "", _dict, mods);
                    _result = _import.LastResult;
                }
            }
        }
        if (ImGui.IsItemHovered())
        {
            ImGui.SetTooltip("直接把词典里的译文应用到模组选项（组名/选项名/描述）并重载，无需 _已翻译.json。\n" +
                             "用外部 AI 翻译时：先执行 ④ 汇总（把译文编入 我的翻译.json）再点这里写回。\n" +
                             "已含中文的选项不会重复覆盖；写回前自动备份（zip）");
        }
        ImGui.Spacing();
        ImGui.Separator();
        ImGui.Spacing();

        // 流程结果日志区：带边框统一风格
        Plugin.ResultBox("##PipelineResult", _result, "操作结果将显示在这里（如：提取完成 N 个模组…）");

        // 轮询 AI 任务完成
        if (_task != null && _task.IsCompleted)
        {
            try
            {
                _result = _ai.LastResult;
            }
            finally
            {
                _task = null;
                _taskStatus = "";
            }
        }
    }

    /// <summary> 列出翻译目录下所有 _未翻译.json（含按模组文件与 全部模组_未翻译.json）。 </summary>
    private static List<string> ListUntranslatedFiles(string transDir)
        => Directory.Exists(transDir)
            ? Directory.GetFiles(transDir, "*_未翻译.json", SearchOption.TopDirectoryOnly).ToList()
            : new List<string>();

    /// <summary> 词典预填：翻译空值项。返回命中数。 </summary>
    private int Prefill(string path, string modRoot)
    {
        try
        {
            var root = JsonNode.Parse(File.ReadAllText(path, Encoding.UTF8)) as JsonObject;
            if (root == null) return -1;
            var hit = 0;

            foreach (var sec in new[] { "_options", "_descriptions" })
            {
                if (root[sec] is not JsonObject obj) continue;
                foreach (var kv in obj.ToList())
                {
                    var text = kv.Value?.ToString() ?? "";
                    if (text.Length > 0) continue;
                    var parts = kv.Key.Split(new[] { "||" }, StringSplitOptions.None);
                    if (parts.Length != 3) continue;
                    var modKey = parts[0];
                    var translated = Translator.Translate(parts[2], modKey, _dict);
                    if (translated.Length > 0 && _dict.ContainsChinese(translated))
                    {
                        obj[kv.Key] = translated;
                        hit++;
                    }
                }
            }

            File.WriteAllText(path, root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }), Encoding.UTF8);
            return hit;
        }
        catch (Exception ex)
        {
            _result = "预翻译失败：" + ex.Message;
            return -1;
        }
    }
}
