#pragma once

#pragma execution_character_set("utf-8")

#include "Resource.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <wininet.h>

#include <string>
#include <vector>
#include <map>
#include <set>
#include <unordered_map>
#include <atomic>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <thread>
#include <mutex>
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <cctype>
#include <functional>

#include "json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

// ------------------------------------------------------------------
// 字符编码工具函数（内部 UTF-8，界面/系统 API UTF-16）
// ------------------------------------------------------------------
inline std::wstring utf8_to_wstring(const std::string& utf8)
{
    if (utf8.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring wstr(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wstr[0], len);
    wstr.resize(wstr.size() - 1); // 去掉结尾 \0
    return wstr;
}

inline std::string wstring_to_utf8(const std::wstring& wstr)
{
    if (wstr.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string utf8(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &utf8[0], len, nullptr, nullptr);
    utf8.resize(utf8.size() - 1);
    return utf8;
}

// 过滤非法 UTF-8 字节
inline std::string clean_utf8(const std::string& input)
{
    std::string out;
    out.reserve(input.size());
    size_t i = 0;
    const size_t n = input.size();
    while (i < n) {
        unsigned char c = (unsigned char)input[i];
        size_t extra = 0;
        unsigned char lo = 0x80, hi = 0xBF; // 第二个续字节的合法范围
        if (c < 0x80) {
            out += (char)c;
            i++;
            continue;
        }
        else if (c >= 0xC2 && c <= 0xDF) {
            extra = 1;
        }
        else if (c >= 0xE0 && c <= 0xEF) {
            extra = 2;
            lo = (c == 0xE0) ? 0xA0 : (c == 0xED) ? 0x80 : 0x80;
            hi = (c == 0xED) ? 0x9F : 0xBF;
        }
        else if (c >= 0xF0 && c <= 0xF4) {
            extra = 3;
            lo = (c == 0xF0) ? 0x90 : (c == 0xF4) ? 0x80 : 0x80;
            hi = (c == 0xF4) ? 0x8F : 0xBF;
        }
        else {
            i++;
            continue;
        }
        if (i + extra >= n) break;
        unsigned char c1 = (unsigned char)input[i + 1];
        if (c1 < lo || c1 > hi) { i++; continue; }
        bool ok = true;
        for (size_t k = 2; k <= extra; ++k) {
            unsigned char cc = (unsigned char)input[i + k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
        }
        if (!ok) { i++; continue; }
        out.append(input, i, extra + 1);
        i += extra + 1;
    }
    return out;
}

// ------------------------------------------------------------------
// AI 服务商预设（下拉选择用；完整列表存 config.json，可自由增删）
// ------------------------------------------------------------------
struct AIPreset {
    std::string name;     // 显示名，如 "DeepSeek"
    std::string model;    // 模型名，如 "deepseek-v4-flash"
    std::string baseUrl;  // API Base URL，如 "https://api.deepseek.com"
    std::string note;     // 中文备注（可选，显示在下拉列表里；config.json 里可写，保存不丢）
};

// AI API Key 条目（下拉选择用；列表存 config.user.json，可自由增删）
struct AIKeyEntry {
    std::string name;     // 备注名，如 "我的主 key"
    std::string key;      // API Key 值
};

// 用户点「保存」写入的自定义 AI 记录（存 config.user.json 的 customSaves 数组）
struct AISaveEntry {
    std::string name;     // 备注名（空 = 自动命名「自定义 N」）
    std::string key;      // API Key
    std::string model;    // 模型名
    std::string baseUrl;  // API Base URL
    std::string note;     // 备注（当前固定「自定义」）
};

// ------------------------------------------------------------------
// 配置结构体
// ------------------------------------------------------------------
struct AppConfig {
    std::string penumbraDir;          // Penumbra 父目录
    std::string translationDir;       // 翻译目录
    std::string dictionaryDir;        // 词典目录（旧版也在此存放 config.json，可迁移）
    bool swapWordOrder = true;        // 词序调换（默认勾选：显示「中文（英文）」）
    bool autoBackup = true;           // 自动备份
    bool pureChinese = false;         // 纯中文模式
    std::vector<std::string> blacklist = { "Uranus", "EXQB", "Yanilla", "Rue", "Lavabod" }; // 黑名单（默认含体型 mod 专名，可在黑名单界面自行增删）
    std::vector<std::string> wikiCategories; // Wiki 导出选中的分类 prefix
    std::string aiApiKey;      // AI 翻译 API Key（当前选中项，OpenAI 兼容）
    std::string aiKeyName;     // 当前选中 Key 的备注名（空 = 自定义输入未保存）
    std::vector<AIKeyEntry> aiKeys; // 已保存的 Key 列表（备注 + Key，可手动增删）
    std::vector<AISaveEntry> customSaves; // 用户点「保存」写入的自定义 AI 记录（存 config.user.json）
    std::string aiModel;       // AI 模型名（空 = 未设置）
    std::string aiBaseUrl;     // API Base URL（空 = 未设置）
    std::vector<AIPreset> aiPresets; // AI 下拉预设列表（存 config.json，可手动增删）
    std::string aiPreset;      // 当前选中的预设名（空 = 自定义输入）
    int aiBatchSize = 40;      // AI 每批翻译的词条数
    int fontSize = 11;         // 主界面字体大小（点，默认 11）
    bool autoFontSize = true;  // 窗口缩放时是否自动调整字体大小
    int winX = -1, winY = -1;  // 上次退出时的窗口位置（-1 表示未保存，使用系统默认）
    int winW = 0, winH = 0;    // 上次退出时的窗口大小（0 表示使用默认尺寸）
};

// ------------------------------------------------------------------
// 全局配置与状态
// ------------------------------------------------------------------
extern AppConfig g_cfg;
extern HWND g_hMainWnd;
extern HWND g_hLogEdit;           // RichEdit / Edit 日志控件
extern std::atomic<bool> g_busy;  // 是否正在执行耗时任务

// ------------------------------------------------------------------
// 工具函数
// ------------------------------------------------------------------
inline std::string now_timestamp()
{
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm t;
    localtime_s(&t, &tt);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", &t);
    return buf;
}

inline std::string now_timestamp_human()
{
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm t;
    localtime_s(&t, &tt);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y年%m月%d日 %H-%M-%S", &t);
    return buf;
}

// 读取文件（二进制）。使用宽字符路径重载，避免 ANSI 代码页转换失败导致丢文件
inline bool read_binary_file(const fs::path& path, std::string& out)
{
    try {
        std::ifstream in(path.c_str(), std::ios::binary);
        if (!in) return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
        return true;
    }
    catch (...) { return false; }
}

// 写入文件（二进制）。使用宽字符路径重载，避免 ANSI 代码页转换失败
inline bool write_binary_file(const fs::path& path, const std::string& data)
{
    try {
        std::ofstream out(path.c_str(), std::ios::binary);
        if (!out) return false;
        out.write(data.data(), (std::streamsize)data.size());
        return true;
    }
    catch (...) { return false; }
}

// 是否包含中文字符
inline bool contains_chinese(const std::string& s)
{
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if ((c & 0xF0) == 0xE0) { // 3 字节 UTF-8，CJK 基本区 U+4E00-U+9FFF
            if (i + 2 < s.size()) {
                unsigned char b0 = (unsigned char)s[i];
                unsigned char b1 = (unsigned char)s[i + 1];
                unsigned char b2 = (unsigned char)s[i + 2];
                if ((b0 & 0xF0) == 0xE0 && (b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80) {
                    int code = ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
                    if (code >= 0x4E00 && code <= 0x9FFF) return true;
                }
            }
        }
    }
    return false;
}

// 是否包含英文字母（ASCII a-z/A-Z）。用于区分"纯数字/符号"与真正的英文残留
inline bool contains_english_letter(const std::string& s)
{
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return true;
    }
    return false;
}

// 从已翻译文本中还原英文原文。
// 支持 "中文（英文）" 与 "英文（中文）" 两种格式；无法解析时返回空串。
// 注意：全角括号 "（" "）" 在 UTF-8 中各占 3 字节。
inline std::string extract_english_from_translated(const std::string& v)
{
    auto open = v.find("（");
    auto close = v.rfind("）");
    if (open == std::string::npos || close == std::string::npos || close <= open + 3) return "";
    std::string a = v.substr(0, open);
    std::string b = v.substr(open + 3, close - open - 3);
    bool aZh = contains_chinese(a);
    bool bZh = contains_chinese(b);
    if (!aZh && bZh) return a;  // 英文（中文）
    if (aZh && !bZh) return b;  // 中文（英文）
    return "";
}

// 判断文本是否命中黑名单（整词完全匹配：trim 后与黑名单词完全相同才命中，忽略大小写）。
// v2.2.10：由子串匹配改为整词匹配——"Lavabod" 只保护 Lavabod 本身，
// 不再拦截 "Lavabod Teardrop" 这类组合（组合条目可正常翻译其余部分，专名交给 AI 按提示词保留），
// 同时消除 rue→true、masc→masculine 之类的子串误伤。
inline bool is_blacklisted(const std::string& text, const std::vector<std::string>& blacklist)
{
    if (text.empty() || blacklist.empty()) return false;
    size_t a = text.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return false;
    size_t b = text.find_last_not_of(" \t\r\n");
    std::string t = text.substr(a, b - a + 1);
    std::transform(t.begin(), t.end(), t.begin(), ::tolower);
    for (const auto& w : blacklist) {
        if (w.empty()) continue;
        std::string bl = w;
        size_t ba = bl.find_first_not_of(" \t\r\n");
        if (ba == std::string::npos) continue;
        size_t bb = bl.find_last_not_of(" \t\r\n");
        bl = bl.substr(ba, bb - ba + 1);
        std::transform(bl.begin(), bl.end(), bl.begin(), ::tolower);
        if (t == bl) return true;
    }
    return false;
}

// URL 编码（UTF-8 字节）
inline std::string url_encode(const std::string& s)
{
    std::ostringstream out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out << c;
        else
            out << '%' << std::uppercase << std::hex << (int)c << std::nouppercase;
    }
    return out.str();
}

// 判断文件是否匹配 group_*.json
inline bool is_group_json(const std::wstring& filename)
{
    return filename.rfind(L"group_", 0) == 0 &&
           filename.size() > 5 &&
           filename.substr(filename.size() - 5) == L".json";
}
