using Dalamud.Configuration;
using System;
using System.Collections.Generic;

namespace FFXIVPenumbraHanhua;

[Serializable]
public class Configuration : IPluginConfiguration
{
    public int Version { get; set; } = 0;

    /// <summary> 词典目录（复用独立版的 E:\FFXIV_MOD\词典目录）。 </summary>
    public string DictionaryPath { get; set; } = "E:\\FFXIV_MOD\\词典目录";

    /// <summary> 翻译目录（独立版的 E:\FFXIV_MOD\翻译目录，AI 翻译管线使用）。 </summary>
    public string TranslationPath { get; set; } = "E:\\FFXIV_MOD\\翻译目录";

    /// <summary> 是否自动刷新模组列表。 </summary>
    public bool AutoRefresh { get; set; } = true;

    /// <summary> 写回前备份轮转保留份数。 </summary>
    public int BackupCount { get; set; } = 5;

    // ── AI 翻译设置 ──
    /// <summary> 已选 AI 供应商（OpenAI 兼容端点预置表的下标）。 </summary>
    public int AiProvider { get; set; }

    /// <summary> 自定义 API 地址（覆盖供应商预设；空 = 用预设）。 </summary>
    public string AiBaseUrl { get; set; } = "";

    /// <summary> API Key（旧版单一保存字段，兼容迁移用；新版按服务商保存在 AiApiKeys）。 </summary>
    public string AiApiKey { get; set; } = "";

    /// <summary> 按服务商分别保存的 API Key（键：供应商名 / 「自定义」）。 </summary>
    public Dictionary<string, string> AiApiKeys { get; set; } = new();

    /// <summary> 模型名（空 = 用供应商预设）。 </summary>
    public string AiModel { get; set; } = "";

    /// <summary> 采样温度。 </summary>
    public float AiTemperature { get; set; } = 0.2f;

    /// <summary> 单次请求最大条数（超过字符上限也会自动拆批）。 </summary>
    public int AiBatchSize { get; set; } = 80;

    /// <summary> 关闭深度思考（仅对支持关闭的模型生效，如 DeepSeek V4 / GLM / Qwen3）。 </summary>
    public bool AiDisableThinking { get; set; }

    /// <summary> 联网搜索（仅通义/百炼 OpenAI 兼容端支持，其他平台自动失效）。 </summary>
    public bool AiWebSearch { get; set; }

    public void Save()
    {
        Plugin.PluginInterface.SavePluginConfig(this);
    }
}
