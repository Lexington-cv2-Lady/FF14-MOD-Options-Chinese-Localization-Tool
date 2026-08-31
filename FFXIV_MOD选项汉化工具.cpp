// FFXIV_MOD选项汉化工具.cpp : 定义应用程序的入口点。
//
// 内部统一 UTF-8，界面/系统 API 使用 UTF-16（Unicode API）。
// 文件 I/O 一律二进制模式。
// JSON 使用 nlohmann/json（原生 UTF-8）。

#include "framework.h"
#include "FFXIV_MOD选项汉化工具.h"
#include <richedit.h>

// 自定义消息：工作线程通知主线程追加日志 / 更新进度 / 结束
#define WM_APP_LOG       (WM_APP + 1)
#define WM_APP_DONE      (WM_APP + 2)
#define WM_APP_PROGRESS  (WM_APP + 3)

// 「个性翻译」按钮 ID：.rc 里控件直接使用数字 1348，避免 IDE 资源编辑器回滚 Resource.h 手加宏
#ifndef IDC_BTN_CUSTOM
#define IDC_BTN_CUSTOM  1348
#endif

AppConfig g_cfg;
HINSTANCE hInst = nullptr;
HWND g_hMainWnd = nullptr;
HWND g_hLogEdit = nullptr;
HFONT g_hFont = nullptr;        // 主界面自定义字体（随字体大小重建）
std::atomic<bool> g_busy{ false };
std::atomic<bool> g_cancel{ false };
volatile HINTERNET g_hActiveReq = nullptr; // 当前活动 HTTP 请求句柄（取消时立即打断，无需等超时）
static std::vector<fs::path> g_extractFiles; // 提取英文对话框选中的文件集合（空 = 全量提取）

// 提取英文对话框控件 ID（rc 中直接使用字面量数字，防止 IDE 资源编辑器回滚 Resource.h 宏）
#ifndef IDD_EXTRACT_DIALOG
#define IDD_EXTRACT_DIALOG 1260
#define IDC_EXTRACT_LEFTLIST 1261
#define IDC_EXTRACT_RIGHTLIST 1262
#define IDC_EXTRACT_ALL 1263
#define IDC_EXTRACT_BTN 1264
#define IDC_EXTRACT_CLOSE 1265
#endif
std::mutex g_logMutex;
std::vector<std::wstring> g_logBuffer; // 日志缓冲：改字号后重放，恢复各行的标签颜色
std::vector<std::wstring> g_logTopBuffer; // 日志置顶区：写入日志.json 时始终置于文件最前（如命中词条汇总）
std::string g_workerName;
std::vector<std::string> g_wikiPrefixes; // Wiki 导出当前选中的分类
std::vector<fs::path> g_importFiles; // 导入翻译：选定的文件（可多选，点中即勾选）
bool g_importAutoFill = true; // 导入翻译：是否先用词典补全空白项
bool g_keyVisible = false;    // AI API Key 是否明文显示
static int g_dlgW0 = 0, g_dlgH0 = 0; // 主窗口 .rc 设计尺寸对应客户区像素（缩放布局基准）
struct CtrlLayout {
    HWND hwnd;
    RECT rc; // 初始位置（客户区坐标，像素）
};
static std::vector<CtrlLayout> g_layout;
// 初始化完成标志：在此之前触发的 WM_SIZE/WM_MOVE 不记录窗口尺寸，
// 否则对话框创建时的默认大小会把 config 里的记忆值冲掉
static bool g_initialized = false;

struct WikiCategory {
    std::wstring display; // 界面显示文本（prefix + 中文说明）
    std::string prefix;   // Data namespace 中的 prefix，如 "Item/"
};
static const std::vector<WikiCategory> g_wikiCategoryList = {
    {L"物品（Item）", "Item/"},
    {L"技能/动作（Action）", "Action/"},
    {L"状态（Status）", "Status/"},
    {L"特性（Trait）", "Trait/"},
    {L"任务（Quest）", "Quest/"},
    {L"NPC（ENpcResident）", "ENpcResident/"},
    {L"怪物（BNpcName）", "BNpcName/"},
    {L"地点（PlaceName）", "PlaceName/"},
    {L"天气（Weather）", "Weather/"},
    {L"地图（Map）", "Map/"},
    {L"FATE（Fate）", "Fate/"},
    {L"成就（Achievement）", "Achievement/"},
    {L"称号（Title）", "Title/"},
    {L"职业（ClassJob）", "ClassJob/"},
    {L"种族（Race）", "Race/"},
    {L"部族（Tribe）", "Tribe/"},
    {L"守护神（GuardianDeity）", "GuardianDeity/"},
    {L"坐骑（Mount）", "Mount/"},
    {L"宠物（Minion）", "Minion/"},
    {L"管弦乐琴（Orchestrion）", "Orchestrion/"},
    {L"情感动作（Emote）", "Emote/"},
    {L"副本（InstanceContent）", "InstanceContent/"},
    {L"理符（Leve）", "Leve/"},
    {L"九宫幻卡（TripleTriadCard）", "TripleTriadCard/"},
    {L"配方（Recipe）", "Recipe/"},
    {L"召唤兽（Pet）", "Pet/"},
    {L"装备系列（ItemSeries）", "ItemSeries/"},
    {L"在线状态（OnlineStatus）", "OnlineStatus/"},
    {L"大国防联军（GrandCompany）", "GrandCompany/"},
    {L"副本搜索器（ContentFinderCondition）", "ContentFinderCondition/"},
};

// 前向声明
INT_PTR CALLBACK MainDlgProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK WikiCatsDlgProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK ImportTransDlgProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK RestoreDlgProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK ExtractDlgProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);

void Log(const std::string& utf8Msg);
void LogW(const std::wstring& wmsg);
void SetProgress(int cur, int max);
void RefreshConfigUI();
fs::path GetExeDir();
bool LoadConfigFrom(const fs::path& cfgPath, bool asUserLayer = false);
bool LoadConfig();
bool SaveConfig();
bool SelectDirDialog(HWND parent, std::wstring& out);
void OpenExplorer(HWND parent, const std::wstring& path);
void OpenDictJson(HWND parent, const char* fname);
std::vector<fs::path> ScanGroupFiles(const fs::path& root);
bool ExtractEnglish(const std::vector<fs::path>* onlyFiles = nullptr);
bool CheckTranslation();
bool ApplyTranslation();
static void FillAIPresetCombos(HWND hDlg);
static void FillAIKeyCombo(HWND hDlg);
bool ImportTranslations(const fs::path& inFile, bool autoFill);
std::vector<std::string> LoadBlacklistFile(const std::vector<std::string>& defaults);
void SaveBlacklistFile(const std::vector<std::string>& words);
static void EnsureBlacklistFile(); // v2.2.8：词典目录确定即确保 单词黑名单.json 存在
static void MigrateDirFiles(const fs::path& srcDir, const fs::path& dstDir,
                            const std::vector<std::wstring>& exactNames,
                            const std::vector<std::wstring>& substrNames = {}); // 目录变更时迁移程序文件
void WikiImportThread();
void RunExtractThread();
void RunImportThread();
void RunApplyThread();
void RunCheckThread();
void RunAITranslateThread();

// ------------------------------------------------------------------
// 日志输出（主线程直接写，工作线程经 PostMessageW）
// ------------------------------------------------------------------
// 根据日志行首标签决定显示颜色
static COLORREF PickLogColor(const std::wstring& text)
{
    if (text.find(L"[完成]") != std::wstring::npos) return RGB(0, 128, 0);      // 绿色
    if (text.find(L"[错误]") != std::wstring::npos) return RGB(220, 20, 60); // 深红
    if (text.find(L"[进度]") != std::wstring::npos) return RGB(0, 0, 255);     // 蓝色
    if (text.find(L"[提示]") != std::wstring::npos) return RGB(0, 128, 128);   // 青色
    if (text.find(L"=====") != std::wstring::npos) return RGB(128, 0, 128);    // 紫色（阶段分隔）
    return RGB(0, 0, 0); // 默认黑色
}

// 当前时间戳前缀，如 [2026-08-29 15:30:45]
static std::wstring NowTimestamp()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[40];
    swprintf_s(buf, L"[%04d-%02d-%02d %02d:%02d:%02d] ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return std::wstring(buf);
}

// 把日志缓冲完整写入 exe 同级目录的「日志.json」（自动导出，JSON 数组格式；
// 调用方需已持有 g_logMutex；每次新增日志后全量重写，保证内容始终最新）
// 置顶区（g_logTopBuffer）始终写在文件最前面。
static void FlushLogToJsonFile()
{
    if (g_logTopBuffer.empty() && g_logBuffer.empty()) return;
    try {
        json j = json::array();
        for (const auto& line : g_logTopBuffer) {
            std::string s = wstring_to_utf8(line);
            while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
            j.push_back(s);
        }
        for (const auto& line : g_logBuffer) {
            std::string s = wstring_to_utf8(line);
            while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
            j.push_back(s);
        }
        write_binary_file(GetExeDir() / L"日志.json", j.dump(2));
    } catch (...) {}
}

// 把一条日志置顶写入「日志.json」：位于文件最前面，之后的普通日志仍排在其后。
// 界面日志面板不显示（普通 LogThread 输出照常显示在面板里）。
static void PrependLogToFile(const std::wstring& text)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logTopBuffer.push_back(NowTimestamp() + text);
    // 限制置顶区大小，只保留最近的若干条
    if (g_logTopBuffer.size() > 50)
        g_logTopBuffer.erase(g_logTopBuffer.begin());
    FlushLogToJsonFile();
}

// 追加一行并按其标签上色（调用方需已持有 g_logMutex）
static void AppendLogLineLocked(const std::wstring& text)
{
    int len = GetWindowTextLengthW(g_hLogEdit);
    SendMessageW(g_hLogEdit, EM_SETSEL, len, len);
    SendMessageW(g_hLogEdit, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());

    // 为新追加的文本设置颜色
    int newLen = GetWindowTextLengthW(g_hLogEdit);
    SendMessageW(g_hLogEdit, EM_SETSEL, len, newLen);
    CHARFORMAT2W cf = {};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    cf.crTextColor = PickLogColor(text);
    cf.dwEffects = 0;
    SendMessageW(g_hLogEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    SendMessageW(g_hLogEdit, EM_SETSEL, -1, 0);
}

void AppendLogText(const std::wstring& text)
{
    if (!g_hLogEdit) return;
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::wstring ts = NowTimestamp();
    g_logBuffer.push_back(ts + text);
    // 限制缓冲大小，避免长期运行内存无限增长（重放时只保留最近部分）
    if (g_logBuffer.size() > 10000)
        g_logBuffer.erase(g_logBuffer.begin(), g_logBuffer.begin() + (g_logBuffer.size() - 5000));
    AppendLogLineLocked(ts + text);
    FlushLogToJsonFile();
    // 强制滚到最底部（覆盖用户手动上滚）
    SendMessageW(g_hLogEdit, WM_VSCROLL, SB_BOTTOM, 0);
    SendMessageW(g_hLogEdit, EM_SCROLLCARET, 0, 0);
}

// 修改字号后重绘日志：清空并按缓冲重放，恢复各行的标签颜色
void RepaintLogView()
{
    if (!g_hLogEdit) return;
    std::lock_guard<std::mutex> lock(g_logMutex);
    SendMessageW(g_hLogEdit, WM_SETTEXT, 0, (LPARAM)L"");
    for (const auto& line : g_logBuffer)
        AppendLogLineLocked(line);
    SendMessageW(g_hLogEdit, WM_VSCROLL, SB_BOTTOM, 0);
    SendMessageW(g_hLogEdit, EM_SCROLLCARET, 0, 0);
}

void Log(const std::string& utf8Msg)
{
    if (!g_hMainWnd) {
        AppendLogText(utf8_to_wstring(utf8Msg + "\r\n"));
        return;
    }
    // 工作线程不能直接 SendMessage 到 RichEdit，否则可能与主线程死锁。
    // 统一通过 PostMessage 转发到主线程，主线程在 WM_APP_LOG 里安全写入。
    std::wstring w = utf8_to_wstring(utf8Msg + "\r\n");
    PostMessageW(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)new std::wstring(w));
}

void LogW(const std::wstring& wmsg)
{
    if (!g_hMainWnd) {
        AppendLogText(wmsg + L"\r\n");
        return;
    }
    PostMessageW(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)new std::wstring(wmsg + L"\r\n"));
}

// 工作线程日志（兼容旧调用，行为与 Log 一致）
void LogThread(const std::string& msg)
{
    Log(msg);
}

// 工作线程更新进度条（转发到主线程）。max<=0 表示不确定进度（滚动）
void SetProgress(int cur, int max)
{
    if (!g_hMainWnd) return;
    PostMessageW(g_hMainWnd, WM_APP_PROGRESS, (WPARAM)(INT_PTR)cur, (LPARAM)(INT_PTR)max);
}

// ------------------------------------------------------------------
// AI 默认预设（首次运行时写入 config.default.json；之后默认配置更新时以新文件为准）
// ------------------------------------------------------------------
static const std::vector<AIPreset> g_defaultAIPresets = {
    { "DeepSeek", "deepseek-v4-flash", "https://api.deepseek.com", "DeepSeek 官方 API" },
    { "智谱 GLM", "GLM-4.5-Air", "https://open.bigmodel.cn/api/paas/v4", "智谱 AI 开放平台（OpenAI 兼容）" },
    { "通义千问", "qwen-plus", "https://dashscope.aliyuncs.com/compatible-mode/v1", "阿里云百炼（OpenAI 兼容，需先开通百炼）" },
    { "Kimi", "kimi-latest", "https://api.moonshot.cn/v1", "月之暗面 Kimi 开放平台" },
    { "豆包", "doubao-1-5-pro-32k-250115", "https://ark.cn-beijing.volces.com/api/v3", "火山方舟（模型名以控制台为准）" },
    { "腾讯混元", "hunyuan-turbo", "https://api.hunyuan.cloud.tencent.com/v1", "腾讯云大模型（OpenAI 兼容）" },
    { "百度千帆", "ernie-4.0-turbo-8k", "https://qianfan.baidubce.com/v2", "百度智能云千帆（OpenAI 兼容）" },
    { "硅基流动", "Qwen/Qwen2.5-7B-Instruct", "https://api.siliconflow.cn/v1", "开源模型聚合平台，一个 Key 调多家开源模型" },
    { "OpenRouter", "openai/gpt-4o-mini", "https://openrouter.ai/api/v1", "海外聚合中转，可调 GPT/Claude/Gemini" },
    { "Groq", "llama-3.3-70b-versatile", "https://api.groq.com/openai/v1", "开源模型超高速推理（海外）" },
    { "OpenAI（GPT）", "gpt-4o-mini", "https://api.openai.com/v1", "官方接口：国内网络不可直连，需代理或中转" },
    { "Google Gemini", "gemini-2.0-flash", "https://generativelanguage.googleapis.com/v1beta/openai", "谷歌官方 OpenAI 兼容端点：国内不可直连，需代理" },
    { "Anthropic Claude", "claude-sonnet-4-20250514", "https://api.anthropic.com/v1", "Anthropic 官方 OpenAI 兼容端点：国内不可直连，需代理" },
    { "xAI Grok", "grok-2-latest", "https://api.x.ai/v1", "马斯克 xAI 官方（OpenAI 兼容）：国内不可直连，需代理" },
    { "Mistral", "mistral-small-latest", "https://api.mistral.ai/v1", "法国 Mistral 官方（OpenAI 兼容）：国内不可直连，需代理" },
};

// ------------------------------------------------------------------
// 配置加载 / 保存
//   config.default.json —— 默认配置（zip 自带，更新程序时被新版覆盖，提供新字段/默认值兜底）
//   config.user.json    —— 用户配置（用户保存可更改的选项后才创建，更新程序时保留）
// 两文件都在程序 exe 所在目录，跟随程序走；启动时「默认 + 用户」叠加，用户值优先。
// ------------------------------------------------------------------
fs::path GetExeDir()
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    fs::path p(buf);
    return p.parent_path();
}

static fs::path DefaultConfigPath() { return GetExeDir() / "config.default.json"; }
static fs::path UserConfigPath() { return GetExeDir() / "config.user.json"; }

// 把一份配置序列化成 JSON（config.default.json / config.user.json 共用）
static json BuildConfigJson(const AppConfig& c)
{
    json j;
    j["penumbraDir"] = c.penumbraDir;
    j["translationDir"] = c.translationDir;
    j["dictionaryDir"] = c.dictionaryDir;
    j["swapWordOrder"] = c.swapWordOrder;
    j["autoBackup"] = c.autoBackup;
    j["pureChinese"] = c.pureChinese;
    j["blacklist"] = c.blacklist;
    j["wikiCategories"] = c.wikiCategories;
    j["aiApiKey"] = c.aiApiKey;
    j["aiKeyName"] = c.aiKeyName;
    {
        json ka = json::array();
        for (const auto& k : c.aiKeys)
            ka.push_back({ {"name", k.name}, {"key", k.key} });
        j["aiKeys"] = ka;
    }
    {
        json ca = json::array();
        for (const auto& s : c.customSaves)
            ca.push_back({ {"name", s.name}, {"key", s.key}, {"model", s.model}, {"baseUrl", s.baseUrl}, {"note", s.note} });
        j["customSaves"] = ca;
    }
    j["aiModel"] = c.aiModel;
    j["aiBaseUrl"] = c.aiBaseUrl;
    j["aiBatchSize"] = c.aiBatchSize;
    {
        json pa = json::array();
        for (const auto& p : c.aiPresets) {
            json item = { {"name", p.name}, {"model", p.model}, {"baseUrl", p.baseUrl} };
            if (!p.note.empty()) item["note"] = p.note;
            pa.push_back(item);
        }
        j["aiPresets"] = pa;
    }
    j["aiPreset"] = c.aiPreset;
    j["fontSize"] = c.fontSize;
    j["autoFontSize"] = c.autoFontSize;
    j["winX"] = c.winX;
    j["winY"] = c.winY;
    j["winW"] = c.winW;
    j["winH"] = c.winH;
    return j;
}

// 写出用户配置 config.user.json（首次保存可更改的选项时创建；内容一致则跳过写入）
bool SaveConfig()
{
    try {
        std::string data = BuildConfigJson(g_cfg).dump(2);
        fs::path cfgPath = UserConfigPath();
        std::error_code ec;
        if (fs::exists(cfgPath, ec)) {
            std::string old;
            if (read_binary_file(cfgPath, old) && old == data) return true;
        }
        return write_binary_file(cfgPath, data);
    }
    catch (...) { return false; }
}

// 用内置默认值生成 config.default.json（首次运行或默认配置缺失时调用，供打包/更新兜底）
static bool SaveDefaultConfig()
{
    try {
        AppConfig def; // 全部取成员默认值
        def.aiPresets = g_defaultAIPresets;
        return write_binary_file(DefaultConfigPath(), BuildConfigJson(def).dump(2));
    }
    catch (...) { return false; }
}

// 从指定路径读取配置填充 g_cfg。
// asUserLayer=false：默认配置——整体覆盖内存，预设缺失时恢复内置默认并写回默认文件；
// asUserLayer=true ：用户配置——只覆盖文件中出现的字段，预设按「名称」与默认层合并
//                    （同名用用户的，新名字追加），保证新版默认配置的更新能生效。
// 找不到或解析失败返回 false
bool LoadConfigFrom(const fs::path& cfgPath, bool asUserLayer)
{
    try {
        if (!fs::exists(cfgPath)) return false;
        std::string data;
        if (!read_binary_file(cfgPath, data)) return false;
        json j = json::parse(data, nullptr, true, true); // 支持 // 与 /* */ 注释（JSONC），方便用户手写中文注释
        g_cfg.penumbraDir = j.value("penumbraDir", g_cfg.penumbraDir);
        g_cfg.translationDir = j.value("translationDir", g_cfg.translationDir);
        g_cfg.dictionaryDir = j.value("dictionaryDir", g_cfg.dictionaryDir);
        g_cfg.swapWordOrder = j.value("swapWordOrder", g_cfg.swapWordOrder);
        g_cfg.autoBackup = j.value("autoBackup", g_cfg.autoBackup);
        g_cfg.pureChinese = j.value("pureChinese", g_cfg.pureChinese);
        // 黑名单：默认层整体覆盖；用户层仅非空时覆盖（空数组=用户未设置过，保留默认专名）
        if (j.contains("blacklist") && j["blacklist"].is_array()) {
            auto bl = j["blacklist"].get<std::vector<std::string>>();
            if (!asUserLayer || !bl.empty()) g_cfg.blacklist = bl;
        }
        if (j.contains("wikiCategories")) g_cfg.wikiCategories = j["wikiCategories"].get<std::vector<std::string>>();
        g_cfg.aiApiKey = j.value("aiApiKey", g_cfg.aiApiKey);
        g_cfg.aiKeyName = j.value("aiKeyName", g_cfg.aiKeyName);
        g_cfg.aiKeys.clear();
        if (j.contains("aiKeys") && j["aiKeys"].is_array()) {
            for (const auto& it : j["aiKeys"]) {
                AIKeyEntry e;
                e.name = it.value("name", "");
                e.key = it.value("key", "");
                if (!e.name.empty() && !e.key.empty()) g_cfg.aiKeys.push_back(e);
            }
        }
        g_cfg.customSaves.clear();
        if (j.contains("customSaves") && j["customSaves"].is_array()) {
            for (const auto& it : j["customSaves"]) {
                AISaveEntry e;
                e.name = it.value("name", "");
                e.key = it.value("key", "");
                e.model = it.value("model", "");
                e.baseUrl = it.value("baseUrl", "");
                e.note = it.value("note", "");
                if (e.name.empty() && e.key.empty() && e.model.empty() && e.baseUrl.empty()) continue;
                g_cfg.customSaves.push_back(e);
            }
        }
        // 兼容旧版：只有单个 aiApiKey 时补成一条「默认 Key」
        if (g_cfg.aiKeys.empty() && !g_cfg.aiApiKey.empty()) {
            g_cfg.aiKeys.push_back({ "默认 Key", g_cfg.aiApiKey });
            if (g_cfg.aiKeyName.empty()) g_cfg.aiKeyName = "默认 Key";
        }
        // 同步当前选中 Key
        if (!g_cfg.aiKeys.empty()) {
            bool found = false;
            for (const auto& e : g_cfg.aiKeys) {
                if (e.name == g_cfg.aiKeyName) { g_cfg.aiApiKey = e.key; found = true; break; }
            }
            if (!found) {
                g_cfg.aiKeyName = g_cfg.aiKeys[0].name;
                g_cfg.aiApiKey = g_cfg.aiKeys[0].key;
            }
        }
        g_cfg.aiModel = j.value("aiModel", g_cfg.aiModel);
        g_cfg.aiBaseUrl = j.value("aiBaseUrl", g_cfg.aiBaseUrl);
        if (j.contains("aiPresets") && j["aiPresets"].is_array()) {
            if (!asUserLayer) g_cfg.aiPresets.clear(); // 默认层：整体替换
            for (const auto& it : j["aiPresets"]) {
                AIPreset p;
                p.name = it.value("name", "");
                p.model = it.value("model", "");
                p.baseUrl = it.value("baseUrl", "");
                p.note = it.value("note", "");
                if (p.name.empty()) continue;
                if (asUserLayer) {
                    bool replaced = false;
                    for (auto& q : g_cfg.aiPresets)
                        if (q.name == p.name) { q = p; replaced = true; break; }
                    if (!replaced) g_cfg.aiPresets.push_back(p);
                } else {
                    g_cfg.aiPresets.push_back(p);
                }
            }
        }
        bool presetRestored = false;
        if (!asUserLayer && g_cfg.aiPresets.empty()) {
            g_cfg.aiPresets = g_defaultAIPresets; // 默认层预设被清空/缺失时恢复默认，保证下拉框有可用项
            presetRestored = true;
        }
        g_cfg.aiPreset = j.value("aiPreset", g_cfg.aiPreset);
        g_cfg.aiBatchSize = j.value("aiBatchSize", g_cfg.aiBatchSize);
        g_cfg.fontSize = j.value("fontSize", g_cfg.fontSize);
        g_cfg.autoFontSize = j.value("autoFontSize", g_cfg.autoFontSize);
        g_cfg.winX = j.value("winX", g_cfg.winX);
        g_cfg.winY = j.value("winY", g_cfg.winY);
        g_cfg.winW = j.value("winW", g_cfg.winW);
        g_cfg.winH = j.value("winH", g_cfg.winH);
        if (presetRestored) SaveDefaultConfig(); // 默认配置预设缺失时写回，避免每次启动都靠内存兜底
        return true;
    }
    catch (...) { return false; }
}

// 启动时自动清理 Penumbra 父目录（递归）、翻译目录、词典目录下残留的
// *.json.bak / *.json.bak2 手动备份文件（程序自身的备份是 *.zip，不受影响）
static void CleanupJsonBakFiles()
{
    auto endsWith = [](const std::wstring& s, const std::wstring& suf) {
        return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
    };
    std::string dirs[3] = { g_cfg.penumbraDir, g_cfg.translationDir, g_cfg.dictionaryDir };
    const char* names[3] = { "Penumbra", "翻译目录", "词典目录" };
    bool rec[3] = { true, false, false };
    for (int i = 0; i < 3; ++i) {
        if (dirs[i].empty()) continue;
        fs::path root = fs::u8path(dirs[i]);
        std::error_code ec;
        if (!fs::exists(root, ec)) continue;
        int n = 0;
        auto process = [&](const fs::path& p) {
            if (!fs::is_regular_file(p, ec)) return;
            std::wstring fn = p.filename().wstring();
            if (endsWith(fn, L".json.bak") || endsWith(fn, L".json.bak2")) {
                fs::remove(p, ec);
                if (!ec) n++;
            }
        };
        if (rec[i]) {
            for (fs::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec))
                process(it->path());
        }
        else {
            for (fs::directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec))
                process(it->path());
        }
        if (n > 0)
            Log("[清理] 已自动删除 " + std::string(names[i]) + " 下 " + std::to_string(n) + " 个 .json.bak 备份文件");
    }
}

// 加载配置：先读默认配置 config.default.json（zip 自带，更新时覆盖），再叠加用户配置
// config.user.json（用户保存的选项，更新时保留）；旧版 config.json 自动迁移为用户配置。
bool LoadConfig()
{
    if (!LoadConfigFrom(DefaultConfigPath(), /*asUserLayer=*/false)) {
        g_cfg.aiPresets = g_defaultAIPresets; // 默认配置缺失：内置默认兜底
        SaveDefaultConfig();                  // 顺手生成默认配置，让更新有基准
    }
    if (LoadConfigFrom(UserConfigPath(), /*asUserLayer=*/true)) return true;
    // 旧版升级：程序目录有 config.json 时迁移为用户配置（不删除旧文件，避免误删用户数据）
    if (LoadConfigFrom(GetExeDir() / "config.json", /*asUserLayer=*/true)) {
        Log("[提示] 已从旧版 config.json 迁移用户配置（生成 config.user.json，旧文件保留可自行删除）");
        SaveConfig();
        return true;
    }
    return false;
}

// ------------------------------------------------------------------
// 目录选择对话框
// ------------------------------------------------------------------
bool SelectDirDialog(HWND parent, std::wstring& out)
{
    BROWSEINFOW bi = {};
    bi.hwndOwner = parent;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpszTitle = L"请选择一个文件夹";
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, path)) {
            out = path;
            CoTaskMemFree(pidl);
            return true;
        }
        CoTaskMemFree(pidl);
    }
    return false;
}

// 打开资源管理器定位到指定文件夹
void OpenExplorer(HWND parent, const std::wstring& path)
{
    if (path.empty()) { LogW(L"[提示] 目录为空，无法打开"); return; }
    fs::path p(path);
    std::error_code ec;
    if (!fs::exists(p, ec) || ec) {
        LogW(L"[提示] 目录不存在: " + path);
        return;
    }
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.hwnd = parent;
    sei.nShow = SW_SHOWNORMAL;
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = p.c_str();
    if (!ShellExecuteExW(&sei)) {
        LogW(L"[提示] 无法打开文件夹: " + path);
    }
}

// 用系统默认程序打开词典目录下的指定 json 文件（如 个性翻译.json / 单词黑名单.json）
void OpenDictJson(HWND parent, const char* fname)
{
    if (g_cfg.dictionaryDir.empty()) {
        Log(std::string("[提示] 未设置词典目录，无法打开 ") + fname);
        return;
    }
    fs::path p = fs::u8path(g_cfg.dictionaryDir) / fs::u8path(fname);
    std::error_code ec;
    if (!fs::exists(p, ec) || ec) {
        Log(std::string("[提示] 文件不存在（设置词典目录时程序会自动创建）: ") + fname);
        return;
    }
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.hwnd = parent;
    sei.nShow = SW_SHOWNORMAL;
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = p.wstring().c_str();
    if (!ShellExecuteExW(&sei)) {
        Log(std::string("[提示] 无法打开文件: ") + fname);
    }
    else {
        Log(std::string("[提示] 已用默认程序打开 ") + fname);
    }
}

// ------------------------------------------------------------------
// 扫描 Penumbra 目录下所有 group_*.json（递归）
// ------------------------------------------------------------------
std::vector<fs::path> ScanGroupFiles(const fs::path& root)
{
    std::vector<fs::path> result;
    std::error_code ec;
    if (!fs::exists(root, ec) || ec) return result;
    try {
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            Log("[错误] 无法遍历目录: " + wstring_to_utf8(root.wstring()) + " - " + ec.message());
            return result;
        }
        for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            auto& entry = *it;
            std::error_code sec;
            if (!entry.is_regular_file(sec) || sec) continue;
            auto& p = entry.path();
            if (is_group_json(p.filename().wstring())) {
                result.push_back(p);
            }
        }
    }
    catch (const std::system_error& e) {
        Log(std::string("[错误] 扫描 group_*.json 时系统异常: ") + e.what());
    }
    catch (const std::exception& e) {
        Log(std::string("[错误] 扫描 group_*.json 时异常: ") + e.what());
    }
    catch (...) {
        Log("[错误] 扫描 group_*.json 时未知异常");
    }
    return result;
}

// 安全计算相对路径，失败时返回原路径的通用字符串
std::string SafeRelativePath(const fs::path& path, const fs::path& base)
{
    std::error_code ec;
    fs::path rel = fs::relative(path, base, ec);
    if (ec) {
        return wstring_to_utf8(path.wstring());
    }
    return wstring_to_utf8(rel.generic_wstring());
}

// ------------------------------------------------------------------
// 用 tar.exe 打包 zip（Windows 10 1809+ 自带）
// ------------------------------------------------------------------
bool CreateZip(const fs::path& workDir, const std::wstring& pattern, const fs::path& zipOut)
{
    // tar -a -c -f <zip> -C <dir> <pattern>
    std::wstring cmd = L"tar.exe -a -c -f \"" + zipOut.wstring() + L"\" -C \"" + workDir.wstring() + L"\" \"" + pattern + L"\"";

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, workDir.wstring().c_str(), &si, &pi);
    if (!ok) return false;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}

bool ExtractZip(const fs::path& zipFile, const fs::path& destDir)
{
    if (!fs::exists(destDir)) fs::create_directories(destDir);
    std::wstring cmd = L"tar.exe -xf \"" + zipFile.wstring() + L"\" -C \"" + destDir.wstring() + L"\"";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, destDir.wstring().c_str(), &si, &pi);
    if (!ok) return false;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}

