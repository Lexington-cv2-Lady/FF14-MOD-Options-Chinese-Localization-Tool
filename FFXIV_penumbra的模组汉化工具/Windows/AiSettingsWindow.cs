using System;
using System.Numerics;
using System.Threading.Tasks;
using Dalamud.Bindings.ImGui;
using Dalamud.Interface.Utility;
using Dalamud.Interface.Windowing;
using FFXIVPenumbraHanhua.Services;

namespace FFXIVPenumbraHanhua.Windows;

/// <summary> AI 设置窗口：供应商 / Key / 模型 / 温度 / 批量 / 测试连接。 </summary>
public class AiSettingsWindow : Window, IDisposable
{
    private readonly Plugin _plugin;
    private readonly AiTranslateService _ai;
    private string _testResult = "";
    private Task<string>? _testTask;
    private bool _showKey; // API Key 明文显示开关

    public AiSettingsWindow(Plugin plugin) : base("AI 设置###HanhuaAi")
    {
        Size = new Vector2(640, 480);
        SizeCondition = ImGuiCond.FirstUseEver;
        SizeConstraints = new WindowSizeConstraints
        {
            MinimumSize = new Vector2(480, 360),
            MaximumSize = new Vector2(float.MaxValue, float.MaxValue)
        };

        _plugin = plugin;
        _ai = plugin.AiTranslate;
    }

    public void Dispose() { }

