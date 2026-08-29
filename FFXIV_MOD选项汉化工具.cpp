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

AppConfig g_cfg;
HINSTANCE hInst = nullptr;
HWND g_hMainWnd = nullptr;
HWND g_hLogEdit = nullptr;
HFONT g_hFont = nullptr;        // 主界面自定义字体（随字体大小重建）
std::atomic<bool> g_busy{ false };
std::atomic<bool> g_cancel{ false };
std::mutex g_logMutex;
std::vector<std::wstring> g_logBuffer; // 日志缓冲：改字号后重放，恢复各行的标签颜色
std::string g_workerName;
std::vector<std::string> g_wikiPrefixes; // Wiki 导出当前选中的分类
fs::path g_importFile;   // 导入翻译：选定的文件
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
INT_PTR CALLBACK BlacklistDlgProc(HWND, UINT, WPARAM, LPARAM);
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
std::vector<fs::path> ScanGroupFiles(const fs::path& root);
bool ExtractEnglish();
bool ApplyTranslation();
static void FillAIPresetCombos(HWND hDlg);
static void FillAIKeyCombo(HWND hDlg);
bool ImportTranslations(const fs::path& inFile, bool autoFill);
std::vector<std::string> LoadBlacklistFile(const std::vector<std::string>& defaults);
void SaveBlacklistFile(const std::vector<std::string>& words);
void WikiImportThread();
void RunExtractThread();
void RunImportThread();
void RunApplyThread();
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
    g_logBuffer.push_back(text);
    // 限制缓冲大小，避免长期运行内存无限增长（重放时只保留最近部分）
    if (g_logBuffer.size() > 10000)
        g_logBuffer.erase(g_logBuffer.begin(), g_logBuffer.begin() + (g_logBuffer.size() - 5000));
    AppendLogLineLocked(text);
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
    { "智谱 GLM", "glm-4.7-flash", "https://open.bigmodel.cn/api/paas/v4", "智谱 AI 开放平台（OpenAI 兼容）" },
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
        if (j.contains("blacklist")) g_cfg.blacklist = j["blacklist"].get<std::vector<std::string>>();
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
bool ExtractEnglish()
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

    // 读取黑名单（默认内置跳过词 + 翻译目录下 单词黑名单.json + 配置）
    std::set<std::string> blacklist = { "---", "-" };
    for (auto& w : LoadBlacklistFile(g_cfg.blacklist)) {
        std::string t = w;
        if (!t.empty()) blacklist.insert(t);
    }

    // 扫描所有模组文件夹
    struct ModEntry { fs::path folder; std::vector<fs::path> files; bool hasPending = false; };
    std::map<std::wstring, ModEntry> modMap; // key: 模组文件夹名

    std::vector<fs::path> allFiles = ScanGroupFiles(penRoot);
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
        {"2. 格式要求", "所有翻译结果统一为\"中文（英文）\"格式，例如\"治疗（Cure）\"。除非满足第3条，否则不得省略英文。"},
        {"3. 去重规则", "仅当括号内的英文与括号外的中文内容完全一致（即意思完全相同）时，才可去掉括号，只保留中文。若中英文意思不同（如版本区分），则必须保留括号及英文。"},
        {"4. 特殊保留项", "纯数字、百分比（如75%）、版本号，以及 MOD 专有名词（如 Yiggle、Rue、Bibo、EXQB、YAB 等）直接保留原文，不翻译。"},
        {"5. 不确定翻译", "如遇到无法确定译名的专有名词，请保留英文原词，不要强行机翻。"},
        {"6. 文件命名", "翻译完成后，将文件名中的\"_未翻译\"改为\"_已翻译\"。"},
        {"7. 交付方式", "每次修改后，请直接提供完整的 JSON 文件内容。"},
        {"说明", "将英文翻译为中文。请保持 JSON 结构，仅填写 _options 和 _descriptions 的翻译。"},
        {"格式提示", "请按照 中文（英文） 格式填写翻译，例如 发型1（Hairstyle 1）。纯中文模式下只填中文。"}
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

    // 黑名单过滤（剔除含黑名单单词的词条，单词=子串包含匹配）
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
// 应用翻译
// ------------------------------------------------------------------
static void EnsureDictFile(); // 前向声明：唯一词典确保函数（定义见「词典工具与导入翻译」节）
bool ApplyTranslation()
{
    try {
    if (g_cfg.penumbraDir.empty()) { Log("[错误] 未设置 Penumbra 目录"); return false; }
    if (g_cfg.dictionaryDir.empty()) { Log("[错误] 未设置词典目录"); return false; }

    fs::path penRoot = fs::u8path(g_cfg.penumbraDir);
    EnsureDictFile(); // 首次生成唯一词典 wiki_术语对照and个人填充.json
    fs::path dictPath = fs::u8path(g_cfg.dictionaryDir) / fs::u8path("wiki_术语对照and个人填充.json");
    std::error_code existEc;
    json dict;
    if (!fs::exists(dictPath, existEc) || existEc) {
        // 唯一词典不存在：从空对象开始，后面自动合并 *_已翻译.json 建立
        dict = json::object();
        Log("[提示] wiki_术语对照and个人填充.json 不存在，将根据 *_已翻译.json 自动建立");
    } else {
        std::string d;
        if (!read_binary_file(dictPath, d)) { Log("[错误] 无法读取词典"); return false; }
        try { dict = json::parse(clean_utf8(d)); }
        catch (...) { Log("[错误] 词典解析失败"); return false; }
    }
    if (!dict.is_object()) dict = json::object();
    if (!dict.contains("_options")) dict["_options"] = json::object();
    if (!dict.contains("_descriptions")) dict["_descriptions"] = json::object();
    if (!dict.contains("terms")) dict["terms"] = json::object();

    // 自动扫描翻译目录下所有 *_已翻译.json：
    // 1) 把没进唯一词典的条目编入 wiki_术语对照and个人填充.json（不覆盖已有非空翻译），并写回文件
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
        fs::path wikiPath = fs::u8path(g_cfg.dictionaryDir) / fs::u8path("wiki_术语对照and个人填充.json");
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
                    // trans 是纯中文
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
        auto applyString = [&](const std::string& original, const std::string& dictTrans) -> std::string {
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
                            std::string res = applyString(orig, trans);
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
                                std::string res = applyString(orig, trans);
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
// 确保唯一词典文件 wiki_术语对照and个人填充.json 存在（仅首次生成）。
// 该文件是程序唯一的词典，含三部分：
//   terms         英文->中文 固定映射（wiki 导出 + 个人填充，先来后到），补全/回滚用
//   _options      完整 key（路径||Name||原文）翻译，应用翻译用
//   _descriptions 同上
// 首次生成规则：wiki_术语对照.json 的词条固定优先，汉化总词典.json 的条目仅补充。
// 生成后程序不再自动重写本文件——要修改已翻译的词条，直接编辑本文件即可。
static void EnsureDictFile()
{
    if (g_cfg.dictionaryDir.empty()) return;
    fs::path dictDir = fs::u8path(g_cfg.dictionaryDir);
    fs::path outPath = dictDir / fs::u8path("wiki_术语对照and个人填充.json");
    std::error_code oec;
    if (fs::exists(outPath, oec) && !oec) return; // 已存在：不覆盖用户手动修改

    json merged;
    merged["terms"] = json::object();
    merged["_options"] = json::object();
    merged["_descriptions"] = json::object();

    // 1) wiki 术语对照（固定优先，先来后到）
    fs::path wikiPath = dictDir / fs::u8path("wiki_术语对照.json");
    std::error_code wec;
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

// 加载词典 → 英文->中文 映射。
// 唯一词典来源：wiki_术语对照and个人填充.json 的 terms 部分
// （wiki 固定优先 + 个人填充，先来后到；要修改词条请直接编辑该文件）。
static bool LoadTermMap(std::unordered_map<std::string, std::string>& termMap, size_t& maxTermLen)
{
    if (g_cfg.dictionaryDir.empty()) return false;
    fs::path dictDir = fs::u8path(g_cfg.dictionaryDir);

    EnsureDictFile(); // 兜底：词典文件不存在时首次生成

    // 读唯一词典的 terms 构建映射
    fs::path mergedPath = dictDir / fs::u8path("wiki_术语对照and个人填充.json");
    std::error_code gec;
    if (fs::exists(mergedPath, gec) && !gec) {
        std::string d;
        if (read_binary_file(mergedPath, d)) {
            try {
                json md = json::parse(clean_utf8(d));
                if (md.is_object() && md.contains("terms") && md["terms"].is_object()) {
                    for (auto& it : md["terms"].items()) {
                        if (!it.value().is_string()) continue;
                        std::string en = it.key(), zh = it.value().get<std::string>();
                        if (en.empty() || zh.empty() || en == zh) continue;
                        termMap[en] = zh;
                        // 同时存入小写 key：选项文本中常出现小写/首字母小写，避免大小写不一致导致漏翻
                        std::string enLower;
                        enLower.reserve(en.size());
                        for (unsigned char c : en) enLower += static_cast<char>(std::tolower(c));
                        if (enLower != en) termMap[enLower] = zh;
                        if (en.size() > maxTermLen) maxTermLen = en.size();
                    }
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

// 用词典补全 JSON 中空白项。返回补全条数；missed=仍未命中；already=已有翻译被保留数
static int AutoFillJsonWithDict(json& tj,
    const std::unordered_map<std::string, std::string>& termMap,
    size_t maxTermLen, int& missed, int& already)
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
            std::string tr = TranslateText(orig, termMap, maxTermLen);
            if (!tr.empty() && tr != orig) { it.value() = tr; filled++; }
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
//   功能一：用唯一词典（wiki_术语对照and个人填充.json）补全未翻译项 → 生成/更新 *_已翻译.json
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

    // ── 功能一：词典补全（仅当勾选「用词典补全空白项」时执行）────────
    if (autoFill) {
        LogThread("[提示] 导入翻译·功能一：词典补全 —— 用唯一词典补全本文件空白项（不覆盖已有翻译）");
        std::unordered_map<std::string, std::string> termMap;
        size_t maxTermLen = 0;
        if (LoadTermMap(termMap, maxTermLen)) {
            LogThread("[提示] 已加载唯一词典 " + std::to_string(termMap.size())
                + " 条（wiki_术语对照and个人填充.json，wiki 固定优先、先来后到）");
            int missed = 0, already = 0;
            int filled = AutoFillJsonWithDict(tj, termMap, maxTermLen, missed, already);
            LogThread("词典补全：本次补全 " + std::to_string(filled) + " 条，已有翻译保留 "
                + std::to_string(already) + " 条，仍未命中 " + std::to_string(missed) + " 条");
        } else {
            LogThread("[提示] 词典为空（未找到 wiki_术语对照and个人填充.json），跳过补全");
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
        + " 的非空翻译编入 wiki_术语对照and个人填充.json（已有翻译不覆盖，先来后到）");
    EnsureDictFile(); // 词典不存在时首次生成
    fs::path dictPath = dictDir / fs::u8path("wiki_术语对照and个人填充.json");
    json dict;
    if (fs::exists(dictPath)) {
        std::string d;
        if (read_binary_file(dictPath, d)) {
            try { dict = json::parse(clean_utf8(d)); }
            catch (...) { dict = json::object(); }
        }
    }
    if (!dict.is_object()) dict = json::object();
    if (!dict.contains("_options")) dict["_options"] = json::object();
    if (!dict.contains("_descriptions")) dict["_descriptions"] = json::object();
    if (!dict.contains("terms")) dict["terms"] = json::object();

    int merged = 0;
    for (auto& sec : { "_options", "_descriptions" }) {
        if (!tj.contains(sec) || !tj[sec].is_object()) continue;
        for (auto& it : tj[sec].items()) {
            if (!it.value().is_string()) continue;
            std::string val = it.value().get<std::string>();
            if (val.empty()) continue;
            std::string existing;
            if (dict[sec].contains(it.key()) && dict[sec][it.key()].is_string())
                existing = dict[sec][it.key()].get<std::string>();
            if (existing.empty()) {
                dict[sec][it.key()] = val;
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
                merged++;
            }
        }
    }
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
static void add_wiki_term(json& result, int& added, const std::string& enRaw, const std::string& zhRaw, int* hitSkip = nullptr)
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
    else if (hitSkip) (*hitSkip)++;
}

// 解析 Data:<类型>/<id>.json 数据页，提取 中文名/英文名 对照。
// Item 用「中文名/英文名」，Action/Status/Trait 用「cn/en」；直接取顶层字段即可。
// 返回 0=既无中文也无英文，1=有中文，2=有英文，3=两者都有（用于诊断统计）。
static int parse_data_page(const std::string& type, const std::string& content, json& result, int& added, int* hitSkip = nullptr)
{
    json o;
    try { o = json::parse(content); }
    catch (...) { return 0; }
    if (!o.is_object()) return 0;

    std::string zh = o.value("中文名", o.value("cn", std::string()));
    std::string en = o.value("英文名", o.value("en", std::string()));
    int flag = (zh.empty() ? 0 : 1) | (en.empty() ? 0 : 2);
    if (flag == 3 && zh != en)
        add_wiki_term(result, added, en, zh, hitSkip);
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
    fs::path wikiPath = dictDir / fs::u8path("wiki_术语对照and个人填充.json");

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
    if (!result.contains("_options")) result["_options"] = json::object();
    if (!result.contains("_descriptions")) result["_descriptions"] = json::object();

    try {
        // 阶段1：批量抓取 Data:Item/、Data:Action/ 等数据页（含中文名/英文名/描述）
        // 用 cdn 只读接口：ff14.huijiwiki.com/api.php 会被 Cloudflare 拦截（返回 HTML 导致解析失败），
        // cdn.huijiwiki.com/ff14/api.php 对爬虫/程序友好，稳定返回完整 JSON（含英文名/日文名等）。
        std::string api = "https://cdn.huijiwiki.com/ff14/api.php?action=query&generator=allpages&format=json&utf8=1&gaplimit=500&prop=revisions&rvprop=content&rvslots=main";
        if (wikiExists && result.contains("terms") && result["terms"].is_object() && result["terms"].size() > 0) {
            LogThread("[提示] 已加载现有 wiki_术语对照and个人填充.json（已有 " + std::to_string(result["terms"].size())
                + " 条术语），本次为增量更新；已存在的词条（含你手动改过的）不会被覆盖");
        }
        int added = 0;
        int pages = 0;
        int hitExisting = 0; // 已存在于词典、本次跳过（增量更新，非故障）
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
                    int flag = parse_data_page(type, content, result, added, &hitExisting);
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

        std::string data = result.dump(2);
        write_binary_file(wikiPath, data);
        // 诊断：报告数据页中中文名/英文名的真实覆盖情况，帮助判断抓取是否正常。
        // 「命中已有」说明词条早已在词典中（增量更新），并非爬虫失效。
        LogThread("[提示] 诊断：共解析 " + std::to_string(pages) + " 个数据页，其中含中文名 " + std::to_string(cntZh)
            + " 页、含英文名 " + std::to_string(cntEn) + " 页、中文+英文都齐全 " + std::to_string(cntBoth)
            + " 页；本次新增术语 " + std::to_string(added) + " 条，命中已有词条 " + std::to_string(hitExisting) + " 条（跳过）");
        uint64_t wikiTotalMs = GetTickCount64() - wikiStartMs;
        long long wSecs = (long long)(wikiTotalMs / 1000);
        std::string wTime = std::to_string(wSecs / 60) + "分" + std::to_string(wSecs % 60) + "秒";
        if (g_cancel.load()) {
            LogThread("[提示] Wiki 导出已中断：共处理 " + std::to_string(pages) + " 页，新增术语 " + std::to_string(added)
                + " 条，命中已有 " + std::to_string(hitExisting) + " 条（跳过），总用时 " + wTime + "，已保存当前结果");
        }
        else {
            LogThread("[完成] Wiki 导出结束：共处理 " + std::to_string(pages) + " 页，新增术语 " + std::to_string(added)
                + " 条，命中已有 " + std::to_string(hitExisting) + " 条（跳过），总用时 " + wTime
                + "，结果已并入 wiki_术语对照and个人填充.json");
        }
        LogThread("[提示] 词典文件：wiki_术语对照and个人填充.json —— 如需修改词条请直接编辑该文件");
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
        bool ok = ExtractEnglish();
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
        bool ok = ImportTranslations(g_importFile, g_importAutoFill);
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

    DWORD timeout = 180000; // 3 分钟超时
    InternetSetOptionW(hReq, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionW(hReq, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));

    BOOL ok = HttpSendRequestW(hReq, headers.c_str(), (DWORD)-1L,
        (LPVOID)body.data(), (DWORD)body.size());
    if (!ok) {
        errMsg = "网络请求失败: " + std::to_string(GetLastError());
        InternetCloseHandle(hReq); InternetCloseHandle(hConn); InternetCloseHandle(hNet);
        return false;
    }

    DWORD status = 0, statusLen = sizeof(status);
    if (HttpQueryInfoW(hReq, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
        &status, &statusLen, nullptr)) {
        if (status != 200) {
            char ebuf[4096]; DWORD rd = 0; std::string resp;
            while (InternetReadFile(hReq, ebuf, sizeof(ebuf), &rd) && rd > 0) resp.append(ebuf, rd);
            errMsg = "HTTP " + std::to_string(status) + (resp.empty() ? "" : ": " + resp);
            InternetCloseHandle(hReq); InternetCloseHandle(hConn); InternetCloseHandle(hNet);
            return false;
        }
    }

    char buf[16384]; DWORD rd = 0; std::string resp;
    while (InternetReadFile(hReq, buf, sizeof(buf), &rd) && rd > 0) {
        resp.append(buf, rd);
        if (resp.size() > 64 * 1024 * 1024) {
            errMsg = "响应过大";
            InternetCloseHandle(hReq); InternetCloseHandle(hConn); InternetCloseHandle(hNet);
            return false;
        }
    }
    outBody = resp;
    InternetCloseHandle(hReq); InternetCloseHandle(hConn); InternetCloseHandle(hNet);
    return true;
}

// 从 AI 回复文本中解析出 JSON 对象（容忍 ```json 代码块、前后说明文字）
static bool ParseAIJson(const std::string& content, json& out)
{
    auto tryParse = [&](const std::string& s) -> bool {
        try { out = json::parse(s); return out.is_object(); } catch (...) { return false; }
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

// 调用一次 AI 翻译一批词条，返回 条目 id -> 译文 的映射
static bool AITranslateBatch(const std::vector<std::pair<std::string, std::string>>& items,
                             std::map<std::string, std::string>& outMap, std::string& errMsg)
{
    if (items.empty()) return true;
    if (g_cfg.aiModel.empty()) { errMsg = "未填写模型名（AI 翻译设置）"; return false; }
    if (g_cfg.aiBaseUrl.empty()) { errMsg = "未填写 API 地址（AI 翻译设置）"; return false; }
    std::string sysMsg =
        "你是《最终幻想14》(FFXIV) 模组本地化的专业译者，把英文模组文本翻译成简体中文。\n"
        "硬性要求：\n"
        "1. 译文格式统一为「中文（英文）」，例如 治疗（Cure）。\n"
        "2. 仅当括号内的英文与括号外的中文完全同义时，才可以省略括号只保留中文。\n"
        "3. 纯数字、百分比（如75%）、版本号、MOD 专有名词（如 Yiggle、Rue、Bibo、EXQB、YAB）直接保留原文，不翻译。\n"
        "4. 无法确定译名的专有名词保留英文，不要强行机翻。\n"
        "5. 只输出一个 JSON 对象：键为条目 id（字符串），值为译文。不要输出任何其他内容。";
    json arr = json::array();
    for (auto& it : items) arr.push_back({ {"id", it.first}, {"text", it.second} });
    std::string userMsg = "请翻译以下 FFXIV 模组文本条目，输出 JSON 对象：\n" + arr.dump();

    json req;
    req["model"] = g_cfg.aiModel;
    req["messages"] = json::array();
    req["messages"].push_back({ {"role","system"}, {"content", sysMsg} });
    req["messages"].push_back({ {"role","user"}, {"content", userMsg} });
    req["stream"] = false;
    req["temperature"] = 0.3;
    req["max_tokens"] = 8192; // 推理模型（如 glm-4.7）思维链会占大量 token，防止 content 被截断/为空
    std::string body = req.dump();

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
    std::string content = resp["choices"][0]["message"]["content"].get<std::string>();
    json result;
    if (!ParseAIJson(content, result)) {
        errMsg = "AI 未返回有效 JSON: " + content.substr(0, 300);
        return false;
    }
    for (auto& it : items) {
        auto found = result.find(it.first);
        if (found != result.end() && found.value().is_string()) {
            std::string v = found.value().get<std::string>();
            if (!v.empty()) outMap[it.first] = v;
        }
    }
    return true;
}

// AI 翻译一个 *_未翻译.json → *_已翻译.json
static bool AITranslateFile(const fs::path& inFile)
{
    if (g_cfg.aiApiKey.empty()) {
        LogThread("[错误] 未设置 AI API Key，请先点『AI 设置』配置");
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

    // 唯一词典预填：词典中已翻译的 key 直接填充，不再浪费 AI 额度
    int dictFilled = 0;
    if (!g_cfg.dictionaryDir.empty()) {
        fs::path dictPath = fs::u8path(g_cfg.dictionaryDir) / fs::u8path("wiki_术语对照and个人填充.json");
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
                                tj[it.sec][it.key] = dict[it.sec][it.key].get<std::string>();
                                dictFilled++; hit = true;
                            }
                            if (!hit) rest.push_back(it);
                        }
                        pending = std::move(rest);
                    }
                } catch (...) {}
            }
        }
    }
    if (dictFilled > 0)
        LogThread("[提示] 唯一词典命中 " + std::to_string(dictFilled) + " 条，直接填充");
    if (pending.empty()) {
        LogThread("[提示] 所有条目已由词典填充，无需调用 AI");
        return false;
    }

    // 分批调用 AI
    int batch = g_cfg.aiBatchSize;
    if (batch < 1) batch = 1;
    if (batch > 100) batch = 100;
    int total = (int)pending.size();
    int batches = (total + batch - 1) / batch;
    int okCount = 0, failCount = 0, cur = 0;
    LogThread("[AI] 正在翻译中，共 " + std::to_string(total) + " 条，分 " + std::to_string(batches) + " 批");
    for (size_t i = 0; i < pending.size(); i += batch) {
        int batchNo = (int)(i / batch) + 1;
        LogThread("[AI] 正在翻译第 " + std::to_string(batchNo) + "/" + std::to_string(batches) + " 批...");
        if (g_cancel) {
            LogThread("[提示] 已中断，剩余 " + std::to_string(total - cur) + " 条未翻译");
            break;
        }
        size_t n = std::min<size_t>(batch, pending.size() - i);
        std::vector<std::pair<std::string, std::string>> items;
        for (size_t k = 0; k < n; ++k)
            items.emplace_back(std::to_string(i + k), pending[i + k].english);

        std::string err;
        std::map<std::string, std::string> got;
        bool okBatch = false;
        for (int retry = 0; retry < 3 && !okBatch; ++retry) {
            if (retry > 0) { LogThread("[提示] 批次 " + std::to_string(i / batch + 1) + " 重试第 " + std::to_string(retry) + " 次..."); Sleep(2000); }
            got.clear();
            if (AITranslateBatch(items, got, err)) okBatch = true;
            else LogThread("[错误] 批次 " + std::to_string(i / batch + 1) + " 失败: " + err);
        }
        if (okBatch) {
            for (size_t k = 0; k < n; ++k) {
                auto f = got.find(std::to_string(i + k));
                if (f != got.end()) {
                    tj[pending[i + k].sec][pending[i + k].key] = f->second;
                    okCount++;
                }
            }
        } else {
            failCount += (int)n;
        }
        cur += (int)n;
        SetProgress(cur, total);
    }

    LogThread("[AI] 翻译完成：" + std::to_string(okCount) + " 条成功，"
        + std::to_string(failCount) + " 条失败");
    if (okCount == 0) return false;

    // 落盘：_未翻译 → _已翻译；同名已翻译含内容则另存 *_已翻译_AI.json
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
                if (dp != std::wstring::npos) alt.insert(dp, L"_AI");
                else alt += L"_AI";
                outFile = inFile.parent_path() / alt;
                LogThread("[提示] " + wstring_to_utf8(outName) + " 已存在且含翻译，本次输出为 " + wstring_to_utf8(alt));
            }
        }
    }
    if (write_binary_file(outFile, tj.dump(2))) {
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
        for (auto& f : files) if (!f.isTranslated) { target = f.path; break; }
        if (target.empty()) {
            Log("[错误] 翻译目录下没有 *_未翻译.json，请先执行『1. 提取英文』");
            g_busy = false;
            PostMessageW(g_hMainWnd, WM_APP_DONE, 0, 0);
            return;
        }
        Log("===== 开始 AI 自动翻译 =====");
        Log("输入文件: " + wstring_to_utf8(target.filename().wstring()));
        bool ok = AITranslateFile(target);
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

    // 把 customSaves 并入当前 Key / 预设
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
        // 预设：model+baseUrl 组合去重（忽略大小写），不同则追加
        if (!e.model.empty() || !e.baseUrl.empty()) {
            bool dup = false;
            for (const auto& p : g_cfg.aiPresets)
                if (NormForDup(p.model) == NormForDup(e.model) && NormForDup(p.baseUrl) == NormForDup(e.baseUrl)) { dup = true; break; }
            if (!dup) g_cfg.aiPresets.push_back({ e.name, e.model, e.baseUrl, e.note });
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
        bool pdup = false;
        for (const auto& p : g_cfg.aiPresets)
            if (NormForDup(p.model) == NormForDup(model) && NormForDup(p.baseUrl) == NormForDup(baseUrl)) { pdup = true; break; }
        if (!pdup) g_cfg.aiPresets.push_back({ name, model, baseUrl, note });
        g_cfg.aiKeyName = name;
        g_cfg.aiApiKey = key;
        // 刷新下拉并回填当前输入
        FillAIKeyCombo(hDlg);
        FillAIPresetCombos(hDlg);
        SetEditText(hDlg, IDC_AI_KEY_NAME, utf8_to_wstring(name));
        SetEditText(hDlg, IDC_AI_KEY, utf8_to_wstring(key));
        SetEditText(hDlg, IDC_AI_MODEL, utf8_to_wstring(model));
        SetEditText(hDlg, IDC_AI_BASEURL, utf8_to_wstring(baseUrl));
        Log("[完成] 已保存自定义 AI 记录「" + name + "」到用户配置（config.user.json）");
    } else {
        MessageBoxW(hDlg, L"写入用户配置文件失败，请检查程序目录是否有写权限。", L"提示", MB_OK | MB_ICONWARNING);
    }
}

// 把预设列表填入「模型名」「API 地址」两个下拉框
static void FillAIPresetCombos(HWND hDlg)
{
    HWND hModel = GetDlgItem(hDlg, IDC_AI_MODEL);
    HWND hBase = GetDlgItem(hDlg, IDC_AI_BASEURL);
    if (!hModel || !hBase) return;
    SendMessageW(hModel, CB_RESETCONTENT, 0, 0);
    SendMessageW(hBase, CB_RESETCONTENT, 0, 0);
    // 下拉项显示「值　（备注）」，选中后用预设索引取真实值回填编辑框（与 Key 下拉同理）；
    // 仅填充非空项，避免保存了单侧值的预设在下拉里出现空行
    for (size_t i = 0; i < g_cfg.aiPresets.size(); ++i) {
        const auto& p = g_cfg.aiPresets[i];
        if (!p.model.empty()) {
            std::wstring m = utf8_to_wstring(p.model);
            if (!p.note.empty()) m += L"　（" + utf8_to_wstring(p.note) + L"）";
            int mi = (int)SendMessageW(hModel, CB_ADDSTRING, 0, (LPARAM)m.c_str());
            SendMessageW(hModel, CB_SETITEMDATA, mi, (LPARAM)i);
        }
        if (!p.baseUrl.empty()) {
            std::wstring b = utf8_to_wstring(p.baseUrl);
            if (!p.note.empty()) b += L"　（" + utf8_to_wstring(p.note) + L"）";
            int bi = (int)SendMessageW(hBase, CB_ADDSTRING, 0, (LPARAM)b.c_str());
            SendMessageW(hBase, CB_SETITEMDATA, bi, (LPARAM)i);
        }
    }
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

// 把已保存的 Key 列表填入「API Key」下拉框（项文本 = 备注（完整 Key），下拉宽度按最长项自适应）
static void FillAIKeyCombo(HWND hDlg)
{
    HWND hKey = GetDlgItem(hDlg, IDC_AI_KEY);
    if (!hKey) return;
    SendMessageW(hKey, CB_RESETCONTENT, 0, 0);
    int sel = -1;
    int maxW = 240;
    HDC hdc = GetDC(hKey);
    HFONT hf = (HFONT)SendMessageW(hKey, WM_GETFONT, 0, 0);
    HFONT hOld = hf ? (HFONT)SelectObject(hdc, hf) : nullptr;
    for (size_t i = 0; i < g_cfg.aiKeys.size(); ++i) {
        std::wstring label = utf8_to_wstring(g_cfg.aiKeys[i].name);
        std::wstring k = utf8_to_wstring(g_cfg.aiKeys[i].key);
        label += L"（" + k + L"）"; // 完整 Key
        int idx = (int)SendMessageW(hKey, CB_ADDSTRING, 0, (LPARAM)label.c_str());
        SendMessageW(hKey, CB_SETITEMDATA, idx, (LPARAM)i);
        if (g_cfg.aiKeyName == g_cfg.aiKeys[i].name) sel = idx;
        SIZE sz = {};
        if (GetTextExtentPoint32W(hdc, label.c_str(), (int)label.size(), &sz))
            if (sz.cx + 24 > maxW) maxW = sz.cx + 24;
    }
    if (hOld) SelectObject(hdc, hOld);
    ReleaseDC(hKey, hdc);
    SendMessageW(hKey, CB_SETDROPPEDWIDTH, maxW, 0);
    if (sel >= 0) SendMessageW(hKey, CB_SETCURSEL, sel, 0);
    // 编辑框显示完整 Key（与下拉项文本不同）
    SetEditText(hDlg, IDC_AI_KEY, utf8_to_wstring(g_cfg.aiApiKey));
}

// 切换 API Key 输入框的密码掩码（Key 现在是下拉框内的编辑框，需运行时设置样式）
static void ApplyKeyPasswordStyle(HWND hDlg)
{
    HWND hCombo = GetDlgItem(hDlg, IDC_AI_KEY);
    if (!hCombo) return;
    COMBOBOXINFO cbi = {};
    cbi.cbSize = sizeof(cbi);
    SendMessageW(hCombo, CB_GETCOMBOBOXINFO, 0, (LPARAM)&cbi);
    HWND he = cbi.hwndItem;
    if (!he) return;
    LONG_PTR st = GetWindowLongPtrW(he, GWL_STYLE);
    if (g_keyVisible) st &= ~ES_PASSWORD; else st |= ES_PASSWORD;
    SetWindowLongPtrW(he, GWL_STYLE, st);
    InvalidateRect(he, nullptr, TRUE);
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
                // 高亮选中（点击 / Ctrl+A 全选）时自动同步勾选复选框，让「勾选」与「选中/全选」等效
                if (nmlv->uNewState & LVIS_SELECTED)
                    ListView_SetCheckState(left, nmlv->iItem, TRUE);
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
// 单词黑名单.json（词典目录）读写
// ------------------------------------------------------------------
// 从词典目录下的 单词黑名单.json 读取黑名单词（每行一个，或用逗号间隔），合并 defaults 后返回
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
        std::stringstream ss2(token);
        std::string t;
        while (std::getline(ss2, t, ',')) {
            size_t s = t.find_first_not_of(" \t"); if (s != std::string::npos) t = t.substr(s);
            size_t e = t.find_last_not_of(" \t"); if (e != std::string::npos) t = t.substr(0, e + 1);
            if (!t.empty()) words.push_back(t);
        }
    }
    return words;
}

// 保存黑名单到词典目录下的 单词黑名单.json
void SaveBlacklistFile(const std::vector<std::string>& words)
{
    std::string base = g_cfg.dictionaryDir.empty() ? g_cfg.translationDir : g_cfg.dictionaryDir;
    if (base.empty()) return;
    fs::path p = fs::u8path(base) / fs::u8path("单词黑名单.json");
    std::string out;
    for (size_t i = 0; i < words.size(); ++i) {
        if (i) out += "\r\n";
        out += words[i];
    }
    if (write_binary_file(p, out))
        Log("已写入 单词黑名单.json（" + std::to_string(words.size()) + " 个词，位于词典目录）");
}

// ------------------------------------------------------------------
// 黑名单对话框
// ------------------------------------------------------------------
INT_PTR CALLBACK BlacklistDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_INITDIALOG: {
        // 显示：配置文件 + 单词黑名单.json 合并去重
        std::set<std::string> uniq;
        for (auto& w : LoadBlacklistFile(g_cfg.blacklist))
            if (!w.empty()) uniq.insert(w);
        std::string text;
        size_t i = 0;
        for (auto& w : uniq) {
            if (i++) text += "\r\n";
            text += w;
        }
        SetDlgItemTextW(hDlg, IDC_BLACKLIST_EDIT, utf8_to_wstring(text).c_str());
        return TRUE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDC_BLACKLIST_OK) {
            wchar_t buf[8192] = {};
            GetDlgItemTextW(hDlg, IDC_BLACKLIST_EDIT, buf, 8192);
            std::string utf8 = wstring_to_utf8(buf);
            std::vector<std::string> words;
            std::stringstream ss(utf8);
            std::string token;
            while (std::getline(ss, token, '\n')) {
                // 移除 \r
                if (!token.empty() && token.back() == '\r') token.pop_back();
                // 按逗号分隔
                std::stringstream ss2(token);
                std::string t;
                while (std::getline(ss2, t, ',')) {
                    size_t s = t.find_first_not_of(" \t"); if (s != std::string::npos) t = t.substr(s);
                    size_t e = t.find_last_not_of(" \t"); if (e != std::string::npos) t = t.substr(0, e + 1);
                    if (!t.empty()) words.push_back(t);
                }
            }
            g_cfg.blacklist = words;
            SaveBlacklistFile(words);
            SaveConfig();
            EndDialog(hDlg, IDOK);
            Log("已保存 " + std::to_string(words.size()) + " 个黑名单词（写入词典目录 单词黑名单.json）");
            return TRUE;
        }
        if (id == IDC_BLACKLIST_CANCEL || id == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

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
        SendMessageW(hList, LB_SETCURSEL, 0, 0);
        CheckDlgButton(hDlg, IDC_IMPORT_AUTOFILL, BST_CHECKED);
        return TRUE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDC_IMPORT_OK || id == IDOK) {
            int idx = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
            if (idx < 0) {
                MessageBoxW(hDlg, L"请选择一个文件。", L"提示", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            fs::path* p = (fs::path*)SendMessageW(hList, LB_GETITEMDATA, (WPARAM)idx, 0);
            if (p) g_importFile = *p;
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
        Log("FFXIV 模组汉化工具已启动");
        if (g_cfg.winW > 0 && g_cfg.winH > 0)
            Log("[窗口] 已恢复上次大小：" + std::to_string(g_cfg.winW) + " x " + std::to_string(g_cfg.winH));
        if (!g_cfg.penumbraDir.empty()) Log("Penumbra 目录：" + g_cfg.penumbraDir);
        if (g_cfg.dictionaryDir.empty())
            Log("[提示] 未找到 config.user.json（首次运行？）。请在『词典目录』处选择一次目录建立用户配置；旧版升级则选择原词典目录即可自动迁移已有设置");
        // 创建初：若唯一词典 wiki_术语对照and个人填充.json 不存在，则从
        // wiki_术语对照.json + 汉化总词典.json 一次性合并生成（wiki 固定优先，先来后到）。
        // 之后程序不再自动重写它，改已翻译的词条请直接编辑该文件。
        if (!g_cfg.dictionaryDir.empty()) {
            fs::path dictFile = fs::u8path(g_cfg.dictionaryDir) / fs::u8path("wiki_术语对照and个人填充.json");
            std::error_code dec0;
            bool dictExists = fs::exists(dictFile, dec0) && !dec0;
            EnsureDictFile();
            if (!dictExists) {
                std::error_code dec1;
                if (fs::exists(dictFile, dec1) && !dec1)
                    Log("[提示] 已生成唯一词典 wiki_术语对照and个人填充.json（wiki 固定优先 + 个人填充，先来后到；改词请直接编辑该文件）");
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
                g_cfg.translationDir = wstring_to_utf8(dir);
                SetEditText(hDlg, IDC_EDIT_TRANSLATION, dir);
            }
            break;
        }
        case IDC_BTN_BROWSE_DICT: {
            std::wstring dir;
            if (SelectDirDialog(hDlg, dir)) {
                g_cfg.dictionaryDir = wstring_to_utf8(dir);
                SetEditText(hDlg, IDC_EDIT_DICTIONARY, dir);
                // 兼容旧版：程序目录还没有用户配置时，若所选词典目录里有旧版 config.json，读取并迁移
                fs::path exeCfg = GetExeDir() / "config.user.json";
                std::error_code eec;
                if (!fs::exists(exeCfg, eec)) {
                    fs::path legacy = fs::u8path(g_cfg.dictionaryDir) / "config.json";
                    std::error_code lec;
                    if (fs::exists(legacy, lec) && !lec) {
                        if (LoadConfigFrom(legacy, true))
                            Log("[提示] 已从词典目录的旧 config.json 迁移配置（不覆盖旧文件）");
                        else
                            Log("[提示] 检测到词典目录有旧 config.json，但读取失败");
                    }
                    g_cfg.dictionaryDir = wstring_to_utf8(dir); // 以用户刚选的目录为准
                }
                SaveConfig();
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
        case IDC_EDIT_DICTIONARY:
            if (wmEvent == EN_CHANGE) g_cfg.dictionaryDir = wstring_to_utf8(GetEditText(hDlg, IDC_EDIT_DICTIONARY));
            break;
        // AI 翻译设置同步到配置
        case IDC_AI_KEY:
            if (wmEvent == CBN_SELCHANGE) {
                HWND hKey = GetDlgItem(hDlg, IDC_AI_KEY);
                int sel = (int)SendMessageW(hKey, CB_GETCURSEL, 0, 0);
                if (sel >= 0) {
                    size_t idx = (size_t)SendMessageW(hKey, CB_GETITEMDATA, sel, 0);
                    if (idx < g_cfg.aiKeys.size()) {
                        g_cfg.aiKeyName = g_cfg.aiKeys[idx].name;
                        g_cfg.aiApiKey = g_cfg.aiKeys[idx].key;
                        SetEditText(hDlg, IDC_AI_KEY, utf8_to_wstring(g_cfg.aiApiKey)); // 编辑框显示完整 Key
                        SetEditText(hDlg, IDC_AI_KEY_NAME, utf8_to_wstring(g_cfg.aiKeyName));
                        SaveConfig();
                    }
                }
            } else if (wmEvent == CBN_EDITCHANGE) {
                // 手动输入/修改：作为当前 Key 使用（保存为条目需点「保存Key」）
                g_cfg.aiApiKey = wstring_to_utf8(GetEditText(hDlg, IDC_AI_KEY));
            }
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
            // 模型名与 API 地址完全独立：选择/输入模型名只更新模型名，不联动地址
            if (wmEvent == CBN_SELCHANGE) {
                int sel = (int)SendMessageW(GetDlgItem(hDlg, IDC_AI_MODEL), CB_GETCURSEL, 0, 0);
                if (sel >= 0) {
                    size_t idx = (size_t)SendMessageW(GetDlgItem(hDlg, IDC_AI_MODEL), CB_GETITEMDATA, sel, 0);
                    if (idx < g_cfg.aiPresets.size()) {
                        g_cfg.aiModel = g_cfg.aiPresets[idx].model;
                        SetEditText(hDlg, IDC_AI_MODEL, utf8_to_wstring(g_cfg.aiModel)); // 编辑框回填纯模型名
                        SaveConfig();
                    }
                }
            } else if (wmEvent == CBN_EDITCHANGE) {
                g_cfg.aiModel = wstring_to_utf8(GetEditText(hDlg, IDC_AI_MODEL));
            }
            break;
        case IDC_AI_BASEURL:
            // 地址与模型名完全独立：选择/输入地址只更新地址，不联动模型名
            if (wmEvent == CBN_SELCHANGE) {
                int sel = (int)SendMessageW(GetDlgItem(hDlg, IDC_AI_BASEURL), CB_GETCURSEL, 0, 0);
                if (sel >= 0) {
                    size_t idx = (size_t)SendMessageW(GetDlgItem(hDlg, IDC_AI_BASEURL), CB_GETITEMDATA, sel, 0);
                    if (idx < g_cfg.aiPresets.size()) {
                        g_cfg.aiBaseUrl = g_cfg.aiPresets[idx].baseUrl;
                        SetEditText(hDlg, IDC_AI_BASEURL, utf8_to_wstring(g_cfg.aiBaseUrl)); // 编辑框回填纯地址
                        SaveConfig();
                    }
                }
            } else if (wmEvent == CBN_EDITCHANGE) {
                g_cfg.aiBaseUrl = wstring_to_utf8(GetEditText(hDlg, IDC_AI_BASEURL));
            }
            break;
        case IDC_AI_BATCH: {
            if (wmEvent == EN_CHANGE) {
                int b = _wtoi(GetEditText(hDlg, IDC_AI_BATCH).c_str());
                if (b >= 1 && b <= 100) g_cfg.aiBatchSize = b;
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
                    Log("[提示] 已开启「词序调换」：应用翻译后名称显示为『中文（英文）』。直接点『3. 应用翻译』即可生效，无需重新提取/导入。");
                else
                    Log("[提示] 已关闭「词序调换」：应用翻译恢复为『英文（中文）』格式。直接点『3. 应用翻译』即可生效。");
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
            SaveConfig();
            g_busy = true;
            g_cancel = false;
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_EXTRACT), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_IMPORT), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_APPLY), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_RESTORE), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_BLACKLIST), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_WIKI), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_AI_TRANSLATE), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_AI_TEST), FALSE);
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
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_WIKI), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_AI_TRANSLATE), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_AI_TEST), FALSE);
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
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_WIKI), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_AI_TRANSLATE), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_AI_TEST), FALSE);
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
        case IDC_BTN_RESTORE: {
            if (g_busy) break;
            DialogBoxW(hInst, MAKEINTRESOURCEW(IDD_RESTORE_DIALOG), hDlg, RestoreDlgProc);
            break;
        }
        case IDC_BTN_BLACKLIST: {
            if (g_busy) break;
            DialogBoxW(hInst, MAKEINTRESOURCEW(IDD_BLACKLIST_DIALOG), hDlg, BlacklistDlgProc);
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
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_WIKI), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_AI_TRANSLATE), FALSE);
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
                MessageBoxW(hDlg, L"尚未配置 AI API Key，请在上方『AI 翻译设置』中填写。", L"提示", MB_OK | MB_ICONINFORMATION);
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
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_WIKI), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_AI_TRANSLATE), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_AI_TEST), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_CANCEL), TRUE);
            Log("===== 开始测试 AI 连接 =====");
            LaunchWorker(RunAITestThread, "AI 测试");
            break;
        }
        case IDC_BTN_AI_TRANSLATE: {
            if (g_busy) break;
            SyncAISettingsFromUI(hDlg); // 界面即真值，先同步再翻译
            if (g_cfg.aiApiKey.empty()) {
                MessageBoxW(hDlg, L"尚未配置 AI API Key，请在上方『AI 翻译设置』中填写。", L"提示", MB_OK | MB_ICONINFORMATION);
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
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_WIKI), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_AI_TRANSLATE), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_AI_TEST), FALSE);
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