// ------------------------------------------------------------------
// 提取英文
// ------------------------------------------------------------------
bool ExtractEnglish(const std::vector<fs::path>* onlyFiles)
{
    try {
    if (g_cfg.penumbraDir.empty()) { Log("[错误] 未设置 Penumbra 目录"); return false; }
    if (g_cfg.translationDir.empty()) { Log("[错误] 未设置翻译目录"); return false; }

    fs::path penRoot = fs::u8path(g_cfg.penumbraDir);
    fs::path transDir = fs::u8path(g_cfg.translationDir);
    std::error_code existEc;
    if (!fs::exists(penRoot, existEc) || existEc) { Log("[错误] Penumbra 目录不存在或无法访问"); return false; }
    if (!fs::exists(transDir, existEc)) {
        std::error_code ec;
        fs::create_directories(transDir, ec);
        if (ec) { Log("[错误] 无法创建翻译目录"); return false; }
    }

    // 确保词典目录下存在 单词黑名单.json（v2.2.8 起目录确定时即建立，这里兜底再确认一次）
    EnsureBlacklistFile();

    // 提取英文不再跳过黑名单词（黑名单仅在 AI 翻译/词典写入Mod 时保留原文），
    // 只跳过纯装饰分隔符 "---"、"-"，避免把非选项文本当词条提取。
    std::set<std::string> blacklist = { "---", "-" };

    // 扫描所有模组文件夹
    struct ModEntry { fs::path folder; std::vector<fs::path> files; bool hasPending = false; };
    std::map<std::wstring, ModEntry> modMap; // key: 模组文件夹名

    // v2.3.4：支持只提取对话框选中的文件集合；空则全量扫描
    std::vector<fs::path> allFiles;
    if (onlyFiles && !onlyFiles->empty()) {
        allFiles = *onlyFiles;
    } else {
        allFiles = ScanGroupFiles(penRoot);
    }
    for (auto& p : allFiles) {
        auto parent = p.parent_path();
        std::wstring key = parent.filename().wstring();
        modMap[key].folder = parent;
        modMap[key].files.push_back(p);
    }

    if (modMap.empty()) { Log("[提示] 未找到任何 group_*.json 文件"); return false; }

    Log("扫描到 " + std::to_string(modMap.size()) + " 个模组文件夹");

    json out;
    out["翻译规则"] = {
        {"1. 翻译范围", "无论是 _descriptions（描述）还是 _options（选项），都必须翻译，一视同仁。"},
        {"2. 格式要求", "短名称（选项/组名）通常为\"中文（英文）\"格式，如\"治疗（Cure）\"，括号一律用全角（）且括号内必须原样保留英文原文，禁止翻译或改写括号内的英文；确实无法翻译的专有名词/缩写/品牌名则原样保留英文，不得输出\"英文（英文）\"重复格式（见第 4 条）；长描述（Description）直接翻译成通顺的中文句子，不套括号，也不得保留英文原文。"},
        {"3. 禁止半翻译", "每条文本必须整体完整翻译，绝对禁止输出\"中文 + 残留英文\"的混合半成品（例如\"隐遁 short boots\"、\"Medium 钻石 Patch\"均为错误）。遇到无法确认的词汇，按上下文推断其通用含义并翻译，不许跳过或保留英文。"},
        {"4. 括号与专名", "能译出中文的短名称用「中文（英文）」格式，如 无（None）；无法翻译的专有名词/缩写/品牌名（如 EXQB、Uranus）原样保留英文，禁止输出「英文（英文）」重复格式。"},
        {"5. 专有名词", "人名、品牌名、作者名等专有名词（如 Yiggle、Lavabod、YAB）可原样保留在译文中，作为专名的一部分，不强求翻译。"},
        {"6. 翻译指令", "对于任何英文短语，即使带有连字符（如 Connectors - Face），也应将其视为一个整体进行翻译，而不是保留原文。带「└─」「├─」等层级装饰符的名字忽略装饰符整体翻译，括号内只放最简英文，禁止嵌套重复。绝对禁止输出\"英文 / 中文\"或\"英文换行中文\"这类双语拼接。"},
        {"7. 同组一致性", "同一文件内的选项通常属于同一维度（体型/尺寸/颜色/材质等），译文应统一语义、用词一致；无法确定通用含义的 mod 自造词/作者专名（如 Masc、Lava、Rue、Lavabod、Yanilla、EXQB、Uranus 这类体型名）优先原样保留英文，不要硬译或编造。"},
        {"8. 3D 建模术语", "poly 是 polygon（多边形）的缩写，higher poly 指模型面数更高，译为「高多边形」；texture=纹理、mesh=网格、rig=骨骼绑定。例如 Fuzzy layer (higher poly) 译为 毛绒层（高多边形），不得按字面硬译或编造。"},
        {"9. 无歧义固定词", "身体部位等没有歧义（多义词）的固定名词必须按通用中文直译，不得保留英文：Feet=脚部、Legs=腿部、Hands=手部、Chest=胸部、Belly=腹部、Thighs=大腿、Back=背部、Arms=手臂、Shoulders=肩部。例如 Feet 只译作「脚部」，不要译成其它生僻说法。"},
        {"10. 文件命名", "翻译完成后，将文件名中的\"_未翻译\"改为\"_已翻译\"。"},
        {"11. 交付方式", "每次修改后，请直接提供完整的 JSON 文件内容。"},
        {"说明", "将英文翻译为中文。请保持 JSON 结构，仅填写 _options 和 _descriptions 的翻译。"},
        {"格式提示", "短名称按 中文（英文） 格式填写，例如 发型1（Hairstyle 1）；长描述直接填中文句子，不要保留英文，也不要输出\"英文 / 中文\"拼接。"}
    };
    out["_options"] = json::object();
    out["_descriptions"] = json::object();

    int optCount = 0, descCount = 0;
    int fileTotal = (int)allFiles.size();
    int fileIdx = 0;

    for (auto& kv : modMap) {
        for (auto& file : kv.second.files) {
            fileIdx++;
            if (fileIdx % 5 == 0 || fileIdx == fileTotal) SetProgress(fileIdx, fileTotal);
            std::string data;
            if (!read_binary_file(file, data)) continue;
            json j;
            try {
                j = json::parse(clean_utf8(data));
            }
            catch (...) {
                Log("[警告] 解析失败: " + wstring_to_utf8(file.filename().wstring()));
                continue;
            }

            std::string relKey = SafeRelativePath(file, penRoot);
            if (!j.is_object()) continue;

            // Name / Description 可能是字符串或对象
            std::function<void(const std::string&, const std::string&, const std::string&)> collect =
                [&](const std::string& baseKey, const std::string& name, const std::string& desc)
            {
                // 处理 Name
                std::string nameStr = name;
                if (!nameStr.empty()) {
                    // 标准化：如 "Hairstyle 1 / 发型1" 转为 "发型1（Hairstyle 1）"
                    std::string stdName = nameStr;
                    auto slash = nameStr.find(" / ");
                    if (slash != std::string::npos) {
                        std::string eng = nameStr.substr(0, slash);
                        std::string zh = nameStr.substr(slash + 3);
                        std::string trimEng = eng, trimZh = zh;
                        // 去首尾空格
                        size_t s = trimEng.find_first_not_of(" \t"); if (s != std::string::npos) trimEng = trimEng.substr(s);
                        s = trimZh.find_first_not_of(" \t"); if (s != std::string::npos) trimZh = trimZh.substr(s);
                        if (contains_chinese(zh) && !contains_chinese(eng)) {
                            stdName = trimZh + "（" + trimEng + "）";
                        }
                    }
                    if (!contains_chinese(stdName)) {
                        std::string key = baseKey + "||Name||" + stdName;
                        out["_options"][key] = "";
                        optCount++;
                    }
                }
                // 处理 Description
                std::string descStr = desc;
                if (!descStr.empty()) {
                    std::string stdDesc = descStr;
                    auto slash = descStr.find(" / ");
                    if (slash != std::string::npos) {
                        std::string eng = descStr.substr(0, slash);
                        std::string zh = descStr.substr(slash + 3);
                        if (contains_chinese(zh) && !contains_chinese(eng)) {
                            stdDesc = zh + "（" + eng + "）";
                        }
                    }
                    if (!contains_chinese(stdDesc)) {
                        std::string key = baseKey + "||Desc||" + stdDesc;
                        out["_descriptions"][key] = "";
                        descCount++;
                    }
                }
            };

            if (j.contains("Name")) {
                if (j["Name"].is_string()) {
                    collect(relKey, j["Name"].get<std::string>(), "");
                }
                else if (j["Name"].is_object()) {
                    for (auto& it : j["Name"].items()) {
                        if (it.value().is_string())
                            collect(relKey + "||" + it.key(), it.value().get<std::string>(), "");
                    }
                }
                else if (j["Name"].is_array()) {
                    int idx = 0;
                    for (auto& it : j["Name"].items()) {
                        if (it.value().is_string())
                            collect(relKey + "||[" + std::to_string(idx) + "]", it.value().get<std::string>(), "");
                        idx++;
                    }
                }
                else {
                    Log("[提示] 跳过非字符串/对象/数组 Name: " + wstring_to_utf8(file.filename().wstring()));
                }
            }
            if (j.contains("Description")) {
                if (j["Description"].is_string()) {
                    collect(relKey, "", j["Description"].get<std::string>());
                }
                else if (j["Description"].is_object()) {
                    for (auto& it : j["Description"].items()) {
                        if (it.value().is_string())
                            collect(relKey + "||" + it.key(), "", it.value().get<std::string>());
                    }
                }
                else if (j["Description"].is_array()) {
                    int idx = 0;
                    for (auto& it : j["Description"].items()) {
                        if (it.value().is_string())
                            collect(relKey + "||[" + std::to_string(idx) + "]", "", it.value().get<std::string>());
                        idx++;
                    }
                }
                else {
                    Log("[提示] 跳过非字符串/对象/数组 Description: " + wstring_to_utf8(file.filename().wstring()));
                }
            }

            // 处理 Options 数组里的选项值（玩家在游戏里实际看到的选项，如 Small/Medium/Large）
            if (j.contains("Options") && j["Options"].is_array()) {
                std::set<std::string> seenOpt; // 同一 group 内按值去重
                for (auto& opt : j["Options"].items()) {
                    if (!opt.value().is_object()) continue;
                    if (!opt.value().contains("Name")) continue;
                    const json& nv = opt.value()["Name"];
                    if (!nv.is_string()) continue;
                    std::string oname = nv.get<std::string>();
                    if (oname.empty()) continue;
                    // 标准化 "英文 / 中文" → "中文（英文）"
                    std::string stdOpt = oname;
                    auto slash = oname.find(" / ");
                    if (slash != std::string::npos) {
                        std::string eng = oname.substr(0, slash);
                        std::string zh = oname.substr(slash + 3);
                        std::string trimEng = eng, trimZh = zh;
                        size_t ss = trimEng.find_first_not_of(" \t");
                        if (ss != std::string::npos) trimEng = trimEng.substr(ss);
                        ss = trimZh.find_first_not_of(" \t");
                        if (ss != std::string::npos) trimZh = trimZh.substr(ss);
                        if (contains_chinese(zh) && !contains_chinese(eng))
                            stdOpt = trimZh + "（" + trimEng + "）";
                    }
                    if (contains_chinese(stdOpt)) continue;   // 已含中文则跳过
                    if (!seenOpt.insert(stdOpt).second) continue; // 同 group 内去重
                    out["_options"][relKey + "||Opt||" + stdOpt] = "";
                    optCount++;
                }
            }
        }
    }

    // 分隔符过滤（剔除纯装饰符词条，如 ---、-）
    std::vector<std::string> toRemove;
    for (auto& it : out["_options"].items()) {
        // key 形如 路径||Name||原文 或 路径||Opt||原文，取最后一个 || 之后的原文
        auto kp = it.key().rfind("||");
        std::string value = (kp == std::string::npos) ? it.key() : it.key().substr(kp + 2);
        std::string lower = value;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        for (auto& b : blacklist) {
            std::string bl = b;
            std::transform(bl.begin(), bl.end(), bl.begin(), ::tolower);
            if (bl.empty()) continue;
            if (lower.find(bl) != std::string::npos) { toRemove.push_back(it.key()); break; }
        }
    }
    for (auto& k : toRemove) out["_options"].erase(k);

    if (optCount == 0 && descCount == 0) {
        Log("[提示] 没有需要翻译的文本");
        return false;
    }

    // 写入 时间戳_未翻译.json
    std::string ts = now_timestamp_human();
    fs::path outPath = transDir / fs::u8path(ts + "_未翻译.json");
    std::string data = out.dump(2);
    write_binary_file(outPath, data);

    Log("提取完成：共 " + std::to_string(optCount) + " 个选项名，"
        + std::to_string(descCount) + " 个描述，写入 " + wstring_to_utf8(outPath.filename().wstring()));

    // 自动备份：为每个模组文件夹生成一个 zip，保存在各自 MOD 文件夹内；已有备份则跳过
    if (g_cfg.autoBackup) {
        int backupCount = 0, skip = 0;
        for (auto& kv : modMap) {
            auto& entry = kv.second;
            if (entry.files.empty()) continue;
            std::error_code bec;
            bool hasBackup = false;
            for (auto& ent : fs::directory_iterator(entry.folder, bec)) {
                if (bec) break;
                if (!ent.is_regular_file(bec)) continue;
                std::wstring fn = ent.path().filename().wstring();
                if (fn.find(L"备份.zip") != std::wstring::npos) { hasBackup = true; break; }
            }
            if (hasBackup) { skip++; continue; }
            std::string zipName = now_timestamp() + "备份.zip";
            fs::path zipOut = entry.folder / fs::u8path(zipName);
            if (CreateZip(entry.folder, L"group_*.json", zipOut)) {
                backupCount++;
            }
        }
        if (skip > 0)
            Log("[备份] 已有备份跳过 " + std::to_string(skip) + " 个，新生成 " + std::to_string(backupCount) + " 个");
        else
            Log("[备份] 已生成 " + std::to_string(backupCount) + " 个备份包");
    }
    return true;
    }
    catch (const std::system_error& e) { Log(std::string("[错误] 提取英文时系统异常: ") + e.what()); return false; }
    catch (const std::exception& e) { Log(std::string("[错误] 提取英文时异常: ") + e.what()); return false; }
    catch (...) { Log("[错误] 提取英文时未知异常"); return false; }
}


// ------------------------------------------------------------------
// 检查翻译：扫描 MOD 的 group_*.json，报告仍未翻译的英文条目
// ------------------------------------------------------------------
bool CheckTranslation()
{
    try {
    if (g_cfg.penumbraDir.empty()) { Log("[错误] 未设置 Penumbra 目录"); return false; }

    fs::path penRoot = fs::u8path(g_cfg.penumbraDir);
    std::error_code existEc;
    if (!fs::exists(penRoot, existEc) || existEc) { Log("[错误] Penumbra 目录不存在或无法访问"); return false; }

    std::vector<fs::path> allFiles = ScanGroupFiles(penRoot);
    if (allFiles.empty()) { Log("[提示] 未找到任何 group_*.json 文件"); return false; }

    struct FileReport {
        std::string rel;       // 相对路径
        std::wstring folder;   // 所属 MOD 文件夹名
        int total = 0, translated = 0, pending = 0;
        std::vector<std::pair<std::string, std::string>> pendingItems; // 类型, 原文（最多记 10 条）
    };

    std::map<std::wstring, std::vector<FileReport>> byMod;
    int fileIdx = 0, fileTotal = (int)allFiles.size();
    int totalAll = 0, totalDone = 0, totalPending = 0;
    int pendingFiles = 0, okFiles = 0;

    auto examine = [&](FileReport& r, const std::string& type, const std::string& text) {
        if (text.empty()) return;
        r.total++; totalAll++;
        if (contains_chinese(text)) { r.translated++; totalDone++; }
        else if (contains_english_letter(text)) {
            r.pending++; totalPending++;
            if ((int)r.pendingItems.size() < 10)
                r.pendingItems.emplace_back(type, text);
        }
        // 纯数字 / 版本号 / 纯符号：不算未翻译
    };

    for (auto& file : allFiles) {
        fileIdx++;
        if (fileIdx % 5 == 0 || fileIdx == fileTotal) SetProgress(fileIdx, fileTotal);
        std::string data;
        if (!read_binary_file(file, data)) continue;
        json j;
        try { j = json::parse(clean_utf8(data)); }
        catch (...) {
            Log("[警告] 解析失败: " + wstring_to_utf8(file.filename().wstring()));
            continue;
        }
        if (!j.is_object()) continue;

        FileReport fr;
        fr.rel = SafeRelativePath(file, penRoot);
        fr.folder = file.parent_path().filename().wstring();

        // Name / Description：字符串 / 对象 / 数组 三种形态都检查
        auto collectField = [&](const char* field, const char* type) {
            if (!j.contains(field)) return;
            const json& v = j[field];
            if (v.is_string()) examine(fr, type, v.get<std::string>());
            else if (v.is_object()) {
                for (auto& it : v.items())
                    if (it.value().is_string()) examine(fr, type, it.value().get<std::string>());
            }
            else if (v.is_array()) {
                for (auto& it : v.items())
                    if (it.value().is_string()) examine(fr, type, it.value().get<std::string>());
            }
        };
        collectField("Name", "组名");
        collectField("Description", "描述");

        // Options 里的选项值
        if (j.contains("Options") && j["Options"].is_array()) {
            for (auto& opt : j["Options"].items()) {
                if (!opt.value().is_object()) continue;
                if (!opt.value().contains("Name")) continue;
                const json& nv = opt.value()["Name"];
                if (!nv.is_string()) continue;
                std::string oname = nv.get<std::string>();
                if (oname.empty()) continue;
                examine(fr, "选项", oname);
            }
        }

        if (fr.pending > 0) pendingFiles++; else okFiles++;
        byMod[fr.folder].push_back(std::move(fr));
    }

    Log("===== 检查翻译结果 =====");
    Log("扫描 " + std::to_string((int)allFiles.size()) + " 个 group_*.json 文件，共 " + std::to_string(totalAll) + " 条组名/描述/选项");
    Log("已翻译（含中文）: " + std::to_string(totalDone) + " 条；未翻译残留: " + std::to_string(totalPending) + " 条");
    Log("完全翻译的文件: " + std::to_string(okFiles) + " 个；有残留的文件: " + std::to_string(pendingFiles) + " 个");

    if (totalPending == 0) {
        Log("[提示] 所有条目均已翻译，无需处理");
        return true;
    }

    // 写详细报告到翻译目录
    json report;
    report["检查时间"] = now_timestamp_human();
    report["扫描文件数"] = (int)allFiles.size();
    report["已翻译条目"] = totalDone;
    report["未翻译条目"] = totalPending;
    json mods = json::array();
    for (auto& kv : byMod) {
        int mTotal = 0, mPending = 0;
        json files = json::array();
        for (auto& fr : kv.second) {
            if (fr.pending == 0) continue;
            mTotal += fr.total; mPending += fr.pending;
            json fj;
            fj["文件"] = fr.rel;
            fj["未翻译"] = fr.pending;
            fj["共"] = fr.total;
            json items = json::array();
            for (auto& pi : fr.pendingItems) {
                json it; it["类型"] = pi.first; it["原文"] = pi.second;
                items.push_back(it);
            }
            fj["条目"] = items;
            files.push_back(fj);
        }
        if (mPending == 0) continue;
        json mj;
        mj["未翻译"] = mPending; mj["共"] = mTotal;
        mj["文件"] = files;
        mods.push_back(mj);
    }
    report["残留MOD"] = mods;
    if (!g_cfg.translationDir.empty()) {
        fs::path tdir = fs::u8path(g_cfg.translationDir);
        std::error_code tdec;
        if (!fs::exists(tdir, tdec) || tdec) fs::create_directories(tdir, tdec);
        if (!tdec) {
            fs::path rp = tdir / fs::u8path(now_timestamp() + "_翻译检查报告.json");
            write_binary_file(rp, report.dump(2));
            Log("[提示] 详细残留清单已写入: " + wstring_to_utf8(rp.filename().wstring()));
        }
    }

    // 日志按 MOD 汇总（只列有残留的）
    for (auto& kv : byMod) {
        int mPending = 0, mTotal = 0;
        for (auto& fr : kv.second) { mPending += fr.pending; mTotal += fr.total; }
        if (mPending == 0) continue;
        Log("[残留] " + wstring_to_utf8(kv.first) + "：未翻译 " + std::to_string(mPending) + "/" + std::to_string(mTotal) + " 条");
    }
    return true;
    }
    catch (const std::system_error& e) { Log(std::string("[错误] 检查翻译时系统异常: ") + e.what()); return false; }
    catch (const std::exception& e) { Log(std::string("[错误] 检查翻译时异常: ") + e.what()); return false; }
    catch (...) { Log("[错误] 检查翻译时未知异常"); return false; }
}