    public override void Draw()
    {
        var cfg = _plugin.Configuration;

        ImGui.TextWrapped("配置 OpenAI 兼容接口（智谱 / 通义 / 混元 / 千帆 / OpenRouter / GPT 等），用于「翻译管线 → AI 翻译」。");
        ImGui.Spacing();
        ImGui.Separator();
        ImGui.Spacing();

        // 供应商（自定义置顶，国内优先，海外在后；可选自定义覆盖）
        ImGui.TextUnformatted("供应商（自定义置顶；国内优先；可选自定义覆盖）：");
        var current = Math.Clamp(cfg.AiProvider, 0, AiTranslateService.Providers.Length - 1);
        var customMode = cfg.AiProvider < 0;
        var shown = customMode ? "自定义（使用下方 API 地址）" : AiTranslateService.Providers[current].Name;
        if (ImGui.BeginCombo("##Provider", shown))
        {
            if (ImGui.Selectable("自定义（使用下方 API 地址）", customMode))
            {
                cfg.AiProvider = -1;
                _testResult = "已切换为「自定义」，Key 各服务商独立保存";
            }
            if (customMode) ImGui.SetItemDefaultFocus();
            for (var i = 0; i < AiTranslateService.Providers.Length; i++)
            {
                var sel = !customMode && i == current;
                if (ImGui.Selectable(AiTranslateService.Providers[i].Name, sel))
                {
                    cfg.AiProvider = i;
                    _testResult = $"已切换为「{AiTranslateService.Providers[i].Name}」，Key 各服务商独立保存";
                }
                if (sel) ImGui.SetItemDefaultFocus();
            }
            ImGui.EndCombo();
        }
        if (ImGui.IsItemHovered())
        {
            ImGui.SetTooltip(customMode
                ? "自定义模式：完全使用下方「API 地址 + 模型」，需自行填写"
                : AiTranslateService.Providers[current].Note);
        }

        ImGui.Spacing();

        // BaseUrl
        ImGui.TextUnformatted("API 地址（留空 = 供应商预设）：");
        var baseUrl = cfg.AiBaseUrl;
        ImGui.SetNextItemWidth(ImGui.GetContentRegionAvail().X);
        if (ImGui.InputText("##AiBaseUrl", ref baseUrl, 512))
        {
            cfg.AiBaseUrl = baseUrl;
        }
        ImGui.TextDisabled($"当前生效：{AiTranslateService.ResolveEndpoint(cfg).BaseUrl}/chat/completions");

        ImGui.Spacing();

        // API Key（按服务商独立保存；带「显示/隐藏」+「粘贴」按钮：绕过游戏内焦点问题）
        ImGui.TextUnformatted("API Key（仅当前服务商）：");
        var key = AiTranslateService.GetApiKey(cfg);
        var pasteW = 64f * ImGuiHelpers.GlobalScale;
        var showW = 52f * ImGuiHelpers.GlobalScale;
        ImGui.SetNextItemWidth(Math.Max(120f, ImGui.GetContentRegionAvail().X - pasteW - showW - 16f * ImGuiHelpers.GlobalScale));
        var keyFlags = _showKey ? ImGuiInputTextFlags.None : ImGuiInputTextFlags.Password;
        if (ImGui.InputText("##AiKey", ref key, 512, keyFlags))
        {
            AiTranslateService.SetApiKey(cfg, key);
        }
        ImGui.SameLine();
        if (ImGui.Button(_showKey ? "隐藏" : "显示", new Vector2(showW, 0)))
        {
            _showKey = !_showKey;
        }
        if (ImGui.IsItemHovered())
        {
            ImGui.SetTooltip(_showKey ? "隐藏 API Key（恢复密文）" : "显示 API Key 明文（确认是否输错）");
        }
        ImGui.SameLine();
        if (ImGui.Button("粘贴", new Vector2(pasteW, 0)))
        {
            var clip = ImGui.GetClipboardText();
            if (!string.IsNullOrWhiteSpace(clip))
            {
                AiTranslateService.SetApiKey(cfg, clip);
                _testResult = "已从剪贴板粘贴 API Key";
            }
            else
            {
                _testResult = "剪贴板为空，粘贴失败";
            }
        }
        if (ImGui.IsItemHovered())
        {
            ImGui.SetTooltip("直接读取剪贴板填入，无需 Ctrl+V（避开游戏内焦点问题）");
        }
        if (string.IsNullOrWhiteSpace(AiTranslateService.GetApiKey(cfg)))
        {
            ImGui.TextColored(new Vector4(1f, 0.5f, 0.2f, 1f), "未填写 Key：AI 翻译不可用，可改用外部 AI 翻译（导出 _未翻译.json → 外部翻译 → ④ 汇总 → ⑤ 写回）。");
        }

        ImGui.Spacing();

        // 模型
        ImGui.TextUnformatted("模型（留空 = 供应商预设）：");
        var model = cfg.AiModel;
        ImGui.SetNextItemWidth(ImGui.GetContentRegionAvail().X);
        if (ImGui.InputText("##AiModel", ref model, 256))
        {
            cfg.AiModel = model;
        }
        ImGui.TextDisabled($"当前生效：{AiTranslateService.ResolveEndpoint(cfg).Model}");

        ImGui.Spacing();

        // 温度 + 批量
        var temp = cfg.AiTemperature;
        if (ImGui.SliderFloat("温度（越低越忠实原文）", ref temp, 0f, 1f))
        {
            cfg.AiTemperature = temp;
        }
        var batch = cfg.AiBatchSize;
        ImGui.SetNextItemWidth(220 * ImGuiHelpers.GlobalScale);
        if (ImGui.InputInt("单批条数", ref batch, 10, 50))
        {
            cfg.AiBatchSize = Math.Clamp(batch, 1, 500);
        }
        ImGui.SameLine();
        ImGui.TextDisabled("（条目过多自动按平台字符上限拆批）");

        ImGui.Spacing();

        // 关闭深度思考 + 联网搜索（联网仅通义/百炼支持，其他平台置灰）
        var disableThinking = cfg.AiDisableThinking;
        if (ImGui.Checkbox("关闭深度思考", ref disableThinking))
        {
            cfg.AiDisableThinking = disableThinking;
        }
        if (ImGui.IsItemHovered())
        {
            ImGui.SetTooltip("对支持关闭思考的模型生效（DeepSeek V4 / GLM-5.2 / Kimi K2.6 / 通义 Qwen3），可加快响应、节省 token");
        }
        ImGui.SameLine();
        var webSupported = AiTranslateService.PlatformSupportsWebSearch(cfg);
        if (!webSupported)
        {
            cfg.AiWebSearch = false; // 平台不支持时自动关闭
        }
        var webSearch = cfg.AiWebSearch;
        ImGui.BeginDisabled(!webSupported);
        if (ImGui.Checkbox("联网搜索", ref webSearch))
        {
            cfg.AiWebSearch = webSearch;
        }
        ImGui.EndDisabled();
        if (ImGui.IsItemHovered())
        {
            ImGui.SetTooltip(webSupported
                ? "AI 翻译时参考互联网搜索结果（通义/百炼专用参数 enable_search）"
                : "当前平台（地址）不支持联网搜索：仅通义/百炼（dashscope/aliyuncs）支持");
        }

        ImGui.Spacing();
        ImGui.Separator();
        ImGui.Spacing();

        // 测试连接
        if (_testTask != null && !_testTask.IsCompleted)
        {
            ImGui.TextDisabled("测试中…");
        }
        else
        {
            if (ImGui.Button("测试连接"))
            {
                _testResult = "";
                _testTask = Task.Run(() => _ai.TestAsync(cfg));
            }
            ImGui.SameLine();
            if (ImGui.Button("保存设置"))
            {
                cfg.Save();
                _testResult = "设置已保存";
            }
            ImGui.Spacing();
            if (_testTask != null && _testTask.IsCompleted)
            {
                _testResult = _testTask.Result;
                _testTask = null;
            }
            // 测试结果日志区：带边框统一风格
            Plugin.ResultBox("##AiResult", _testResult, "测试结果将显示在这里（如：连接成功…）");
        }

        ImGui.Spacing();
        ImGui.Separator();
        ImGui.Spacing();
        ImGui.TextDisabled("提示：无 Key 可走「免费AI」——在翻译管线里导出 _未翻译.json，交给外部 AI 翻译后放回翻译目录，再点「翻译写入MOD」。");
    }
}
