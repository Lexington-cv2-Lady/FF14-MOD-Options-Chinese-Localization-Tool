using System;
using System.Collections.Generic;
using System.Linq;
using Dalamud.Plugin;
using Penumbra.Api.Enums;
using Penumbra.Api.IpcSubscribers;

namespace FFXIVPenumbraHanhua.Services;

/// <summary> Penumbra 外部 API 封装：模组列表、写后重载、增删事件。 </summary>
public sealed class PenumbraService : IDisposable
{
    private readonly GetModList _getModList;
    private readonly ReloadMod _reloadMod;
    private readonly GetModDirectory _getModDirectory;
    private readonly IDisposable _modAddedSub;
    private readonly IDisposable _modDeletedSub;
    private readonly IDisposable _disposedSub;

    /// <summary> Penumbra 可用状态变化（含初始连接成功）。 </summary>
    public event Action? ModsChanged;

    /// <summary> Penumbra 已卸载/失效。 </summary>
    public event Action? PenumbraDisposed;

    /// <summary> 当前已加载的模组快照（目录名 → 显示名）。 </summary>
    public IReadOnlyList<ModEntry> Mods { get; private set; } = [];

    /// <summary> 最近一次获取模组列表的结果描述（供界面显示）。 </summary>
    public string Status { get; internal set; } = "未连接 Penumbra";

    public PenumbraService(IDalamudPluginInterface pi)
    {
        _getModList = new GetModList(pi);
        _reloadMod = new ReloadMod(pi);
        _getModDirectory = new GetModDirectory(pi);

        _modAddedSub = ModAdded.Subscriber(pi, _ => Refresh());
        _modDeletedSub = ModDeleted.Subscriber(pi, _ => Refresh());
        _disposedSub = Disposed.Subscriber(pi, () => PenumbraDisposed?.Invoke());
    }

    /// <summary> 拉取最新模组列表。返回是否成功。 </summary>
    public bool Refresh()
    {
        try
        {
            var dict = _getModList.Invoke();
            Mods = dict
                .Select(kv => new ModEntry(kv.Key, kv.Value))
                .OrderBy(m => m.Name, StringComparer.OrdinalIgnoreCase)
                .ToList();
            Status = $"Penumbra 已连接，共 {Mods.Count} 个模组";
            ModsChanged?.Invoke();
            return true;
        }
        catch (Exception ex)
        {
            Mods = [];
            Status = $"Penumbra 不可用：{ex.Message}";
            return false;
        }
    }

    /// <summary> 写回文件后触发 Penumbra 重新加载该模组，游戏内立即生效。 </summary>
    public PenumbraApiEc Reload(string modDirectory, string modName = "")
    {
        try
        {
            return _reloadMod.Invoke(modDirectory, modName);
        }
        catch (Exception)
        {
            return PenumbraApiEc.InvalidArgument;
        }
    }

    /// <summary> 获取 Penumbra 当前模组根目录，失败返回 null。 </summary>
    public string? GetModRoot()
    {
        try
        {
            return _getModDirectory.Invoke();
        }
        catch (Exception)
        {
            return null;
        }
    }

    public void Dispose()
    {
        _modAddedSub.Dispose();
        _modDeletedSub.Dispose();
        _disposedSub.Dispose();
    }
}

/// <summary> 一个已安装模组的最小信息。 </summary>
public sealed record ModEntry(string Directory, string Name);