// ------------------------------------------------------------------
// 应用翻译
// ------------------------------------------------------------------
static void EnsureDictFile(); // 前向声明：唯一词典确保函数（定义见「词典工具与导入翻译」节）
static fs::path MergedDictPath(const fs::path& dictDir); // 前向声明：汇总词典路径（含旧文件名迁移）
static fs::path CustomDictPath(const fs::path& dictDir); // 前向声明：个性翻译.json 路径
bool ApplyTranslation()
{
    try {
    if (g_cfg.penumbraDir.empty()) { Log("[错误] 未设置 Penumbra 目录"); return false; }
    if (g_cfg.dictionaryDir.empty()) { Log("[错误] 未设置词典目录"); return false; }

    fs::path penRoot = fs::u8path(g_cfg.penumbraDir);
    fs::path dictDir = fs::u8path(g_cfg.dictionaryDir);
    EnsureDictFile(); // 首次生成汇总词典 汇总词典.json + 个性翻译.json
    fs::path dictPath = MergedDictPath(dictDir);
    std::error_code existEc;
    json dict;
    if (!fs::exists(dictPath, existEc) || existEc) {
        // 唯一词典不存在：从空对象开始，后面自动合并 *_已翻译.json 建立
        dict = json::object();
        Log("[提示] 汇总词典.json 不存在，将根据 *_已翻译.json 自动建立");
    } else {
        std::string d;
        if (!read_binary_file(dictPath, d)) { Log("[错误] 无法读取词典"); return false; }
        try { dict = json::parse(clean_utf8(d)); }
        catch (...) {
            // 汇总词典损坏：备份后按「wiki 原典优先 + 汉化总词典补充」重建，避免整个流程卡死
            Log("[错误] 汇总词典.json 解析失败（可能损坏），已备份并从 wiki 原典重建");
            std::error_code bec;
            fs::path bak = dictPath;
            bak += L".损坏备份";
            fs::rename(dictPath, bak, bec);
            EnsureDictFile();
            dict = json::object();
        }
    }
    if (!dict.is_object()) dict = json::object();
    if (!dict.contains("_options")) dict["_options"] = json::object();
    if (!dict.contains("_descriptions")) dict["_descriptions"] = json::object();
    if (!dict.contains("terms")) dict["terms"] = json::object();

    // 自动扫描翻译目录下所有 *_已翻译.json：
    // 1) 把没进唯一词典的条目编入 汇总词典.json（不覆盖已有非空翻译），并写回文件
    // 2) 同时载入内存作为备选翻译源（唯一词典优先，查不到再查 *_已翻译.json）
    int autoMerged = 0;
    json backupOpt = json::object();
    json backupDesc = json::object();
    if (!g_cfg.translationDir.empty()) {
        fs::path tdir = fs::u8path(g_cfg.translationDir);
        std::error_code taec;
        if (fs::exists(tdir, taec) && !taec) {
            std::vector<fs::path> tranFiles;
            std::error_code itec;
            for (fs::directory_iterator it(tdir, itec), end; it != end && !itec; it.increment(itec)) {
                std::wstring fn = it->path().filename().wstring();
                if (it->is_regular_file(itec) && fn.find(L"_已翻译") != std::wstring::npos
                    && it->path().extension().wstring() == L".json")
                    tranFiles.push_back(it->path());
            }
            for (auto& tf : tranFiles) {
                std::string td;
                if (!read_binary_file(tf, td)) continue;
                try {
                    json tj = json::parse(clean_utf8(td));
                    if (!tj.is_object()) continue;
                    for (auto& sec : { "_options", "_descriptions" }) {
                        if (!tj.contains(sec) || !tj[sec].is_object()) continue;
                        for (auto& it : tj[sec].items()) {
                            if (!it.value().is_string()) continue;
                            std::string val = it.value().get<std::string>();
                            if (val.empty()) continue;
                            json& dst = (sec == "_options") ? backupOpt : backupDesc;
                            if (!dst.contains(it.key())) dst[it.key()] = val;   // 备选源（不覆盖已有）
                            std::string existing;
                            if (dict[sec].contains(it.key()) && dict[sec][it.key()].is_string())
                                existing = dict[sec][it.key()].get<std::string>();
                            if (existing.empty()) {
                                dict[sec][it.key()] = val; // 编入唯一词典（完整 key）
                                // 同步并入 terms（先来后到，不覆盖 wiki 固定词）
                                auto kp = it.key().rfind("||");
                                if (kp != std::string::npos) {
                                    std::string en = it.key().substr(kp + 2);
                                    std::string zh = val;
                                    auto op = zh.find("（");
                                    if (op != std::string::npos) zh = zh.substr(0, op);
                                    if (!en.empty() && !contains_chinese(en)
                                        && !zh.empty() && zh != en
                                        && !dict["terms"].contains(en))
                                        dict["terms"][en] = zh;
                                }
                                autoMerged++;
                            }
                        }
                    }
                } catch (...) {}
            }
        }
    }
    if (autoMerged > 0) {
        write_binary_file(dictPath, dict.dump(2));
        Log("[提示] 已自动将 *_已翻译.json 的 " + std::to_string(autoMerged) + " 条编入唯一词典");
    }
    size_t backupCount = backupOpt.size() + backupDesc.size();
    if (backupCount > 0)
        Log("[提示] 已加载 *_已翻译.json 作为备选翻译源（" + std::to_string(backupCount) + " 条，唯一词典优先）");

    // 单词黑名单（翻译目录下 单词黑名单.json + 配置）：
    // 命中黑名单的英文原文在应用翻译时跳过；若之前已应用过翻译，则按
    // "当前翻译值 → 英文原文" 还原。唯一词典保持完整，不做清理。
    std::vector<std::string> blacklist = LoadBlacklistFile(g_cfg.blacklist);

    // 个性翻译.json：用户手工修正的词汇/短语翻译（英文:中文）。
    // 优先级：黑名单 > 个性翻译 > 汇总词典。命中黑名单的词条不覆盖，
    // 保持汇总词典原样（应用时仍按黑名单逻辑还原为英文）。覆盖后不再写回文件，
    // 汇总词典保持原样，只有写入 Mod 的文本以个性翻译为准。
    int customApplied = 0, customSkipped = 0;
    fs::path customPath = CustomDictPath(dictDir);
    std::error_code cec;
    if (fs::exists(customPath, cec) && !cec) {
        std::string cd;
        if (read_binary_file(customPath, cd)) {
            try {
                json custom = json::parse(clean_utf8(cd));
                if (custom.is_object() && !custom.empty()) {
                    for (auto& it : custom.items()) {
                        if (!it.value().is_string()) continue;
                        std::string key = it.key();
                        std::string val = it.value().get<std::string>();
                        if (key.empty() || val.empty()) continue;
                        std::string en = key;
                        auto kp = key.rfind("||");
                        if (kp != std::string::npos) en = key.substr(kp + 2);
                        if (is_blacklisted(en, blacklist)) { customSkipped++; continue; } // 黑名单 > 个性翻译
                        if (kp == std::string::npos) {
                            // 简单英文→中文：覆盖 terms，并覆盖 _options/_descriptions 中原文匹配的条目
                            dict["terms"][key] = val;
                            bool hitFull = false;
                            for (auto& sec : { "_options", "_descriptions" }) {
                                if (!dict.contains(sec) || !dict[sec].is_object()) continue;
                                for (auto& e : dict[sec].items()) {
                                    if (!e.value().is_string()) continue;
                                    auto p = e.key().rfind("||");
                                    if (p == std::string::npos) continue;
                                    if (e.key().substr(p + 2) == key) { e.value() = val; hitFull = true; }
                                }
                            }
                            if (hitFull) customApplied++;
                        } else {
                            // 完整 key（路径||Name||原文）：直接覆盖对应 section 的已有条目
                            bool done = false;
                            for (auto& sec : { "_options", "_descriptions" }) {
                                if (dict.contains(sec) && dict[sec].is_object() && dict[sec].contains(key)) {
                                    dict[sec][key] = val;
                                    done = true;
                                }
                            }
                            if (done) customApplied++;
                        }
                    }
                    if (customApplied > 0)
                        Log("[个性翻译] 已用 个性翻译.json 覆盖汇总词典 " + std::to_string(customApplied) + " 条");
                    if (customSkipped > 0)
                        Log("[个性翻译] " + std::to_string(customSkipped) + " 条命中黑名单未生效（黑名单 > 个性翻译）");
                }
            } catch (...) { Log("[错误] 个性翻译.json 解析失败，请检查 JSON 格式"); }
        }
    }

    std::map<std::string, std::string> rollbackMap; // key = relKey + '\0' + field + '\0' + translatedValue
    if (!blacklist.empty()) {
        auto collectSection = [&](const std::string& sec, const std::string& defaultField) {
            if (!dict.contains(sec) || !dict[sec].is_object()) return;
            for (auto& it : dict[sec].items()) {
                auto p1 = it.key().rfind("||");
                if (p1 == std::string::npos) continue;
                std::string orig = it.key().substr(p1 + 2);
                if (!is_blacklisted(orig, blacklist)) continue;
                // 解析 relKey 与 field（示例 key：hs-Rue+/group_003_fur materials.json||Desc||Required for Hrothgar models.）
                std::string relKey, field;
                if (p1 >= 2) {
                    auto p2 = it.key().rfind("||", p1 - 2);
                    if (p2 != std::string::npos) {
                        relKey = it.key().substr(0, p2);
                        field = it.key().substr(p2 + 2, p1 - p2 - 2);
                    } else {
                        relKey = it.key().substr(0, p1);
                        field = defaultField;
                    }
                } else {
                    field = defaultField;
                }
                if (it.value().is_string()) {
                    std::string rbKey = relKey + std::string(1, '\0') + field + std::string(1, '\0') + it.value().get<std::string>();
                    rollbackMap[rbKey] = orig;
                }
            }
        };
        collectSection("_options", "Name");
        collectSection("_descriptions", "Desc");
        Log("[黑名单] 加载 " + std::to_string(blacklist.size()) + " 个词，其中 "
            + std::to_string(rollbackMap.size()) + " 条词典词条命中（应用时跳过翻译并还原）");
    }

    auto options = dict.value("_options", json::object());
    auto descriptions = dict.value("_descriptions", json::object());

    // 加载唯一词典 terms，用于回滚那些只被 Wiki/词典自动补全写进文件、
    // 但还没进 _options/_descriptions 的翻译（例如 "Hrothgar" -> "硌狮族"）
    std::vector<std::pair<std::string, std::string>> wikiZhToEn; // (中文, 英文)，仅黑名单命中的术语
    if (!blacklist.empty()) {
        fs::path wikiPath = MergedDictPath(dictDir);
        std::error_code wec;
        if (fs::exists(wikiPath, wec) && !wec) {
            std::string wd;
            if (read_binary_file(wikiPath, wd)) {
                try {
                    json wj = json::parse(clean_utf8(wd));
                    if (wj.is_object() && wj.contains("terms") && wj["terms"].is_object()) {
                        for (auto& it : wj["terms"].items()) {
                            if (!it.value().is_string()) continue;
                            std::string en = it.key();
                            std::string zh = it.value().get<std::string>();
                            if (en.empty() || zh.empty()) continue;
                            if (is_blacklisted(en, blacklist)) wikiZhToEn.emplace_back(zh, en);
                        }
                    }
                } catch (...) {}
            }
        }
        std::sort(wikiZhToEn.begin(), wikiZhToEn.end(),
            [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    }

    // 加载格式转换词典（可选，用于把"英文"或"英文 / 中文"转换为标准"中文（英文）"格式）
    json formatDict = json::object();
    fs::path formatDictPath = fs::u8path(g_cfg.dictionaryDir) / fs::u8path("格式转换词典.json");
    {
        std::error_code fde;
        if (fs::exists(formatDictPath, fde) && !fde) {
            std::string fd;
            if (read_binary_file(formatDictPath, fd)) {
                try { formatDict = json::parse(clean_utf8(fd)); }
                catch (...) { formatDict = json::object(); }
            }
        }
    }
    // 构建原始串 -> 标准格式 的快速查找表
    std::map<std::string, std::string> formatMap;
    if (formatDict.is_object()) {
        for (auto& it : formatDict.items()) {
            if (it.value().is_string())
                formatMap[it.key()] = it.value().get<std::string>();
        }
        if (!formatMap.empty())
            Log("[提示] 已加载格式转换词典 " + std::to_string(formatMap.size()) + " 条");
    }

    // 应用前备份
    std::vector<fs::path> allFiles = ScanGroupFiles(penRoot);
    if (allFiles.empty()) { Log("[提示] 未找到 group_*.json"); return false; }

    if (g_cfg.autoBackup) {
        // 每个模组文件夹各存自己的备份包；已有备份则跳过，只保留最初那份
        std::map<std::wstring, fs::path> modDirs;
        for (auto& p : allFiles) modDirs[p.parent_path().filename().wstring()] = p.parent_path();
        int bc = 0, skip = 0;
        for (auto& kv : modDirs) {
            std::error_code bec;
            bool hasBackup = false;
            for (auto& ent : fs::directory_iterator(kv.second, bec)) {
                if (bec) break;
                if (!ent.is_regular_file(bec)) continue;
                std::wstring fn = ent.path().filename().wstring();
                if (fn.find(L"备份.zip") != std::wstring::npos) { hasBackup = true; break; }
            }
            if (hasBackup) { skip++; continue; }
            std::string zipName = now_timestamp() + "备份.zip";
            if (CreateZip(kv.second, L"group_*.json", kv.second / fs::u8path(zipName))) bc++;
        }
        if (skip > 0)
            Log("[备份] 已有备份跳过 " + std::to_string(skip) + " 个，新生成 " + std::to_string(bc) + " 个");
        else
            Log("[备份] 已生成 " + std::to_string(bc) + " 个备份包");
    }

    int appliedOpt = 0, appliedDesc = 0, rolledBack = 0;
    int applyTotal = (int)allFiles.size();
    int applyIdx = 0;
    for (auto& file : allFiles) {
        applyIdx++;
        if (applyIdx % 5 == 0 || applyIdx == applyTotal) SetProgress(applyIdx, applyTotal);
        std::string data;
        if (!read_binary_file(file, data)) continue;
        json j;
        try { j = json::parse(clean_utf8(data)); }
        catch (...) { continue; }
        if (!j.is_object()) continue;

        std::string relKey = SafeRelativePath(file, penRoot);
        bool changed = false;

        // 辅助：从 "中文（英文）" 或 "英文（中文）" 中解析出中文/英文部分
        auto parseParenPair = [&](const std::string& s, std::string& zh, std::string& en) -> bool {
            auto open = s.find("（");
            auto close = s.rfind("）");
            if (open == std::string::npos || close == std::string::npos || close <= open + 3) return false;
            std::string a = s.substr(0, open);
            std::string b = s.substr(open + 3, close - open - 3);
            bool aZh = contains_chinese(a);
            bool bZh = contains_chinese(b);
            if (aZh && !bZh) { zh = a; en = b; return true; }       // 中文（英文）
            if (!aZh && bZh) { en = a; zh = b; return true; }       // 英文（中文）
            return false;
        };

        // 辅助：去除首尾空白
        auto trimStr = [&](const std::string& s) -> std::string {
            size_t a = s.find_first_not_of(" \t");
            if (a == std::string::npos) return std::string();
            size_t b = s.find_last_not_of(" \t");
            return s.substr(a, b - a + 1);
        };

        // 辅助：解析 "英文/中文" 或 "中文/英文" 斜杠双语格式
        auto parseSlashPair = [&](const std::string& s, std::string& zh, std::string& en) -> bool {
            auto slash = s.find('/');
            if (slash == std::string::npos || slash == 0 || slash + 1 >= s.size()) return false;
            std::string a = trimStr(s.substr(0, slash));
            std::string b = trimStr(s.substr(slash + 1));
            if (a.empty() || b.empty()) return false;
            bool aZh = contains_chinese(a);
            bool bZh = contains_chinese(b);
            if (aZh && !bZh) { zh = a; en = b; return true; }       // 中文/英文
            if (!aZh && bZh) { en = a; zh = b; return true; }       // 英文/中文
            return false;
        };

        // 辅助：从双语串中提取 zh/en 之前的前缀装饰（如 ■ ├ └ ─），用于斜杠格式时保留
        auto extractPrefix = [&](const std::string& original, const std::string& zh, const std::string& en) -> std::string {
            auto p1 = original.find(en);
            auto p2 = original.find(zh);
            size_t start = std::string::npos;
            if (p1 != std::string::npos && p2 != std::string::npos) start = (p1 < p2) ? p1 : p2;
            else if (p1 != std::string::npos) start = p1;
            else if (p2 != std::string::npos) start = p2;
            if (start == std::string::npos) return std::string();
            return original.substr(0, start);
        };

        // 辅助：按当前设置组合 中文/英文（纯中文 / 中文（英文） / 英文（中文））
        auto combineZhEn = [&](const std::string& prefix, const std::string& zh, const std::string& en) -> std::string {
            std::string z = trimStr(zh);
            std::string e = trimStr(en);
            if (g_cfg.pureChinese) return prefix + z;
            if (g_cfg.swapWordOrder) return prefix + z + "（" + e + "）";
            return prefix + e + "（" + z + "）";
        };

        // 辅助：拼接翻译
        auto makeTranslation = [&](const std::string& original, const std::string& trans) -> std::string {
            if (trans.empty()) return original; // 没查到保留原文
            if (g_cfg.pureChinese) {
                // 纯中文：优先取括号外的中文；括号外是英文则取括号内中文
                std::string zh, dummy;
                if (parseParenPair(trans, zh, dummy)) {
                    zh = trimStr(zh);
                    if (!zh.empty()) return zh;
                }
                return trans;
            }
            else {
                std::string zh, en;
                if (parseParenPair(trans, zh, en)) {
                    // 已有括号格式：按当前设置重新组合，词序调换即时生效
                    return combineZhEn("", zh, en);
                }
                else if (contains_chinese(trans)) {
                    // trans 是纯中文：拼括号
                    if (contains_english_letter(trans)) {
                        // 中英混合（旧数据半翻译 / 含英文专名）：不再拼括号，
                        // 避免生成 "XXX(残留)（原文）" 垃圾格式
                        return trimStr(trans);
                    }
                    if (g_cfg.swapWordOrder) return trimStr(trans) + "（" + original + "）";
                    else return original + "（" + trimStr(trans) + "）";
                }
                else {
                    // trans 是纯英文/异常：兜底
                    return original + "（" + trans + "）";
                }
            }
        };

        // 应用翻译到单个字符串：
        // - 已是中文：识别双语格式（括号 / 斜杠）按当前设置重新规范化，实现
        //   ① 斜杠"英文/中文"转"中文（英文）对照/纯中文"；② 纯中文可覆盖之前应用的对照格式
        // - 纯英文：优先查格式转换词典，回退到唯一词典 + 拼接规则
        // - isDesc=true（描述字段）：译文已是中文时直接采用，不强制拼上括号英文
        auto applyString = [&](const std::string& original, const std::string& dictTrans, bool isDesc = false) -> std::string {
            if (contains_chinese(original)) {
                std::string zh, en;
                if (parseParenPair(original, zh, en))          // 中文（英文）/ 英文（中文）
                    return combineZhEn("", zh, en);
                if (parseSlashPair(original, zh, en))          // 英文/中文 斜杠双语
                    return combineZhEn(extractPrefix(original, zh, en), zh, en);
                return original;                               // 已是纯中文或其他，不动
            }
            // 1. 优先查格式转换词典：把"英文"或"英文 / 中文"直接转成标准"中文（英文）"
            auto fit = formatMap.find(original);
            if (fit != formatMap.end()) return fit->second;
            if (isDesc) {
                // 描述字段：译文含中文即视为已翻译，直接采用（规范化括号/斜杠格式）
                std::string z, e;
                if (parseParenPair(dictTrans, z, e))
                    return combineZhEn("", z, e);              // 已有括号格式则按设置规范化
                if (parseSlashPair(dictTrans, z, e))
                    return combineZhEn(extractPrefix(dictTrans, z, e), z, e); // 斜杠双语转标准格式
                if (contains_chinese(dictTrans)) return dictTrans;             // 纯中文直接采用
                return dictTrans.empty() ? original : dictTrans;               // 仍是英文则回退
            }
            // 2. 回退到唯一词典 + 拼接规则
            return makeTranslation(original, dictTrans);
        };

        // 翻译查询：唯一词典优先，备选 *_已翻译.json 兜底。
        // 若唯一词典已有该 key 的非空翻译，直接采用，不会用备选源覆盖。
        auto lookupTrans = [&](const json& primary, const json& backup, const std::string& key) -> std::string {
            if (primary.contains(key) && primary[key].is_string() && !primary[key].get<std::string>().empty())
                return primary[key].get<std::string>();
            if (backup.contains(key) && backup[key].is_string() && !backup[key].get<std::string>().empty())
                return backup[key].get<std::string>();
            return "";
        };

        // 黑名单回滚：
        // 1) 精确回滚：唯一词典里被移除的 key/value（relKey + field + 当前翻译值 -> 英文原文）
        // 2) Wiki 术语全局子串回滚：处理只被 Wiki/自动补全写进文件、但没进 _options/_descriptions 的翻译
        // 3) 兜底：括号格式解析
        auto rollbackIfBlacklisted = [&](const std::string& cur, const std::string& field) -> std::string {
            if (blacklist.empty()) return cur;
            if (!contains_chinese(cur)) return cur;  // 未翻译，无需回滚
            // 1) 精确回滚
            std::string rbKey = relKey + std::string(1, '\0') + field + std::string(1, '\0') + cur;
            auto fit = rollbackMap.find(rbKey);
            if (fit != rollbackMap.end()) return fit->second;
            // 2) Wiki 术语全局子串回滚（长中文优先，避免短词误替换）
            std::string replaced = cur;
            bool changed = false;
            for (const auto& kv : wikiZhToEn) {
                const std::string& zh = kv.first;
                const std::string& en = kv.second;
                size_t pos = 0;
                while ((pos = replaced.find(zh, pos)) != std::string::npos) {
                    replaced.replace(pos, zh.size(), en);
                    pos += en.size();
                    changed = true;
                }
            }
            if (changed) return replaced;
            // 3) 兜底：括号格式解析
            std::string eng = extract_english_from_translated(cur);
            if (!eng.empty() && is_blacklisted(eng, blacklist)) return eng;
            return cur;
        };

        // Name
        if (j.contains("Name")) {
            if (j["Name"].is_string()) {
                std::string orig = j["Name"].get<std::string>();
                std::string rb = rollbackIfBlacklisted(orig, "Name");
                if (rb != orig) { j["Name"] = rb; rolledBack++; changed = true; }
                else {
                    std::string key = relKey + "||Name||" + orig;
                    if (!is_blacklisted(orig, blacklist)) {
                        std::string trans = lookupTrans(options, backupOpt, key);
                        if (!trans.empty()) {
                            std::string res = applyString(orig, trans);
                            if (res != orig) { j["Name"] = res; appliedOpt++; changed = true; }
                        }
                    }
                }
            }
            else if (j["Name"].is_object()) {
                for (auto& it : j["Name"].items()) {
                    if (!it.value().is_string()) continue;
                    std::string orig = it.value().get<std::string>();
                    std::string rb = rollbackIfBlacklisted(orig, "Name");
                    if (rb != orig) { it.value() = rb; rolledBack++; changed = true; }
                    else {
                        std::string key = relKey + "||" + it.key() + "||Name||" + orig;
                        if (!is_blacklisted(orig, blacklist)) {
                            std::string trans = lookupTrans(options, backupOpt, key);
                            if (!trans.empty()) {
                                std::string res = applyString(orig, trans);
                                if (res != orig) { it.value() = res; appliedOpt++; changed = true; }
                            }
                        }
                    }
                }
            }
        }

        // Description
        if (j.contains("Description")) {
            if (j["Description"].is_string()) {
                std::string orig = j["Description"].get<std::string>();
                std::string rb = rollbackIfBlacklisted(orig, "Desc");
                if (rb != orig) { j["Description"] = rb; rolledBack++; changed = true; }
                else {
                    std::string key = relKey + "||Desc||" + orig;
                    if (!is_blacklisted(orig, blacklist)) {
                        std::string trans = lookupTrans(descriptions, backupDesc, key);
                        if (!trans.empty()) {
                            std::string res = applyString(orig, trans, true); // 描述：纯中文直接采用
                            if (res != orig) { j["Description"] = res; appliedDesc++; changed = true; }
                        }
                    }
                }
            }
            else if (j["Description"].is_object()) {
                for (auto& it : j["Description"].items()) {
                    if (!it.value().is_string()) continue;
                    std::string orig = it.value().get<std::string>();
                    std::string rb = rollbackIfBlacklisted(orig, "Desc");
                    if (rb != orig) { it.value() = rb; rolledBack++; changed = true; }
                    else {
                        std::string key = relKey + "||" + it.key() + "||Desc||" + orig;
                        if (!is_blacklisted(orig, blacklist)) {
                            std::string trans = lookupTrans(descriptions, backupDesc, key);
                            if (!trans.empty()) {
                                std::string res = applyString(orig, trans, true); // 描述：纯中文直接采用
                                if (res != orig) { it.value() = res; appliedDesc++; changed = true; }
                            }
                        }
                    }
                }
            }
        }

        // 选项值：Options[].Name（玩家在游戏里实际看到的选项）
        if (j.contains("Options") && j["Options"].is_array()) {
            for (auto& opt : j["Options"].items()) {
                if (!opt.value().is_object()) continue;
                if (!opt.value().contains("Name")) continue;
                json& nv = opt.value()["Name"];
                if (!nv.is_string()) continue;
                std::string orig = nv.get<std::string>();
                std::string rb = rollbackIfBlacklisted(orig, "Opt");
                if (rb != orig) { nv = rb; rolledBack++; changed = true; }
                else {
                    std::string key = relKey + "||Opt||" + orig;
                    if (!is_blacklisted(orig, blacklist) && options.contains(key)) {
                        std::string trans = options[key].get<std::string>();
                        std::string res = applyString(orig, trans);
                        if (res != orig) { nv = res; appliedOpt++; changed = true; }
                    }
                }
            }
        }

        if (changed) {
            write_binary_file(file, j.dump(2));
        }
    }

    Log("应用完成：修改选项名 " + std::to_string(appliedOpt)
        + " 个，描述 " + std::to_string(appliedDesc) + " 个");
    if (rolledBack > 0)
        Log("[黑名单] 已回滚 " + std::to_string(rolledBack) + " 条命中黑名单的翻译（还原为英文原文）");
    Log("提示：如有未翻译文本，请手动清理 未翻译.json 文件（不再自动删除）");
    return true;
    }
    catch (const std::system_error& e) { Log(std::string("[错误] 应用翻译时系统异常: ") + e.what()); return false; }
    catch (const std::exception& e) { Log(std::string("[错误] 应用翻译时异常: ") + e.what()); return false; }
    catch (...) { Log("[错误] 应用翻译时未知异常"); return false; }
}

// ------------------------------------------------------------------
// 词典工具与导入翻译
// ------------------------------------------------------------------
// 汇总词典文件名（v2.2.7 起由「wiki_术语对照and个人填充.json」改名为「汇总词典.json」）。
// 返回 dictDir 下汇总词典路径；若新文件名不存在而旧文件名存在，自动迁移（重命名），
// 保证老用户升级后已有词典不丢失。迁移失败时退回旧文件名继续使用。
static fs::path MergedDictPath(const fs::path& dictDir)
{
    fs::path p = dictDir / fs::u8path("汇总词典.json");
    std::error_code ec;
    if (!fs::exists(p, ec) || ec) {
        fs::path old = dictDir / fs::u8path("wiki_术语对照and个人填充.json");
        std::error_code oec;
        if (fs::exists(old, oec) && !oec) {
            fs::rename(old, p, ec);
            if (ec) return old; // 迁移失败：退回旧名继续使用
            LogThread("[提示] 已把旧词典 wiki_术语对照and个人填充.json 迁移为 汇总词典.json");
        }
    }
    return p;
}

// 个性翻译.json：保存 AI 翻译不理想、用户想单独指定译法的词汇/短语（格式：英文:中文）。
// 用户建立词典目录后即创建；点「3. 词典写入Mod」时，其内容会以"覆盖"方式压过
// 汇总词典里的对应词条，再写入 Mod。命中黑名单的词条不生效（黑名单 > 个性翻译）。
static fs::path CustomDictPath(const fs::path& dictDir)
{
    return dictDir / fs::u8path("个性翻译.json");
}

// 确保 个性翻译.json 存在（仅首次创建，绝不覆盖用户已编辑内容——含未来版本更新）。
// 首次创建时：若 exe 旁存在内置模板 内置个性翻译.json（预填 FF14 种族名译法作格式参考，
// 随程序打包、隐藏属性），复制其内容作为起点；否则退回创建空对象。
static void EnsureCustomDictFile(const fs::path& dictDir)
{
    std::error_code ec;
    fs::path p = CustomDictPath(dictDir);
    if (fs::exists(p, ec) && !ec) return; // 已存在（含用户编辑过）：绝不覆盖
    if (fs::exists(dictDir, ec) && !ec) {
        // v2.3.3：内置模板统一放在 exe 旁的「内置模板」子文件夹（zip 不保留隐藏属性）
        fs::path builtin = GetExeDir() / fs::u8path("内置模板") / fs::u8path("内置个性翻译.json");
        std::error_code bec;
        if (fs::exists(builtin, bec) && !bec) {
            fs::copy_file(builtin, p, fs::copy_options::overwrite_existing, bec);
            if (!bec) {
                LogThread("[提示] 已创建 个性翻译.json（内置模板：预填 FF14 种族名译法作格式参考，可编辑增删；点『3. 词典写入Mod』时覆盖汇总词典对应词条，命中黑名单的词条不生效）");
                return;
            }
        }
        write_binary_file(p, json::object().dump(2));
        LogThread("[提示] 已创建 个性翻译.json（格式：英文:中文，一行一词；点『3. 词典写入Mod』时覆盖汇总词典对应词条，命中黑名单的词条不生效）");
    }
}

// 确保词典目录下存在 单词黑名单.json（不存在则自动导出创建，方便用户直接查看/编辑）。
// v2.2.8 起：只要词典目录路径确定（选择/手输/启动加载）就建立，不再依赖点「1. 提取英文」。
static void EnsureBlacklistFile()
{
    std::string base = g_cfg.dictionaryDir.empty() ? g_cfg.translationDir : g_cfg.dictionaryDir;
    if (base.empty()) return;
    fs::path blPath = fs::u8path(base) / fs::u8path("单词黑名单.json");
    std::error_code bec;
    if (!fs::exists(blPath, bec) || bec) {
        std::error_code dec;
        fs::create_directories(fs::u8path(base), dec);
        std::vector<std::string> merged = LoadBlacklistFile(g_cfg.blacklist);
        SaveBlacklistFile(merged);
        std::string list;
        for (size_t i = 0; i < merged.size(); ++i) { if (i) list += "、"; list += merged[i]; }
        LogThread("[提示] 单词黑名单.json 不存在，已自动导出创建（词典目录），当前黑名单 = " + list);
    }
}

// 目录变更时把程序相关文件从旧目录迁移到新目录（仅当新目录还没有同名文件，避免覆盖用户数据）。
// exactNames：精确文件名；substrNames：文件名包含子串即迁移。
// 同盘用 rename，跨盘回退 copy + remove。
static void MigrateDirFiles(const fs::path& srcDir, const fs::path& dstDir,
                            const std::vector<std::wstring>& exactNames,
                            const std::vector<std::wstring>& substrNames)
{
    if (srcDir.empty() || dstDir.empty() || srcDir == dstDir) return;
    std::error_code dec;
    if (!fs::exists(dstDir, dec) || dec) fs::create_directories(dstDir, dec);
    int moved = 0;
    auto tryMove = [&](const fs::path& src, const fs::path& dst) {
        std::error_code e1, e2;
        if (!(fs::exists(src, e1) && !e1)) return;  // 旧目录无此文件
        if (fs::exists(dst, e2) && !e2) return;     // 新目录已有同名文件：不覆盖
        std::error_code mec;
        fs::rename(src, dst, mec);                  // 同盘：直接移动
        if (mec) {                                  // 跨盘：复制后删除
            std::error_code cec, rec;
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing, cec);
            if (!cec) { fs::remove(src, rec); moved++; }
        } else { moved++; }
    };
    for (auto& n : exactNames) tryMove(srcDir / n, dstDir / n);
    for (auto& sub : substrNames) {
        std::error_code sec;
        fs::directory_iterator dit(srcDir, sec);
        for (; dit != fs::directory_iterator(); dit.increment(sec)) {
            if (!dit->is_regular_file(sec)) continue;
            std::wstring fn = dit->path().filename().wstring();
            if (fn.find(sub) != std::wstring::npos)
                tryMove(dit->path(), dstDir / dit->path().filename());
        }
    }
    if (moved > 0)
        LogThread("[提示] 已把目录变更前生成的 " + std::to_string(moved) + " 个程序文件迁移到新目录");
}

// 词典目录变更统一处理：旧目录的词典文件迁移到新目录，并在新目录确保 汇总词典.json /
// 个性翻译.json / 单词黑名单.json / wiki_术语对照.json 全部就位。
static void OnDictionaryDirChanged(const std::string& newDir, bool doSave)
{
    if (newDir.empty()) return;
    std::string old = g_cfg.dictionaryDir;
    if (old == newDir) { EnsureDictFile(); return; }
    MigrateDirFiles(fs::u8path(old), fs::u8path(newDir),
        { L"汇总词典.json", L"个性翻译.json", L"单词黑名单.json", L"wiki_术语对照.json" });
    g_cfg.dictionaryDir = newDir;
    EnsureDictFile();
    if (doSave) SaveConfig();
}

// 词典目录没有 wiki_术语对照.json 时，从 exe 旁的内置文件释放一份（打包随程序发布的 wiki 原典）。
// 已有文件则不覆盖（用户可能已自行导出更新）。返回是否成功释放。
static bool ReleaseBuiltinWiki(const fs::path& dictDir)
{
    std::error_code ec;
    fs::path out = dictDir / fs::u8path("wiki_术语对照.json");
    if (fs::exists(out, ec) && !ec) return false; // 已有：不覆盖
    // v2.3.3：内置模板统一放在 exe 旁的「内置模板」子文件夹（zip 不保留隐藏属性）
    fs::path builtin = GetExeDir() / fs::u8path("内置模板") / fs::u8path("内置wiki_术语对照.json");
    if (!fs::exists(builtin, ec) || ec) return false; // 未随程序打包：跳过
    std::error_code cc;
    fs::create_directories(dictDir, cc);
    fs::copy_file(builtin, out, fs::copy_options::overwrite_existing, cc);
    return !cc;
}

// 确保汇总词典 汇总词典.json 存在（仅首次生成）。
// 该文件是程序唯一的词典，含三部分：
//   terms         英文->中文 固定映射（wiki 原典 + 个人填充，先来后到），补全/回滚用
//   _options      完整 key（路径||Name||原文）翻译，应用翻译用
//   _descriptions 同上
// 首次生成规则：wiki_术语对照.json（wiki 导出的原典，固定优先）→ 汉化总词典.json 仅补充。
// 生成后程序不再自动重写本文件——要修改已翻译的词条，直接编辑本文件即可。
// 若汇总词典缺失或损坏，程序会以 wiki 原典为基准重建，个人填充靠 *_已翻译.json 自动回填。
static void EnsureDictFile()
{
    if (g_cfg.dictionaryDir.empty()) return;
    fs::path dictDir = fs::u8path(g_cfg.dictionaryDir);
    EnsureCustomDictFile(dictDir); // 词典目录就绪即创建 个性翻译.json（已存在不覆盖）
    EnsureBlacklistFile();         // v2.2.8：词典目录就绪即创建 单词黑名单.json
    fs::path outPath = MergedDictPath(dictDir); // 含旧文件名自动迁移
    std::error_code oec;
    if (fs::exists(outPath, oec) && !oec) return; // 已存在：不覆盖用户手动修改

    json merged;
    merged["terms"] = json::object();
    merged["_options"] = json::object();
    merged["_descriptions"] = json::object();

    // 1) wiki 术语对照（固定优先，先来后到）
    // 词典目录没有原典时，先从 exe 旁的内置 wiki_术语对照.json 释放一份（打包随程序发布），保证首次即有词条
    fs::path wikiPath = dictDir / fs::u8path("wiki_术语对照.json");
    std::error_code wec;
    if ((!fs::exists(wikiPath, wec) || wec) && ReleaseBuiltinWiki(dictDir))
        LogThread("[提示] 词典目录无 wiki 原典，已释放内置 wiki_术语对照.json 作为初始原典");
    if (fs::exists(wikiPath, wec) && !wec) {
        std::string d;
        if (read_binary_file(wikiPath, d)) {
            try {
                json wd = json::parse(clean_utf8(d));
                if (wd.is_object() && wd.contains("terms") && wd["terms"].is_object()) {
                    for (auto& it : wd["terms"].items()) {
                        if (!it.value().is_string()) continue;
                        std::string en = it.key(), zh = it.value().get<std::string>();
                        if (en.empty() || zh.empty() || en == zh) continue;
                        merged["terms"][en] = zh;
                    }
                }
            } catch (...) {}
        }
    }

    // 2) 汉化总词典（个人填充：完整 key 保留，原文并入 terms，仅补充不覆盖）
    fs::path masterPath = dictDir / fs::u8path("汉化总词典.json");
    std::error_code mec;
    if (fs::exists(masterPath, mec) && !mec) {
        std::string d;
        if (read_binary_file(masterPath, d)) {
            try {
                json md = json::parse(clean_utf8(d));
                if (md.is_object()) {
                    for (auto& sec : { "_options", "_descriptions" }) {
                        if (!md.contains(sec) || !md[sec].is_object()) continue;
                        for (auto& it : md[sec].items()) {
                            if (!it.value().is_string()) continue;
                            std::string val = it.value().get<std::string>();
                            if (val.empty()) continue;
                            merged[sec][it.key()] = val; // 完整 key 直接保留
                            // key 格式：路径||Name||原文，取最后一个 || 后的原文
                            auto kp = it.key().rfind("||");
                            if (kp == std::string::npos) continue;
                            std::string en = it.key().substr(kp + 2);
                            if (en.empty() || contains_chinese(en)) continue;
                            // 剥离括号，取纯中文部分
                            std::string zh = val;
                            auto op = zh.find("（");
                            if (op != std::string::npos) zh = zh.substr(0, op);
                            size_t s = zh.find_first_not_of(" \t");
                            size_t e = zh.find_last_not_of(" \t");
                            if (s != std::string::npos && e != std::string::npos) zh = zh.substr(s, e - s + 1);
                            if (zh.empty() || zh == en) continue;
                            // 先来后到：wiki 固定词或已填充的词，不覆盖
                            if (merged["terms"].contains(en)) continue;
                            merged["terms"][en] = zh;
                        }
                    }
                }
            } catch (...) {}
        }
    }

    std::error_code dec;
    if (!fs::exists(dictDir, dec)) fs::create_directories(dictDir, dec);
    write_binary_file(outPath, merged.dump(2));
}

// Wiki 原典导出后，把新增词条同步进汇总词典 汇总词典.json：
// - 汇总词典不存在 → 按 EnsureDictFile 规则从原典 + 汉化总词典 重新生成
// - 已存在 → 只补入原典有而汇总没有的 terms（不覆盖已有，先来后到）
static void SyncWikiToMerged(const fs::path& dictDir)
{
    std::error_code ec;
    fs::path mergedPath = MergedDictPath(dictDir);
    if (!fs::exists(mergedPath, ec) || ec) {
        EnsureDictFile();
        LogThread("[提示] 汇总词典 汇总词典.json 不存在，已按「wiki 原典优先 + 汉化总词典补充」重新生成");
        return;
    }
    std::string d;
    if (!read_binary_file(mergedPath, d)) return;
    json merged;
    try { merged = json::parse(clean_utf8(d)); }
    catch (...) { merged = json::object(); }
    if (!merged.is_object()) merged = json::object();
    if (!merged.contains("terms")) merged["terms"] = json::object();
    if (!merged["terms"].is_object()) merged["terms"] = json::object();

    fs::path wikiPath = dictDir / fs::u8path("wiki_术语对照.json");
    std::error_code wec;
    if (!fs::exists(wikiPath, wec) || wec) return;
    std::string wd;
    if (!read_binary_file(wikiPath, wd)) return;
    json wiki;
    try { wiki = json::parse(clean_utf8(wd)); }
    catch (...) { return; }
    if (!wiki.is_object() || !wiki.contains("terms") || !wiki["terms"].is_object()) return;

    int added = 0;
    for (auto& it : wiki["terms"].items()) {
        if (!it.value().is_string()) continue;
        std::string en = it.key(), zh = it.value().get<std::string>();
        if (en.empty() || zh.empty() || en == zh) continue;
        if (!merged["terms"].contains(en)) {
            merged["terms"][en] = zh;
            added++;
        }
    }
    if (added > 0) {
        if (!merged.contains("_options")) merged["_options"] = json::object();
        if (!merged.contains("_descriptions")) merged["_descriptions"] = json::object();
        write_binary_file(mergedPath, merged.dump(2));
        LogThread("[提示] 已把 wiki 原典新增的 " + std::to_string(added) + " 条术语同步进汇总词典");
    }
    else {
        LogThread("[提示] 汇总词典已包含全部 wiki 原典词条，无需同步");
    }
}

// 加载词典 → 英文->中文 映射。
// 唯一词典来源：汇总词典.json 的 terms 部分
// （wiki 固定优先 + 个人填充，先来后到；要修改词条请直接编辑该文件）。
static bool LoadTermMap(std::unordered_map<std::string, std::string>& termMap, size_t& maxTermLen)
{
    if (g_cfg.dictionaryDir.empty()) return false;
    fs::path dictDir = fs::u8path(g_cfg.dictionaryDir);

    EnsureDictFile(); // 兜底：词典文件不存在时首次生成

    // 读唯一词典的 terms 构建映射
    fs::path mergedPath = MergedDictPath(dictDir);
    std::error_code gec;
    if (fs::exists(mergedPath, gec) && !gec) {
        auto loadInto = [&](const json& md) {
            if (!md.is_object() || !md.contains("terms") || !md["terms"].is_object()) return;
            for (auto& it : md["terms"].items()) {
                if (!it.value().is_string()) continue;
                std::string en = it.key(), zh = it.value().get<std::string>();
                if (en.empty() || zh.empty()) continue; // 允许 en==zh 的"保英文"词条（如 Lava→Lava），保留原文不翻译
                termMap[en] = zh;
                // 同时存入小写 key：选项文本中常出现小写/首字母小写，避免大小写不一致导致漏翻
                std::string enLower;
                enLower.reserve(en.size());
                for (unsigned char c : en) enLower += static_cast<char>(std::tolower(c));
                if (enLower != en) termMap[enLower] = zh;
                if (en.size() > maxTermLen) maxTermLen = en.size();
            }
        };
        std::string d;
        if (read_binary_file(mergedPath, d)) {
            try {
                json md = json::parse(clean_utf8(d));
                loadInto(md);
            }
            catch (...) {
                // 汇总词典损坏：备份后按「wiki 原典优先 + 汉化总词典补充」重建，再读取
                LogThread("[错误] 汇总词典.json 解析失败（可能损坏），已备份并从 wiki 原典重建");
                std::error_code bec;
                fs::path bak = mergedPath;
                bak += L".损坏备份";
                fs::rename(mergedPath, bak, bec);
                EnsureDictFile();
                std::string d2;
                if (read_binary_file(mergedPath, d2)) {
                    try { loadInto(json::parse(clean_utf8(d2))); } catch (...) {}
                }
            }
        }
    }

    // 原典兜底：唯一词典未收录的词条，再查 wiki 原典（wiki_术语对照.json），
    // 不覆盖唯一词典已有词条（含个人填充），实现「词典 → 原典」的读取顺序
    fs::path wikiPath = dictDir / fs::u8path("wiki_术语对照.json");
    std::error_code wec;
    if (fs::exists(wikiPath, wec) && !wec) {
        std::string wd;
        if (read_binary_file(wikiPath, wd)) {
            try {
                json wj = json::parse(clean_utf8(wd));
                if (wj.is_object() && wj.contains("terms") && wj["terms"].is_object()) {
                    int added = 0;
                    for (auto& it : wj["terms"].items()) {
                        if (!it.value().is_string()) continue;
                        std::string en = it.key(), zh = it.value().get<std::string>();
                        if (en.empty() || zh.empty()) continue;
                        if (termMap.find(en) != termMap.end()) continue; // 唯一词典优先
                        termMap[en] = zh;
                        std::string enLower;
                        enLower.reserve(en.size());
                        for (unsigned char c : en) enLower += static_cast<char>(std::tolower(c));
                        if (enLower != en && termMap.find(enLower) == termMap.end()) termMap[enLower] = zh;
                        if (en.size() > maxTermLen) maxTermLen = en.size();
                        added++;
                    }
                    if (added > 0)
                        LogThread("[提示] 原典兜底补充 " + std::to_string(added) + " 条词条（wiki_术语对照.json）");
                }
            } catch (...) {}
        }
    }
    if (maxTermLen > 200) maxTermLen = 200;
    return !termMap.empty();
}

// 单条文本翻译：完整命中直接返回；否则按词边界做最长子串替换
static std::string TranslateText(const std::string& orig,
    const std::unordered_map<std::string, std::string>& termMap, size_t maxTermLen)
{
    if (orig.empty() || contains_chinese(orig)) return "";
    auto fit = termMap.find(orig);
    if (fit != termMap.end()) return fit->second; // 完全命中

    std::string result;
    size_t n = orig.size();
    size_t i = 0;
    bool replaced = false;
    while (i < n) {
        size_t best = 0;
        bool startB = (i == 0 || isspace((unsigned char)orig[i - 1]) || ispunct((unsigned char)orig[i - 1]));
        if (startB) {
            size_t maxLen = (std::min)(maxTermLen, n - i);
            for (size_t len = maxLen; len >= 1; --len) {
                auto itm = termMap.find(orig.substr(i, len));
                if (itm != termMap.end()) {
                    size_t endPos = i + len;
                    bool endB = (endPos == n || isspace((unsigned char)orig[endPos]) || ispunct((unsigned char)orig[endPos]));
                    if (endB) { best = len; break; }
                }
            }
        }
        if (best > 0) {
            std::string sub = orig.substr(i, best);
            auto itb = termMap.find(sub);
            result += (itb != termMap.end()) ? itb->second : sub;
            i += best;
            replaced = true;
        } else {
            result += orig[i];
            ++i;
        }
    }
    return replaced ? result : "";
}

// 判断字符串中是否残留多个英文单词（>=2 个 ASCII 字母段）。
// 用于识别"部分替换后仍有英文句子"的半翻译（如 "Required for 硌狮族 models."），
// 单英文专名（如词典译名里的 "Lavabod"）不算残留。
static bool hasEnglishWordResidue(const std::string& s)
{
    int words = 0;
    bool inWord = false;
    for (char c : s) {
        bool isLetter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        if (isLetter) { if (!inWord) { inWord = true; ++words; } }
        else inWord = false;
    }
    return words >= 2;
}

// 词典预填写入的展示格式：Opt/Name 拼成"中文（英文）"（按词序设置），
// Desc 描述保持纯中文（与 applyString 的 isDesc 规则一致）。
static std::string dictFillDisplay(const std::string& fullKey, const std::string& zh, const std::string& english)
{
    if (g_cfg.pureChinese) return zh;
    if (fullKey.find("||Desc||") != std::string::npos) return zh;
    size_t a = zh.find_first_not_of(" \t\r\n");
    size_t b = zh.find_last_not_of(" \t\r\n");
    std::string z = (a == std::string::npos) ? zh : zh.substr(a, b - a + 1);
    if (!contains_chinese(z)) return z; // 词典译名仍是英文（专名等）时原样保留，不拼括号
    if (g_cfg.swapWordOrder) return z + "（" + english + "）";
    return english + "（" + z + "）";
}

// 用词典补全 JSON 中空白项。返回补全条数；missed=仍未命中；already=已有翻译被保留数
static int AutoFillJsonWithDict(json& tj,
    const std::unordered_map<std::string, std::string>& termMap,
    size_t maxTermLen, int& missed, int& already,
    const json* fullDict = nullptr)
{
    int filled = 0;
    missed = 0;
    already = 0;
    for (auto& sec : { "_options", "_descriptions" }) {
        if (!tj.contains(sec) || !tj[sec].is_object()) continue;
        for (auto& it : tj[sec].items()) {
            if (!it.value().is_string()) continue;
            if (!it.value().get<std::string>().empty()) { already++; continue; } // 已有翻译，不动
            std::string key = it.key();
            auto kp = key.rfind("||");
            std::string orig = (kp == std::string::npos) ? key : key.substr(kp + 2);
            // 1. 汇总词典完整 key 命中：同源翻译直接复用
            if (fullDict && fullDict->contains(sec) && (*fullDict)[sec].is_object()
                && (*fullDict)[sec].contains(key) && (*fullDict)[sec][key].is_string()
                && !(*fullDict)[sec][key].get<std::string>().empty()) {
                it.value() = dictFillDisplay(key, (*fullDict)[sec][key].get<std::string>(), orig);
                filled++;
                continue;
            }
            // 2. 术语映射（wiki 术语 / 个人词条）
            std::string tr = TranslateText(orig, termMap, maxTermLen);
            // 有英文句子残留（如 "Required for 硌狮族 models."）视为未完成，留给后续处理；
            // tr==orig 的"保英文"词条（如 Lava→Lava）也视为命中，直接保留原文不翻译
            if (!tr.empty() && !hasEnglishWordResidue(tr)) { it.value() = dictFillDisplay(key, tr, orig); filled++; }
            else missed++;
        }
    }
    return filled;
}

// 候选翻译文件（*_未翻译.json / *_已翻译.json），带统计，按修改时间降序
struct TransFileInfo { fs::path path; std::time_t mtime; bool isTranslated; int total; int filled; };
static std::vector<TransFileInfo> ScanTransFiles(const fs::path& transDir)
{
    struct TmpInfo { fs::path path; std::time_t mtime; bool isTranslated; };
    std::vector<TmpInfo> files;
    try {
        fs::directory_iterator it(transDir, fs::directory_options::skip_permission_denied);
        for (; it != fs::directory_iterator(); ++it) {
            std::error_code ec;
            auto& e = *it;
            if (!e.is_regular_file(ec) || ec) continue;
            auto fname = e.path().filename().wstring();
            if (fname.size() <= 5 || fname.substr(fname.size() - 5) != L".json") continue;
            bool isUntr = fname.find(L"_未翻译") != std::wstring::npos;
            bool isTran = fname.find(L"_已翻译") != std::wstring::npos;
            if (!isUntr && !isTran) continue;
            auto ftime = fs::last_write_time(e.path(), ec);
            if (!ec) files.push_back({ e.path(), ftime.time_since_epoch().count(), isTran });
        }
    }
    catch (const std::system_error&) {}
    std::sort(files.begin(), files.end(), [](const TmpInfo& a, const TmpInfo& b) { return a.mtime > b.mtime; });

    std::vector<TransFileInfo> out;
    for (auto& f : files) {
        TransFileInfo info{ f.path, f.mtime, f.isTranslated, 0, 0 };
        std::string d;
        if (read_binary_file(f.path, d)) {
            try {
                json tj = json::parse(clean_utf8(d));
                if (tj.is_object()) {
                    for (auto& sec : { "_options", "_descriptions" }) {
                        if (!tj.contains(sec) || !tj[sec].is_object()) continue;
                        for (auto& it : tj[sec].items()) {
                            if (!it.value().is_string()) continue;
                            info.total++;
                            if (!it.value().get<std::string>().empty()) info.filled++;
                        }
                    }
                }
            } catch (...) {}
        }
        out.push_back(info);
    }
    return out;
}

// 导入翻译：一个入口依次完成两个功能（执行时会在日志输出给用户的说明）：
//   功能一：用唯一词典（汇总词典.json）补全未翻译项 → 生成/更新 *_已翻译.json
//   功能二：将已翻译文件的所有非空翻译编入唯一词典（不覆盖已有，先来后到）
bool ImportTranslations(const fs::path& inFile, bool autoFill)
{
    try {
    if (g_cfg.dictionaryDir.empty()) { LogThread("[错误] 未设置词典目录"); return false; }
    fs::path dictDir = fs::u8path(g_cfg.dictionaryDir);
    std::error_code existEc;
    if (!fs::exists(dictDir, existEc)) {
        std::error_code ec;
        fs::create_directories(dictDir, ec);
        if (ec) { LogThread("[错误] 无法创建词典目录"); return false; }
    }

    std::string data;
    if (!read_binary_file(inFile, data)) {
        LogThread("[错误] 读取文件失败：" + wstring_to_utf8(inFile.filename().wstring())); return false;
    }
    json tj;
    try { tj = json::parse(clean_utf8(data)); }
    catch (...) { LogThread("[错误] JSON 解析失败：" + wstring_to_utf8(inFile.filename().wstring())); return false; }
    if (!tj.is_object()) { LogThread("[错误] 文件格式不正确（应为 JSON 对象）"); return false; }
    if (!tj.contains("_options") || !tj.contains("_descriptions")) {
        LogThread("[错误] 缺少 _options / _descriptions 字段"); return false;
    }

    // ── 功能一：词典补全（仅当勾选「导入前用词典自动补全未翻译项」时执行）────────
    if (autoFill) {
        LogThread("[提示] 导入翻译·功能一：词典补全 —— 用唯一词典补全本文件空白项（不覆盖已有翻译）");
        std::unordered_map<std::string, std::string> termMap;
        size_t maxTermLen = 0;
        // 读取汇总词典完整内容，用于按完整 key 精确补全（不限于术语映射）
        json fullDict = json::object();
        fs::path fdPath = MergedDictPath(dictDir);
        std::error_code fdec;
        if (fs::exists(fdPath, fdec) && !fdec) {
            std::string fd;
            if (read_binary_file(fdPath, fd)) {
                try { fullDict = json::parse(clean_utf8(fd)); }
                catch (...) {}
            }
        }
        if (LoadTermMap(termMap, maxTermLen) || fullDict.is_object()) {
            LogThread("[提示] 已加载唯一词典（汇总词典.json，wiki 固定优先、先来后到）");
            int missed = 0, already = 0;
            int filled = AutoFillJsonWithDict(tj, termMap, maxTermLen, missed, already,
                fullDict.is_object() && !fullDict.empty() ? &fullDict : nullptr);
            LogThread("词典补全：本次补全 " + std::to_string(filled) + " 条，已有翻译保留 "
                + std::to_string(already) + " 条，仍未命中 " + std::to_string(missed) + " 条");
        } else {
            LogThread("[提示] 词典为空（未找到 汇总词典.json），跳过补全");
        }
    }

    // ── 功能一收尾：结果落盘，生成/更新 *_已翻译.json ────────────
    fs::path outFile = inFile;
    std::wstring inName = inFile.filename().wstring();
    size_t untrPos = inName.find(L"_未翻译");
    if (untrPos != std::wstring::npos) {
        std::wstring outName = inName;
        outName.replace(untrPos, std::wstring(L"_未翻译").size(), L"_已翻译");
        outFile = inFile.parent_path() / outName;
        std::error_code oec;
        if (fs::exists(outFile, oec) && !oec) {
            bool hasContent = false;
            std::string existData;
            if (read_binary_file(outFile, existData)) {
                try {
                    json ej = json::parse(clean_utf8(existData));
                    if (ej.is_object()) {
                        for (auto& sec : { "_options", "_descriptions" }) {
                            if (!ej.contains(sec) || !ej[sec].is_object()) continue;
                            for (auto& it : ej[sec].items()) {
                                if (it.value().is_string() && !it.value().get<std::string>().empty()) { hasContent = true; break; }
                            }
                            if (hasContent) break;
                        }
                    }
                } catch (...) {}
            }
            if (hasContent) {
                std::wstring alt = outName;
                size_t dp = alt.rfind(L'.');
                if (dp != std::wstring::npos) alt.insert(dp, L"_自动补全");
                else alt += L"_自动补全";
                outFile = inFile.parent_path() / alt;
                LogThread("[提示] " + wstring_to_utf8(outName)
                    + " 已存在且含翻译内容，为避免覆盖，本次输出为 " + wstring_to_utf8(alt));
            }
        }
    }
    write_binary_file(outFile, tj.dump(2));
    LogThread("[提示] 已写入 " + wstring_to_utf8(outFile.filename().wstring()));

    // ── 功能二：编入唯一词典 ──────────────────────────────
    LogThread("[提示] 导入翻译·功能二：编入唯一词典 —— 把 "
        + wstring_to_utf8(outFile.filename().wstring())
        + " 的非空翻译编入 汇总词典.json（已有翻译不覆盖，先来后到）");
    EnsureDictFile(); // 词典不存在时首次生成
    fs::path dictPath = MergedDictPath(dictDir);
    json dict;
    if (fs::exists(dictPath)) {
        std::string d;
        if (read_binary_file(dictPath, d)) {
            try { dict = json::parse(clean_utf8(d)); }
            catch (...) {
                // 汇总词典损坏：备份后从 wiki 原典重建，防止后续写入覆盖丢失全部数据
                LogThread("[错误] 汇总词典.json 解析失败（可能损坏），已备份并从 wiki 原典重建");
                std::error_code bec;
                fs::path bak = dictPath;
                bak += L".损坏备份";
                fs::rename(dictPath, bak, bec);
                EnsureDictFile();
                dict = json::object();
            }
        }
    }
    if (!dict.is_object()) dict = json::object();
    if (!dict.contains("_options")) dict["_options"] = json::object();
    if (!dict.contains("_descriptions")) dict["_descriptions"] = json::object();
    if (!dict.contains("terms")) dict["terms"] = json::object();

    // 个性翻译.json：命中词条以个性翻译为准（优先级：个性翻译 > 汇总词典，已有翻译不覆盖、先来后到）
    std::unordered_map<std::string, std::string> customFull, customEn;
    {
        fs::path cp = CustomDictPath(dictDir);
        std::error_code cec3;
        if (fs::exists(cp, cec3) && !cec3) {
            std::string cd;
            if (read_binary_file(cp, cd)) {
                try {
                    json custom = json::parse(clean_utf8(cd));
                    if (custom.is_object()) {
                        for (auto& cit : custom.items()) {
                            if (!cit.value().is_string() || cit.value().get<std::string>().empty()) continue;
                            auto ckp = cit.key().rfind("||");
                            if (ckp == std::string::npos) customEn[cit.key()] = cit.value().get<std::string>();
                            else customFull[cit.key()] = cit.value().get<std::string>();
                        }
                    }
                } catch (...) { LogThread("[错误] 个性翻译.json 解析失败，请检查 JSON 格式"); }
            }
        }
    }

    int merged = 0, customUsed = 0;
    for (auto& sec : { "_options", "_descriptions" }) {
        if (!tj.contains(sec) || !tj[sec].is_object()) continue;
        for (auto& it : tj[sec].items()) {
            if (!it.value().is_string()) continue;
            std::string val = it.value().get<std::string>();
            if (val.empty()) continue;
            // 个性翻译优先：命中则用个性翻译的值（即使汇总词典已有翻译也覆盖）
            std::string customVal;
            auto kp = it.key().rfind("||");
            if (kp == std::string::npos) {
                auto ce = customEn.find(it.key());
                if (ce != customEn.end()) customVal = ce->second;
            } else {
                auto cf = customFull.find(it.key());
                auto ce = customEn.find(it.key().substr(kp + 2));
                if (cf != customFull.end()) customVal = cf->second;
                else if (ce != customEn.end()) customVal = ce->second;
            }
            if (!customVal.empty() && customVal != val) {
                val = customVal;
                customUsed++;
            }
            std::string existing;
            if (dict[sec].contains(it.key()) && dict[sec][it.key()].is_string())
                existing = dict[sec][it.key()].get<std::string>();
            bool writeIt = false;
            if (existing.empty()) writeIt = true;
            else if (!customVal.empty() && existing != customVal) writeIt = true; // 个性翻译覆盖已有
            if (writeIt) {
                dict[sec][it.key()] = val;
                // 同步并入 terms（先来后到，不覆盖 wiki 固定词）
                if (kp != std::string::npos) {
                    std::string en = it.key().substr(kp + 2);
                    std::string zh = val;
                    auto op = zh.find("（");
                    if (op != std::string::npos) zh = zh.substr(0, op);
                    if (!en.empty() && !contains_chinese(en)
                        && !zh.empty() && zh != en
                        && !dict["terms"].contains(en))
                        dict["terms"][en] = zh;
                }
                merged++;
            }
        }
    }
    if (customUsed > 0)
        LogThread("[提示] 个性翻译命中 " + std::to_string(customUsed) + " 条，已以个性翻译为准写入汇总词典");
    write_binary_file(dictPath, dict.dump(2));
    LogThread("导入完成：本次编入唯一词典 " + std::to_string(merged) + " 条，词典位于 "
        + wstring_to_utf8(dictPath.wstring()));
    return true;
    }
    catch (const std::system_error& e) { LogThread(std::string("[错误] 导入翻译时系统异常: ") + e.what()); return false; }
    catch (const std::exception& e) { LogThread(std::string("[错误] 导入翻译时异常: ") + e.what()); return false; }
    catch (...) { LogThread("[错误] 导入翻译时未知异常"); return false; }
}

// ------------------------------------------------------------------
// Wiki 全站导出
// ------------------------------------------------------------------
bool HttpGet(const std::string& url, std::string& out, int timeoutMs = 30000)
{
    out.clear();
    HINTERNET hNet = InternetOpenW(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hNet) return false;
    HINTERNET hUrl = InternetOpenUrlW(hNet, utf8_to_wstring(url).c_str(), nullptr, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hUrl) { InternetCloseHandle(hNet); return false; }
    // 模拟浏览器请求头，提高对 cdn 只读接口/部分 CDN 的通过率
    const wchar_t* hdrs = L"Accept: application/json, text/javascript, */*; q=0.01\r\n"
                          L"Accept-Language: zh-CN,zh;q=0.9\r\n"
                          L"Referer: https://ff14.huijiwiki.com/\r\n"
                          L"Connection: keep-alive\r\n";
    HttpAddRequestHeadersW(hUrl, hdrs, (DWORD)-1L, HTTP_ADDREQ_FLAG_ADD | HTTP_ADDREQ_FLAG_REPLACE);
    char buf[8192];
    DWORD read = 0;
    bool ok = false;
    while (InternetReadFile(hUrl, buf, sizeof(buf), &read) && read > 0) {
        out.append(buf, read);
        ok = true;
    }
    InternetCloseHandle(hUrl);
    InternetCloseHandle(hNet);
    return ok;
}

// ---------- Wiki 术语提取辅助函数 ----------
// 判断是否为可接受的"英文侧"：必须含 ASCII 字母，不含中文，不含 wiki 标记，长度合理
static bool is_valid_english_side(const std::string& raw)
{
    std::string s = raw;
    size_t a = s.find_first_not_of(" \t\r\n"); if (a == std::string::npos) return false;
    size_t b = s.find_last_not_of(" \t\r\n");
    s = s.substr(a, b - a + 1);
    if (s.empty() || s.size() > 80) return false;
    if (contains_chinese(s)) return false; // 英文侧不能含中文
    bool hasLetter = false;
    for (char c : s) if (isalpha((unsigned char)c)) { hasLetter = true; break; }
    if (!hasLetter) return false;
    // 排除含 wiki/HTML 标记或命名参数的值
    if (s.find("[[") != std::string::npos || s.find("]]") != std::string::npos ||
        s.find("{{") != std::string::npos || s.find("}}") != std::string::npos ||
        s.find('<') != std::string::npos || s.find('>') != std::string::npos ||
        s.find('=') != std::string::npos)
        return false;
    return true;
}

// 判断是否为可接受的"中文侧"：必须含中文，不含 wiki/HTML 标记，长度合理
static bool is_valid_chinese_side(const std::string& raw)
{
    std::string s = raw;
    size_t a = s.find_first_not_of(" \t\r\n"); if (a == std::string::npos) return false;
    size_t b = s.find_last_not_of(" \t\r\n");
    s = s.substr(a, b - a + 1);
    if (s.empty() || s.size() > 120) return false;
    if (!contains_chinese(s)) return false;
    if (s.find("[[") != std::string::npos || s.find("]]") != std::string::npos ||
        s.find("{{") != std::string::npos || s.find("}}") != std::string::npos ||
        s.find('<') != std::string::npos || s.find('>') != std::string::npos ||
        s.find('=') != std::string::npos)
        return false;
    return true;
}

// 添加一对术语（英文 -> 中文），按键去重。
// hitSkip 非空时，统计"词条已存在于词典、本次跳过"的次数（用于日志，避免误判爬虫失效）。
// hitList 非空时，把命中的英文原词记入列表（供日志打印具体词条）。
static void add_wiki_term(json& result, int& added, const std::string& enRaw, const std::string& zhRaw, int* hitSkip = nullptr, std::vector<std::string>* hitList = nullptr)
{
    std::string e = enRaw, z = zhRaw;
    size_t ae = e.find_first_not_of(" \t\r\n"); if (ae == std::string::npos) return;
    size_t be = e.find_last_not_of(" \t\r\n");
    size_t az = z.find_first_not_of(" \t\r\n"); if (az == std::string::npos) return;
    size_t bz = z.find_last_not_of(" \t\r\n");
    e = e.substr(ae, be - ae + 1);
    z = z.substr(az, bz - az + 1);
    if (e.empty() || z.empty()) return;
    if (!is_valid_english_side(e)) return;
    if (!is_valid_chinese_side(z)) return;
    if (e == z) return;
    if (!result["terms"].contains(e)) {
        result["terms"][e] = z;
        added++;
    }
    else {
        if (hitSkip) (*hitSkip)++;
        if (hitList) hitList->push_back(e);
    }
}

// 解析 Data:<类型>/<id>.json 数据页，提取 中文名/英文名 对照。
// Item 用「中文名/英文名」，Action/Status/Trait 用「cn/en」；直接取顶层字段即可。
// 返回 0=既无中文也无英文，1=有中文，2=有英文，3=两者都有（用于诊断统计）。
static int parse_data_page(const std::string& type, const std::string& content, json& result, int& added, int* hitSkip = nullptr, std::vector<std::string>* hitList = nullptr)
{
    json o;
    try { o = json::parse(content); }
    catch (...) { return 0; }
    if (!o.is_object()) return 0;

    std::string zh = o.value("中文名", o.value("cn", std::string()));
    std::string en = o.value("英文名", o.value("en", std::string()));
    int flag = (zh.empty() ? 0 : 1) | (en.empty() ? 0 : 2);
    if (flag == 3 && zh != en)
        add_wiki_term(result, added, en, zh, hitSkip, hitList);
    return flag;
}

// 固定词表：玩家种族（含子种族）官方中文名/英文名。
// 数据来源：灰机 wiki「种族」页面（ff14.huijiwiki.com/wiki/种族），
// 英文名为游戏内建 Race/Tribe 标准名。
static void add_race_terms(json& result, int& added, int* hitSkip = nullptr)
{
    struct R { const char* en; const char* zh; };
    static const R races[] = {
        // 玩家可选种族（8 大种族）
        { "Hyur", "人族" },
        { "Elezen", "精灵族" },
        { "Lalafell", "拉拉菲尔族" },
        { "Miqo'te", "猫魅族" },
        { "Roegadyn", "鲁加族" },
        { "Au Ra", "敖龙族" },
        { "AuRa", "敖龙族" },          // MOD 选项中常见的无空格拼写变体
        { "Hrothgar", "硌狮族" },
        { "Viera", "维埃拉族" },
        // 各种族可选子种族（官方 Tribe 标准名）
        { "Midlander", "中原之民" },
        { "Highlander", "高地之民" },
        { "Wildwood", "森岭之民" },
        { "Duskwight", "黑影之民" },
        { "Plainsfolk", "平原之民" },
        { "Dunesfolk", "沙漠之民" },
        { "Sunseeker", "逐日之民" },
        { "Moonkeeper", "护月之民" },
        { "Sea Wolf", "北洋之民" },
        { "Hellsguard", "红焰之民" },
        { "Raen", "晨曦之民" },
        { "Xaela", "暮晖之民" },
        { "Helions", "掠日之民" },
        { "Lost", "迷失之民" },
        { "Veena", "密林之民" },
        { "Rava", "山林之民" },
        // 其他世界对应的种族分支（灰机 wiki 种族名称对照表）
        { "Hume", "尘族" },
        { "Elf", "茕灵族" },
        { "Dwarf", "矮人族" },
        { "Mystel", "猫秘族" },
        { "Galdjent", "迦震族" },
        { "Drahn", "朵龙族" },
        { "Ronso", "隆索族" },
        { "Viis", "维斯族" },
    };
    for (const auto& r : races)
        add_wiki_term(result, added, r.en, r.zh, hitSkip);
}

void WikiImportThread()
{
    try {
    LogThread("开始 Wiki 全站导出（灰机 wiki）...");
    if (g_cfg.dictionaryDir.empty()) { LogThread("[错误] 未设置词典目录"); PostMessageW(g_hMainWnd, WM_APP_DONE, 0, 0); return; }

    fs::path dictDir = fs::u8path(g_cfg.dictionaryDir);
    std::error_code existEc;
    if (!fs::exists(dictDir, existEc)) {
        std::error_code ec;
        fs::create_directories(dictDir, ec);
        if (ec) { LogThread("[错误] 无法创建词典目录"); PostMessageW(g_hMainWnd, WM_APP_DONE, 0, 0); return; }
    }
    // wiki 原典（纯 wiki 导出，只含 terms）；汇总词典由原典 + 个人填充生成
    fs::path wikiPath = dictDir / fs::u8path("wiki_术语对照.json");

    json result;
    bool wikiExists = fs::exists(wikiPath, existEc);
    if (wikiExists) {
        std::string d;
        if (read_binary_file(wikiPath, d)) {
            try { result = json::parse(clean_utf8(d)); }
            catch (...) { result = json::object(); }
        }
    }
    if (!result.is_object()) result = json::object();
    if (!result.contains("terms")) result["terms"] = json::object();
    if (!result["terms"].is_object()) result["terms"] = json::object();
    // 原典不存在时：优先释放内置 wiki 原典作为基线（最完整）；否则以现有汇总词典的 terms 兜底
    if (result["terms"].empty() && !wikiExists) {
        if (ReleaseBuiltinWiki(dictDir)) {
            std::string bd;
            if (read_binary_file(wikiPath, bd)) {
                try {
                    json bj = json::parse(clean_utf8(bd));
                    if (bj.is_object() && bj.contains("terms") && bj["terms"].is_object()) {
                        result["terms"] = bj["terms"];
                        LogThread("[提示] 词典目录无 wiki 原典，已释放内置 wiki_术语对照.json（"
                            + std::to_string(result["terms"].size()) + " 条）作为初始原典");
                    }
                } catch (...) {}
            }
        }
    }
    if (result["terms"].empty()) {
        fs::path mergedPath = MergedDictPath(dictDir);
        std::error_code mec;
        if (fs::exists(mergedPath, mec) && !mec) {
            std::string d;
            if (read_binary_file(mergedPath, d)) {
                try {
                    json mj = json::parse(clean_utf8(d));
                    if (mj.is_object() && mj.contains("terms") && mj["terms"].is_object()) {
                        result["terms"] = mj["terms"];
                        LogThread("[提示] wiki 原典 wiki_术语对照.json 不存在，已从现有汇总词典复制 "
                            + std::to_string(mj["terms"].size()) + " 条术语作为初始原典");
                    }
                } catch (...) {}
            }
        }
    }

    try {
        // 阶段1：批量抓取 Data:Item/、Data:Action/ 等数据页（含中文名/英文名/描述）
        // 用 cdn 只读接口：ff14.huijiwiki.com/api.php 会被 Cloudflare 拦截（返回 HTML 导致解析失败），
        // cdn.huijiwiki.com/ff14/api.php 对爬虫/程序友好，稳定返回完整 JSON（含英文名/日文名等）。
        std::string api = "https://cdn.huijiwiki.com/ff14/api.php?action=query&generator=allpages&format=json&utf8=1&gaplimit=500&prop=revisions&rvprop=content&rvslots=main";
        if (wikiExists && result["terms"].size() > 0) {
            LogThread("[提示] 已加载现有 wiki 原典 wiki_术语对照.json（已有 " + std::to_string(result["terms"].size())
                + " 条术语），本次为增量更新；已存在的词条不会被覆盖");
        }
        int added = 0;
        int pages = 0;
        int hitExisting = 0; // 已存在于词典、本次跳过（增量更新，非故障）
        std::vector<std::string> hitList; // 命中的具体英文词条，供日志展示
        long long cntZh = 0, cntEn = 0, cntBoth = 0;
        uint64_t wikiStartMs = GetTickCount64();
        std::string gapCont, rvCont;
        int dataset = 0;
        bool done = false;
        std::vector<std::string> prefixes = g_wikiPrefixes;
        if (prefixes.empty()) prefixes = { "Item/", "Action/", "Status/", "Trait/" };
        LogThread("[提示] 开始抓取 " + prefixes[dataset] + " ...");

        while (!done) {
            // 用户点击中断按钮：提前结束，但仍保存已抓取结果
            if (g_cancel.load()) {
                LogThread("[提示] 检测到中断请求，提前结束抓取");
                done = true;
                break;
            }

            std::string url = api + "&gapnamespace=3500&gapprefix=" + url_encode(prefixes[dataset]);
            if (!gapCont.empty()) url += "&gapcontinue=" + url_encode(gapCont);
            if (!rvCont.empty()) url += "&rvcontinue=" + url_encode(rvCont);
            std::string resp;
            if (!HttpGet(url, resp)) { LogThread("[错误] 获取数据页失败"); break; }

            json j;
            try { j = json::parse(clean_utf8(resp)); }
            catch (...) { LogThread("[错误] 数据页解析失败"); break; }

            if (j.contains("error")) {
                LogThread("[错误] API: " + j["error"].value("info", std::string()));
                break;
            }

            std::string type = prefixes[dataset];
            type.pop_back(); // 去掉尾 "/"，得到 "Item"/"Action"

            if (j.contains("query") && j["query"].contains("pages")) {
                for (auto& kv : j["query"]["pages"].items()) {
                    auto& p = kv.value();
                    if (!p.is_object() || !p.contains("revisions") || p["revisions"].empty()) continue;
                    if (!p["revisions"][0].is_object()) continue;
                    std::string content;
                    if (p["revisions"][0].contains("slots") && p["revisions"][0]["slots"].contains("main"))
                        content = p["revisions"][0]["slots"]["main"].value("*", std::string());
                    else
                        content = p["revisions"][0].value("*", std::string());
                    if (content.empty()) continue;
                    int flag = parse_data_page(type, content, result, added, &hitExisting, &hitList);
                    if (flag & 1) cntZh++;
                    if (flag & 2) cntEn++;
                    if (flag == 3) cntBoth++;
                    pages++;
                }
            }

            // 每 100 页更新进度（附带用时与速度；页面总数未知，无法精确预估剩余时间）
            if (pages > 0 && pages % 100 == 0) {
                uint64_t elapsed = GetTickCount64() - wikiStartMs;
                long long secs = (long long)(elapsed / 1000);
                int speed = (int)(elapsed > 0 ? (double)pages * 1000.0 / elapsed : 0);
                LogThread("[进度] " + type + " 已处理 " + std::to_string(pages) + " 页，新增术语 " + std::to_string(added)
                    + "（命中已有 " + std::to_string(hitExisting) + "，跳过），用时 "
                    + std::to_string(secs / 60) + "分" + std::to_string(secs % 60) + "秒"
                    + "，约 " + std::to_string(speed) + " 页/秒");
            }
            SetProgress(pages, -1); // 页面总数未知，用不确定进度

            // 分页继续：MediaWiki generator 模式下，revisions 每批只返回 50 个，
            // 必须用 rvcontinue 把当前这批 500 个 allpages 的 revisions 全部拿完，
            // 才能再用 gapcontinue 进入下一批 allpages。否则每批 500 页只拿到 50 页。
            bool hasContinue = false;
            if (j.contains("continue")) {
                auto& c = j["continue"];
                // 优先用 rvcontinue 完成当前批的 revisions 分页
                if (c.is_object() && c.contains("rvcontinue")) {
                    rvCont = c["rvcontinue"].get<std::string>();
                    // 保留当前 gapCont 不变，继续拿同一批 allpages 的剩余 revisions
                    hasContinue = true;
                }
                else {
                    rvCont.clear();
                    // 当前批 revisions 已拿完，改用 gapcontinue 进入下一批 allpages
                    if (c.is_object() && c.contains("gapcontinue")) {
                        gapCont = c["gapcontinue"].get<std::string>();
                        hasContinue = true;
                    }
                    else gapCont.clear();
                }
            }
            if (!hasContinue) { gapCont.clear(); rvCont.clear(); }

            // 当前数据集翻页到底，切下一个前缀
            if (!hasContinue) {
                dataset++;
                if (dataset < (int)prefixes.size()) {
                    gapCont.clear(); rvCont.clear();
                    LogThread("[提示] 开始抓取 " + prefixes[dataset] + " ...");
                }
                else {
                    done = true;
                }
            }
        }

        // 阶段2：并入固定种族/子种族词表
        add_race_terms(result, added, &hitExisting);
        LogThread("[提示] 已并入种族/子种族词表，本次新增术语 " + std::to_string(added)
            + " 条，其中命中已有 " + std::to_string(hitExisting) + " 条（跳过）");

        // 写回 wiki 原典（纯净结构：仅 terms），再同步进汇总词典
        json wikiOut;
        wikiOut["terms"] = result["terms"];
        write_binary_file(wikiPath, wikiOut.dump(2));
        SyncWikiToMerged(dictDir);
        // 诊断：报告数据页中中文名/英文名的真实覆盖情况，帮助判断抓取是否正常。
        // 「命中已有」说明词条早已在词典中（增量更新），并非爬虫失效。
        LogThread("[提示] 诊断：共解析 " + std::to_string(pages) + " 个数据页，其中含中文名 " + std::to_string(cntZh)
            + " 页、含英文名 " + std::to_string(cntEn) + " 页、中文+英文都齐全 " + std::to_string(cntBoth)
            + " 页；本次新增术语 " + std::to_string(added) + " 条，命中已有词条 " + std::to_string(hitExisting) + " 条（跳过）");
        if (!hitList.empty()) {
            // 去重后展示：同一词条会在多个数据页/数据集重复命中，避免刷屏
            std::vector<std::string> uniq;
            std::set<std::string> seen;
            for (auto& t : hitList)
                if (seen.insert(t).second) uniq.push_back(t);
            std::string detail = "[提示] 命中已有的词条（去重后 " + std::to_string(uniq.size())
                + " 个，原始命中 " + std::to_string(hitExisting) + " 次）：";
            size_t show = std::min<size_t>(uniq.size(), 20);
            for (size_t i = 0; i < show; ++i) {
                if (i) detail += "、";
                detail += uniq[i];
            }
            if (uniq.size() > show) detail += " 等";
            LogThread(detail);
            PrependLogToFile(utf8_to_wstring(detail + "\r\n")); // 置顶到日志.json 文件最前
        }
        uint64_t wikiTotalMs = GetTickCount64() - wikiStartMs;
        long long wSecs = (long long)(wikiTotalMs / 1000);
        std::string wTime = std::to_string(wSecs / 60) + "分" + std::to_string(wSecs % 60) + "秒";
        if (g_cancel.load()) {
            LogThread("[提示] Wiki 导出已中断：共处理 " + std::to_string(pages) + " 页，新增术语 " + std::to_string(added)
                + " 条，命中已有 " + std::to_string(hitExisting) + " 条（跳过），总用时 " + wTime + "，原典已保存");
        }
        else {
            LogThread("[完成] Wiki 导出结束：共处理 " + std::to_string(pages) + " 页，新增术语 " + std::to_string(added)
                + " 条，命中已有 " + std::to_string(hitExisting) + " 条（跳过），总用时 " + wTime
                + "，结果已写入 wiki 原典 wiki_术语对照.json 并同步进汇总词典");
        }
        LogThread("[提示] 原典：wiki_术语对照.json（纯 wiki 词条，防止汇总词典出错时丢失）；"
            "程序实际使用：汇总词典.json（原典 + 个人填充 + 个性翻译.json 覆盖）—— 个人词条请直接编辑汇总词典，单独修正的词汇可写 个性翻译.json");
    }
    catch (const std::exception& e) {
        LogThread("[错误] Wiki 导出异常：" + std::string(e.what()));
    }
    catch (...) {
        LogThread("[错误] Wiki 导出异常");
    }
    PostMessageW(g_hMainWnd, WM_APP_DONE, 0, 0);
    }
    catch (const std::system_error& e) { LogThread(std::string("[错误] Wiki 导出系统异常: ") + e.what()); PostMessageW(g_hMainWnd, WM_APP_DONE, 0, 0); }
    catch (const std::exception& e) { LogThread(std::string("[错误] Wiki 导出异常: ") + e.what()); PostMessageW(g_hMainWnd, WM_APP_DONE, 0, 0); }
    catch (...) { LogThread("[错误] Wiki 导出未知异常"); PostMessageW(g_hMainWnd, WM_APP_DONE, 0, 0); }
}

// ------------------------------------------------------------------
// 工作线程包装
// ------------------------------------------------------------------
void RunExtractThread()
{
    try {
        g_busy = true;
        bool ok = ExtractEnglish(g_extractFiles.empty() ? nullptr : &g_extractFiles);
        Log("提取流程结束");
        g_busy = false;
    }
    catch (const std::system_error& e) { Log(std::string("[错误] 提取线程系统异常: ") + e.what()); }
    catch (const std::exception& e) { Log(std::string("[错误] 提取线程异常: ") + e.what()); }
    catch (...) { Log("[错误] 提取线程未知异常"); }
    PostMessageW(g_hMainWnd, WM_APP_DONE, 0, 0);
}

void RunImportThread()
{
    try {
        g_busy = true;
        for (const auto& f : g_importFiles)
            ImportTranslations(f, g_importAutoFill);
        Log("导入翻译流程结束");
        g_busy = false;
    }
    catch (const std::system_error& e) { Log(std::string("[错误] 导入翻译线程系统异常: ") + e.what()); }
    catch (const std::exception& e) { Log(std::string("[错误] 导入翻译线程异常: ") + e.what()); }
    catch (...) { Log("[错误] 导入翻译线程未知异常"); }
    PostMessageW(g_hMainWnd, WM_APP_DONE, 0, 0);
}

void RunApplyThread()
{
    try {
        g_busy = true;
        bool ok = ApplyTranslation();
        Log("应用流程结束");
        g_busy = false;
    }
    catch (const std::system_error& e) { Log(std::string("[错误] 应用线程系统异常: ") + e.what()); }
    catch (const std::exception& e) { Log(std::string("[错误] 应用线程异常: ") + e.what()); }
    catch (...) { Log("[错误] 应用线程未知异常"); }
    PostMessageW(g_hMainWnd, WM_APP_DONE, 0, 0);
}

void RunCheckThread()
{
    try {
        g_busy = true;
        bool ok = CheckTranslation();
        Log("检查翻译结束");
        g_busy = false;
    }
    catch (const std::system_error& e) { Log(std::string("[错误] 检查翻译线程系统异常: ") + e.what()); }
    catch (const std::exception& e) { Log(std::string("[错误] 检查翻译线程异常: ") + e.what()); }
    catch (...) { Log("[错误] 检查翻译线程未知异常"); }
    PostMessageW(g_hMainWnd, WM_APP_DONE, 0, 0);
}

void LaunchWorker(void (*fn)(), const std::string& name)
{
    try {
        g_workerName = name;
        std::thread(fn).detach();
    }
    catch (const std::system_error& e) {
        Log("[错误] " + name + " 线程启动失败: " + e.what());
        g_busy = false;
        PostMessageW(g_hMainWnd, WM_APP_DONE, 0, 0);
    }
    catch (const std::exception& e) {
        Log("[错误] " + name + " 线程启动失败: " + e.what());
        g_busy = false;
        PostMessageW(g_hMainWnd, WM_APP_DONE, 0, 0);
    }
    catch (...) {
        Log("[错误] " + name + " 线程启动失败");
        g_busy = false;
        PostMessageW(g_hMainWnd, WM_APP_DONE, 0, 0);
    }
}

// ------------------------------------------------------------------
// AI 自动翻译（DeepSeek 等 OpenAI 兼容接口）
// ------------------------------------------------------------------

// 用 WinINet 发送 HTTPS POST 请求，返回响应体。成功返回 true。
static bool HttpPostJson(const std::string& url, const std::string& apiKey,
                         const std::string& body, std::string& outBody,
                         std::string& errMsg)
{
    bool secure = (url.rfind("https://", 0) == 0);
    std::string rest = url;
    if (secure) rest = rest.substr(8);
    else if (url.rfind("http://", 0) == 0) rest = rest.substr(7);

    std::string host = rest, path = "/";
    auto slash = rest.find('/');
    if (slash != std::string::npos) {
        host = rest.substr(0, slash);
        path = rest.substr(slash);
        if (path.empty()) path = "/";
    }
    INTERNET_PORT port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    std::string hostOnly = host;
    auto colon = host.find(':');
    if (colon != std::string::npos) {
        hostOnly = host.substr(0, colon);
        port = (INTERNET_PORT)std::atoi(host.substr(colon + 1).c_str());
        if (port == 0) port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    }

    HINTERNET hNet = InternetOpenW(L"FFXIV-Mod-Translation-Tool/1.0",
        INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hNet) { errMsg = "InternetOpen 失败: " + std::to_string(GetLastError()); return false; }

    HINTERNET hConn = InternetConnectW(hNet, utf8_to_wstring(hostOnly).c_str(), port,
        nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConn) {
        errMsg = "InternetConnect 失败: " + std::to_string(GetLastError());
        InternetCloseHandle(hNet);
        return false;
    }

    std::wstring headers = L"Content-Type: application/json\r\nAuthorization: Bearer "
        + utf8_to_wstring(apiKey) + L"\r\n";
    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE;
    if (secure) flags |= INTERNET_FLAG_SECURE;
    HINTERNET hReq = HttpOpenRequestW(hConn, L"POST", utf8_to_wstring(path).c_str(),
        nullptr, nullptr, nullptr, flags, 0);
    if (!hReq) {
        errMsg = "HttpOpenRequest 失败: " + std::to_string(GetLastError());
        InternetCloseHandle(hConn); InternetCloseHandle(hNet);
        return false;
    }
    g_hActiveReq = hReq; // 登记活动请求句柄，供取消按钮立即打断

    DWORD timeout = 180000; // 3 分钟超时（取消时会被立即打断，不必等满）
    InternetSetOptionW(hReq, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionW(hReq, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));

    BOOL ok = HttpSendRequestW(hReq, headers.c_str(), (DWORD)-1L,
        (LPVOID)body.data(), (DWORD)body.size());
    if (!ok) {
        errMsg = "网络请求失败: " + std::to_string(GetLastError());
        g_hActiveReq = nullptr;
        if (!g_cancel) InternetCloseHandle(hReq); // 被取消时该句柄已由取消线程关闭
        InternetCloseHandle(hConn); InternetCloseHandle(hNet);
        return false;
    }

    DWORD status = 0, statusLen = sizeof(status);
    if (HttpQueryInfoW(hReq, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
        &status, &statusLen, nullptr)) {
        if (status != 200) {
            char ebuf[4096]; DWORD rd = 0; std::string resp;
            while (InternetReadFile(hReq, ebuf, sizeof(ebuf), &rd) && rd > 0) resp.append(ebuf, rd);
            errMsg = "HTTP " + std::to_string(status) + (resp.empty() ? "" : ": " + resp);
            g_hActiveReq = nullptr;
            if (!g_cancel) InternetCloseHandle(hReq);
            InternetCloseHandle(hConn); InternetCloseHandle(hNet);
            return false;
        }
    }

    char buf[16384]; DWORD rd = 0; std::string resp;
    while (InternetReadFile(hReq, buf, sizeof(buf), &rd) && rd > 0) {
        resp.append(buf, rd);
        if (resp.size() > 64 * 1024 * 1024) {
            errMsg = "响应过大";
            g_hActiveReq = nullptr;
            if (!g_cancel) InternetCloseHandle(hReq);
            InternetCloseHandle(hConn); InternetCloseHandle(hNet);
            return false;
        }
    }
    g_hActiveReq = nullptr;
    outBody = resp;
    InternetCloseHandle(hReq); InternetCloseHandle(hConn); InternetCloseHandle(hNet);
    return true;
}

// 严格解析一个字符串为 JSON 对象
static bool tryParseRaw(const std::string& s, json& out)
{
    try { out = json::parse(s); return out.is_object(); } catch (...) { return false; }
}

// 从 AI 回复文本中解析出 JSON 对象（容忍 ```json 代码块、前后说明文字、尾逗号）
static bool ParseAIJson(const std::string& content, json& out)
{
    auto tryParse = [&](const std::string& s) -> bool {
        if (tryParseRaw(s, out)) return true;
        // 修复 AI 常见错误：多余的尾逗号（",}" / ",]"），再试一次
        std::string t = s;
        for (int pass = 0; pass < 4; ++pass) {
            bool changed = false;
            size_t pos;
            while ((pos = t.find(",}")) != std::string::npos) { t.replace(pos, 2, 1, '}'); changed = true; }
            while ((pos = t.find(",]")) != std::string::npos) { t.replace(pos, 2, 1, ']'); changed = true; }
            if (!changed) break;
        }
        return tryParseRaw(t, out);
    };
    if (tryParse(content)) return true;
    auto f1 = content.find("```");
    if (f1 != std::string::npos) {
        auto f2 = content.find("```", f1 + 3);
        if (f2 != std::string::npos) {
            std::string inner = content.substr(f1 + 3, f2 - f1 - 3);
            auto b = inner.find('{'); auto e = inner.rfind('}');
            if (b != std::string::npos && e != std::string::npos && e > b) {
                if (tryParse(inner.substr(b, e - b + 1))) return true;
            }
        }
    }
    auto b = content.find('{'); auto e = content.rfind('}');
    if (b != std::string::npos && e != std::string::npos && e > b) {
        if (tryParse(content.substr(b, e - b + 1))) return true;
    }
    return false;
}

// 判断 term 是否以词边界（空格/连字符/下划线/字符串首尾）整体出现在 text 中
static bool termInText(const std::string& text, const std::string& term)
{
    if (term.empty() || text.empty()) return false;
    if (text == term) return true;
    size_t p = 0;
    while ((p = text.find(term, p)) != std::string::npos) {
        bool lb = (p == 0) || (text[p - 1] == ' ' || text[p - 1] == '-' || text[p - 1] == '_');
        size_t ep = p + term.size();
        bool rb = (ep == text.size()) || (text[ep] == ' ' || text[ep] == '-' || text[ep] == '_');
        if (lb && rb) return true;
        p += term.size();
    }
    return false;
}

// 从 AI 译文（「中文（英文）」/ 纯中文 / 其他）提取中文部分，作为会话术语保存
static std::string extractChineseTerm(const std::string& trans)
{
    if (trans.empty() || !contains_chinese(trans)) return "";
    size_t lp = trans.find_first_of("（(");
    if (lp != std::string::npos) {
        std::string zh = trans.substr(0, lp);
        while (!zh.empty() && (zh.back() == ' ' || zh.back() == '\t')) zh.pop_back();
        if (!zh.empty() && contains_chinese(zh)) return zh;
    }
    return trans;
}

// 安全序列化：strict dump 遇到非法 UTF-8 会抛 type_error.316，这里用 replace 模式兜底，
// 把非法字节替换为 U+FFFD，避免异常导致 AI 翻译线程崩溃。
static std::string safeDump(const json& j, int indent = -1)
{
    try {
        return j.dump(indent);
    }
    catch (...) {
        try { return j.dump(indent, ' ', false, json::error_handler_t::replace); }
        catch (...) { return "{}"; }
    }
}

// 修正 AI 输出的「英文（英文）」无效格式：括号内外完全一致且不含中文时，回退为原英文
static std::string fixRepeatParen(const std::string& t)
{
    auto trim = [](std::string s) {
        size_t x = s.find_first_not_of(" \t\r\n");
        size_t y = s.find_last_not_of(" \t\r\n");
        return (x == std::string::npos) ? std::string() : s.substr(x, y - x + 1);
    };
    size_t open = t.find("（");
    if (open == std::string::npos) return t;
    size_t close = t.rfind("）");
    // 全角括号 UTF-8 占 3 字节，必须按「起始字节 + 3」判断右括号是否在末尾
    if (close == std::string::npos || close + 3 != t.size()) return t;
    std::string a = trim(t.substr(0, open));
    std::string b = trim(t.substr(open + 3, close - open - 3));
    if (a.empty() || b.empty() || a != b) return t;
    if (contains_chinese(a)) return t; // 中文（中文）例外情况不处理
    return a;                          // 英文（英文）→ 英文
}

// 单条 AI 翻译条目：id（批次内序号）+ 原文 text + 来源上下文 context
// context 附带模组路径/文件名/同组选项，帮助 AI 理解词义（如 Masc 是体型专名而非"男性化"）
struct AIItem { std::string id, text, context; };

// 调用一次 AI 翻译一批词条，返回 条目 id -> 译文 的映射
// glossary：会话内已确定的术语对照（英文 -> 固定中文译名），注入 prompt 保证同词同译
static bool AITranslateBatch(const std::vector<AIItem>& items,
                             std::map<std::string, std::string>& outMap, std::string& errMsg,
                             const std::vector<std::pair<std::string, std::string>>& glossary)
{
    if (items.empty()) return true;
    if (g_cfg.aiModel.empty()) { errMsg = "未填写模型名（AI 翻译设置）"; return false; }
    if (g_cfg.aiBaseUrl.empty()) { errMsg = "未填写 API 地址（AI 翻译设置）"; return false; }
    std::string sysMsg =
        "你是《最终幻想14》(FFXIV) 模组本地化的专业译者，把英文模组文本翻译成简体中文。\n"
        "硬性要求：\n"
        "1. 翻译范围：_descriptions（描述）与 _options（选项）一视同仁，都必须翻译。\n"
        "2. 短名称（选项名/组名）格式统一为「中文（英文）」，例如 治疗（Cure）；括号一律用全角（），括号内英文为原名的本体英文（最简形式），必须原样保留、禁止翻译括号内的英文，也不得省略。\n"
        "3. 长描述（Description）直接翻译成自然通顺的中文句子，不使用括号格式，也不得保留英文原文；仅当描述本身是单个术语或短语时才用「中文（英文）」。\n"
        "4. 禁止半翻译：每条文本必须整体完整翻译，绝对禁止输出「中文 + 残留英文」的混合半成品（如「隐遁 short boots」「Medium 钻石 Patch」均为错误）；不确定的词按上下文推断通用含义翻译，不许跳过。\n"
        "5. 禁止双语拼接：绝对禁止输出「英文 / 中文」或「英文 换行 中文」这类把原文与译文并排的文本。\n"
        "6. 禁止嵌套与重复：原名形如「└─ animation (cure&haelan(pvp) / 治疗&治愈(pvp))」时，忽略「└─」「├─」等层级装饰符，把整串名字作为一个整体翻译成一对「中文（英文）」；括号内只放一个最简英文，禁止输出「中文（半截原文）（另半截原文）」或重复原名。示例：应输出 角色动作（Animation），而不是 角色动作 (Cure&Haelan(PVP) / 治疗&治愈(PVP))（└─ Animation）。\n"
        "7. 括号规则：能译出中文的短名称用「中文（英文）」格式，例如 治疗（Cure）、无（None）；若某词确为无法翻译的专有名词/缩写/品牌名（如 EXQB、Uranus），则原样保留英文本身，禁止输出「英文（英文）」（禁止 EXQB（EXQB）这类重复）。\n"
        "8. 形如「XXX - YYY」的英文（如 Connectors - Face）是普通选项名，应视为整体翻译，不得保留原文。\n"
        "9. 原文拼写错误（如 devine caress、care ii）按正确词义理解翻译，括号内英文保留原文拼写。\n"
        "10. 专有名词：人名、品牌名、作者名（如 Yiggle、Lavabod、YAB）可原样保留在译文中，作为专名的一部分，不强求翻译。\n"
        "11. 遇到不确定含义的词汇，根据上下文推断其通用含义进行翻译，译文仍按第 2 条格式保留「中文（英文）」，括号内为原文英文。\n"
        "12. 3D 建模/图形常用语按通用译法翻译：poly 是 polygon（多边形）的缩写，higher poly 指模型面数更高，译为「高多边形」；texture=纹理、mesh=网格、rig=骨骼绑定、LOD=细节层级。例如 Fuzzy layer (higher poly) 应译为 毛绒层（高多边形），不得按字面硬译或编造译名。\n"
        "13. 无歧义固定词：身体部位等没有歧义（多义词）的固定名词必须按通用中文直译，不得保留英文（如 Feet=脚部、Legs=腿部、Hands=手部、Chest=胸部、Belly=腹部、Thighs=大腿、Back=背部、Arms=手臂、Shoulders=肩部）。\n"
        "14. 只输出一个 JSON 对象：键为条目 id（字符串），值为译文。不要输出任何其他内容。\n"
        "15. 上下文参考：若条目附带 context 字段，它是该条目的来源上下文（所属模组、文件名、同组其他选项），"
        "仅用于帮助你判断词义（如 Masc 出现在体型选项组中多为 mod 自造体型名）；只翻译 text 字段，禁止翻译、复述或输出 context 的内容。\n"
        "16. 同一选项组（同一文件）内的选项通常属于同一维度（体型/尺寸/颜色/材质等），译文应保持统一语义；"
        "无法确定通用含义的 mod 自造词/作者专名（如 Masc、Lava、Rue、Lavabod、Yanilla、EXQB、Uranus）优先原样保留英文，不要硬译或编造。";
    if (!glossary.empty()) {
        sysMsg +=
            "\n17. 术语一致性：以下为本次翻译已确定的固定术语对照（英文 → 中文）。"
            "待翻译文本中出现这些英文词/短语时，必须原样套用固定中文译名，禁止另译或换说法：\n";
        for (const auto& g : glossary) sysMsg += "    " + g.first + " → " + g.second + "\n";
    }
    json arr = json::array();
    for (auto& it : items) {
        json obj;
        obj["id"] = it.id;
        obj["text"] = it.text;
        if (!it.context.empty()) obj["context"] = it.context;
        arr.push_back(std::move(obj));
    }
    std::string userMsg = "请翻译以下 FFXIV 模组文本条目，输出 JSON 对象：\n" + safeDump(arr);

    json req;
    req["model"] = g_cfg.aiModel;
    req["messages"] = json::array();
    req["messages"].push_back({ {"role","system"}, {"content", sysMsg} });
    req["messages"].push_back({ {"role","user"}, {"content", userMsg} });
    req["stream"] = false;
    req["temperature"] = 0.3;
    req["max_tokens"] = 16384; // 推理模型（如 glm-4.7）思维链会占大量 token，给 JSON 输出留足空间
    std::string body = safeDump(req);

    std::string url = g_cfg.aiBaseUrl;
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (url.find("/chat/completions") == std::string::npos) url += "/chat/completions";

    std::string respBody, err;
    if (!HttpPostJson(url, g_cfg.aiApiKey, body, respBody, err)) { errMsg = err; return false; }

    json resp;
    try { resp = json::parse(clean_utf8(respBody)); }
    catch (...) { errMsg = "响应 JSON 解析失败: " + respBody.substr(0, 500); return false; }
    if (!resp.contains("choices") || resp["choices"].empty() ||
        !resp["choices"][0].contains("message") ||
        !resp["choices"][0]["message"].contains("content")) {
        errMsg = "响应缺少 choices/message/content: " + respBody.substr(0, 500);
        return false;
    }
    const auto& aiMsg = resp["choices"][0]["message"];
    std::string content = aiMsg["content"].get<std::string>();
    // 推理模型（如 glm thinking 模式）可能把答案放进 reasoning_content 而 content 为空
    if (content.empty() && aiMsg.contains("reasoning_content") && aiMsg["reasoning_content"].is_string())
        content = aiMsg["reasoning_content"].get<std::string>();
    json result;
    if (!ParseAIJson(content, result)) {
        std::string hint = content.empty()
            ? "（返回内容为空，模型可能处于思考模式）"
            : "（长度 " + std::to_string(content.size()) + "）";
        errMsg = "AI 未返回有效 JSON" + hint + ": " + content.substr(0, 500);
        return false;
    }
    for (auto& it : items) {
        auto found = result.find(it.id);
        if (found != result.end() && found.value().is_string()) {
            std::string v = found.value().get<std::string>();
            if (!v.empty()) outMap[it.id] = v;
        }
    }
    return true;
}

// AI 翻译一个 *_未翻译.json → *_已翻译.json
static bool AITranslateFile(const fs::path& inFile)
{
    if (g_cfg.aiApiKey.empty()) {
        LogThread("[错误] 未设置 AI API_Key，请先点『AI 设置』配置");
        return false;
    }
    std::string data;
    if (!read_binary_file(inFile, data)) {
        LogThread("[错误] 无法读取 " + wstring_to_utf8(inFile.filename().wstring()));
        return false;
    }
    json tj;
    try { tj = json::parse(clean_utf8(data)); }
    catch (...) {
        LogThread("[错误] JSON 解析失败: " + wstring_to_utf8(inFile.filename().wstring()));
        return false;
    }
    if (!tj.is_object()) { LogThread("[错误] JSON 不是对象"); return false; }

    // 收集待翻译条目（值为空）；key 格式：模组路径||字段||英文原文
    struct Item { std::string sec; std::string key; std::string english; };
    std::vector<Item> pending;
    int alreadyFilled = 0;
    for (auto& sec : { "_options", "_descriptions" }) {
        if (!tj.contains(sec) || !tj[sec].is_object()) continue;
        for (auto& it : tj[sec].items()) {
            if (!it.value().is_string()) continue;
            std::string val = it.value().get<std::string>();
            if (!val.empty()) { alreadyFilled++; continue; }
            std::string english = it.key();
            auto p = english.rfind("||");
            if (p != std::string::npos) english = english.substr(p + 2);
            pending.push_back({ sec, it.key(), english });
        }
    }
    if (pending.empty()) {
        LogThread("[提示] 该文件所有条目已有翻译，无需 AI 翻译");
        return false;
    }
    LogThread("待翻译条目：" + std::to_string(pending.size())
        + " 条（其中 " + std::to_string(alreadyFilled) + " 条已有翻译跳过）");

    // 会话内术语记忆：英文原文 -> 固定中文译名（来自词典预填 + AI 已翻条目），保证同词同译
    std::unordered_map<std::string, std::string> sessionTerms;

    // ① 黑名单跳过：命中黑名单的英文原文不翻译，原样保留英文（优先级：黑名单 > 个性翻译 > 词典）
    int blackSkipped = 0;
    {
        std::vector<std::string> blacklist = LoadBlacklistFile(g_cfg.blacklist);
        if (!blacklist.empty()) {
            std::vector<Item> rest;
            for (auto& it : pending) {
                if (is_blacklisted(it.english, blacklist)) {
                    tj[it.sec][it.key] = it.english;
                    blackSkipped++;
                }
                else rest.push_back(it);
            }
            pending = std::move(rest);
        }
    }
    if (blackSkipped > 0)
        LogThread("[提示] 黑名单命中 " + std::to_string(blackSkipped) + " 条，原样保留英文跳过翻译");

    // ② 个性翻译.json 预填：用户手工指定译法（英文:中文 或 完整key:中文），优先于词典
    int customFilled = 0;
    if (!g_cfg.dictionaryDir.empty()) {
        fs::path cp = CustomDictPath(fs::u8path(g_cfg.dictionaryDir));
        std::error_code ccec;
        if (fs::exists(cp, ccec) && !ccec) {
            std::string cd;
            if (read_binary_file(cp, cd)) {
                try {
                    json custom = json::parse(clean_utf8(cd));
                    if (custom.is_object() && !custom.empty()) {
                        std::vector<Item> rest;
                        for (auto& it : pending) {
                            std::string cv;
                            if (custom.contains(it.key) && custom[it.key].is_string())
                                cv = custom[it.key].get<std::string>();
                            else {
                                auto kp = it.key.rfind("||");
                                if (kp != std::string::npos) {
                                    std::string en = it.key.substr(kp + 2);
                                    if (custom.contains(en) && custom[en].is_string())
                                        cv = custom[en].get<std::string>();
                                }
                            }
                            if (cv.empty()) { rest.push_back(it); continue; }
                            tj[it.sec][it.key] = cv;
                            customFilled++;
                            std::string zh = extractChineseTerm(cv);
                            if (!zh.empty() && zh != it.english) sessionTerms[it.english] = zh;
                        }
                        pending = std::move(rest);
                    }
                } catch (...) { LogThread("[错误] 个性翻译.json 解析失败，请检查 JSON 格式"); }
            }
        }
    }
    if (customFilled > 0)
        LogThread("[提示] 个性翻译命中 " + std::to_string(customFilled) + " 条，直接填充");

    // ③ 翻译失败.json 重翻成功后：把成功条目合并进最新 *_已翻译.json（已有非空不覆盖）
    auto mergeFailIntoTranslated = [&]() -> int {
        int mergedN = 0;
        std::wstring fn = inFile.filename().wstring();
        if (fn.find(L"翻译失败") == std::wstring::npos) return 0;
        if (g_cfg.translationDir.empty()) return 0;
        fs::path tdir = fs::u8path(g_cfg.translationDir);
        fs::path latest;
        long long bestT = -1;
        try {
            for (fs::directory_iterator dit(tdir, fs::directory_options::skip_permission_denied); dit != fs::directory_iterator(); ++dit) {
                std::error_code ec2;
                auto& de = *dit;
                if (!de.is_regular_file(ec2) || ec2) continue;
                auto fname = de.path().filename().wstring();
                if (fname.size() <= 5 || fname.substr(fname.size() - 5) != L".json") continue;
                if (fname.find(L"_已翻译") == std::wstring::npos) continue;
                auto t = fs::last_write_time(de.path(), ec2);
                if (ec2) continue;
                long long ts = t.time_since_epoch().count();
                if (ts > bestT) { bestT = ts; latest = de.path(); }
            }
        } catch (const std::system_error&) {}
        if (latest.empty()) return 0;
        std::string ld;
        if (!read_binary_file(latest, ld)) return 0;
        json ej;
        try { ej = json::parse(clean_utf8(ld)); } catch (...) { return 0; }
        if (!ej.is_object()) return 0;
        for (auto& sec : { "_options", "_descriptions" }) {
            if (!tj.contains(sec) || !tj[sec].is_object()) continue;
            for (auto& it : tj[sec].items()) {
                if (!it.value().is_string() || it.value().get<std::string>().empty()) continue;
                if (!ej.contains(sec) || !ej[sec].is_object()) ej[sec] = json::object();
                if (ej[sec].contains(it.key()) && ej[sec][it.key()].is_string()
                    && !ej[sec][it.key()].get<std::string>().empty()) continue; // 已有非空不覆盖
                ej[sec][it.key()] = it.value().get<std::string>();
                mergedN++;
            }
        }
        if (mergedN > 0 && write_binary_file(latest, safeDump(ej, 2)))
            LogThread("[提示] 翻译失败.json 重翻成功，已把 " + std::to_string(mergedN)
                + " 条新增/补全进 " + wstring_to_utf8(latest.filename().wstring()));
        return mergedN;
    };

    // 唯一词典预填：词典中已翻译的 key 直接填充，不再浪费 AI 额度
    int dictFilled = 0;
    if (!g_cfg.dictionaryDir.empty()) {
        fs::path dictPath = MergedDictPath(fs::u8path(g_cfg.dictionaryDir));
        std::error_code dec;
        if (fs::exists(dictPath, dec) && !dec) {
            std::string dd;
            if (read_binary_file(dictPath, dd)) {
                try {
                    json dict = json::parse(clean_utf8(dd));
                    if (dict.is_object()) {
                        std::vector<Item> rest;
                        for (auto& it : pending) {
                            bool hit = false;
                            if (dict.contains(it.sec) && dict[it.sec].is_object()
                                && dict[it.sec].contains(it.key) && dict[it.sec][it.key].is_string()
                                && !dict[it.sec][it.key].get<std::string>().empty()) {
                                std::string zv = dict[it.sec][it.key].get<std::string>();
                                tj[it.sec][it.key] = dictFillDisplay(it.key, zv, it.english);
                                dictFilled++; hit = true;
                                std::string zh = extractChineseTerm(zv);
                                if (!zh.empty() && zh != it.english) sessionTerms[it.english] = zh;
                            }
                            if (!hit) rest.push_back(it);
                        }
                        pending = std::move(rest);
                    }
                } catch (...) {}
            }
        }
        // 纯词条术语（wiki 术语表/个人词条）预填：按英文原文查术语映射，
        // 命中则直接填词典译名（纯中文），避免 AI 不知道术语乱翻（如 Tycoon Bootlets）
        if (!pending.empty()) {
            std::unordered_map<std::string, std::string> termMap;
            size_t maxTermLen = 0;
            if (LoadTermMap(termMap, maxTermLen)) {
                std::vector<Item> rest;
                for (auto& it : pending) {
                    std::string tr = TranslateText(it.english, termMap, maxTermLen);
                    // 有英文句子残留（半翻译）时不给 AI 省额度，继续交给 AI 补全整句；
                    // tr==原文的"保英文"词条（如 Lava→Lava）视为命中，保留英文原样
                    if (!tr.empty() && !hasEnglishWordResidue(tr)) {
                        tj[it.sec][it.key] = dictFillDisplay(it.key, tr, it.english);
                        dictFilled++;
                        if (tr != it.english) sessionTerms[it.english] = tr;
                    }
                    else rest.push_back(it);
                }
                pending = std::move(rest);
            }
        }
    }
    if (dictFilled > 0)
        LogThread("[提示] 唯一词典命中 " + std::to_string(dictFilled) + " 条，直接填充");
    if (pending.empty()) {
        LogThread("[提示] 所有条目已由词典/个性翻译填充，无需调用 AI");
        // 若输入是翻译失败.json：成功条目合并进 *_已翻译.json 后清理残留清单
        std::wstring fn = inFile.filename().wstring();
        if (fn.find(L"翻译失败") != std::wstring::npos) {
            mergeFailIntoTranslated();
            std::error_code dec;
            fs::remove(inFile, dec);
            LogThread("[提示] 翻译失败.json 已全部填充完成，已自动删除");
        }
        return false;
    }

    // 来源上下文映射：完整 key -> "来源: <模组路径/文件名>（选项/描述） 同组选项: ..."
    // 让 AI 知道每条的出处与同类选项，便于正确理解词义（如 Masc 是体型专名而非"男性化"）
    std::unordered_map<std::string, std::string> ctxByKey;
    {
        std::unordered_map<std::string, std::vector<std::string>> fileOpts; // 文件路径 -> 该文件下所有选项原文
        for (auto& sec : { "_options", "_descriptions" }) {
            if (!tj.contains(sec) || !tj[sec].is_object()) continue;
            for (auto& it : tj[sec].items()) {
                auto p = it.key().rfind("||");
                if (p == std::string::npos) continue;
                std::string en = it.key().substr(p + 2);
                std::string filePart = it.key().substr(0, p);
                auto p2 = filePart.rfind("||");
                if (p2 == std::string::npos) continue;
                std::string fileRel = filePart.substr(0, p2);
                if (sec == "_options") fileOpts[fileRel].push_back(en);
            }
        }
        for (auto& it : pending) {
            auto p = it.key.rfind("||");
            if (p == std::string::npos) continue;
            std::string filePart = it.key.substr(0, p);
            auto p2 = filePart.rfind("||");
            std::string fileRel = p2 == std::string::npos ? filePart : filePart.substr(0, p2);
            std::string ctx = "来源: " + fileRel + (it.sec == "_descriptions" ? "（描述）" : "（选项）");
            auto gl = fileOpts.find(fileRel);
            if (gl != fileOpts.end()) {
                std::string siblings;
                int cnt = 0;
                for (auto& s : gl->second) {
                    if (s == it.english) continue;
                    if (!siblings.empty()) siblings += ", ";
                    siblings += s;
                    if (++cnt >= 8) break;
                }
                if (!siblings.empty()) ctx += " 同组选项: " + siblings;
            }
            ctxByKey[it.key] = ctx;
        }
    }

    // 分批调用 AI（第一轮 + 最多 2 轮自动补翻，减少 AI 漏翻）
    int batch = g_cfg.aiBatchSize;
    if (batch < 1) batch = 1;
    if (batch > 1000) batch = 1000; // v2.3.4：上限从 100 提高到 1000
    int total = (int)pending.size();
    int batches = (total + batch - 1) / batch;
    int okCount = 0;
    LogThread("[AI] 正在翻译中，共 " + std::to_string(total) + " 条，每批 " + std::to_string(batch) + " 条，分 " + std::to_string(batches) + " 批");
    auto translateList = [&](std::vector<Item>& list) -> std::vector<Item> {
        std::vector<Item> missed;
        int cur = 0;
        int nTotal = (int)list.size();
        int nBatches = (nTotal + batch - 1) / batch;
        for (size_t i = 0; i < list.size(); i += batch) {
            int batchNo = (int)(i / batch) + 1;
            LogThread("[AI] 正在翻译第 " + std::to_string(batchNo) + "/" + std::to_string(nBatches) + " 批...");
            if (g_cancel) {
                LogThread("[提示] 已中断，剩余 " + std::to_string(nTotal - cur) + " 条未翻译");
                break;
            }
            size_t n = std::min<size_t>(batch, list.size() - i);
            std::vector<AIItem> items;
            for (size_t k = 0; k < n; ++k) {
                std::string ctx;
                auto cf = ctxByKey.find(list[i + k].key);
                if (cf != ctxByKey.end()) ctx = cf->second;
                items.emplace_back(AIItem{ std::to_string(i + k), list[i + k].english, ctx });
            }

            // 会话术语：把与本批文本相关的已确定译法注入 prompt，保证同词同译
            std::vector<std::pair<std::string, std::string>> glossary;
            if (!sessionTerms.empty()) {
                for (const auto& kv : sessionTerms) {
                    bool relevant = false;
                    for (size_t k = 0; k < n && !relevant; ++k)
                        if (termInText(list[i + k].english, kv.first)) relevant = true;
                    if (relevant) glossary.push_back(kv);
                    if (glossary.size() >= 60) break; // 防止 prompt 过长
                }
            }

            std::string err;
            std::map<std::string, std::string> got;
            bool okBatch = false;
            bool retried = false;
            for (int retry = 0; retry < 3 && !okBatch; ++retry) {
                if (g_cancel) break; // 用户已中断：不再重试
                if (retry > 0) { retried = true; LogThread("[提示] 批次 " + std::to_string(i / batch + 1) + " 重试第 " + std::to_string(retry) + " 次..."); Sleep(2000); }
                got.clear();
                if (AITranslateBatch(items, got, err, glossary)) okBatch = true;
                else LogThread("[错误] 批次 " + std::to_string(i / batch + 1) + " 失败: " + err);
            }
            if (okBatch) {
                if (retried)
                    LogThread("[完成] 批次 " + std::to_string(i / batch + 1) + " 重试成功（该批结果已正常写入）");
                for (size_t k = 0; k < n; ++k) {
                    auto f = got.find(std::to_string(i + k));
                    if (f != got.end()) {
                        // 兜底修正：AI 把无法翻译的专名输出成「英文（英文）」时回退为原英文
                        std::string trans = fixRepeatParen(f->second);
                        tj[list[i + k].sec][list[i + k].key] = trans;
                        okCount++;
                        // 记录会话术语：同词后续批次强制统一译法
                        std::string zh = extractChineseTerm(trans);
                        if (!zh.empty() && zh != list[i + k].english)
                            sessionTerms[list[i + k].english] = zh;
                    }
                    else missed.push_back(list[i + k]);
                }
            }
            else {
                for (size_t k = 0; k < n; ++k) missed.push_back(list[i + k]);
            }
            cur += (int)n;
            SetProgress(cur, nTotal);
        }
        return missed;
    };

    std::vector<Item> missed = translateList(pending);
    for (int round = 1; round <= 2 && !missed.empty() && !g_cancel; ++round) {
        LogThread("[AI] 第 " + std::to_string(round) + " 轮补翻：仍有 " + std::to_string(missed.size()) + " 条漏项，重新发送...");
        missed = translateList(missed);
    }
    if (!missed.empty())
        LogThread("[提示] 经补翻后仍有 " + std::to_string(missed.size()) + " 条未翻译（见下方清单）");
    LogThread("[AI] 翻译完成：" + std::to_string(okCount) + " 条成功，"
        + std::to_string(missed.size()) + " 条最终未翻译");
    // 生成/更新「翻译失败.json」：保留仍未翻译的条目（整批失败 + AI 漏翻），下次翻译优先重试
    if (!missed.empty()) {
        json failJ;
        for (auto& sec : { "_options", "_descriptions" }) {
            if (!tj.contains(sec) || !tj[sec].is_object()) continue;
            json obj = json::object();
            for (auto& it : tj[sec].items()) {
                if (!it.value().is_string() || it.value().get<std::string>().empty())
                    obj[it.key()] = "";
            }
            if (!obj.empty()) failJ[sec] = obj;
        }
        if (!failJ.empty()) {
            fs::path failPath = fs::u8path(g_cfg.translationDir) / fs::u8path("翻译失败.json");
            if (write_binary_file(failPath, safeDump(failJ, 2)))
                LogThread("[提示] 已生成/更新 翻译失败.json（" + std::to_string(missed.size())
                    + " 条待重试，下次点 AI 翻译将优先处理）");
        }
    }
    if (okCount == 0) return false;

    // 落盘：_未翻译 → _已翻译；同名已翻译含内容则另存 *_已翻译_AI.json
    fs::path outFile = inFile;
    std::wstring inName = inFile.filename().wstring();
    bool isFailInput = (inName.find(L"翻译失败") != std::wstring::npos);
    size_t untrPos = inName.find(L"_未翻译");
    if (untrPos != std::wstring::npos) {
        std::wstring outName = inName;
        outName.replace(untrPos, std::wstring(L"_未翻译").size(), L"_已翻译");
        outFile = inFile.parent_path() / outName;
        std::error_code oec;
        if (fs::exists(outFile, oec) && !oec) {
            bool hasContent = false;
            std::string existData;
            if (read_binary_file(outFile, existData)) {
                try {
                    json ej = json::parse(clean_utf8(existData));
                    if (ej.is_object()) {
                        for (auto& sec : { "_options", "_descriptions" }) {
                            if (!ej.contains(sec) || !ej[sec].is_object()) continue;
                            for (auto& it : ej[sec].items()) {
                                if (it.value().is_string() && !it.value().get<std::string>().empty()) { hasContent = true; break; }
                            }
                            if (hasContent) break;
                        }
                    }
                } catch (...) {}
            }
            if (hasContent) {
                std::wstring alt = outName;
                size_t dp = alt.rfind(L'.');
                if (dp != std::wstring::npos) alt.insert(dp, L"_AI");
                else alt += L"_AI";
                outFile = inFile.parent_path() / alt;
                LogThread("[提示] " + wstring_to_utf8(outName) + " 已存在且含翻译，本次输出为 " + wstring_to_utf8(alt));
            }
        }
    } else if (isFailInput) {
        // 翻译失败.json 重翻：成功条目合并进最新 *_已翻译.json，而不是覆盖失败清单
        mergeFailIntoTranslated();
        // 统计仍空白条目：还有则写回翻译失败.json，全部成功则删除残留清单
        int stillEmpty = 0;
        for (auto& sec : { "_options", "_descriptions" }) {
            if (!tj.contains(sec) || !tj[sec].is_object()) continue;
            for (auto& it : tj[sec].items())
                if (it.value().is_string() && it.value().get<std::string>().empty()) stillEmpty++;
        }
        if (stillEmpty == 0) {
            std::error_code fec;
            fs::remove(inFile, fec);
            LogThread("[提示] 翻译失败.json 已全部翻译完成，已自动删除");
            outFile.clear();
        } else {
            outFile = inFile; // 更新翻译失败.json，保留剩余空白条目下次重试
        }
    }
    // 统计仍未翻译的条目（AI 漏掉 / 返回为空的），方便用户定位后手动补翻或重跑
    {
        std::vector<std::string> stillUntranslated;
        for (auto& sec : { "_options", "_descriptions" }) {
            if (!tj.contains(sec) || !tj[sec].is_object()) continue;
            for (auto& it : tj[sec].items()) {
                std::string v = it.value().is_string() ? it.value().get<std::string>() : "";
                if (v.empty()) {
                    auto kp = it.key().rfind("||");
                    stillUntranslated.push_back((kp == std::string::npos) ? it.key() : it.key().substr(kp + 2));
                }
            }
        }
        if (!stillUntranslated.empty()) {
            std::string msg = "[提示] 仍有 " + std::to_string(stillUntranslated.size()) + " 条未翻译（AI 漏掉/未返回）：";
            for (size_t z = 0; z < stillUntranslated.size() && z < 10; ++z)
                msg += "\r\n  - " + stillUntranslated[z];
            if (stillUntranslated.size() > 10)
                msg += "\r\n  ... 其余 " + std::to_string(stillUntranslated.size() - 10) + " 条省略";
            LogThread(msg);
        }
    }
    if (outFile.empty()) return true; // 翻译失败.json 已全部完成并删除，无需落盘
    if (write_binary_file(outFile, safeDump(tj, 2))) {
        LogThread("[提示] 已写入 " + wstring_to_utf8(outFile.filename().wstring()));
        return true;
    }
    LogThread("[错误] 写入失败: " + wstring_to_utf8(outFile.filename().wstring()));
    return false;
}

std::wstring GetEditText(HWND hDlg, int id); // 前向声明（定义在文件后部）

// ------------------------------------------------------------------
// 测试 AI API 连接（工作线程）：用界面当前配置发一条最小请求
// ------------------------------------------------------------------
static void RunAITestThread()
{
    try {
        g_busy = true;
        std::string key = wstring_to_utf8(GetEditText(g_hMainWnd, IDC_AI_KEY));
        std::string model = wstring_to_utf8(GetEditText(g_hMainWnd, IDC_AI_MODEL));
        std::string base = wstring_to_utf8(GetEditText(g_hMainWnd, IDC_AI_BASEURL));
        if (key.empty()) { Log("[错误] 未填写 API Key，请先在『AI 翻译设置』中填写。"); g_busy = false; PostMessageW(g_hMainWnd, WM_APP_DONE, 0, 0); return; }
        if (model.empty()) { Log("[错误] 未填写模型名，请先在『AI 翻译设置』中填写。"); g_busy = false; PostMessageW(g_hMainWnd, WM_APP_DONE, 0, 0); return; }
        if (base.empty()) { Log("[错误] 未填写 API 地址，请先在『AI 翻译设置』中填写。"); g_busy = false; PostMessageW(g_hMainWnd, WM_APP_DONE, 0, 0); return; }

        json req;
        req["model"] = model;
        req["messages"] = json::array();
        req["messages"].push_back({ {"role","user"}, {"content","请只回复四个字：连接成功"} });
        req["stream"] = false;
        req["max_tokens"] = 1024;
        std::string body = req.dump();

        std::string url = base;
        while (!url.empty() && url.back() == '/') url.pop_back();
        if (url.find("/chat/completions") == std::string::npos) url += "/chat/completions";

        ULONGLONG t0 = GetTickCount64();
        std::string respBody, err;
        if (!HttpPostJson(url, key, body, respBody, err)) {
            Log("[错误] AI 测试失败：" + err);
            g_busy = false; PostMessageW(g_hMainWnd, WM_APP_DONE, 0, 0);
            return;
        }
        int ms = (int)(GetTickCount64() - t0);

        json resp;
        try { resp = json::parse(clean_utf8(respBody)); }
        catch (...) {
            Log("[错误] AI 测试失败：响应不是合法 JSON：" + respBody.substr(0, 300));
            g_busy = false; PostMessageW(g_hMainWnd, WM_APP_DONE, 0, 0);
            return;
        }
        if (!resp.contains("choices") || resp["choices"].empty()) {
            if (resp.contains("error"))
                Log("[错误] AI 测试失败：" + resp["error"].dump().substr(0, 400));
            else
                Log("[错误] AI 测试失败：响应缺少 choices：" + respBody.substr(0, 300));
            g_busy = false; PostMessageW(g_hMainWnd, WM_APP_DONE, 0, 0);
            return;
        }
        std::string content;
        try { content = resp["choices"][0]["message"]["content"].get<std::string>(); }
        catch (...) { content = "(非文本回复)"; }
        // 测试通过说明当前界面配置可用：立即写入用户配置 config.user.json，避免手动输入的地址/模型重启后丢失
        SaveConfig();
        Log("[成功] AI 连接测试通过！模型 " + model + " 回复：" + content + "（耗时 " + std::to_string(ms) + " ms）");
        g_busy = false;
        PostMessageW(g_hMainWnd, WM_APP_DONE, 0, 0);
    }
    catch (const std::system_error& e) { Log(std::string("[错误] AI 测试线程系统异常: ") + e.what()); }
    catch (const std::exception& e) { Log(std::string("[错误] AI 测试线程异常: ") + e.what()); }
    catch (...) { Log("[错误] AI 测试线程未知异常"); }
    PostMessageW(g_hMainWnd, WM_APP_DONE, 0, 0);
}

void RunAITranslateThread()
{
    try {
        g_busy = true;
        if (g_cfg.translationDir.empty()) {
            Log("[错误] 未设置翻译目录");
            g_busy = false;
            PostMessageW(g_hMainWnd, WM_APP_DONE, 0, 0);
            return;
        }
        auto files = ScanTransFiles(fs::u8path(g_cfg.translationDir));
        fs::path target;
        // 优先重试「翻译失败.json」里的失败条目，其次再处理 *_未翻译.json
        fs::path failPath = fs::u8path(g_cfg.translationDir) / fs::u8path("翻译失败.json");
        std::error_code fec;
        if (fs::exists(failPath, fec) && !fec) {
            target = failPath;
            Log("[提示] 检测到 翻译失败.json，优先重新翻译失败条目");
        }
        else {
            for (auto& f : files) if (!f.isTranslated) { target = f.path; break; }
        }
        if (target.empty()) {
            Log("[错误] 翻译目录下没有 *_未翻译.json，请先执行『1. 提取英文』");
            g_busy = false;
            PostMessageW(g_hMainWnd, WM_APP_DONE, 0, 0);
            return;
        }
        Log("===== 开始 AI 自动翻译 =====");
        Log("输入文件: " + wstring_to_utf8(target.filename().wstring()));
        bool ok = AITranslateFile(target);
        // 翻译失败.json 已无空值（全部翻译成功）时自动删除，避免残留干扰下次
        if (!target.empty() && target == failPath) {
            std::string fd;
            if (read_binary_file(failPath, fd)) {
                try {
                    json fj = json::parse(clean_utf8(fd));
                    bool anyEmpty = false;
                    if (fj.is_object()) {
                        for (auto& sec : { "_options", "_descriptions" }) {
                            if (!fj.contains(sec) || !fj[sec].is_object()) continue;
                            for (auto& it : fj[sec].items())
                                if (it.value().is_string() && it.value().get<std::string>().empty()) { anyEmpty = true; break; }
                            if (anyEmpty) break;
                        }
                    }
                    if (!anyEmpty) {
                        std::error_code dec;
                        fs::remove(failPath, dec);
                        Log("[提示] 翻译失败.json 已全部翻译成功，已自动删除");
                    }
                } catch (...) {}
            }
        }
        Log("AI 翻译流程结束");
        g_busy = false;
    }
    catch (const std::system_error& e) { Log(std::string("[错误] AI 翻译线程系统异常: ") + e.what()); }
    catch (const std::exception& e) { Log(std::string("[错误] AI 翻译线程异常: ") + e.what()); }
    catch (...) { Log("[错误] AI 翻译线程未知异常"); }
    PostMessageW(g_hMainWnd, WM_APP_DONE, 0, 0);
}

// ------------------------------------------------------------------
// 主窗口可缩放布局（等比缩放：所有子控件按窗口与设计尺寸的比例同步缩放）
// ------------------------------------------------------------------
static int g_currentAppliedFontSize = 0; // 当前实际应用的字号（含联动缩放）

// 前向声明：LayoutMainDlg 在窗口缩放联动字体时会调用
void CreateUiFont(int size);
void ApplyFontToDialog(HWND hDlg);

static void LayoutMainDlg(HWND hDlg)
{
    if (g_dlgW0 <= 0 || g_dlgH0 <= 0 || g_layout.empty()) return;
    RECT rc;
    GetClientRect(hDlg, &rc);
    const int W = rc.right, H = rc.bottom;
    const float sx = (float)W / (float)g_dlgW0;
    const float sy = (float)H / (float)g_dlgH0;
    // 等比缩放所有子控件（包括静态标签、GroupBox、RichEdit 等）
    for (const auto& c : g_layout) {
        if (!c.hwnd) continue;
        int x = (int)(c.rc.left * sx);
        int y = (int)(c.rc.top * sy);
        int w = (int)((c.rc.right - c.rc.left) * sx);
        int h = (int)((c.rc.bottom - c.rc.top) * sy);
        // 组合框（CBS_DROPDOWN / CBS_DROPDOWNLIST）的高度参数被系统解释为
        // 「下拉列表弹出高度」：窗口缩放时保持 .rc 设计高度不变，
        // 否则列表弹出高度被缩放破坏，导致点开箭头看不到列表项
        LONG_PTR st = GetWindowLongPtrW(c.hwnd, GWL_STYLE);
        if ((st & 0x0003L) == CBS_DROPDOWN || (st & 0x0003L) == CBS_DROPDOWNLIST)
            h = c.rc.bottom - c.rc.top;
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        MoveWindow(c.hwnd, x, y, w, h, TRUE);
    }

    // 窗口缩放时自动联动字体大小
    if (g_cfg.autoFontSize && W > 0 && H > 0 && g_dlgW0 > 0 && g_dlgH0 > 0) {
        float scaleW = (float)W / (float)g_dlgW0;
        float scaleH = (float)H / (float)g_dlgH0;
        float scale = (scaleW > scaleH) ? scaleW : scaleH;
        int newSize = (int)std::round(g_cfg.fontSize * scale);
        if (newSize < 8) newSize = 8;
        if (newSize > 24) newSize = 24;
        if (newSize != g_currentAppliedFontSize) {
            CreateUiFont(newSize);
            ApplyFontToDialog(hDlg);
        }
    }
}

// ------------------------------------------------------------------
// 字体大小
// ------------------------------------------------------------------
void CreateUiFont(int size = 0)
{
    if (g_hFont) { DeleteObject(g_hFont); g_hFont = nullptr; }
    int s = (size > 0) ? size : g_cfg.fontSize;
    if (s < 8) s = 8;
    if (s > 24) s = 24;
    g_currentAppliedFontSize = s;
    HDC dc = GetDC(nullptr);
    int px = -MulDiv(s, GetDeviceCaps(dc, LOGPIXELSY), 72);
    ReleaseDC(nullptr, dc);
    g_hFont = CreateFontW(px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"MS Shell Dlg");
}

void ApplyFontToDialog(HWND hDlg)
{
    if (!g_hFont) return;
    HWND child = GetWindow(hDlg, GW_CHILD);
    while (child) {
        SendMessageW(child, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        // 日志 RichEdit：WM_SETFONT 只改默认格式，已存在的文本不会换字体，
        // 需用 EM_SETCHARFORMAT(SCF_ALL) 强制所有已有文本跟随缩放
        wchar_t cls[64];
        if (GetClassNameW(child, cls, 64) > 0 && wcsncmp(cls, L"RichEdit", 8) == 0) {
            CHARFORMAT2W cf = {};
            cf.cbSize = sizeof(cf);
            cf.dwMask = CFM_FACE | CFM_SIZE | CFM_CHARSET;
            cf.dwEffects = 0;
            cf.yHeight = g_currentAppliedFontSize * 20; // 磅值 -> twips（1pt = 20 twips）
            cf.bCharSet = DEFAULT_CHARSET;
            wcscpy_s(cf.szFaceName, L"MS Shell Dlg");
            SendMessageW(child, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
            // SCF_ALL 会把已有文本的颜色重置为默认黑色，重放日志缓冲以恢复各行的标签颜色
            RepaintLogView();
        }
        child = GetWindow(child, GW_HWNDNEXT);
    }
}

// ------------------------------------------------------------------
// UI 刷新
// ------------------------------------------------------------------
// 所有 Edit 填写框子类化：Ctrl+A 全选（Win32 Edit 默认不响应 Ctrl+A）
LRESULT CALLBACK EditSelectAllProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                   UINT_PTR /*uIdSubclass*/, DWORD_PTR /*dwRefData*/)
{
    if (msg == WM_CHAR && wParam == 1) { // 1 = Ctrl+A
        SendMessageW(hwnd, EM_SETSEL, 0, -1);
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

BOOL CALLBACK SubclassEditForSelectAll(HWND hwnd, LPARAM /*lParam*/)
{
    wchar_t cls[32] = {};
    if (GetClassNameW(hwnd, cls, 32) > 0 && _wcsicmp(cls, L"Edit") == 0)
        SetWindowSubclass(hwnd, EditSelectAllProc, 0, 0);
    return TRUE;
}

// 给对话框内所有 Edit 填写框启用 Ctrl+A 全选
static void EnableEditSelectAll(HWND hDlg)
{
    EnumChildWindows(hDlg, SubclassEditForSelectAll, 0);
}

void SetEditText(HWND hDlg, int id, const std::wstring& text)
{
    SetDlgItemTextW(hDlg, id, text.c_str());
}

std::wstring GetEditText(HWND hDlg, int id)
{
    wchar_t buf[4096] = {};
    GetDlgItemTextW(hDlg, id, buf, 4096);
    return buf;
}

// 从界面控件同步 AI 设置到 g_cfg（界面即真值），避免 UI 与配置不同步
static void SyncAISettingsFromUI(HWND hDlg)
{
    g_cfg.aiApiKey = wstring_to_utf8(GetEditText(hDlg, IDC_AI_KEY));
    g_cfg.aiKeyName = wstring_to_utf8(GetEditText(hDlg, IDC_AI_KEY_NAME));
    g_cfg.aiModel = wstring_to_utf8(GetEditText(hDlg, IDC_AI_MODEL));
    g_cfg.aiBaseUrl = wstring_to_utf8(GetEditText(hDlg, IDC_AI_BASEURL));
}

// 检查模型名与 API 地址是否分属不同的「内置」预设（忽略大小写）；不匹配时返回警告文本。
// 用户自定义保存的预设（name 非内置名）不参与警告，避免误报。
static std::string CheckAIMatch()
{
    if (g_cfg.aiModel.empty() || g_cfg.aiBaseUrl.empty()) return "";
    const AIPreset* m = nullptr;
    const AIPreset* b = nullptr;
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    };
    std::string cm = lower(g_cfg.aiModel);
    std::string cb = lower(g_cfg.aiBaseUrl);
    for (const auto& p : g_cfg.aiPresets) {
        if (lower(p.model) == cm) m = &p;
        if (lower(p.baseUrl) == cb) b = &p;
    }
    if (!m || !b || m->name == b->name) return "";
    auto isBuiltin = [](const AIPreset* p) {
        for (const auto& d : g_defaultAIPresets)
            if (d.name == p->name) return true;
        return false;
    };
    if (!isBuiltin(m) || !isBuiltin(b)) return "";
    return "模型名「" + g_cfg.aiModel + "」属于预设「" + m->name +
           "」，但 API 地址「" + g_cfg.aiBaseUrl + "」属于预设「" + b->name +
           "」，两者不匹配，可能认证失败（401），请核对";
}

// 规范化用于重复检测：转小写并去掉空白字符
static std::string NormForDup(std::string s)
{
    std::string r;
    r.reserve(s.size());
    for (unsigned char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        r += (char)std::tolower(c);
    }
    return r;
}

// 自动生成不重复的预设名（如「自定义 1」「自定义 2」…）
static std::string NextPresetName()
{
    for (int n = 1;; ++n) {
        std::string name = "自定义 " + std::to_string(n);
        bool used = false;
        for (const auto& p : g_cfg.aiPresets)
            if (p.name == name) { used = true; break; }
        if (!used)
            for (const auto& s : g_cfg.customSaves)
                if (s.name == name) { used = true; break; }
        if (!used) return name;
    }
}

// 用户「保存」的自定义 AI 记录存于用户配置 config.user.json 的 customSaves（数组：
// [{name,key,model,baseUrl,note}, …]），不再单独写「自定义AI存档.json」。
// 启动时把它们并入当前 Key/预设（用户自定义优先：同名 Key 用存档值覆盖）。
static void LoadCustomSaves()
{
    // 旧版兼容：程序目录残留的「自定义AI存档.json」自动迁移并入 customSaves
    size_t migrated = 0;
    fs::path legacy = GetExeDir() / L"自定义AI存档.json";
    std::string ldata;
    if (read_binary_file(legacy, ldata) && !ldata.empty()) {
        try {
            json t = json::parse(ldata, nullptr, true, true);
            if (t.is_array()) {
                for (const auto& it : t) {
                    if (!it.is_object()) continue;
                    AISaveEntry e;
                    e.name = it.value("name", "");
                    e.key = it.value("key", "");
                    e.model = it.value("model", "");
                    e.baseUrl = it.value("baseUrl", "");
                    e.note = it.value("note", "");
                    if (e.name.empty() && e.key.empty() && e.model.empty() && e.baseUrl.empty()) continue;
                    bool dup = false;
                    for (const auto& x : g_cfg.customSaves)
                        if (x.name == e.name && !e.name.empty()) { dup = true; break; }
                    if (!dup) { g_cfg.customSaves.push_back(e); ++migrated; }
                }
            }
        } catch (...) {}
    }
    if (migrated > 0) {
        SaveConfig();
        Log("[提示] 已把「自定义AI存档.json」迁移并入用户配置 config.user.json（" + std::to_string(migrated) + " 条；旧文件保留可自行删除）");
    }

    // 把 customSaves 并入当前 Key（预设列表不动：内置预设只来自 config.default.json，
    // 自定义记录单独存于 customSaves，由「配置列表」统一展示，避免污染预设）
    size_t used = 0;
    for (const auto& e : g_cfg.customSaves) {
        // Key：同名覆盖，否则（key 非空）追加
        if (!e.key.empty()) {
            bool updated = false;
            for (auto& k : g_cfg.aiKeys) {
                if (!e.name.empty() && k.name == e.name) { k.key = e.key; updated = true; break; }
                if (e.name.empty() && k.key == e.key) { updated = true; break; }
            }
            if (!updated) g_cfg.aiKeys.push_back({ e.name, e.key });
        }
        ++used;
    }
    // 当前选择的 Key 若在 customSaves 并入的列表中，以最新值为准
    if (!g_cfg.aiKeyName.empty())
        for (const auto& k : g_cfg.aiKeys)
            if (k.name == g_cfg.aiKeyName) { g_cfg.aiApiKey = k.key; break; }
    if (used > 0)
        Log("[完成] 已读取自定义存档（" + std::to_string(used) + " 条）：用户配置 config.user.json");
}

// 把当前 Key + 模型名 + API 地址（+ 备注名）统一保存为一条自定义记录到用户配置
// config.user.json 的 customSaves（「保存」按钮不改写预设列表等其它用户配置项）。
static void SaveCustomSaves(HWND hDlg)
{
    std::string name = wstring_to_utf8(GetEditText(hDlg, IDC_AI_KEY_NAME));
    std::string key = wstring_to_utf8(GetEditText(hDlg, IDC_AI_KEY));
    std::string model = wstring_to_utf8(GetEditText(hDlg, IDC_AI_MODEL));
    std::string baseUrl = wstring_to_utf8(GetEditText(hDlg, IDC_AI_BASEURL));
    if (name.empty() && key.empty() && model.empty() && baseUrl.empty()) {
        MessageBoxW(hDlg, L"Key、模型名、API 地址均为空，没有可保存的内容。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (name.empty()) name = NextPresetName();
    const std::string note = "自定义";

    // 更新内存中的 customSaves（同名覆盖，否则追加），随后写入用户配置
    bool updated = false;
    for (auto& s : g_cfg.customSaves) {
        if (s.name == name) {
            s.key = key; s.model = model; s.baseUrl = baseUrl; s.note = note;
            updated = true;
            break;
        }
    }
    if (!updated) g_cfg.customSaves.push_back({ name, key, model, baseUrl, note });

    if (SaveConfig()) {
        // 同步内存（下次启动读取用户配置时同样生效）
        if (!key.empty()) {
            bool kdup = false;
            for (auto& k : g_cfg.aiKeys)
                if (k.name == name || k.key == key) { k.name = name; k.key = key; kdup = true; break; }
            if (!kdup) g_cfg.aiKeys.push_back({ name, key });
        }
        g_cfg.aiKeyName = name;
        g_cfg.aiApiKey = key;
        // 刷新下拉并回填当前输入
        FillAIKeyCombo(hDlg);
        FillAIPresetCombos(hDlg);
        SetEditText(hDlg, IDC_AI_KEY_NAME, utf8_to_wstring(name));
        SetEditText(hDlg, IDC_AI_KEY, utf8_to_wstring(key));
        SetEditText(hDlg, IDC_AI_MODEL, utf8_to_wstring(model));
        SetEditText(hDlg, IDC_AI_BASEURL, utf8_to_wstring(baseUrl));
        Log("[完成] 已保存自定义 AI 记录「" + name + "」到用户配置（config.user.json），可在「配置列表」中查看或删除");
    } else {
        MessageBoxW(hDlg, L"写入用户配置文件失败，请检查程序目录是否有写权限。", L"提示", MB_OK | MB_ICONWARNING);
    }
}

// 把预设列表填入「模型名」「API 地址」两个下拉框
// 模型名 / API 地址已改为普通输入框（无下拉列表），这里只负责回填当前值
static void FillAIPresetCombos(HWND hDlg)
{
    SetEditText(hDlg, IDC_AI_MODEL, utf8_to_wstring(g_cfg.aiModel));
    SetEditText(hDlg, IDC_AI_BASEURL, utf8_to_wstring(g_cfg.aiBaseUrl));
}

// 让输入框宽度随文本字节长度自适应（minDlu~maxDlu，DLU 单位），宽度不足自动截断滚动
static void AutoFitEditWidth(HWND hDlg, int id, int minDlu, int maxDlu)
{
    HWND he = GetDlgItem(hDlg, id);
    if (!he) return;
    std::wstring t = GetEditText(hDlg, id);
    HDC hdc = GetDC(he);
    HFONT hf = (HFONT)SendMessageW(he, WM_GETFONT, 0, 0);
    HFONT hOld = hf ? (HFONT)SelectObject(hdc, hf) : nullptr;
    SIZE sz = {};
    GetTextExtentPoint32W(hdc, t.c_str(), (int)t.size(), &sz);
    if (hOld) SelectObject(hdc, hOld);
    ReleaseDC(he, hdc);
    RECT rd = {0, 0, 1, 1};
    MapDialogRect(hDlg, &rd);
    int dluPx = rd.right;
    if (dluPx < 1) dluPx = 1;
    int w = sz.cx + 20; // 内容宽度 + 边距
    if (w < minDlu * dluPx) w = minDlu * dluPx;
    if (w > maxDlu * dluPx) w = maxDlu * dluPx;
    RECT wr;
    GetWindowRect(he, &wr);
    if (wr.right - wr.left != w)
        SetWindowPos(he, nullptr, 0, 0, w, wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// API Key 已改为普通输入框（无下拉列表），这里只负责回填当前 Key
static void FillAIKeyCombo(HWND hDlg)
{
    SetEditText(hDlg, IDC_AI_KEY, utf8_to_wstring(g_cfg.aiApiKey));
}

// 切换 API Key 输入框的密码掩码。
// 运行时改 ES_PASSWORD 样式对「已创建」的编辑框不生效，必须用 EM_SETPASSWORDCHAR：
// 传 0 = 取消掩码（明文）；传掩码字符 = 隐藏
static void ApplyKeyPasswordStyle(HWND hDlg)
{
    HWND he = GetDlgItem(hDlg, IDC_AI_KEY);
    if (!he) return;
    if (g_keyVisible)
        SendMessageW(he, EM_SETPASSWORDCHAR, 0, 0);                 // 明文
    else
        SendMessageW(he, EM_SETPASSWORDCHAR, (WPARAM)L'\u25CF', 0); // 圆点掩码
    InvalidateRect(he, nullptr, TRUE);
}

// ------------------------------------------------------------------
// AI 配置选择窗口：列出内置预设 + 用户自定义保存，勾选后整套套用
// ------------------------------------------------------------------
struct AICfgItem {
    std::string name;
    std::string key;
    std::string model;
    std::string baseUrl;
    std::string note;
    bool fromCustom = false;  // true = 来自用户自定义存档（可删除）；false = 内置预设
};
static std::vector<AICfgItem> g_aiSelItems;
static int g_aiSelResult = -1;

// 判断自定义记录与内置预设是否可视为同一条（同名 或 同一 API 地址）
static bool AISameEntry(const AISaveEntry& s, const AIPreset& p)
{
    bool nameMatch = !s.name.empty() && !p.name.empty() && s.name == p.name;
    bool urlMatch  = !s.baseUrl.empty() && !p.baseUrl.empty() && s.baseUrl == p.baseUrl;
    return nameMatch || urlMatch;
}

// 构建/重建「AI 配置列表」：内置预设 + 用户自定义。
// 若内置预设存在同名/同地址且带 Key 的自定义记录，则合并成一行（带 Key，可删除），
// 避免出现两条「智谱」、勾选无 Key 行导致 Key 不切换的问题。
static void BuildAISelectList(HWND hList)
{
    g_aiSelItems.clear();
    for (const auto& p : g_cfg.aiPresets) {
        AICfgItem item{ p.name, "", p.model, p.baseUrl,
                        p.note.empty() ? std::string("内置预设") : p.note, false };
        for (const auto& s : g_cfg.customSaves) {
            if (AISameEntry(s, p)) {
                item.key = s.key;
                item.name = s.name.empty() ? p.name : s.name;
                // 用户自定义的模型名 / API 地址优先，避免合并后自定义模型名被内置预设覆盖
                if (!s.model.empty()) item.model = s.model;
                if (!s.baseUrl.empty()) item.baseUrl = s.baseUrl;
                item.note = (item.name == p.name)
                            ? std::string("自定义")
                            : ("源自：" + p.name);
                item.fromCustom = true;
                break;
            }
        }
        g_aiSelItems.push_back(std::move(item));
    }
    // 未被合并进预设的自定义记录，单独追加显示
    for (const auto& e : g_cfg.customSaves) {
        bool merged = false;
        for (const auto& p : g_cfg.aiPresets)
            if (AISameEntry(e, p)) { merged = true; break; }
        if (merged) continue;
        g_aiSelItems.push_back({ e.name, e.key, e.model, e.baseUrl,
                                 e.note.empty() ? std::string("自定义") : e.note, true });
    }

    ListView_DeleteAllItems(hList);
    for (size_t i = 0; i < g_aiSelItems.size(); ++i) {
        const auto& it = g_aiSelItems[i];
        std::wstring wName = utf8_to_wstring(it.name);
        std::wstring wKey = it.key.empty() ? L"" : L"******"; // 预设无 Key 显示空；有 Key 默认掩码隐藏
        std::wstring wModel = utf8_to_wstring(it.model);
        std::wstring wBase = utf8_to_wstring(it.baseUrl);
        std::wstring wNote = utf8_to_wstring(it.note);
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = (int)i;
        item.pszText = (LPWSTR)wName.c_str();
        int idx = (int)ListView_InsertItem(hList, &item);
        ListView_SetItemText(hList, idx, 1, (LPWSTR)wKey.c_str());
        ListView_SetItemText(hList, idx, 2, (LPWSTR)wModel.c_str());
        ListView_SetItemText(hList, idx, 3, (LPWSTR)wBase.c_str());
        ListView_SetItemText(hList, idx, 4, (LPWSTR)wNote.c_str());
    }
}

static INT_PTR CALLBACK SelectAICfgDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_INITDIALOG: {
        g_aiSelResult = -1;
        HWND hList = GetDlgItem(hDlg, IDC_AI_SELECT_LIST);
        if (!hList) return TRUE;
        ListView_SetExtendedListViewStyle(hList, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.fmt = LVCFMT_LEFT;
        struct ColDef { LPCWSTR t; int w; } cols[] = {
            { L"名称", 100 }, { L"API_Key", 130 }, { L"模型名", 110 }, { L"API 地址", 160 }, { L"说明", 70 }
        };
        for (const auto& c : cols) {
            col.pszText = (LPWSTR)c.t;
            col.cx = c.w;
            ListView_InsertColumn(hList, 9999, &col);
        }
        BuildAISelectList(hList);
        return TRUE;
    }
    case WM_NOTIFY: {
        NMHDR* nm = (NMHDR*)lParam;
        if (nm->idFrom == IDC_AI_SELECT_LIST) {
            if (nm->code == NM_CLICK) {
                // 点击行 = 选中并自动勾选；再次点击已勾选的行 = 取消勾选
                NMITEMACTIVATE* nmia = (NMITEMACTIVATE*)lParam;
                int idx = nmia->iItem;
                if (idx < 0 || idx >= (int)g_aiSelItems.size()) break;
                HWND hList = nm->hwndFrom;
                // 点在复选框区域（最左侧约 20px）时交给 ListView 默认处理，避免双重切换
                if (nmia->ptAction.x < 20) break;
                bool checked = ListView_GetCheckState(hList, idx) != FALSE;
                bool selected = (ListView_GetItemState(hList, idx, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                if (selected && checked) {
                    // 再点一次：取消勾选并取消选中
                    ListView_SetCheckState(hList, idx, FALSE);
                    ListView_SetItemState(hList, idx, 0, LVIS_SELECTED);
                } else {
                    // 选中并勾选（单选互斥：取消其它行勾选）
                    ListView_SetItemState(hList, idx, LVIS_SELECTED, LVIS_SELECTED);
                    for (int i = 0; i < ListView_GetItemCount(hList); ++i)
                        if (i != idx) ListView_SetCheckState(hList, i, FALSE);
                    ListView_SetCheckState(hList, idx, TRUE);
                }
                return TRUE;
            }
            if (nm->code == NM_DBLCLK) {
                // 双击列表行 = 确认套用该配置（等同点「确定」）
                NMITEMACTIVATE* nmia = (NMITEMACTIVATE*)lParam;
                int idx = nmia->iItem;
                if (idx < 0 || idx >= (int)g_aiSelItems.size()) break;
                HWND hList = nm->hwndFrom;
                ListView_SetItemState(hList, idx, LVIS_SELECTED, LVIS_SELECTED);
                for (int i = 0; i < ListView_GetItemCount(hList); ++i)
                    if (i != idx) ListView_SetCheckState(hList, i, FALSE);
                ListView_SetCheckState(hList, idx, TRUE);
                g_aiSelResult = idx;
                EndDialog(hDlg, IDOK);
                return TRUE;
            }
            if (nm->code == LVN_ITEMCHANGED) {
                NMLISTVIEW* nmlv = (NMLISTVIEW*)lParam;
                bool changed = (nmlv->uChanged & LVIF_STATE) &&
                    ((nmlv->uNewState & LVIS_STATEIMAGEMASK) != (nmlv->uOldState & LVIS_STATEIMAGEMASK));
                if (changed) {
                    // 单选：勾选某项时自动取消其它项
                    if (ListView_GetCheckState(nm->hwndFrom, nmlv->iItem)) {
                        for (int i = 0; i < ListView_GetItemCount(nm->hwndFrom); ++i)
                            if (i != nmlv->iItem) ListView_SetCheckState(nm->hwndFrom, i, FALSE);
                        g_aiSelResult = nmlv->iItem;
                    } else if (g_aiSelResult == nmlv->iItem) {
                        g_aiSelResult = -1;
                    }
                }
            }
        }
        break;
    }
    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        if (id == IDOK || id == IDC_AI_SELECT_OK) {
            if (g_aiSelResult < 0) {
                MessageBoxW(hDlg, L"请先勾选一条配置，再点确定。", L"提示", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL || id == IDC_AI_SELECT_CANCEL) { EndDialog(hDlg, IDCANCEL); return TRUE; }
        if (id == IDC_AI_SELECT_DEL) {
            // 删除选中的自定义配置（内置预设不可删）
            HWND hList = GetDlgItem(hDlg, IDC_AI_SELECT_LIST);
            int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
            if (sel < 0 || sel >= (int)g_aiSelItems.size()) {
                MessageBoxW(hDlg, L"请先点击选中一条配置，再点「删除选中」。", L"提示", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            const auto& it = g_aiSelItems[sel];
            if (!it.fromCustom) {
                MessageBoxW(hDlg, L"这是内置预设，不能删除。\n如需调整请编辑程序目录下的 config.default.json（aiPresets）。", L"提示", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            if (MessageBoxW(hDlg, (L"确定删除自定义配置「" + utf8_to_wstring(it.name) + L"」吗？").c_str(),
                            L"确认删除", MB_YESNO | MB_ICONQUESTION) != IDYES)
                return TRUE;
            bool removed = false;
            for (size_t i = 0; i < g_cfg.customSaves.size(); ++i) {
                if (g_cfg.customSaves[i].name == it.name) {
                    g_cfg.customSaves.erase(g_cfg.customSaves.begin() + i);
                    removed = true;
                    break;
                }
            }
            if (!removed) { // 备用：按模型名 + 地址匹配
                for (size_t i = 0; i < g_cfg.customSaves.size(); ++i) {
                    const auto& s = g_cfg.customSaves[i];
                    if (s.model == it.model && s.baseUrl == it.baseUrl && !s.model.empty()) {
                        g_cfg.customSaves.erase(g_cfg.customSaves.begin() + i);
                        removed = true;
                        break;
                    }
                }
            }
            if (removed) SaveConfig();
            // 若删掉的是当前正在套用的记录，清掉引用名（Key 值保留，不打断使用）
            if (g_cfg.aiKeyName == it.name) g_cfg.aiKeyName.clear();
            g_aiSelResult = -1;
            BuildAISelectList(hList);
            Log("[完成] 已删除自定义配置「" + it.name + "」");
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

void RefreshConfigUI()
{
    if (!g_hMainWnd) return;
    SetEditText(g_hMainWnd, IDC_EDIT_PENUMBRA, utf8_to_wstring(g_cfg.penumbraDir));
    SetEditText(g_hMainWnd, IDC_EDIT_TRANSLATION, utf8_to_wstring(g_cfg.translationDir));
    SetEditText(g_hMainWnd, IDC_EDIT_DICTIONARY, utf8_to_wstring(g_cfg.dictionaryDir));
    CheckDlgButton(g_hMainWnd, IDC_CHK_SWAP, g_cfg.swapWordOrder ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_hMainWnd, IDC_CHK_BACKUP, g_cfg.autoBackup ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_hMainWnd, IDC_CHK_AUTO_FONT, g_cfg.autoFontSize ? BST_CHECKED : BST_UNCHECKED);
    CheckRadioButton(g_hMainWnd, IDC_RADIO_PURE_CN, IDC_RADIO_CN_EN, g_cfg.pureChinese ? IDC_RADIO_PURE_CN : IDC_RADIO_CN_EN);
    // AI 翻译设置
    FillAIKeyCombo(g_hMainWnd);
    SetEditText(g_hMainWnd, IDC_AI_KEY_NAME, utf8_to_wstring(g_cfg.aiKeyName));
    ApplyKeyPasswordStyle(g_hMainWnd);
    FillAIPresetCombos(g_hMainWnd);
    SetEditText(g_hMainWnd, IDC_AI_MODEL, utf8_to_wstring(g_cfg.aiModel));
    SetEditText(g_hMainWnd, IDC_AI_BASEURL, utf8_to_wstring(g_cfg.aiBaseUrl));
    SetEditText(g_hMainWnd, IDC_AI_BATCH, std::to_wstring(g_cfg.aiBatchSize));
    SetEditText(g_hMainWnd, IDC_EDIT_FONT_SIZE, std::to_wstring(g_cfg.fontSize));
    SetDlgItemTextW(g_hMainWnd, IDC_BTN_SHOW_KEY, g_keyVisible ? L"隐藏" : L"显示");
}

// ------------------------------------------------------------------
// 恢复备份对话框
// ------------------------------------------------------------------
INT_PTR CALLBACK RestoreDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_INITDIALOG: {
        // 初始化左侧 ListView：单选高亮 + 复选框
        HWND left = GetDlgItem(hDlg, IDC_RESTORE_LEFTLIST);
        ListView_SetExtendedListViewStyle(left, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
        LVCOLUMN col = {};
        col.mask = LVCF_WIDTH;
        col.cx = 1000; // 留足宽度，避免长文件名被截断
        ListView_InsertColumn(left, 0, &col);

        // 填充左侧：所有模组文件夹
        std::vector<std::wstring> mods;
        try {
            if (!g_cfg.penumbraDir.empty()) {
                fs::path penRoot = fs::u8path(g_cfg.penumbraDir);
                std::error_code ec;
                if (fs::exists(penRoot, ec) && !ec) {
                    for (auto& e : fs::directory_iterator(penRoot, fs::directory_options::skip_permission_denied, ec)) {
                        if (ec) { ec.clear(); continue; }
                        if (!e.is_directory(ec) || ec) continue;
                        mods.push_back(e.path().filename().wstring());
                    }
                }
            }
        }
        catch (...) {}
        for (auto& m : mods) {
            LVITEM item = {};
            item.mask = LVIF_TEXT;
            item.iItem = INT_MAX;
            item.pszText = (LPWSTR)m.c_str();
            ListView_InsertItem(left, &item);
        }
        return TRUE;
    }
    case WM_NOTIFY: {
        // 左侧当前选中项变化时，刷新右侧备份列表
        NMHDR* nm = (NMHDR*)lParam;
        if (nm->idFrom == IDC_RESTORE_LEFTLIST && nm->code == LVN_ITEMCHANGED) {
            NMLISTVIEW* nmlv = (NMLISTVIEW*)lParam;
            if ((nmlv->uChanged & LVIF_STATE)
                && ((nmlv->uNewState & LVIS_SELECTED) != (nmlv->uOldState & LVIS_SELECTED))) {
                HWND left = GetDlgItem(hDlg, IDC_RESTORE_LEFTLIST);
                // 高亮选中（点击 / Ctrl+A 全选）时自动同步勾选复选框，让「勾选」与「选中/全选」等效；
                // 取消选中时同步取消勾选
                if (nmlv->uNewState & LVIS_SELECTED) {
                    ListView_SetCheckState(left, nmlv->iItem, TRUE);
                } else if (nmlv->uOldState & LVIS_SELECTED) {
                    ListView_SetCheckState(left, nmlv->iItem, FALSE);
                }
                HWND right = GetDlgItem(hDlg, IDC_RESTORE_RIGHTLIST);
                int sel = ListView_GetNextItem(left, -1, LVNI_SELECTED);
                SendMessageW(right, LB_RESETCONTENT, 0, 0);
                if (sel < 0) return TRUE;
                wchar_t name[1024] = {};
                ListView_GetItemText(left, sel, 0, name, 1024);
                fs::path modDir = fs::u8path(g_cfg.penumbraDir) / name;
                try {
                    std::error_code ec;
                    if (fs::exists(modDir, ec) && !ec) {
                        std::vector<std::wstring> zips;
                        for (auto& fe : fs::directory_iterator(modDir, fs::directory_options::skip_permission_denied, ec)) {
                            if (ec) { ec.clear(); continue; }
                            if (fe.is_regular_file(ec) && !ec && fe.path().extension() == L".zip")
                                zips.push_back(fe.path().filename().wstring());
                        }
                        for (auto& z : zips) SendMessageW(right, LB_ADDSTRING, 0, (LPARAM)z.c_str());
                    }
                }
                catch (...) {}
            }
        }
        return TRUE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDC_RESTORE_CLOSE || id == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        if (id == IDC_RESTORE_ALL) {
            // 全选：勾选左侧所有模组文件夹
            HWND left = GetDlgItem(hDlg, IDC_RESTORE_LEFTLIST);
            int count = ListView_GetItemCount(left);
            for (int i = 0; i < count; ++i) ListView_SetCheckState(left, i, TRUE);
            return TRUE;
        }
        if (id == IDC_RESTORE_BTN) {
            HWND left = GetDlgItem(hDlg, IDC_RESTORE_LEFTLIST);

            // 读取左侧选中的文件夹：优先用勾选（复选框）；未勾选任何项时，用高亮选中项（支持 Ctrl/Shift 多选）
            std::vector<int> selIndices;
            int count = ListView_GetItemCount(left);
            for (int i = 0; i < count; ++i) {
                if (ListView_GetCheckState(left, i)) selIndices.push_back(i);
            }
            if (selIndices.empty()) {
                int s = -1;
                while ((s = ListView_GetNextItem(left, s, LVNI_SELECTED)) != -1)
                    selIndices.push_back(s);
            }
            if (selIndices.empty()) { MessageBoxW(hDlg, L"请勾选或选中要恢复的模组文件夹", L"提示", MB_OK); return TRUE; }

            // 收集每个文件夹要恢复的备份
            int success = 0, skip = 0, fail = 0;
            fs::path penRoot = fs::u8path(g_cfg.penumbraDir);
            for (int idx : selIndices) {
                wchar_t name[1024] = {};
                ListView_GetItemText(left, idx, 0, name, 1024);
                fs::path modDir = penRoot / name;
                // 该文件夹勾选的具体备份
                std::vector<fs::path> selectedZips;
                HWND rightList = GetDlgItem(hDlg, IDC_RESTORE_RIGHTLIST);
                int rcount = (int)SendMessageW(rightList, LB_GETCOUNT, 0, 0);
                for (int ri = 0; ri < rcount; ++ri) {
                    if (SendMessageW(rightList, LB_GETSEL, ri, 0)) {
                        wchar_t zname[1024];
                        SendMessageW(rightList, LB_GETTEXT, ri, (LPARAM)zname);
                        selectedZips.push_back(modDir / zname);
                    }
                }
                // 如果右侧没有勾选，默认最新
                if (selectedZips.empty()) {
                    fs::path latest;
                    std::time_t lt = 0;
                    try {
                        std::error_code ec;
                        for (auto& fe : fs::directory_iterator(modDir, fs::directory_options::skip_permission_denied, ec)) {
                            if (ec) { ec.clear(); continue; }
                            if (!fe.is_regular_file(ec) || ec || fe.path().extension() != L".zip") continue;
                            auto ftime = fs::last_write_time(fe.path(), ec);
                            if (ec) continue;
                            std::time_t t = ftime.time_since_epoch().count();
                            if (t > lt) { lt = t; latest = fe.path(); }
                        }
                    }
                    catch (...) {}
                    if (!latest.empty()) selectedZips.push_back(latest);
                }
                if (selectedZips.empty()) { skip++; continue; }

                bool allOk = true;
                for (auto& z : selectedZips) {
                    if (!ExtractZip(z, modDir)) { allOk = false; fail++; }
                    else success++;
                }
                if (!allOk) fail++;
            }
            wchar_t msg[256];
            swprintf_s(msg, L"恢复完成：成功 %d，跳过 %d，失败 %d", success, skip, fail);
            MessageBoxW(hDlg, msg, L"恢复备份", MB_OK);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

// ------------------------------------------------------------------
// 提取英文对话框（v2.3.4）：左侧模组文件夹（勾选），右侧 group_*.json（多选）
// ------------------------------------------------------------------
INT_PTR CALLBACK ExtractDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_INITDIALOG: {
        HWND left = GetDlgItem(hDlg, IDC_EXTRACT_LEFTLIST);
        ListView_SetExtendedListViewStyle(left, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
        LVCOLUMN col = {};
        col.mask = LVCF_WIDTH;
        col.cx = 1000;
        ListView_InsertColumn(left, 0, &col);

        // 填充左侧：所有含 group_*.json 的模组文件夹（按文件统计排序，多的在前方便优先）
        std::map<std::wstring, int> modFiles; // 文件夹名 -> group_*.json 数量
        try {
            if (!g_cfg.penumbraDir.empty()) {
                fs::path penRoot = fs::u8path(g_cfg.penumbraDir);
                std::error_code ec;
                if (fs::exists(penRoot, ec) && !ec) {
                    for (auto& f : ScanGroupFiles(penRoot))
                        modFiles[f.parent_path().filename().wstring()]++;
                }
            }
        }
        catch (...) {}
        std::vector<std::pair<std::wstring, int>> mods(modFiles.begin(), modFiles.end());
        std::sort(mods.begin(), mods.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
        for (auto& m : mods) {
            LVITEM item = {};
            item.mask = LVIF_TEXT;
            item.iItem = INT_MAX;
            std::wstring label = m.first + L"  (" + std::to_wstring(m.second) + L")";
            item.pszText = (LPWSTR)label.c_str();
            ListView_InsertItem(left, &item);
        }
        if (mods.empty()) {
            MessageBoxW(hDlg, L"未找到任何 group_*.json 文件", L"提示", MB_OK);
            EndDialog(hDlg, IDCANCEL);
        }
        return TRUE;
    }
    case WM_NOTIFY: {
        NMHDR* nm = (NMHDR*)lParam;
        if (nm->idFrom == IDC_EXTRACT_LEFTLIST && nm->code == LVN_ITEMCHANGED) {
            NMLISTVIEW* nmlv = (NMLISTVIEW*)lParam;
            if ((nmlv->uChanged & LVIF_STATE)
                && ((nmlv->uNewState & LVIS_SELECTED) != (nmlv->uOldState & LVIS_SELECTED))) {
                HWND left = GetDlgItem(hDlg, IDC_EXTRACT_LEFTLIST);
                // 高亮选中 ↔ 勾选同步（与恢复备份一致）
                if (nmlv->uNewState & LVIS_SELECTED) {
                    ListView_SetCheckState(left, nmlv->iItem, TRUE);
                } else if (nmlv->uOldState & LVIS_SELECTED) {
                    ListView_SetCheckState(left, nmlv->iItem, FALSE);
                }
                // 刷新右侧：当前选中文件夹的 group_*.json
                HWND right = GetDlgItem(hDlg, IDC_EXTRACT_RIGHTLIST);
                SendMessageW(right, LB_RESETCONTENT, 0, 0);
                int sel = ListView_GetNextItem(left, -1, LVNI_SELECTED);
                if (sel < 0) return TRUE;
                wchar_t label[1100] = {};
                ListView_GetItemText(left, sel, 0, label, 1100);
                // 去掉行尾的 "  (N)" 统计后缀，还原真实文件夹名
                std::wstring name = label;
                size_t pos = name.rfind(L"  (");
                if (pos != std::wstring::npos) name = name.substr(0, pos);
                fs::path modDir = fs::u8path(g_cfg.penumbraDir) / name;
                try {
                    std::error_code ec;
                    if (fs::exists(modDir, ec) && !ec) {
                        for (auto& fe : fs::directory_iterator(modDir, fs::directory_options::skip_permission_denied, ec)) {
                            if (ec) { ec.clear(); continue; }
                            if (!fe.is_regular_file(ec) || ec) continue;
                            std::wstring fn = fe.path().filename().wstring();
                            if (fn.rfind(L"group_", 0) == 0 && fe.path().extension() == L".json")
                                SendMessageW(right, LB_ADDSTRING, 0, (LPARAM)fn.c_str());
                        }
                    }
                }
                catch (...) {}
            }
        }
        return TRUE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDC_EXTRACT_CLOSE || id == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        if (id == IDC_EXTRACT_ALL) {
            HWND left = GetDlgItem(hDlg, IDC_EXTRACT_LEFTLIST);
            int count = ListView_GetItemCount(left);
            for (int i = 0; i < count; ++i) ListView_SetCheckState(left, i, TRUE);
            return TRUE;
        }
        if (id == IDC_EXTRACT_BTN) {
            HWND left = GetDlgItem(hDlg, IDC_EXTRACT_LEFTLIST);
            HWND right = GetDlgItem(hDlg, IDC_EXTRACT_RIGHTLIST);

            // 收集左侧勾选的文件夹（未勾选时退回高亮选中项）
            std::vector<int> checked;
            int count = ListView_GetItemCount(left);
            for (int i = 0; i < count; ++i)
                if (ListView_GetCheckState(left, i)) checked.push_back(i);
            if (checked.empty()) {
                int s = -1;
                while ((s = ListView_GetNextItem(left, s, LVNI_SELECTED)) != -1) checked.push_back(s);
            }
            if (checked.empty()) { MessageBoxW(hDlg, L"请勾选要提取的模组文件夹", L"提示", MB_OK); return TRUE; }

            // 右侧当前选中的文件（仅对当前高亮文件夹生效；未选则取该文件夹全部）
            int curSel = ListView_GetNextItem(left, -1, LVNI_SELECTED);
            std::vector<int> selR;
            int rcount = (int)SendMessageW(right, LB_GETCOUNT, 0, 0);
            for (int ri = 0; ri < rcount; ++ri)
                if (SendMessageW(right, LB_GETSEL, ri, 0)) selR.push_back(ri);

            wchar_t curLabel[1100] = {};
            std::wstring curName;
            if (curSel >= 0) {
                ListView_GetItemText(left, curSel, 0, curLabel, 1100);
                curName = curLabel;
                size_t pos = curName.rfind(L"  (");
                if (pos != std::wstring::npos) curName = curName.substr(0, pos);
            }

            fs::path penRoot = fs::u8path(g_cfg.penumbraDir);
            std::vector<fs::path> files;
            for (int idx : checked) {
                wchar_t label[1100] = {};
                ListView_GetItemText(left, idx, 0, label, 1100);
                std::wstring name = label;
                size_t pos = name.rfind(L"  (");
                if (pos != std::wstring::npos) name = name.substr(0, pos);
                fs::path modDir = penRoot / name;
                // 当前高亮文件夹且右侧选中了文件：只取选中文件；否则取文件夹全部 group_*.json
                if (idx == curSel && !selR.empty()) {
                    for (int ri : selR) {
                        wchar_t fn[1024];
                        SendMessageW(right, LB_GETTEXT, ri, (LPARAM)fn);
                        files.push_back(modDir / fn);
                    }
                } else {
                    std::error_code ec;
                    if (fs::exists(modDir, ec) && !ec) {
                        for (auto& fe : fs::directory_iterator(modDir, fs::directory_options::skip_permission_denied, ec)) {
                            if (ec) { ec.clear(); continue; }
                            if (!fe.is_regular_file(ec) || ec) continue;
                            std::wstring fn = fe.path().filename().wstring();
                            if (fn.rfind(L"group_", 0) == 0 && fe.path().extension() == L".json")
                                files.push_back(fe.path());
                        }
                    }
                }
            }
            if (files.empty()) { MessageBoxW(hDlg, L"选中的模组文件夹没有 group_*.json 文件", L"提示", MB_OK); return TRUE; }
            g_extractFiles = files;
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

// ------------------------------------------------------------------
// 单词黑名单.json（词典目录）读写
// ------------------------------------------------------------------
// 从词典目录下的 单词黑名单.json 读取黑名单词（每行一个，或用逗号间隔），合并 defaults 后返回
// v2.3.1：支持 # 注释——以 # 开头（trim 后）的整行为注释行；行内 # 及其之后内容也视为注释，
// 方便在文件里写入使用说明而不被当作黑名单词。
// 兼容旧版：词典目录没有该文件时，回退读取翻译目录下的旧文件
std::vector<std::string> LoadBlacklistFile(const std::vector<std::string>& defaults)
{
    std::vector<std::string> words = defaults;
    std::string base = g_cfg.dictionaryDir.empty() ? g_cfg.translationDir : g_cfg.dictionaryDir;
    if (base.empty()) return words;
    fs::path p = fs::u8path(base) / fs::u8path("单词黑名单.json");
    std::error_code ec;
    if (!fs::exists(p, ec) || ec) {
        // 兼容旧版：词典目录没有时回退到翻译目录
        if (!g_cfg.translationDir.empty() && g_cfg.translationDir != g_cfg.dictionaryDir) {
            fs::path old = fs::u8path(g_cfg.translationDir) / fs::u8path("单词黑名单.json");
            std::error_code oec;
            if (fs::exists(old, oec) && !oec) p = old;
        }
    }
    if (!fs::exists(p, ec) || ec) return words;
    std::string d;
    if (!read_binary_file(p, d)) return words;
    std::stringstream ss(d);
    std::string token;
    while (std::getline(ss, token, '\n')) {
        if (!token.empty() && token.back() == '\r') token.pop_back();
        size_t ts = token.find_first_not_of(" \t");
        if (ts == std::string::npos) continue;      // 空行
        if (token[ts] == '#') continue;             // 整行注释
        std::stringstream ss2(token);
        std::string t;
        while (std::getline(ss2, t, ',')) {
            size_t h = t.find('#');                 // 行内注释：# 及之后忽略
            if (h != std::string::npos) t = t.substr(0, h);
            size_t s = t.find_first_not_of(" \t"); if (s != std::string::npos) t = t.substr(s);
            size_t e = t.find_last_not_of(" \t"); if (e != std::string::npos) t = t.substr(0, e + 1);
            if (!t.empty()) words.push_back(t);
        }
    }
    return words;
}

// 保存黑名单到词典目录下的 单词黑名单.json（v2.3.1：开头写入使用说明，# 注释行不会影响读取）
void SaveBlacklistFile(const std::vector<std::string>& words)
{
    std::string base = g_cfg.dictionaryDir.empty() ? g_cfg.translationDir : g_cfg.dictionaryDir;
    if (base.empty()) return;
    fs::path p = fs::u8path(base) / fs::u8path("单词黑名单.json");
    std::string out =
        "# 单词黑名单.json —— 使用方法\r\n"
        "# 每行写一个英文单词/词组；同一行也可用英文逗号分隔多个词，例如：rue,bibo\r\n"
        "# 以 # 开头的行（或行内 # 及之后的内容）是注释，不会被当作黑名单词\r\n"
        "# 命中黑名单的词在 AI 翻译、词典写入Mod 时保留英文原文，不进行翻译\r\n"
        "# 编辑保存后下次相关操作自动生效，无需重启程序\r\n"
        "# 默认词（可增删，删掉即解除保护）：\r\n";
    for (size_t i = 0; i < words.size(); ++i) {
        if (words[i].empty()) continue;
        out += words[i];
        out += "\r\n";
    }
    if (write_binary_file(p, out))
        Log("已写入 单词黑名单.json（" + std::to_string(words.size()) + " 个词，位于词典目录）");
}

// ------------------------------------------------------------------
// 黑名单编辑：v2.3 起取消二级窗口，直接打开词典目录下的 单词黑名单.json 编辑保存
// ------------------------------------------------------------------

// ------------------------------------------------------------------
// Wiki 分类选择对话框
// ------------------------------------------------------------------
INT_PTR CALLBACK WikiCatsDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    HWND hList = GetDlgItem(hDlg, IDC_WIKI_CATS_LIST);

    switch (message) {
    case WM_INITDIALOG: {
        // 与恢复备份一致：复选框 + 整行高亮；单击行即勾选（LVN_ITEMCHANGED 联动），Ctrl+A / 全选按钮可框选全部
        ListView_SetExtendedListViewStyle(hList, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
        LVCOLUMN col = {};
        col.mask = LVCF_WIDTH;
        col.cx = 1000; // 留足宽度，避免长分类名被截断
        ListView_InsertColumn(hList, 0, &col);
        // 填充列表（行序与 g_wikiCategoryList 一一对应）
        for (const auto& cat : g_wikiCategoryList) {
            LVITEM item = {};
            item.mask = LVIF_TEXT;
            item.iItem = INT_MAX;
            item.pszText = (LPWSTR)cat.display.c_str();
            ListView_InsertItem(hList, &item);
        }
        // 恢复上次勾选状态；无历史时默认勾选第一项（Item/）
        bool anyChecked = false;
        for (size_t i = 0; i < g_wikiCategoryList.size(); ++i) {
            bool selected = false;
            for (const auto& saved : g_cfg.wikiCategories) {
                if (saved == g_wikiCategoryList[i].prefix) { selected = true; break; }
            }
            if (selected) anyChecked = true;
            ListView_SetCheckState(hList, (int)i, selected ? TRUE : FALSE);
        }
        if (!anyChecked && !g_wikiCategoryList.empty())
            ListView_SetCheckState(hList, 0, TRUE);
        return TRUE;
    }
    case WM_NOTIFY: {
        // 点击/选中行时自动勾选复选框，让「勾选」与「选中/全选」等效（与恢复备份一致）
        NMHDR* nm = (NMHDR*)lParam;
        if (nm->idFrom == IDC_WIKI_CATS_LIST && nm->code == LVN_ITEMCHANGED) {
            NMLISTVIEW* nmlv = (NMLISTVIEW*)lParam;
            if ((nmlv->uChanged & LVIF_STATE)
                && ((nmlv->uNewState & LVIS_SELECTED) != (nmlv->uOldState & LVIS_SELECTED))
                && (nmlv->uNewState & LVIS_SELECTED))
                ListView_SetCheckState(hList, nmlv->iItem, TRUE);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDC_WIKI_CATS_ALL) {
            // 全选：整行选中 + 勾选
            for (size_t i = 0; i < g_wikiCategoryList.size(); ++i) {
                ListView_SetItemState(hList, (int)i, LVIS_SELECTED, LVIS_SELECTED);
                ListView_SetCheckState(hList, (int)i, TRUE);
            }
            return TRUE;
        }
        if (id == IDC_WIKI_CATS_NONE) {
            // 取消全选：取消选中 + 取消勾选
            for (size_t i = 0; i < g_wikiCategoryList.size(); ++i) {
                ListView_SetItemState(hList, (int)i, 0, LVIS_SELECTED);
                ListView_SetCheckState(hList, (int)i, FALSE);
            }
            return TRUE;
        }
        if (id == IDC_WIKI_CATS_OK || id == IDOK) {
            std::vector<std::string> selected;
            for (size_t i = 0; i < g_wikiCategoryList.size(); ++i) {
                if (ListView_GetCheckState(hList, (int)i))
                    selected.push_back(g_wikiCategoryList[i].prefix);
            }
            if (selected.empty()) {
                MessageBoxW(hDlg, L"请至少勾选一个分类。", L"提示", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            g_wikiPrefixes = selected;
            g_cfg.wikiCategories = selected;
            SaveConfig();
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        if (id == IDC_WIKI_CATS_CANCEL || id == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

// ------------------------------------------------------------------
// 导入翻译对话框：选择文件 + 是否用词典补全空白项
// ------------------------------------------------------------------

// 子类化文件列表：禁止 Ctrl+A 全选（用户要求点中即勾选、无全选，避免误操作批量写入）
static LRESULT CALLBACK ImportListSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KEYDOWN && wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000))
        return 0;
    WNDPROC oldProc = (WNDPROC)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    return CallWindowProcW(oldProc, hWnd, msg, wParam, lParam);
}

INT_PTR CALLBACK ImportTransDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    HWND hList = GetDlgItem(hDlg, IDC_IMPORT_LIST);

    switch (message) {
    case WM_INITDIALOG: {
        if (g_cfg.translationDir.empty()) {
            MessageBoxW(hDlg, L"请先选择翻译目录。", L"提示", MB_OK | MB_ICONINFORMATION);
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        std::vector<TransFileInfo> files = ScanTransFiles(fs::u8path(g_cfg.translationDir));
        if (files.empty()) {
            MessageBoxW(hDlg, L"翻译目录下没有 *_未翻译.json / *_已翻译.json 文件。\n请先执行『1. 提取英文』。",
                L"提示", MB_OK | MB_ICONINFORMATION);
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        for (const auto& f : files) {
            std::wstring line = f.path.filename().wstring()
                + L"    （已填 " + std::to_wstring(f.filled)
                + L" / 共 " + std::to_wstring(f.total) + L" 条）";
            int idx = (int)SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)line.c_str());
            SendMessageW(hList, LB_SETITEMDATA, (WPARAM)idx, (LPARAM) new fs::path(f.path));
        }
        // 挂上子类化，拦截 Ctrl+A 全选
        SetWindowLongPtrW(hList, GWLP_USERDATA, (LONG_PTR)GetWindowLongPtrW(hList, GWLP_WNDPROC));
        SetWindowLongPtrW(hList, GWLP_WNDPROC, (LONG_PTR)ImportListSubclassProc);
        CheckDlgButton(hDlg, IDC_IMPORT_AUTOFILL, BST_CHECKED);
        return TRUE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDC_IMPORT_OK || id == IDOK) {
            int cnt = (int)SendMessageW(hList, LB_GETCOUNT, 0, 0);
            g_importFiles.clear();
            for (int i = 0; i < cnt; ++i) {
                if (SendMessageW(hList, LB_GETSEL, (WPARAM)i, 0) > 0) {
                    fs::path* p = (fs::path*)SendMessageW(hList, LB_GETITEMDATA, (WPARAM)i, 0);
                    if (p) g_importFiles.push_back(*p);
                }
            }
            if (g_importFiles.empty()) {
                MessageBoxW(hDlg, L"请至少勾选一个文件（点行即勾选，可多选）。", L"提示", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            g_importAutoFill = (IsDlgButtonChecked(hDlg, IDC_IMPORT_AUTOFILL) == BST_CHECKED);
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        if (id == IDC_IMPORT_CANCEL || id == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    case WM_DESTROY: {
        int cnt = (int)SendMessageW(hList, LB_GETCOUNT, 0, 0);
        for (int i = 0; i < cnt; ++i) {
            fs::path* p = (fs::path*)SendMessageW(hList, LB_GETITEMDATA, (WPARAM)i, 0);
            delete p;
            SendMessageW(hList, LB_SETITEMDATA, (WPARAM)i, 0);
        }
        return TRUE;
    }
    }
    return FALSE;
}

// ------------------------------------------------------------------
// 关于
// ------------------------------------------------------------------
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message) {
    case WM_INITDIALOG: return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, LOWORD(wParam));
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ------------------------------------------------------------------
// 主对话框
// ------------------------------------------------------------------
INT_PTR CALLBACK MainDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    try {
    switch (message) {
    case WM_INITDIALOG: {
        g_hMainWnd = hDlg;
        g_hLogEdit = GetDlgItem(hDlg, IDC_LOG_EDIT);
        // 使用 RichEdit 风格（普通 Edit 也够用）
        HWND prog = GetDlgItem(hDlg, IDC_PROGRESS);
        if (prog) SendMessageW(prog, PBM_SETRANGE32, 0, 100);
        EnableWindow(GetDlgItem(hDlg, IDC_BTN_CANCEL), FALSE);
        LoadConfig();
        LoadCustomSaves(); // 读取用户配置 config.user.json 的 customSaves，用户自定义的 Key/模型/地址优先并入
        CleanupJsonBakFiles(); // 自动清理各目录残留的 .json.bak 手动备份

        // 记录 .rc 设计尺寸对应的客户区像素，作为等比缩放基准（必须在 SetWindowPos 之前）
        RECT rc0;
        GetClientRect(hDlg, &rc0);
        g_dlgW0 = rc0.right;
        g_dlgH0 = rc0.bottom;

        // 恢复上次退出时的窗口位置和大小（记忆窗口大小）
        // 必须放在任何可能触发 WM_SIZE 的操作之前，否则默认大小会覆盖记忆值
        if (g_cfg.winW > 0 && g_cfg.winH > 0) {
            int x = (g_cfg.winX >= 0) ? g_cfg.winX : CW_USEDEFAULT;
            int y = (g_cfg.winY >= 0) ? g_cfg.winY : CW_USEDEFAULT;
            SetWindowPos(hDlg, nullptr, x, y, g_cfg.winW, g_cfg.winH, SWP_NOZORDER);
            // 若窗口中心已落在虚拟屏幕外（分辨率/显示器变化），重置回主屏居中
            RECT wr;
            GetWindowRect(hDlg, &wr);
            int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            int cx = (wr.left + wr.right) / 2, cy = (wr.top + wr.bottom) / 2;
            if (cx < vx || cx > vx + vw || cy < vy || cy > vy + vh) {
                int sx = (GetSystemMetrics(SM_CXSCREEN) - (wr.right - wr.left)) / 2;
                int sy = (GetSystemMetrics(SM_CYSCREEN) - (wr.bottom - wr.top)) / 2;
                SetWindowPos(hDlg, nullptr, (sx > 0) ? sx : 0, (sy > 0) ? sy : 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            }
        }

        // 记录所有子窗口初始位置（基于 .rc 设计尺寸），用于后续纯等比缩放
        g_layout.clear();
        HWND child = GetWindow(hDlg, GW_CHILD);
        while (child) {
            RECT rc;
            GetWindowRect(child, &rc);
            ScreenToClient(hDlg, (LPPOINT)&rc);
            ScreenToClient(hDlg, ((LPPOINT)&rc) + 1);
            g_layout.push_back({ child, rc });
            child = GetWindow(child, GW_HWNDNEXT);
        }

        CreateUiFont();
        ApplyFontToDialog(hDlg);
        RefreshConfigUI();
        EnableEditSelectAll(hDlg); // 所有填写框支持 Ctrl+A 全选
        Log("FFXIV 模组汉化工具已启动");
        {
            std::string names;
            for (const auto& p : g_cfg.aiPresets) { if (!names.empty()) names += "、"; names += p.name; }
            Log("[AI] 已加载 " + std::to_string(g_cfg.aiPresets.size()) + " 个 AI 预设：" + names);
        }
        if (g_cfg.winW > 0 && g_cfg.winH > 0)
            Log("[窗口] 已恢复上次大小：" + std::to_string(g_cfg.winW) + " x " + std::to_string(g_cfg.winH));
        if (!g_cfg.penumbraDir.empty()) Log("Penumbra 目录：" + g_cfg.penumbraDir);
        if (g_cfg.dictionaryDir.empty())
            Log("[提示] 未找到 config.user.json（首次运行？）。请在『词典目录』处选择一次目录建立用户配置；旧版升级则选择原词典目录即可自动迁移已有设置");
        // 创建初：若汇总词典 汇总词典.json 不存在，则从
        // wiki_术语对照.json + 汉化总词典.json 一次性合并生成（wiki 固定优先，先来后到）。
        // 之后程序不再自动重写它，改已翻译的词条请直接编辑该文件；单独修正的词汇可写 个性翻译.json。
        if (!g_cfg.dictionaryDir.empty()) {
            fs::path dictFile = MergedDictPath(fs::u8path(g_cfg.dictionaryDir));
            std::error_code dec0;
            bool dictExists = fs::exists(dictFile, dec0) && !dec0;
            EnsureDictFile();
            if (!dictExists) {
                std::error_code dec1;
                if (fs::exists(dictFile, dec1) && !dec1)
                    Log("[提示] 已生成唯一词典 汇总词典.json（wiki 固定优先 + 个人填充，先来后到；改词请直接编辑该文件）");
            }
        }
        LayoutMainDlg(hDlg);
        g_initialized = true; // 此后 WM_SIZE/WM_MOVE 的记录才生效
        return TRUE;
    }

    case WM_SIZE: {
        LayoutMainDlg(hDlg);
        // 初始化完成前不记录，避免创建期的默认大小覆盖 config 中的记忆值；
        // 最小化时窗口矩形不在屏幕上，同样不记录
        if (g_initialized && wParam != SIZE_MINIMIZED) {
            RECT wrc;
            GetWindowRect(hDlg, &wrc);
            g_cfg.winW = wrc.right - wrc.left;
            g_cfg.winH = wrc.bottom - wrc.top;
        }
        return TRUE;
    }

    case WM_MOVE: {
        if (g_initialized && !IsIconic(hDlg)) {
            RECT wrc;
            GetWindowRect(hDlg, &wrc);
            g_cfg.winX = wrc.left;
            g_cfg.winY = wrc.top;
        }
        return TRUE;
    }

    case WM_GETMINMAXINFO: {
        // 限制最小为 .rc 设计尺寸的 55%，再小文字和间距会太难用
        if (g_dlgW0 > 0 && g_dlgH0 > 0) {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            mmi->ptMinTrackSize.x = (LONG)(g_dlgW0 * 0.55f);
            mmi->ptMinTrackSize.y = (LONG)(g_dlgH0 * 0.55f);
        }
        return TRUE;
    }

    case WM_APP_LOG: {
        std::wstring* w = (std::wstring*)lParam;
        if (w) { AppendLogText(*w); delete w; }
        return TRUE;
    }
    case WM_APP_DONE: {
        g_busy = false;
        // 重新启用按钮
        EnableWindow(GetDlgItem(hDlg, IDC_BTN_EXTRACT), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_BTN_IMPORT), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_BTN_APPLY), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_BTN_RESTORE), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_BTN_BLACKLIST), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_BTN_CUSTOM), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_BTN_WIKI), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_BTN_AI_TRANSLATE), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_BTN_AI_TEST), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_BTN_CANCEL), FALSE);
        g_cancel = false;
        // 操作结束：进度条归零
        HWND prog = GetDlgItem(hDlg, IDC_PROGRESS);
        if (prog) {
            SendMessageW(prog, PBM_SETMARQUEE, (WPARAM)FALSE, 0);
            SendMessageW(prog, PBM_SETPOS, 0, 0);
        }
        // 操作完成统一提示
        Log(std::string("[完成] ") + (g_workerName.empty() ? "操作" : g_workerName) + " 结束");
        g_workerName.clear();
        return TRUE;
    }

    case WM_APP_PROGRESS: {
        HWND prog = GetDlgItem(hDlg, IDC_PROGRESS);
        if (!prog) return TRUE;
        int cur = (int)(INT_PTR)wParam;
        int max = (int)(INT_PTR)lParam;
        if (max <= 0) {
            // 不确定进度：开启滚动
            SendMessageW(prog, PBM_SETMARQUEE, (WPARAM)TRUE, 0);
        }
        else {
            SendMessageW(prog, PBM_SETMARQUEE, (WPARAM)FALSE, 0);
            SendMessageW(prog, PBM_SETRANGE32, 0, max);
            SendMessageW(prog, PBM_SETPOS, cur, 0);
        }
        return TRUE;
    }

    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        int wmEvent = HIWORD(wParam);

        switch (wmId) {
        case IDM_ABOUT:
            DialogBoxW(hInst, MAKEINTRESOURCEW(IDD_ABOUTBOX), hDlg, About);
            break;
        case IDM_EXIT:
            EndDialog(hDlg, IDOK);
            break;

        // 浏览目录
        case IDC_BTN_BROWSE_PENUMBRA: {
            std::wstring dir;
            if (SelectDirDialog(hDlg, dir)) {
                g_cfg.penumbraDir = wstring_to_utf8(dir);
                SetEditText(hDlg, IDC_EDIT_PENUMBRA, dir);
            }
            break;
        }
        case IDC_BTN_BROWSE_TRANS: {
            std::wstring dir;
            if (SelectDirDialog(hDlg, dir)) {
                std::string newT = wstring_to_utf8(dir);
                if (newT != g_cfg.translationDir) {
                    // v2.2.8：翻译目录变更，把旧目录的翻译产物迁移过去（新目录有同名则保留新目录）
                    MigrateDirFiles(fs::u8path(g_cfg.translationDir), fs::u8path(newT),
                        { L"翻译失败.json" }, { L"_未翻译", L"_已翻译", L"_翻译检查报告" });
                    g_cfg.translationDir = newT;
                }
                std::error_code dec;
                fs::create_directories(fs::u8path(newT), dec); // 确定路径即建立目录
                SetEditText(hDlg, IDC_EDIT_TRANSLATION, dir);
            }
            break;
        }
        case IDC_BTN_BROWSE_DICT: {
            std::wstring dir;
            if (SelectDirDialog(hDlg, dir)) {
                std::string oldDict = g_cfg.dictionaryDir;   // 记住旧目录（LoadConfigFrom 可能覆盖 g_cfg）
                std::string newDir = wstring_to_utf8(dir);
                // 兼容旧版：程序目录还没有用户配置时，若所选词典目录里有旧版 config.json，读取并迁移
                fs::path exeCfg = GetExeDir() / "config.user.json";
                std::error_code eec;
                if (!fs::exists(exeCfg, eec)) {
                    fs::path legacy = fs::u8path(oldDict) / "config.json";
                    std::error_code lec;
                    if (fs::exists(legacy, lec) && !lec) {
                        if (LoadConfigFrom(legacy, true))
                            Log("[提示] 已从词典目录的旧 config.json 迁移配置（不覆盖旧文件）");
                        else
                            Log("[提示] 检测到词典目录有旧 config.json，但读取失败");
                    }
                    g_cfg.dictionaryDir = oldDict; // 用户尚未确认前保持旧值，保证迁移源正确
                }
                // v2.2.8：确定目录即把旧目录的词典文件迁移过来，并确保 汇总词典/个性翻译/单词黑名单 建立
                OnDictionaryDirChanged(newDir, true);
                SetEditText(hDlg, IDC_EDIT_DICTIONARY, dir); // g_cfg 已更新，EN_CHANGE 不会重复处理
                RefreshConfigUI();
            }
            break;
        }

        // 打开目标文件夹
        case IDC_BTN_OPEN_PENUMBRA: {
            OpenExplorer(hDlg, GetEditText(hDlg, IDC_EDIT_PENUMBRA));
            break;
        }
        case IDC_BTN_OPEN_TRANS: {
            OpenExplorer(hDlg, GetEditText(hDlg, IDC_EDIT_TRANSLATION));
            break;
        }
        case IDC_BTN_OPEN_DICT: {
            OpenExplorer(hDlg, GetEditText(hDlg, IDC_EDIT_DICTIONARY));
            break;
        }

        // 选项同步到配置
        case IDC_EDIT_PENUMBRA:
            if (wmEvent == EN_CHANGE) g_cfg.penumbraDir = wstring_to_utf8(GetEditText(hDlg, IDC_EDIT_PENUMBRA));
            break;
        case IDC_EDIT_TRANSLATION:
            if (wmEvent == EN_CHANGE) g_cfg.translationDir = wstring_to_utf8(GetEditText(hDlg, IDC_EDIT_TRANSLATION));
            break;
        case IDC_EDIT_DICTIONARY: {
            std::string v = wstring_to_utf8(GetEditText(hDlg, IDC_EDIT_DICTIONARY));
            if (wmEvent == EN_CHANGE) {
                if (!v.empty() && v != g_cfg.dictionaryDir) {
                    std::error_code ve;
                    // v2.2.8：输入的是已存在目录即视为「确定」，立即迁移并建立词典文件
                    if (fs::is_directory(fs::u8path(v), ve) && !ve)
                        OnDictionaryDirChanged(v, false);
                    else
                        g_cfg.dictionaryDir = v; // 仍在输入/目录尚不存在，先记录
                }
            }
            break;
        }
        // AI 翻译设置同步到配置
        case IDC_AI_KEY:
            // 手动输入/修改：作为当前 Key 使用（保存为条目需点「保存」）
            if (wmEvent == EN_CHANGE)
                g_cfg.aiApiKey = wstring_to_utf8(GetEditText(hDlg, IDC_AI_KEY));
            break;
        case IDC_AI_KEY_NAME:
            if (wmEvent == EN_CHANGE) {
                g_cfg.aiKeyName = wstring_to_utf8(GetEditText(hDlg, IDC_AI_KEY_NAME));
                AutoFitEditWidth(hDlg, IDC_AI_KEY_NAME, 60, 121); // 备注框宽度随内容自适应（60~121 DLU）
            }
            break;
        case IDC_BTN_SAVE_KEY:
            // 统一保存：Key + 模型名 + API 地址（+ 备注名）一起存为一条自定义记录到用户配置 config.user.json 的 customSaves
            if (wmEvent == BN_CLICKED) SaveCustomSaves(hDlg);
            break;
        case IDC_AI_MODEL:
            // 模型名与 API 地址完全独立：输入模型名只更新模型名，不联动地址
            if (wmEvent == EN_CHANGE)
                g_cfg.aiModel = wstring_to_utf8(GetEditText(hDlg, IDC_AI_MODEL));
            break;
        case IDC_AI_BASEURL:
            // 地址与模型名完全独立：输入地址只更新地址，不联动模型名
            if (wmEvent == EN_CHANGE)
                g_cfg.aiBaseUrl = wstring_to_utf8(GetEditText(hDlg, IDC_AI_BASEURL));
            break;
        case IDC_BTN_AI_SELECT: {
            // 打开配置列表窗口：勾选内置预设 / 自定义记录后整套套用
            if (wmEvent != BN_CLICKED || g_busy) break;
            if (DialogBoxW(hInst, MAKEINTRESOURCEW(IDD_AI_SELECT_DIALOG), hDlg, SelectAICfgDlgProc) == IDOK) {
                if (g_aiSelResult >= 0 && g_aiSelResult < (int)g_aiSelItems.size()) {
                    const auto& it = g_aiSelItems[g_aiSelResult];
                    // v2.3.2：预设/自定义记录自带 Key 则套用；无 Key 则清空 Key 与名称，
                    // 避免切换预设后 API_Key 框仍保留上一个预设的 Key。
                    if (!it.key.empty()) { g_cfg.aiApiKey = it.key; g_cfg.aiKeyName = it.name; }
                    else { g_cfg.aiApiKey.clear(); g_cfg.aiKeyName.clear(); }
                    if (!it.model.empty()) g_cfg.aiModel = it.model;
                    if (!it.baseUrl.empty()) g_cfg.aiBaseUrl = it.baseUrl;
                    RefreshConfigUI();
                    SaveConfig();
                    Log("[完成] 已套用配置「" + it.name + "」：模型 " + g_cfg.aiModel + "，地址 " + g_cfg.aiBaseUrl);
                }
            }
            break;
        }
        case IDC_AI_BATCH: {
            if (wmEvent == EN_CHANGE) {
                int b = _wtoi(GetEditText(hDlg, IDC_AI_BATCH).c_str());
                if (b >= 1 && b <= 1000) g_cfg.aiBatchSize = b; // v2.3.4：上限提高到 1000
            }
            break;
        }
        case IDC_BTN_SHOW_KEY: {
            g_keyVisible = !g_keyVisible;
            ApplyKeyPasswordStyle(hDlg);
            SetDlgItemTextW(hDlg, IDC_BTN_SHOW_KEY, g_keyVisible ? L"隐藏" : L"显示");
            break;
        }
        case IDC_CHK_SWAP:
            if (wmEvent == BN_CLICKED) {
                g_cfg.swapWordOrder = (IsDlgButtonChecked(hDlg, IDC_CHK_SWAP) == BST_CHECKED);
                SaveConfig();
                if (g_cfg.swapWordOrder)
                    Log("[提示] 已开启「词序调换」：应用翻译后名称显示为『中文（英文）』。直接点『3. 词典写入Mod』即可生效，无需重新提取/导入。");
                else
                    Log("[提示] 已关闭「词序调换」：应用翻译恢复为『英文（中文）』格式。直接点『3. 词典写入Mod』即可生效。");
            }
            break;
        case IDC_CHK_BACKUP:
            if (wmEvent == BN_CLICKED) g_cfg.autoBackup = (IsDlgButtonChecked(hDlg, IDC_CHK_BACKUP) == BST_CHECKED);
            break;
        case IDC_CHK_AUTO_FONT:
            if (wmEvent == BN_CLICKED) {
                g_cfg.autoFontSize = (IsDlgButtonChecked(hDlg, IDC_CHK_AUTO_FONT) == BST_CHECKED);
                SaveConfig();
                if (g_cfg.autoFontSize)
                    Log("[提示] 已开启「联动字体」：拉伸窗口时会自动按比例放大/缩小字号。");
                else
                    Log("[提示] 已关闭「联动字体」：窗口缩放不再改变字号，保持当前字体大小。");
                LayoutMainDlg(hDlg);
            }
            break;
        case IDC_RADIO_PURE_CN:
        case IDC_RADIO_CN_EN:
            if (wmEvent == BN_CLICKED) g_cfg.pureChinese = (IsDlgButtonChecked(hDlg, IDC_RADIO_PURE_CN) == BST_CHECKED);
            break;

        // 动作
        case IDC_BTN_EXTRACT: {
            if (g_busy) break;
            // v2.3.4：先弹二级窗口选择要提取的模组文件夹 / group_*.json 文件
            g_extractFiles.clear();
            INT_PTR extractRet = DialogBoxW(hInst, MAKEINTRESOURCEW(IDD_EXTRACT_DIALOG), hDlg, ExtractDlgProc);
            if (extractRet != IDOK) break; // 未确认选择：不执行提取
            SaveConfig();
            g_busy = true;
            g_cancel = false;
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_EXTRACT), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_IMPORT), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_APPLY), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_RESTORE), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_BLACKLIST), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_CUSTOM), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_WIKI), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_AI_TRANSLATE), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_AI_TEST), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_CHECK), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_CANCEL), TRUE);
            Log("===== 开始提取英文 =====");
            LaunchWorker(RunExtractThread, "提取英文");
            break;
        }
        case IDC_BTN_IMPORT: {
            if (g_busy) break;
            // 弹窗选择要导入的翻译文件，以及是否用词典补全空白项
            INT_PTR ret = DialogBoxW(hInst, MAKEINTRESOURCEW(IDD_IMPORT_TRANS_DIALOG), hDlg, ImportTransDlgProc);
            if (ret != IDOK) break;
            SaveConfig();
            g_busy = true;
            g_cancel = false;
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_EXTRACT), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_IMPORT), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_APPLY), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_RESTORE), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_BLACKLIST), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_CUSTOM), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_WIKI), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_AI_TRANSLATE), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_AI_TEST), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_CHECK), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_CANCEL), TRUE);
            Log("===== 开始导入翻译 =====");
            LaunchWorker(RunImportThread, "导入翻译");
            break;
        }
        case IDC_BTN_APPLY: {
            if (g_busy) break;
            SaveConfig();
            g_busy = true;
            g_cancel = false;
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_EXTRACT), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_IMPORT), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_APPLY), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_RESTORE), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_BLACKLIST), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_CUSTOM), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_WIKI), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_AI_TRANSLATE), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_AI_TEST), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_CHECK), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_CANCEL), TRUE);
            if (g_cfg.pureChinese)
                Log("[提示] 当前为「纯中文」模式：应用后名称仅保留中文（去掉英文括号）。");
            else if (g_cfg.swapWordOrder)
                Log("[提示] 当前为「中文（英文）在前」模式（已勾选词序调换）：应用后名称显示为『中文（英文）』。");
            else
                Log("[提示] 当前为默认对照格式：应用后名称显示为『英文（中文）』；如需『中文（英文）』请在应用设置里勾选「词序调换」。");
            Log("===== 开始应用翻译 =====");
            LaunchWorker(RunApplyThread, "应用翻译");
            break;
        }
        case IDC_BTN_CHECK: {
            if (g_busy) break;
            SaveConfig();
            g_busy = true;
            g_cancel = false;
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_EXTRACT), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_IMPORT), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_APPLY), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_RESTORE), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_BLACKLIST), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_CUSTOM), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_WIKI), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_AI_TRANSLATE), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_AI_TEST), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_CHECK), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_CANCEL), TRUE);
            Log("===== 开始检查翻译 =====");
            LaunchWorker(RunCheckThread, "检查翻译");
            break;
        }
        case IDC_BTN_RESTORE: {
            if (g_busy) break;
            DialogBoxW(hInst, MAKEINTRESOURCEW(IDD_RESTORE_DIALOG), hDlg, RestoreDlgProc);
            break;
        }
        case IDC_BTN_BLACKLIST: {
            if (g_busy) break;
            OpenDictJson(hDlg, "单词黑名单.json");
            break;
        }
        case IDC_BTN_CUSTOM: { // 个性翻译：直接打开词典目录下的 个性翻译.json
            if (g_busy) break;
            OpenDictJson(hDlg, "个性翻译.json");
            break;
        }
        case IDC_BTN_WIKI: {
            if (g_busy) break;
            // 先弹窗让用户选择要导出的分类
            INT_PTR ret = DialogBoxW(hInst, MAKEINTRESOURCEW(IDD_WIKI_CATS_DIALOG), hDlg, WikiCatsDlgProc);
            if (ret != IDOK) break;
            SaveConfig();
            g_busy = true;
            g_cancel = false;
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_EXTRACT), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_IMPORT), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_APPLY), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_RESTORE), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_BLACKLIST), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_CUSTOM), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_WIKI), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_AI_TRANSLATE), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_CHECK), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_CANCEL), TRUE);
            Log("===== 开始 Wiki 全站导出 =====");
            LaunchWorker(WikiImportThread, "Wiki 导出");
            break;
        }
        case IDC_BTN_APPLY_FONT: {
            wchar_t buf[16] = {};
            GetDlgItemTextW(hDlg, IDC_EDIT_FONT_SIZE, buf, 16);
            int size = _wtoi(buf);
            if (size < 8) size = 8;
            if (size > 24) size = 24;
            g_cfg.fontSize = size;
            CreateUiFont();
            ApplyFontToDialog(hDlg);
            SetDlgItemTextW(hDlg, IDC_EDIT_FONT_SIZE, std::to_wstring(size).c_str());
            SaveConfig();
            LayoutMainDlg(hDlg);
            Log("已应用字体大小：" + std::to_string(size) + " 号");
            return TRUE;
        }
        case IDC_BTN_AI_TEST: {
            if (g_busy) break;
            SyncAISettingsFromUI(hDlg); // 界面即真值，先同步再测试
            if (g_cfg.aiApiKey.empty()) {
                MessageBoxW(hDlg, L"尚未配置 AI API_Key，请在上方『AI 翻译设置』中填写。", L"提示", MB_OK | MB_ICONINFORMATION);
                break;
            }
            SaveConfig();
            g_busy = true;
            g_cancel = false;
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_EXTRACT), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_IMPORT), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_APPLY), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_RESTORE), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_BLACKLIST), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_CUSTOM), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_WIKI), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_AI_TRANSLATE), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_AI_TEST), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_CHECK), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_CANCEL), TRUE);
            Log("===== 开始测试 AI 连接 =====");
            LaunchWorker(RunAITestThread, "AI 测试");
            break;
        }
        case IDC_BTN_AI_TRANSLATE: {
            if (g_busy) break;
            SyncAISettingsFromUI(hDlg); // 界面即真值，先同步再翻译
            if (g_cfg.aiApiKey.empty()) {
                MessageBoxW(hDlg, L"尚未配置 AI API_Key，请在上方『AI 翻译设置』中填写。", L"提示", MB_OK | MB_ICONINFORMATION);
                break;
            }
            std::string aiWarn = CheckAIMatch();
            if (!aiWarn.empty()) Log("[提示] " + aiWarn);
            SaveConfig();
            g_busy = true;
            g_cancel = false;
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_EXTRACT), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_IMPORT), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_APPLY), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_RESTORE), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_BLACKLIST), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_CUSTOM), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_WIKI), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_AI_TRANSLATE), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_AI_TEST), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_CHECK), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_CANCEL), TRUE);
            Log("===== 开始 AI 自动翻译 =====");
            LaunchWorker(RunAITranslateThread, "AI 翻译");
            break;
        }
        case IDC_BTN_CANCEL: {
            if (g_busy) {
                g_cancel = true;
                Log("[提示] 已请求中断当前操作...");
                EnableWindow(GetDlgItem(hDlg, IDC_BTN_CANCEL), FALSE);
                // 立即打断正在进行的网络请求（若有），不必等它超时或返回
                HINTERNET hReq = g_hActiveReq;
                if (hReq) { InternetCloseHandle(hReq); g_hActiveReq = nullptr; }
            }
            break;
        }
        default:
            break;
        }
        break;
    }

    case WM_CLOSE:
        // 退出前直接读取当前窗口矩形，确保记忆的是关闭瞬间的真实大小/位置
        if (!IsIconic(hDlg)) {
            RECT wrc;
            GetWindowRect(hDlg, &wrc);
            g_cfg.winX = wrc.left;
            g_cfg.winY = wrc.top;
            g_cfg.winW = wrc.right - wrc.left;
            g_cfg.winH = wrc.bottom - wrc.top;
        }
        SaveConfig(); // 确保窗口大小与 AI 设置等改动在退出前落盘
        EndDialog(hDlg, IDOK);
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    }
    }
    catch (const std::system_error& e) { Log(std::string("[错误] 主窗口系统异常: ") + e.what()); }
    catch (const std::exception& e) { Log(std::string("[错误] 主窗口异常: ") + e.what()); }
    catch (...) { Log("[错误] 主窗口未知异常"); }
    return FALSE;
}

