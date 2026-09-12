using System;
using System.Collections.Generic;
using System.Linq;

namespace FFXIVPenumbraHanhua.Services;

/// <summary> 应用日志（内存环形缓冲）。级别：信息/警告/错误。 </summary>
public sealed class AppLog
{
    public enum Level { Info, Warn, Error }

    public sealed record Entry(DateTime Time, Level Lv, string Text);

    private readonly List<Entry> _entries = new();
    private readonly object _lock = new();
    private const int Capacity = 2000;

    /// <summary> 最近一条错误（置底显示用）。 </summary>
    public string LastError { get; private set; } = "";

    public event Action? Changed;

    public void Info(string text) => Add(Level.Info, text);
    public void Warn(string text) => Add(Level.Warn, text);
    public void Error(string text)
    {
        Add(Level.Error, text);
        LastError = $"[{DateTime.Now:HH:mm:ss}] {text}";
        Changed?.Invoke();
    }

    private void Add(Level lv, string text)
    {
        lock (_lock)
        {
            _entries.Add(new Entry(DateTime.Now, lv, text));
            if (_entries.Count > Capacity) _entries.RemoveRange(0, _entries.Count - Capacity);
        }
    }

    /// <summary> 快照（新→旧）。 </summary>
    public IReadOnlyList<Entry> Snapshot()
    {
        lock (_lock)
        {
            return _entries.AsEnumerable().Reverse().ToList();
        }
    }

    /// <summary> 清除日志。 </summary>
    public void Clear()
    {
        lock (_lock)
        {
            _entries.Clear();
            LastError = "";
        }
        Changed?.Invoke();
    }
}