// ------------------------------------------------------------------
// WinMain
// ------------------------------------------------------------------
int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                      _In_opt_ HINSTANCE hPrevInstance,
                      _In_ LPWSTR lpCmdLine,
                      _In_ int nCmdShow)
{
    try {
        UNREFERENCED_PARAMETER(hPrevInstance);
        UNREFERENCED_PARAMETER(lpCmdLine);
        hInst = hInstance;

        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&icc);

        // 加载 RichEdit 控件所需的 DLL
        if (!LoadLibraryW(L"riched20.dll")) {
            MessageBoxW(nullptr, L"无法加载 riched20.dll，日志颜色功能将不可用。", L"启动警告", MB_OK | MB_ICONWARNING);
        }

        DialogBoxW(hInstance, MAKEINTRESOURCEW(IDD_FFXIVMOD_DIALOG), nullptr, MainDlgProc);

        return 0;
    }
    catch (const std::system_error& e) {
        MessageBoxA(nullptr, e.what(), "启动失败（系统异常）", MB_OK | MB_ICONERROR);
        return 1;
    }
    catch (const std::exception& e) {
        MessageBoxA(nullptr, e.what(), "启动失败", MB_OK | MB_ICONERROR);
        return 1;
    }
    catch (...) {
        MessageBoxA(nullptr, "未知异常", "启动失败", MB_OK | MB_ICONERROR);
        return 1;
    }
}
