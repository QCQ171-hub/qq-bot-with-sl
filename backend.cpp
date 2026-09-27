// QQAI 聊天工具 - 后端（零第三方依赖版）
//
// 本机 MinGW 没有 libcurl / nlohmann_json，所以按原方案的模块划分改用 Windows 自带能力：
//   WinHTTP  -> 请求智谱 GLM（OpenAI 兼容）与 OneBot v11 HTTP API
//   Winsock2 -> 内置 HTTP 服务，直接托管 index.html / static/ 与 /api/*
// 表情压缩走外部 ffmpeg（PATH 或环境变量 QQAI_FFMPEG），不链接 GDI+。
// 保留原方案的类型与函数命名：Config / Group / PrivateChat / Sticker / Message、
//   load_config / save_config / load_data_file / save_data_file / process_message_template /
//   validate_time_format / parse_message_content / send_to_qq / process_pending_messages / main_loop
//
// 消息格式（模型输出与手动输入共用同一套解析器）：
//   {send:内容}                  立即发一条文本
//   {send:内容,time:HHMM}        今天 HH:MM 发（早于当前时间报错）
//   {send:内容,time:YYMMDDHHMM}  指定时刻发
//   {image:编号}                 发表情包（编号来自 data/stickers.json）
//   {image:编号,time:...}        定时发表情包
//   没出现 {send:}/{image:}      表示不发消息
// 只要解析出 send/image，就先落进“待确认”批次，由前端询问“还发不发”，确认后才真正排队发送。

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std;
namespace fs = std::filesystem;

static const char* APP_NAME = "QQAI聊天工具";
static const char* APP_VERSION = "1.1.0";

// ───────────────────────── 通用工具 ─────────────────────────

static int64_t now_ms() {
    return chrono::duration_cast<chrono::milliseconds>(
               chrono::system_clock::now().time_since_epoch())
        .count();
}

static string fmt_time(int64_t ms) {
    if (ms <= 0) return "";
    time_t t = (time_t)(ms / 1000);
    struct tm tmv;
    localtime_s(&tmv, &t);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

static string now_string() { return fmt_time(now_ms()); }

static string trim(const string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
    return s.substr(a, b - a);
}

static bool starts_with(const string& s, const string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

static string replace_all(string s, const string& from, const string& to) {
    if (from.empty()) return s;
    string out;
    size_t pos = 0;
    while (true) {
        size_t idx = s.find(from, pos);
        if (idx == string::npos) { out += s.substr(pos); break; }
        out += s.substr(pos, idx - pos);
        out += to;
        pos = idx + from.size();
    }
    return out;
}

static string join_path(const string& a, const string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    char last = a.back();
    if (last == '/' || last == '\\') return a + b;
    return a + "/" + b;
}

static string url_path_join(const string& base, const string& tail) {
    string b = base;
    while (!b.empty() && (b.back() == '/' || b.back() == '\\')) b.pop_back();
    if (tail.empty()) return b;
    return b + (tail[0] == '/' ? tail : "/" + tail);
}

static string to_lower(string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static string wide_to_utf8(const wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static wstring utf8_to_wide(const string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static string base64_encode(const string& in) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < in.size()) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];  out += tbl[v & 63];
        i += 3;
    }
    size_t rem = in.size() - i;
    if (rem == 1) {
        unsigned v = (unsigned char)in[i] << 16;
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += "==";
    } else if (rem == 2) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8);
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += '=';
    }
    return out;
}

static int64_t g_id_counter = 0;
static string make_id(const string& prefix) {
    g_id_counter++;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s-%llx-%llx", prefix.c_str(),
             (unsigned long long)now_ms(), (unsigned long long)g_id_counter);
    return buf;
}

// ───────────────────────── 迷你 JSON ─────────────────────────

struct JValue {
    enum Type { TNull, TBool, TNum, TStr, TArr, TObj } type = TNull;
    bool bval = false;
    double nval = 0;
    string sval;
    vector<JValue> aval;
    vector<pair<string, JValue>> oval;

    JValue() {}

    static JValue mkNull() { return JValue(); }
    static JValue mkBool(bool b) { JValue v; v.type = TBool; v.bval = b; return v; }
    static JValue mkNum(double n) { JValue v; v.type = TNum; v.nval = n; return v; }
    static JValue mkStr(const string& s) { JValue v; v.type = TStr; v.sval = s; return v; }
    static JValue mkArr() { JValue v; v.type = TArr; return v; }
    static JValue mkObj() { JValue v; v.type = TObj; return v; }

    bool isObj() const { return type == TObj; }
    bool isArr() const { return type == TArr; }
    bool isStr() const { return type == TStr; }
    bool isNull() const { return type == TNull; }

    JValue& set(const string& k, const JValue& v) {
        if (type != TObj) { type = TObj; aval.clear(); }
        for (auto& p : oval) if (p.first == k) { p.second = v; return p.second; }
        oval.push_back(make_pair(k, v));
        return oval.back().second;
    }
    JValue& operator[](const string& k) { return set(k, JValue()); }
    void push(const JValue& v) {
        if (type != TArr) { type = TArr; oval.clear(); }
        aval.push_back(v);
    }
    const JValue* find(const string& k) const {
        if (type != TObj) return nullptr;
        for (auto& p : oval) if (p.first == k) return &p.second;
        return nullptr;
    }
    bool has(const string& k) const { return find(k) != nullptr; }
    string sget(const string& k, const string& def = "") const {
        const JValue* v = find(k);
        if (!v) return def;
        if (v->type == TStr) return v->sval;
        if (v->type == TNum) {
            // 整数值按整数输出，避免 749189016 变成 7.49189e+08（群号精度丢失）
            char b[32];
            if (v->nval == (double)(long long)v->nval && v->nval > -9.2e15 && v->nval < 9.2e15)
                snprintf(b, sizeof(b), "%lld", (long long)v->nval);
            else
                snprintf(b, sizeof(b), "%g", v->nval);
            return b;
        }
        if (v->type == TBool) return v->bval ? "true" : "false";
        return def;
    }
    double nget(const string& k, double def = 0) const {
        const JValue* v = find(k);
        if (!v) return def;
        if (v->type == TNum) return v->nval;
        if (v->type == TStr) { try { return stod(v->sval); } catch (...) { return def; } }
        if (v->type == TBool) return v->bval ? 1 : 0;
        return def;
    }
    int64_t iget(const string& k, int64_t def = 0) const {
        const JValue* v = find(k);
        if (!v) return def;
        if (v->type == TNum) return (int64_t)v->nval;
        if (v->type == TStr) { try { return (int64_t)stoll(v->sval); } catch (...) { return def; } }
        if (v->type == TBool) return v->bval ? 1 : 0;
        return def;
    }
    bool bget(const string& k, bool def = false) const {
        const JValue* v = find(k);
        if (!v) return def;
        if (v->type == TBool) return v->bval;
        if (v->type == TNum) return v->nval != 0;
        if (v->type == TStr) { string s = to_lower(v->sval); return s == "true" || s == "1" || s == "yes"; }
        return def;
    }

    string dump() const { string o; write(o); return o; }

    void write(string& o) const {
        switch (type) {
            case TNull: o += "null"; break;
            case TBool: o += bval ? "true" : "false"; break;
            case TNum: {
                if (nval == (double)(long long)nval && nval > -9.2e15 && nval < 9.2e15) {
                    char b[32]; snprintf(b, sizeof(b), "%lld", (long long)nval); o += b;
                } else {
                    char b[40]; snprintf(b, sizeof(b), "%.10g", nval); o += b;
                }
                break;
            }
            case TStr: escape(sval, o); break;
            case TArr: {
                o += '[';
                for (size_t i = 0; i < aval.size(); i++) { if (i) o += ','; aval[i].write(o); }
                o += ']';
                break;
            }
            case TObj: {
                o += '{';
                for (size_t i = 0; i < oval.size(); i++) {
                    if (i) o += ',';
                    escape(oval[i].first, o);
                    o += ':';
                    oval[i].second.write(o);
                }
                o += '}';
                break;
            }
        }
    }

    static void escape(const string& s, string& o) {
        o += '"';
        for (unsigned char c : s) {
            if (c == '"') o += "\\\"";
            else if (c == '\\') o += "\\\\";
            else if (c == '\n') o += "\\n";
            else if (c == '\r') o += "\\r";
            else if (c == '\t') o += "\\t";
            else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
            else o += (char)c;  // UTF-8 原样输出
        }
        o += '"';
    }

    static void skip_ws(const string& s, size_t& i) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    }

    static void append_utf8(string& out, unsigned int cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
        }
    }

    static bool parse_string(const string& s, size_t& i, string& out) {
        skip_ws(s, i);
        if (i >= s.size() || s[i] != '"') return false;
        i++;
        out.clear();
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) return false;
            char e = s[i++];
            if (e == 'n') out += '\n';
            else if (e == 't') out += '\t';
            else if (e == 'r') out += '\r';
            else if (e == 'b') out += '\b';
            else if (e == 'f') out += '\f';
            else if (e == '/') out += '/';
            else if (e == '\\') out += '\\';
            else if (e == '"') out += '"';
            else if (e == 'u') {
                if (i + 4 > s.size()) return false;
                unsigned int cp = 0;
                for (int k = 0; k < 4; k++) {
                    char h = s[i + k];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                    else return false;
                }
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                    unsigned int lo = 0;
                    bool ok = true;
                    for (int k = 0; k < 4; k++) {
                        char h = s[i + 2 + k];
                        lo <<= 4;
                        if (h >= '0' && h <= '9') lo |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') lo |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') lo |= (unsigned)(h - 'A' + 10);
                        else { ok = false; break; }
                    }
                    if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                }
                append_utf8(out, cp);
            } else return false;
        }
        return false;
    }

    static bool parse(const string& s, size_t& i, JValue& out) {
        skip_ws(s, i);
        if (i >= s.size()) return false;
        char c = s[i];
        if (c == '{') {
            out = mkObj(); i++;
            skip_ws(s, i);
            if (i < s.size() && s[i] == '}') { i++; return true; }
            while (i < s.size()) {
                string key;
                if (!parse_string(s, i, key)) return false;
                skip_ws(s, i);
                if (i >= s.size() || s[i] != ':') return false;
                i++;
                JValue val;
                if (!parse(s, i, val)) return false;
                out.set(key, val);
                skip_ws(s, i);
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == '}') { i++; return true; }
                return false;
            }
            return false;
        }
        if (c == '[') {
            out = mkArr(); i++;
            skip_ws(s, i);
            if (i < s.size() && s[i] == ']') { i++; return true; }
            while (i < s.size()) {
                JValue val;
                if (!parse(s, i, val)) return false;
                out.push(val);
                skip_ws(s, i);
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == ']') { i++; return true; }
                return false;
            }
            return false;
        }
        if (c == '"') {
            string sval;
            if (!parse_string(s, i, sval)) return false;
            out = mkStr(sval);
            return true;
        }
        if (s.compare(i, 4, "true") == 0) { out = mkBool(true); i += 4; return true; }
        if (s.compare(i, 5, "false") == 0) { out = mkBool(false); i += 5; return true; }
        if (s.compare(i, 4, "null") == 0) { out = mkNull(); i += 4; return true; }
        size_t st = i;
        while (i < s.size() && (isdigit((unsigned char)s[i]) || s[i] == '-' || s[i] == '+' ||
                                s[i] == '.' || s[i] == 'e' || s[i] == 'E')) i++;
        if (i == st) return false;
        try { out = mkNum(stod(s.substr(st, i - st))); } catch (...) { return false; }
        return true;
    }

    static JValue parse_or_null(const string& s) {
        JValue out;
        size_t i = 0;
        if (parse(s, i, out)) return out;
        return JValue();
    }
};

// ───────────────────────── 文件读写 ─────────────────────────

static bool read_text_file(const string& path, string& out) {
    ifstream f(fs::u8path(path), ios::binary);
    if (!f.is_open()) return false;
    stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB &&
        (unsigned char)out[2] == 0xBF) out.erase(0, 3);
    return true;
}

static bool write_text_file(const string& path, const string& content) {
    error_code ec;
    fs::path p = fs::u8path(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    ofstream f(p, ios::binary | ios::trunc);
    if (!f.is_open()) return false;
    f.write(content.data(), (streamsize)content.size());
    return f.good();
}

// ───────────────────────── WinHTTP 客户端 ─────────────────────────

struct HttpResp {
    bool ok = false;
    long status = 0;
    string body;
    string err;
};

static HttpResp http_call(const string& url, const string& method = "GET", const string& body = "",
                          const vector<pair<string, string>>& headers = {}, DWORD timeout_ms = 40000) {
    HttpResp r;
    wstring wurl = utf8_to_wide(url);
    URL_COMPONENTS uc;
    wchar_t host[512] = {0}, path[3000] = {0}, extra[1024] = {0};
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host; uc.dwHostNameLength = 511;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2999;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 1023;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) { r.err = "URL 解析失败: " + url; return r; }
    bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    HINTERNET session = WinHttpOpen(L"QQAI-Backend", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { r.err = "WinHttpOpen 失败"; return r; }
    HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
    if (!conn) { WinHttpCloseHandle(session); r.err = "无法连接主机 " + wide_to_utf8(host); return r; }
    wstring wpath = wstring(path) + wstring(extra);
    wstring wmethod = utf8_to_wide(method);
    HINTERNET req = WinHttpOpenRequest(conn, wmethod.c_str(), wpath.c_str(), L"HTTP/1.1",
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req) {
        WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
        r.err = "WinHttpOpenRequest 失败";
        return r;
    }
    WinHttpSetTimeouts(req, (int)timeout_ms, (int)timeout_ms, (int)timeout_ms, (int)timeout_ms);
    for (const auto& h : headers) {
        wstring wl = utf8_to_wide(h.first + ": " + h.second + "\r\n");
        WinHttpAddRequestHeaders(req, wl.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    }
    BOOL sent;
    if (body.empty())
        sent = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    else
        sent = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (LPVOID)body.data(),
                                  (DWORD)body.size(), (DWORD)body.size(), 0);
    if (!sent) {
        r.err = "请求发送失败 (winerr " + to_string((long long)GetLastError()) + ")";
        WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
        return r;
    }
    if (!WinHttpReceiveResponse(req, nullptr)) {
        r.err = "等待响应超时或失败 (winerr " + to_string((long long)GetLastError()) + ")";
        WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
        return r;
    }
    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    r.status = (long)status;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        string chunk(avail, '\0');
        DWORD readn = 0;
        if (!WinHttpReadData(req, &chunk[0], avail, &readn)) break;
        chunk.resize(readn);
        r.body += chunk;
    }
    WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
    r.ok = (r.status >= 200 && r.status < 300);
    if (!r.ok && r.err.empty()) r.err = "HTTP " + to_string(r.status);
    return r;
}

// ───────────────────────── 数据结构 ─────────────────────────

static const string DEFAULT_PROMPT_TEMPLATE =
    "%name发送了%s，用格式可以发表情包{image:编号}，没写{send:内容}=不发消息，这个代表发消息，可以发很多条，"
    "同时发，如果有send/image就会询问还发不发，使用{send:内容,time:时间（YYMMDD+4位小时分钟，也可以只写4位小时分钟今天发送，"
    "如果比现在的时间早要报错，image也可以这么写）";

struct Group {
    string id;
    string name;
    int members = 0;
    bool enabled = true;
    vector<string> selected_members;
};

struct PrivateChat {
    string id;
    string name;
    bool enabled = true;
};

struct Sticker {
    int64_t id = 0;              // 编号，供 {image:编号} 使用
    string name;
    string description;
    string url;
    bool favorite = false;
    string note;
    string emoji_id;
    string md5;
    string source = "manual";    // manual / qq / ai
};

struct Message {
    string id;
    string batch;
    string target;               // group:123 / private:456
    string target_name;
    string dir;                  // in / out
    string sender;
    string type;                 // send / image
    string content;              // 文本内容 或 图片地址
    int64_t sticker_id = 0;
    string time_raw;
    int64_t epoch = 0;
    string status;               // pending / queued / sent / failed / cancelled
    string error;
    int64_t created_at = 0;
    string qq_msg_id;            // 来自 QQ 的消息 id（去重用，dir=in 时有值）
    string sender_id;            // 发送者 QQ 号
    string at_id;                // 非空=发送前先 @ 这个 QQ 号（AI 写 %id 展开来的）
};

struct ParsedItem {
    string type;
    string content;
    int64_t sticker_id = 0;
    string time_raw;
    int64_t epoch = 0;           // 0 = 立即
    string at_id;                // 非空=这条消息要 @ 的人
};

struct Config {
    // 兼容原字段：api_url 里存的其实是 API Key，base_url 存接口地址
    string api_url;
    string base_url;
    string api_key;              // 新字段，优先于 api_url
    string model;                // 模型名字，例如 glm-4-flash
    string persona;
    string prompt_template;
    string onebot_url;           // OneBot v11 HTTP API
    string onebot_token;
    string send_mode;            // auto / real / simulate
    int64_t default_group = 0;
    int port = 8788;
    int max_tokens = 1200;
    double temperature = 0.9;
    int64_t send_interval_ms = 0;
    bool attach_format_hint = true;
    bool attach_sticker_list = true;
    int sticker_count = 60;
    // 自动收信回复
    bool auto_reply = true;
    int auto_check_ms = 5000;
    int reply_probability = 30;      // 无 @、无关键词时的触发概率（%），越低越冷淡
    vector<string> must_reply_keywords;
    int64_t reply_cooldown_ms = 60000;
    int context_window = 15;
    bool append_hint = true;         // 提示里鼓励追加/定时消息
    bool auto_confirm = true;        // 自动回复跳过“还发不发”，直接进发送队列
    bool private_always_reply = true;
    int sticker_long_edge = 32;      // 发表情前把长边压到这个像素，0=原图直发

    Config()
        : base_url("https://open.bigmodel.cn/api/paas/v4/"), model("glm-4-flash"),
          prompt_template(DEFAULT_PROMPT_TEMPLATE), onebot_url("http://127.0.0.1:3000"),
          send_mode("auto") {}

    string key() const { return api_key.empty() ? api_url : api_key; }
    bool force_simulate() const { return send_mode == "simulate"; }
};

// ───────────────────────── 全局数据 ─────────────────────────

static string app_dir = ".";
static string config_file, groups_file, private_file, stickers_file, messages_file;

static void hot_restart_worker();  // 保存配置后的热重启（定义在 main 前）

static Config config;
static vector<Group> groups;
static vector<PrivateChat> private_chats;
static vector<Sticker> stickers;
static vector<Message> message_list;

// 部分接口会“外层加锁 + 内层函数再次加锁”（例如 /api/confirm 里落盘、/api/pending 里查表情名），
// 用 recursive_mutex 避免自死锁。
static recursive_mutex data_mutex;
static atomic<bool> running{true};
static atomic<bool> g_onebot_up{false};
static string g_self_nickname;
static int64_t g_next_send_at = 0;

// 自动收信
struct PollState {
    int64_t last_time = 0;      // 已见到的最新消息时间（秒）
    string last_msg_id;
    int64_t last_check = 0;
    int64_t last_reply_at = 0;  // 该会话上次触发模型的时间
};
static map<string, PollState> g_poll_state;
static recursive_mutex poll_mutex;
static atomic<bool> g_auto_busy{false};
static int64_t g_self_uin = 0;
static string g_last_auto_info;
static string poll_state_file;

// 黑名单/白名单（存 QQ 号 id，不存名字）
static vector<string> user_black;
static vector<string> user_white;
static string user_lists_file;

// 今日 token 用量：按本地日期累计，跨天自动清零；AI 输出里的 %token 会展开成这个数，前端状态栏也显示
static string tokens_file;
static string g_token_date;          // 累计对应的日期 YYYY-MM-DD
static int64_t g_tokens_today = 0;   // total_tokens 累计
static int64_t g_prompt_today = 0;
static int64_t g_completion_today = 0;
static string current_target_name(const string& target);  // 定义在 HTTP 服务一节
static void load_token_usage();
static void save_token_usage();
static void account_tokens(const JValue& usage);
static void expand_output_placeholders(vector<ParsedItem>& items, const string& target,
                                       const string& sender_id);

// 是否走本地模拟（auto 模式下 OneBot 探活失败即模拟）
static bool using_simulate() {
    if (config.force_simulate()) return true;
    if (config.send_mode == "real") return false;
    return !g_onebot_up;
}

// ───────────────────────── 结构 <-> JSON ─────────────────────────

static JValue group_to_json(const Group& g) {
    JValue j = JValue::mkObj();
    j.set("id", JValue::mkStr(g.id));
    j.set("name", JValue::mkStr(g.name));
    j.set("members", JValue::mkNum(g.members));
    j.set("enabled", JValue::mkBool(g.enabled));
    JValue m = JValue::mkArr();
    for (const auto& s : g.selected_members) m.push(JValue::mkStr(s));
    j.set("selectedMembers", m);
    return j;
}

static Group group_from_json(const JValue& j) {
    Group g;
    g.id = j.sget("id");
    g.name = j.sget("name", g.id);
    g.members = (int)j.iget("members");
    g.enabled = j.bget("enabled", true);
    const JValue* m = j.find("selectedMembers") ? j.find("selectedMembers") : j.find("selected_members");
    if (m && m->isArr()) for (const auto& v : m->aval) if (v.isStr()) g.selected_members.push_back(v.sval);
    return g;
}

static JValue private_to_json(const PrivateChat& p) {
    JValue j = JValue::mkObj();
    j.set("id", JValue::mkStr(p.id));
    j.set("name", JValue::mkStr(p.name));
    j.set("enabled", JValue::mkBool(p.enabled));
    return j;
}

static PrivateChat private_from_json(const JValue& j) {
    PrivateChat p;
    p.id = j.sget("id");
    p.name = j.sget("name", p.id);
    p.enabled = j.bget("enabled", true);
    return p;
}

static JValue sticker_to_json(const Sticker& s) {
    JValue j = JValue::mkObj();
    j.set("id", JValue::mkNum((double)s.id));
    j.set("name", JValue::mkStr(s.name));
    j.set("description", JValue::mkStr(s.description));
    j.set("url", JValue::mkStr(s.url));
    j.set("favorite", JValue::mkBool(s.favorite));
    j.set("note", JValue::mkStr(s.note));
    j.set("emojiId", JValue::mkStr(s.emoji_id));
    j.set("md5", JValue::mkStr(s.md5));
    j.set("source", JValue::mkStr(s.source));
    return j;
}

static Sticker sticker_from_json(const JValue& j) {
    Sticker s;
    s.id = j.iget("id");
    s.name = j.sget("name");
    s.description = j.sget("description");
    s.url = j.sget("url");
    s.favorite = j.bget("favorite", false);
    s.note = j.sget("note");
    s.emoji_id = j.has("emojiId") ? j.sget("emojiId") : j.sget("emoji_id");
    if (s.emoji_id.empty()) s.emoji_id = j.sget("resId");
    s.md5 = j.sget("md5");
    s.source = j.sget("source", "manual");
    return s;
}

static JValue message_to_json(const Message& m) {
    JValue j = JValue::mkObj();
    j.set("id", JValue::mkStr(m.id));
    j.set("batch", JValue::mkStr(m.batch));
    j.set("target", JValue::mkStr(m.target));
    j.set("targetName", JValue::mkStr(m.target_name));
    j.set("dir", JValue::mkStr(m.dir));
    j.set("sender", JValue::mkStr(m.sender));
    j.set("type", JValue::mkStr(m.type));
    j.set("content", JValue::mkStr(m.content));
    j.set("stickerId", JValue::mkNum((double)m.sticker_id));
    j.set("timeRaw", JValue::mkStr(m.time_raw));
    j.set("epoch", JValue::mkNum((double)m.epoch));
    j.set("status", JValue::mkStr(m.status));
    j.set("error", JValue::mkStr(m.error));
    j.set("createdAt", JValue::mkNum((double)m.created_at));
    j.set("qqMsgId", JValue::mkStr(m.qq_msg_id));
    j.set("senderId", JValue::mkStr(m.sender_id));
    j.set("atId", JValue::mkStr(m.at_id));
    return j;
}

static Message message_from_json(const JValue& j) {
    Message m;
    m.id = j.sget("id");
    m.batch = j.sget("batch");
    m.target = j.sget("target");
    m.target_name = j.sget("targetName", j.sget("target_name"));
    m.dir = j.sget("dir", "out");
    m.sender = j.sget("sender");
    m.type = j.sget("type", "send");
    m.content = j.sget("content");
    m.sticker_id = j.iget("stickerId", j.iget("sticker_id"));
    m.time_raw = j.sget("timeRaw", j.sget("time_raw"));
    m.epoch = j.iget("epoch");
    m.status = j.sget("status", "sent");
    m.error = j.sget("error");
    m.created_at = j.iget("createdAt", j.iget("created_at"));
    m.qq_msg_id = j.sget("qqMsgId", j.sget("qq_msg_id"));
    m.sender_id = j.sget("senderId", j.sget("sender_id"));
    m.at_id = j.sget("atId", j.sget("at_id"));
    return m;
}

// ───────────────────────── 配置 / 数据文件 ─────────────────────────

static bool load_config() {
    string text;
    if (!read_text_file(config_file, text)) {
        cerr << "无法读取配置文件: " << config_file << "，使用默认值" << endl;
        return false;
    }
    size_t i = 0;
    JValue j;
    if (!JValue::parse(text, i, j) || !j.isObj()) {
        cerr << "配置文件格式错误: " << config_file << "，使用默认值" << endl;
        return false;
    }
    config.api_url = j.sget("api_url");
    config.api_key = j.sget("api_key");
    if (!j.sget("base_url").empty()) config.base_url = j.sget("base_url");
    if (!j.sget("model").empty()) config.model = j.sget("model");
    config.persona = j.sget("persona");
    if (!j.sget("prompt_template").empty()) config.prompt_template = j.sget("prompt_template");
    if (!j.sget("onebot_url").empty()) config.onebot_url = j.sget("onebot_url");
    config.onebot_token = j.sget("onebot_token");
    if (!j.sget("send_mode").empty()) config.send_mode = j.sget("send_mode");
    config.default_group = j.iget("default_group");
    config.port = (int)j.iget("port", config.port);
    config.max_tokens = (int)j.iget("max_tokens", config.max_tokens);
    config.temperature = j.nget("temperature", config.temperature);
    config.send_interval_ms = j.iget("send_interval_ms", config.send_interval_ms);
    config.attach_format_hint = j.bget("attach_format_hint", config.attach_format_hint);
    config.attach_sticker_list = j.bget("attach_sticker_list", config.attach_sticker_list);
    config.sticker_count = (int)j.iget("sticker_count", config.sticker_count);
    config.auto_reply = j.bget("auto_reply", config.auto_reply);
    config.auto_check_ms = (int)j.iget("auto_check_ms", config.auto_check_ms);
    config.reply_probability = (int)j.iget("reply_probability", config.reply_probability);
    const JValue* kw = j.find("must_reply_keywords");
    if (kw && kw->isArr()) {
        config.must_reply_keywords.clear();
        for (const auto& v : kw->aval) if (v.isStr() && !v.sval.empty()) config.must_reply_keywords.push_back(v.sval);
    }
    config.reply_cooldown_ms = j.iget("reply_cooldown_ms", config.reply_cooldown_ms);
    config.context_window = (int)j.iget("context_window", config.context_window);
    config.append_hint = j.bget("append_hint", config.append_hint);
    config.auto_confirm = j.bget("auto_confirm", config.auto_confirm);
    config.private_always_reply = j.bget("private_always_reply", config.private_always_reply);
    config.sticker_long_edge = (int)j.iget("sticker_long_edge", config.sticker_long_edge);
    if (config.model.empty()) config.model = "glm-4-flash";
    if (config.port <= 0 || config.port > 65535) config.port = 8788;
    if (config.send_mode != "auto" && config.send_mode != "real" && config.send_mode != "simulate")
        config.send_mode = "auto";
    return true;
}

static bool save_config() {
    JValue j = JValue::mkObj();
    string key = config.key();
    j.set("api_url", JValue::mkStr(key));  // 原字段位置仍存 API Key，保持兼容
    j.set("api_key", JValue::mkStr(key));
    j.set("base_url", JValue::mkStr(config.base_url));
    j.set("model", JValue::mkStr(config.model));
    j.set("persona", JValue::mkStr(config.persona));
    j.set("prompt_template", JValue::mkStr(config.prompt_template));
    j.set("onebot_url", JValue::mkStr(config.onebot_url));
    j.set("onebot_token", JValue::mkStr(config.onebot_token));
    j.set("send_mode", JValue::mkStr(config.send_mode));
    j.set("default_group", JValue::mkNum((double)config.default_group));
    j.set("port", JValue::mkNum(config.port));
    j.set("max_tokens", JValue::mkNum(config.max_tokens));
    j.set("temperature", JValue::mkNum(config.temperature));
    j.set("send_interval_ms", JValue::mkNum((double)config.send_interval_ms));
    j.set("attach_format_hint", JValue::mkBool(config.attach_format_hint));
    j.set("attach_sticker_list", JValue::mkBool(config.attach_sticker_list));
    j.set("sticker_count", JValue::mkNum(config.sticker_count));
    j.set("auto_reply", JValue::mkBool(config.auto_reply));
    j.set("auto_check_ms", JValue::mkNum(config.auto_check_ms));
    j.set("reply_probability", JValue::mkNum(config.reply_probability));
    JValue kw = JValue::mkArr();
    for (const auto& k : config.must_reply_keywords) kw.push(JValue::mkStr(k));
    j.set("must_reply_keywords", kw);
    j.set("reply_cooldown_ms", JValue::mkNum((double)config.reply_cooldown_ms));
    j.set("context_window", JValue::mkNum(config.context_window));
    j.set("append_hint", JValue::mkBool(config.append_hint));
    j.set("auto_confirm", JValue::mkBool(config.auto_confirm));
    j.set("private_always_reply", JValue::mkBool(config.private_always_reply));
    j.set("sticker_long_edge", JValue::mkNum(config.sticker_long_edge));
    return write_text_file(config_file, j.dump());
}

static JValue config_to_json() {
    JValue j = JValue::mkObj();
    j.set("apiUrl", JValue::mkStr(config.key()));
    j.set("baseUrl", JValue::mkStr(config.base_url));
    j.set("model", JValue::mkStr(config.model));
    j.set("persona", JValue::mkStr(config.persona));
    j.set("promptTemplate", JValue::mkStr(config.prompt_template));
    j.set("onebotUrl", JValue::mkStr(config.onebot_url));
    j.set("onebotToken", JValue::mkStr(config.onebot_token));
    j.set("sendMode", JValue::mkStr(config.send_mode));
    j.set("defaultGroup", JValue::mkStr(config.default_group ? to_string(config.default_group) : ""));
    j.set("port", JValue::mkNum(config.port));
    j.set("maxTokens", JValue::mkNum(config.max_tokens));
    j.set("temperature", JValue::mkNum(config.temperature));
    j.set("sendIntervalMs", JValue::mkNum((double)config.send_interval_ms));
    j.set("attachFormatHint", JValue::mkBool(config.attach_format_hint));
    j.set("attachStickerList", JValue::mkBool(config.attach_sticker_list));
    j.set("stickerCount", JValue::mkNum(config.sticker_count));
    j.set("defaultPromptTemplate", JValue::mkStr(DEFAULT_PROMPT_TEMPLATE));
    // 自动收信
    j.set("autoReply", JValue::mkBool(config.auto_reply));
    j.set("autoCheckMs", JValue::mkNum(config.auto_check_ms));
    j.set("replyProbability", JValue::mkNum(config.reply_probability));
    JValue kw = JValue::mkArr();
    for (const auto& k : config.must_reply_keywords) kw.push(JValue::mkStr(k));
    j.set("mustReplyKeywords", kw);
    j.set("replyCooldownMs", JValue::mkNum((double)config.reply_cooldown_ms));
    j.set("contextWindow", JValue::mkNum(config.context_window));
    j.set("appendHint", JValue::mkBool(config.append_hint));
    j.set("autoConfirm", JValue::mkBool(config.auto_confirm));
    j.set("privateAlwaysReply", JValue::mkBool(config.private_always_reply));
    j.set("stickerLongEdge", JValue::mkNum(config.sticker_long_edge));
    return j;
}

static void apply_config_json(const JValue& body) {
    auto pick = [&](const string& a, const string& b, string& dst) {
        const JValue* v = body.find(a);
        if (!v && !b.empty()) v = body.find(b);
        if (v && v->isStr()) { string s = trim(v->sval); if (!s.empty()) dst = s; }
    };
    string key;
    pick("apiUrl", "", key);
    if (key.empty()) pick("apiKey", "api_key", key);
    if (key.empty()) pick("api_url", "", key);
    if (!key.empty()) { config.api_key = key; config.api_url = key; }
    pick("baseUrl", "base_url", config.base_url);
    pick("model", "modelName", config.model);
    pick("persona", "", config.persona);
    pick("promptTemplate", "prompt_template", config.prompt_template);
    pick("onebotUrl", "onebot_url", config.onebot_url);
    pick("onebotToken", "onebot_token", config.onebot_token);
    pick("sendMode", "send_mode", config.send_mode);
    string dg;
    pick("defaultGroup", "default_group", dg);
    if (!dg.empty()) { try { config.default_group = stoll(dg); } catch (...) {} }
    config.port = (int)body.iget("port", config.port);
    config.max_tokens = (int)body.iget("maxTokens", config.max_tokens);
    config.temperature = body.nget("temperature", config.temperature);
    config.send_interval_ms = body.iget("sendIntervalMs", config.send_interval_ms);
    config.attach_format_hint = body.bget("attachFormatHint", config.attach_format_hint);
    config.attach_sticker_list = body.bget("attachStickerList", config.attach_sticker_list);
    config.sticker_count = (int)body.iget("stickerCount", config.sticker_count);
    if (config.send_mode != "auto" && config.send_mode != "real" && config.send_mode != "simulate")
        config.send_mode = "auto";
    if (config.port <= 0 || config.port > 65535) config.port = 8788;
    if (config.send_interval_ms < 0) config.send_interval_ms = 0;
    if (config.sticker_count <= 0 || config.sticker_count > 500) config.sticker_count = 60;
    if (config.max_tokens <= 0) config.max_tokens = 1200;
    config.auto_reply = body.bget("autoReply", body.bget("auto_reply", config.auto_reply));
    config.auto_check_ms = (int)body.iget("autoCheckMs", config.auto_check_ms);
    config.reply_probability = (int)body.iget("replyProbability", body.iget("reply_probability", config.reply_probability));
    const JValue* kw = body.find("mustReplyKeywords") ? body.find("mustReplyKeywords") : body.find("must_reply_keywords");
    if (kw && kw->isArr()) {
        config.must_reply_keywords.clear();
        for (const auto& v : kw->aval) if (v.isStr() && !v.sval.empty()) config.must_reply_keywords.push_back(v.sval);
    } else if (kw && kw->isStr()) {
        config.must_reply_keywords.clear();
        string s = kw->sval;
        size_t pos = 0;
        while (pos < s.size()) {
            size_t c = s.find_first_of(",， ", pos);
            if (c == string::npos) c = s.size();
            string one = trim(s.substr(pos, c - pos));
            if (!one.empty()) config.must_reply_keywords.push_back(one);
            pos = c + 1;
        }
    }
    config.reply_cooldown_ms = body.iget("replyCooldownMs", body.iget("reply_cooldown_ms", config.reply_cooldown_ms));
    config.context_window = (int)body.iget("contextWindow", body.iget("context_window", config.context_window));
    config.append_hint = body.bget("appendHint", body.bget("append_hint", config.append_hint));
    config.auto_confirm = body.bget("autoConfirm", body.bget("auto_confirm", config.auto_confirm));
    config.private_always_reply = body.bget("privateAlwaysReply", body.bget("private_always_reply", config.private_always_reply));
    if (config.auto_check_ms < 1000) config.auto_check_ms = 1000;
    if (config.reply_probability < 0) config.reply_probability = 0;
    if (config.reply_probability > 100) config.reply_probability = 100;
    if (config.reply_cooldown_ms < 0) config.reply_cooldown_ms = 0;
    if (config.context_window <= 0) config.context_window = 15;
    config.sticker_long_edge = (int)body.iget("stickerLongEdge", body.iget("sticker_long_edge", config.sticker_long_edge));
    if (config.sticker_long_edge < 0 || config.sticker_long_edge > 2000) config.sticker_long_edge = 32;
}

template <typename T>
static bool load_data_file(const string& filename, vector<T>& data, T (*conv)(const JValue&)) {
    string text;
    if (!read_text_file(filename, text)) return false;
    size_t i = 0;
    JValue j;
    if (!JValue::parse(text, i, j) || !j.isArr()) {
        cerr << "解析数据文件失败: " << filename << endl;
        return false;
    }
    data.clear();
    for (const auto& item : j.aval) data.push_back(conv(item));
    return true;
}

template <typename T>
static bool save_data_file(const string& filename, const vector<T>& data, JValue (*conv)(const T&)) {
    JValue j = JValue::mkArr();
    for (const auto& item : data) j.push(conv(item));
    return write_text_file(filename, j.dump());
}

static bool load_all_data() {
    bool ok = true;
    ok = load_data_file(groups_file, groups, group_from_json) && ok;
    ok = load_data_file(private_file, private_chats, private_from_json) && ok;
    ok = load_data_file(stickers_file, stickers, sticker_from_json) && ok;
    ok = load_data_file(messages_file, message_list, message_from_json) && ok;
    return ok;
}

static bool save_groups() { return save_data_file(groups_file, groups, group_to_json); }
static bool save_privates() { return save_data_file(private_file, private_chats, private_to_json); }
static bool save_stickers() { return save_data_file(stickers_file, stickers, sticker_to_json); }
static bool save_messages() {
    if (message_list.size() > 5000) message_list.erase(message_list.begin(), message_list.begin() + (message_list.size() - 5000));
    return save_data_file(messages_file, message_list, message_to_json);
}

static void init_data_directory() {
    error_code ec;
    fs::create_directories(fs::u8path(join_path(app_dir, "data")), ec);
    config_file = join_path(app_dir, "config.json");
    groups_file = join_path(app_dir, "data/groups.json");
    private_file = join_path(app_dir, "data/private.json");
    stickers_file = join_path(app_dir, "data/stickers.json");
    messages_file = join_path(app_dir, "data/messages.json");
    poll_state_file = join_path(app_dir, "data/poll_state.json");
    user_lists_file = join_path(app_dir, "data/user_lists.json");
    tokens_file = join_path(app_dir, "data/token_usage.json");
    string tmp;
    if (!read_text_file(config_file, tmp)) {
        cout << "首次运行：生成默认 config.json（默认提示词已按需求写入）" << endl;
        save_config();
    }
    if (!read_text_file(groups_file, tmp)) write_text_file(groups_file, "[]");
    if (!read_text_file(private_file, tmp)) write_text_file(private_file, "[]");
    if (!read_text_file(stickers_file, tmp)) write_text_file(stickers_file, "[]");
    if (!read_text_file(messages_file, tmp)) write_text_file(messages_file, "[]");
}

// ───────────────────────── 表情包 ─────────────────────────

static Sticker* find_sticker(int64_t id) {
    for (auto& s : stickers) if (s.id == id) return &s;
    return nullptr;
}

static int64_t next_sticker_id() {
    int64_t mx = 0;
    for (const auto& s : stickers) mx = max(mx, s.id);
    return mx + 1;
}

// 从 QQ（OneBot fetch_custom_face_detail）同步收藏表情；编号稳定，命中 emoji_id 不重编号。
static bool sync_stickers_from_qq(string& err, int& added, int& total) {
    added = 0;
    total = 0;
    if (config.onebot_url.empty()) { err = "未配置 onebot_url"; return false; }
    JValue body = JValue::mkObj();
    body.set("count", JValue::mkNum(config.sticker_count));
    vector<pair<string, string>> headers = {{"Content-Type", "application/json"}};
    if (!config.onebot_token.empty()) headers.push_back({"Authorization", "Bearer " + config.onebot_token});
    HttpResp r = http_call(url_path_join(config.onebot_url, "/fetch_custom_face_detail"), "POST",
                           body.dump(), headers, 20000);
    if (!r.ok && r.body.empty()) {
        err = "OneBot fetch_custom_face_detail 失败：" + (r.err.empty() ? to_string(r.status) : r.err);
        if (r.status == 426) err += "（426：onebot_url 要填 OneBot HTTP API 端口，不是 WS 端口）";
        return false;
    }
    JValue resp = JValue::parse_or_null(r.body);
    if (resp.sget("status") != "ok") {
        err = "fetch_custom_face_detail 失败：" +
              resp.sget("wording", resp.sget("status", "非 JSON（HTTP " + to_string(r.status) + "）")) +
              (resp.sget("wording") == "unauthorized" || r.status == 401
                   ? "（OneBot accessToken 不对或未填，去配置里填 accessToken）" : "");
        return false;
    }
    const JValue* data = resp.find("data");
    if (!data || !data->isArr()) { err = "OneBot 没返回表情数组（SnowLuma 可能不支持该扩展接口）"; return false; }
    lock_guard<recursive_mutex> lock(data_mutex);
    for (const auto& e : data->aval) {
        string eid = e.sget("emoji_id");
        if (eid.empty()) eid = e.sget("id");
        if (eid.empty()) eid = e.sget("resId");
        string url = e.sget("url");
        if (url.empty()) continue;
        string desc = e.sget("desc");
        Sticker* exist = nullptr;
        if (!eid.empty()) {
            for (auto& s : stickers) if (!s.emoji_id.empty() && s.emoji_id == eid) { exist = &s; break; }
        }
        if (!exist) for (auto& s : stickers) if (s.source == "qq" && s.url == url) { exist = &s; break; }
        if (exist) {
            exist->url = url;
            if (!desc.empty()) exist->description = desc;
            exist->md5 = e.sget("md5", exist->md5);
            if (exist->name.empty()) exist->name = desc;
        } else {
            Sticker s;
            s.id = next_sticker_id();
            s.emoji_id = eid;
            s.url = url;
            s.md5 = e.sget("md5");
            s.description = desc;
            s.name = desc.empty() ? ("表情" + to_string((long long)s.id)) : desc;
            s.source = "qq";
            stickers.push_back(s);
            added++;
        }
    }
    stable_sort(stickers.begin(), stickers.end(), [](const Sticker& a, const Sticker& b) {
        return a.id < b.id;
    });
    total = (int)stickers.size();
    save_stickers();
    return true;
}

// ───────────────────────── 模板 / 时间 / 解析 ─────────────────────────

// 单遍扫描替换占位符：避免 regex_replace 把内容里的 $ & 当转义，也避免 %name 被 %s 抢先吃掉。
static string process_message_template(const string& template_str, const string& content,
                                       const string& sender_name, const string& sender_id,
                                       const string& target) {
    map<string, string> vals;
    vals["%name"] = sender_name;
    vals["%id"] = sender_id;
    vals["%s"] = content;
    vals["%time"] = now_string();
    vals["%model"] = config.model;
    vals["%target"] = target;
    vals["%persona"] = config.persona;
    vals["%token"] = to_string((long long)g_tokens_today);   // 今日已用 token
    {
        lock_guard<recursive_mutex> lock(data_mutex);
        string gname = current_target_name(target);
        vals["%gname"] = gname.empty() ? target : gname;      // 群名（私聊时退化为 target）
    }
    vector<string> order = {"%name", "%persona", "%model", "%gname", "%token", "%target", "%time", "%id", "%s"};
    string result;
    size_t i = 0;
    while (i < template_str.size()) {
        if (template_str[i] != '%') { result += template_str[i++]; continue; }
        bool matched = false;
        for (const auto& tk : order) {
            if (template_str.compare(i, tk.size(), tk) == 0) {
                result += vals[tk];
                i += tk.size();
                matched = true;
                break;
            }
        }
        if (!matched) result += template_str[i++];
    }
    return result;
}

// time 解析：4 位 HHMM（今天）/ 6 位 YYMMDD / 8 位 YYMMDDHH / 10 位 YYMMDDHHMM / 12 位 YYYYMMDDHHMM，
// 也容忍 "08:30" 这类带分隔符写法（只取数字）。早于当前时间 -> 报错。
static bool parse_schedule(const string& raw, int64_t& epoch_ms, string& err) {
    err.clear();
    epoch_ms = 0;
    string digits;
    for (char c : raw) if (isdigit((unsigned char)c)) digits += c;
    auto num = [](const string& s, size_t at) -> int {
        if (at + 2 > s.size()) return -1;
        int v = 0;
        for (size_t k = 0; k < 2; k++) {
            if (!isdigit((unsigned char)s[at + k])) return -1;
            v = v * 10 + (s[at + k] - '0');
        }
        return v;
    };
    time_t nowt = time(nullptr);
    struct tm now_tm;
    localtime_s(&now_tm, &nowt);
    int y = now_tm.tm_year + 1900, mo = now_tm.tm_mon + 1, d = now_tm.tm_mday;
    int hh = now_tm.tm_hour, mi = now_tm.tm_min + 1;  // 只写日期时给“下一分钟”，避免被判过期
    bool date_only = false;
    if (digits.size() == 4) {
        hh = num(digits, 0); mi = num(digits, 2);
    } else if (digits.size() == 6) {
        y = 2000 + num(digits, 0); mo = num(digits, 2); d = num(digits, 4); date_only = true;
    } else if (digits.size() == 8) {
        y = 2000 + num(digits, 0); mo = num(digits, 2); d = num(digits, 4); hh = num(digits, 6); date_only = true;
    } else if (digits.size() == 10) {
        y = 2000 + num(digits, 0); mo = num(digits, 2); d = num(digits, 4);
        hh = num(digits, 6); mi = num(digits, 8);
    } else if (digits.size() == 12) {
        y = stoi(digits.substr(0, 4)); mo = num(digits, 4); d = num(digits, 6);
        hh = num(digits, 8); mi = num(digits, 10);
    } else {
        err = "时间格式不对：" + raw + "（4 位 HHMM 表示今天发送，或 YYMMDD + 4 位 HHMM 共 10 位）";
        return false;
    }
    if (hh < 0 || hh > 23 || mi < 0 || mi > 59 || mo < 1 || mo > 12 || d < 1 || d > 31) {
        err = "时间数值不合法：" + raw;
        return false;
    }
    struct tm tv;
    memset(&tv, 0, sizeof(tv));
    tv.tm_year = y - 1900; tv.tm_mon = mo - 1; tv.tm_mday = d;
    tv.tm_hour = hh; tv.tm_min = mi; tv.tm_sec = 0; tv.tm_isdst = -1;
    time_t et = mktime(&tv);
    if (et == (time_t)-1) { err = "时间无法解析：" + raw; return false; }
    epoch_ms = (int64_t)et * 1000;
    if (epoch_ms <= now_ms()) {
        err = string("发送时间比现在早：") + raw + " → " + fmt_time(epoch_ms) +
              (date_only ? "（只写了日期，按时分 " + string(hh < 10 ? "0" : "") + to_string(hh) + ":" +
               (mi < 10 ? "0" : "") + to_string(mi) + "）" : "") +
              "，现在是 " + now_string() + "，已拒绝";
        return false;
    }
    return true;
}

[[maybe_unused]] static bool validate_time_format(const string& time_str) {  // 保留原方案函数名
    if (time_str.empty()) return true;
    string err;
    int64_t epoch = 0;
    return parse_schedule(time_str, epoch, err);
}

static string strip_code_fence(const string& s) {
    string out, line;
    bool in_fence = false;
    istringstream is(s);
    while (getline(is, line)) {
        string lt = trim(line);
        if (!lt.empty() && lt[0] == '`') { in_fence = !in_fence; continue; }
        if (!in_fence) out += line + "\n";
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out.empty() ? s : out;
}

// 去掉 {send:...} / {image:...} 指令（含 ,time:...），只留 AI 的自然语言内容（日志展示用）。
static string strip_directives(const string& content) {
    string text = strip_code_fence(content);
    string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '{') {
            size_t j = text.find('}', i);
            if (j != string::npos) {
                string inner = trim(text.substr(i + 1, j - i - 1));
                size_t colon = inner.find(':');
                string head = trim(colon == string::npos ? inner : inner.substr(0, colon));
                if (head == "send" || head == "image") { i = j + 1; continue; }
            }
        }
        out += text[i];
        i++;
    }
    // 指令被移除后可能留下连续空行，压成单换行
    string res;
    for (size_t k = 0; k < out.size(); k++) {
        char c = out[k];
        if (c == '\n' && !res.empty() && (res.back() == '\n' || res.back() == ' ')) {
            while (!res.empty() && (res.back() == '\n' || res.back() == ' ')) res.pop_back();
            res += '\n';
            continue;
        }
        res += c;
    }
    return trim(res);
}

// 解析 {send:...} / {image:编号}（可带 ,time:...）。被丢弃的条目原因写进 errors。
static vector<ParsedItem> parse_message_content(const string& content, vector<string>& errors) {
    vector<ParsedItem> result;
    errors.clear();
    string text = strip_code_fence(content);
    size_t i = 0;
    while (i < text.size()) {
        size_t lb = text.find('{', i);
        if (lb == string::npos) break;
        size_t rb = text.find('}', lb + 1);
        string head = to_lower(trim(text.substr(lb + 1, min<size_t>(16, text.size() - lb - 1))));
        bool is_tag = head.compare(0, 4, "send") == 0 || head.compare(0, 5, "image") == 0;
        if (!is_tag) { i = lb + 1; continue; }
        if (rb == string::npos) {
            errors.push_back("{ 缺少右花括号（" + head + "…），该条被忽略");
            break;
        }
        string inner = text.substr(lb + 1, rb - lb - 1);
        i = rb + 1;
        // 标签分隔符允许半角 ':' 或全角 '：'（模型经常混用中文标点）
        size_t colon = inner.find(':');
        size_t colon_fw = inner.find("\xEF\xBC\x9A");
        size_t colon_len = 1;
        if (colon_fw != string::npos && (colon == string::npos || colon_fw < colon)) {
            colon = colon_fw;
            colon_len = 3;
        }
        if (colon == string::npos) { errors.push_back("{ 里缺少冒号：{" + inner + "}"); continue; }
        string tag = trim(inner.substr(0, colon));
        string rest = inner.substr(colon + colon_len);
        ParsedItem item;
        // 只按最后一个 time 分隔符切分，内容里的逗号不受影响。
        // 分隔符 = (半角/全角逗号) + "time" + (半角/全角冒号)，模型经常混用中文标点。
        size_t tp = string::npos, tlen = 6;
        {
            const string comma_half = ",";
            const string comma_fw = "\xEF\xBC\x8C";   // ，
            const string colon_half = ":";
            const string colon_fw = "\xEF\xBC\x9A";   // ：
            vector<pair<string, size_t>> cands;
            for (const auto* c : {comma_half.c_str(), comma_fw.c_str()})
                for (const auto* k : {colon_half.c_str(), colon_fw.c_str()}) {
                    string s = string(c) + "time" + k;
                    cands.push_back(make_pair(s, s.size()));
                }
            for (const auto& cand : cands) {
                size_t p = rest.rfind(cand.first);
                if (p != string::npos && (tp == string::npos || p > tp)) { tp = p; tlen = cand.second; }
            }
        }
        if (tp != string::npos) {
            item.time_raw = trim(rest.substr(tp + tlen));
            rest = rest.substr(0, tp);
        }
        if (to_lower(tag) == "send") {
            item.type = "send";
            size_t bs = rest.find_first_not_of(" \t");
            rest = (bs == string::npos) ? "" : rest.substr(bs);
            while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t')) rest.pop_back();
            if (rest.empty()) { errors.push_back("{send:} 内容为空，已跳过"); continue; }
            item.content = rest;
        } else {
            item.type = "image";
            string sid = trim(rest);
            if (sid.empty() || sid.find_first_not_of("0123456789") != string::npos) {
                errors.push_back("表情包编号不是数字：{image:" + sid + "}");
                continue;
            }
            int64_t want = stoll(sid);
            const Sticker* found = nullptr;
            {
                lock_guard<recursive_mutex> lock(data_mutex);
                found = find_sticker(want);
            }
            if (!found || found->url.empty()) {
                errors.push_back("未找到表情包编号 " + sid + "（先在表情包管理里同步 QQ 表情或手动添加）");
                continue;
            }
            item.content = found->url;
            item.sticker_id = found->id;
        }
        if (!item.time_raw.empty()) {
            string err;
            if (!parse_schedule(item.time_raw, item.epoch, err)) { errors.push_back(err); continue; }
        } else {
            item.epoch = 0;
        }
        result.push_back(item);
    }
    return result;
}

static JValue item_to_json(const string& type, const string& content, int64_t sticker_id,
                           const string& time_raw, int64_t epoch, const string& at_id = "") {
    JValue j = JValue::mkObj();
    j.set("type", JValue::mkStr(type));
    j.set("content", JValue::mkStr(content));
    j.set("stickerId", JValue::mkNum((double)sticker_id));
    j.set("timeRaw", JValue::mkStr(time_raw));
    j.set("epoch", JValue::mkNum((double)epoch));
    j.set("timeText", JValue::mkStr(epoch ? fmt_time(epoch) : "立即"));
    j.set("immediate", JValue::mkBool(!epoch));
    if (!at_id.empty()) j.set("atId", JValue::mkStr(at_id));
    if (sticker_id) {
        string label;
        {
            lock_guard<recursive_mutex> lock(data_mutex);
            Sticker* s = find_sticker(sticker_id);
            if (s) label = s->note.empty() ? s->name : s->note;
        }
        j.set("stickerName", JValue::mkStr(label));
    }
    return j;
}

static JValue parsed_items_json(const vector<ParsedItem>& items, const vector<string>& errors) {
    JValue arr = JValue::mkArr();
    for (const auto& it : items)
        arr.push(item_to_json(it.type, it.content, it.sticker_id, it.time_raw, it.epoch, it.at_id));
    JValue errs = JValue::mkArr();
    for (const auto& e : errors) errs.push(JValue::mkStr(e));
    JValue out = JValue::mkObj();
    out.set("items", arr);
    out.set("errors", errs);
    return out;
}

// 解析结果落成“待确认”批次（前端询问还发不发），确认后才进发送队列
static string enqueue_pending(const vector<ParsedItem>& items, const string& target,
                              const string& target_name, const string& sender) {
    string batch = make_id("batch");
    int64_t n = now_ms();
    lock_guard<recursive_mutex> lock(data_mutex);
    for (const auto& it : items) {
        Message m;
        m.id = make_id("msg");
        m.batch = batch;
        m.target = target;
        m.target_name = target_name;
        m.dir = "out";
        m.sender = sender;
        m.type = it.type;
        m.content = it.content;
        m.sticker_id = it.sticker_id;
        m.time_raw = it.time_raw;
        m.epoch = it.epoch ? it.epoch : n;
        m.status = "pending";
        m.created_at = n;
        m.at_id = it.at_id;
        message_list.push_back(m);
    }
    save_messages();
    return batch;
}

static JValue pending_batches_json() {
    vector<string> order;
    map<string, JValue> batches;
    for (const auto& m : message_list) {
        if (m.status != "pending") continue;
        if (batches.find(m.batch) == batches.end()) {
            order.push_back(m.batch);
            JValue b = JValue::mkObj();
            b.set("batchId", JValue::mkStr(m.batch));
            b.set("target", JValue::mkStr(m.target));
            b.set("targetName", JValue::mkStr(m.target_name));
            b.set("sender", JValue::mkStr(m.sender));
            b.set("createdAt", JValue::mkNum((double)m.created_at));
            b.set("createdText", JValue::mkStr(fmt_time(m.created_at)));
            b.set("items", JValue::mkArr());
            batches[m.batch] = b;
        }
        batches[m.batch]["items"].push(
            item_to_json(m.type, m.content, m.sticker_id, m.time_raw, m.epoch, m.at_id));
    }
    JValue arr = JValue::mkArr();
    for (const auto& b : order) arr.push(batches[b]);
    return arr;
}

// ───────────────────────── AI 调用 ─────────────────────────

static string today_date_str() {
    time_t t = time(nullptr);
    struct tm lt = *localtime(&t);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &lt);
    return buf;
}

static void load_token_usage() {
    string text;
    if (!read_text_file(tokens_file, text)) return;
    JValue j = JValue::parse_or_null(text);
    if (!j.isObj()) return;
    lock_guard<recursive_mutex> lock(data_mutex);
    g_token_date = j.sget("date");
    if (g_token_date != today_date_str()) {  // 不是今天的数据：清零重记
        g_token_date = today_date_str();
        g_tokens_today = g_prompt_today = g_completion_today = 0;
        return;
    }
    g_tokens_today = j.iget("total");
    g_prompt_today = j.iget("prompt");
    g_completion_today = j.iget("completion");
}

static void save_token_usage() {
    JValue j = JValue::mkObj();
    j.set("date", JValue::mkStr(g_token_date));
    j.set("total", JValue::mkNum((double)g_tokens_today));
    j.set("prompt", JValue::mkNum((double)g_prompt_today));
    j.set("completion", JValue::mkNum((double)g_completion_today));
    write_text_file(tokens_file, j.dump());
}

// 把一次模型调用的 usage 记进今日累计（跨天自动清零）。
static void account_tokens(const JValue& usage) {
    int64_t p = usage.iget("prompt_tokens");
    int64_t c = usage.iget("completion_tokens");
    int64_t t = usage.iget("total_tokens");
    if (t <= 0) t = p + c;
    if (t <= 0) return;
    string d = today_date_str();
    {
        lock_guard<recursive_mutex> lock(data_mutex);
        if (g_token_date != d) {
            g_token_date = d;
            g_tokens_today = g_prompt_today = g_completion_today = 0;
        }
        g_prompt_today += p;
        g_completion_today += c;
        g_tokens_today += t;
        save_token_usage();
    }
    cout << "[Token] 本次 " << p << " + " << c << " = " << t
         << "，今日累计 " << g_tokens_today << endl;
}

// AI 输出内容里的占位符：
//   %token → 今日已用 token 数；%gname → 当前群名；%id → 移除文字并改成 @回话的人（段首 at）
static void expand_output_placeholders(vector<ParsedItem>& items, const string& target,
                                       const string& sender_id) {
    bool sender_is_qq = !sender_id.empty() && sender_id.find_first_not_of("0123456789") == string::npos;
    for (auto& it : items) {
        if (it.content.find('%') == string::npos) continue;
        string& s = it.content;
        for (size_t pos = 0; (pos = s.find("%token", pos)) != string::npos; ) {
            string rep = to_string(g_tokens_today);
            s = s.substr(0, pos) + rep + s.substr(pos + 6);
            pos += rep.size();
        }
        for (size_t pos = 0; (pos = s.find("%gname", pos)) != string::npos; ) {
            string rep;
            {
                lock_guard<recursive_mutex> lock(data_mutex);
                rep = current_target_name(target);
            }
            if (rep.empty()) rep = target;
            s = s.substr(0, pos) + rep + s.substr(pos + 6);
            pos += rep.size();
        }
        while (s.find("%id") != string::npos) {
            size_t pos = s.find("%id");
            s = s.substr(0, pos) + s.substr(pos + 3);
            if (sender_is_qq) it.at_id = sender_id;
        }
        if (!it.at_id.empty() && !s.empty() && s[0] != ' ' && s[0] != '\n') s = " " + s;
    }
}

static string build_system_prompt() {
    string sys;
    if (!config.persona.empty()) sys += "你的人设：" + config.persona + "\n";
    sys += "你在扮演 QQ 里的群友，说话要像真人：口语、短、不要解释自己在做什么。\n"
           "输出格式（严格遵守，不要 Markdown 代码块，不要多余说明）：\n"
           "1. 每条要发的文字消息写成 {send:内容}，一条气泡一个 {send:...}。\n"
           "2. 没写 {send:...} 就代表这条不发；这次不想说话就直接输出普通文本或 [SILENT]，别输出空的 {send:}。\n"
           "3. 一次回复里可以写多个 {send:...}，确认后会同时连发出去。\n"
           "4. 表情包写成 {image:编号}，编号只能用下面列出的，不能自己编。\n"
           "5. 定时发送写成 {send:内容,time:HHMM}（HHMM 是今天的 4 位小时分钟）或 "
           "{send:内容,time:YYMMDDHHMM}；{image:编号,time:...} 同理。时间早于当前时间会直接报错。\n"
           "6. 因为发之前会先问你“还发不发”，想连发多条就一次全写出来。\n"
           "7. 消息内容里可以写这三个占位符，发送前会自动替换成真实值：%token = 今日已用的 token 数，"
           "%gname = 当前群名，%id = @ 正在跟你说话的这个人（例如「%id 你今天行啊」会先 @ 对方再接着说）。"
           "别自己造其他 %xx，那些不会被替换。\n\n"
           "群聊分寸（重要）：\n"
           "- 你不是客服，也不是每条消息都要接。大多数闲聊你都可以不说话：话题和你无关、"
           "你在忙、接不上话，就直接 [SILENT] 或输出普通文本，不要硬聊、不要每轮都发言。\n"
           "- 值得开口的情况：有人 @ 你或喊你、直接问你问题、聊到你擅长的东西、群里冷场了很久，"
           "或者你的人设就是忍不住想怼一句。\n"
           "- 回复欲望要低：宁可少说，也不要抢话刷屏。\n";
    if (config.append_hint) {
        sys += "- 说完之后如果还有想说的，可以在同一次回复里多写一两条 {send:...} 追加，像真人连发；"
               "偶尔（不是每次）给稍后设一条定时消息 {send:内容,time:HHMM}，比如答应等会儿做某事、"
               "说晚点再聊、吊人胃口；不要每轮都定时。\n";
    }
    sys += "\n";
    sys += "当前时间：" + now_string() + "（本地时区）。提示词里的 %time 就代表这个时间。\n";
    sys += "今日 token 用量：" + to_string((long long)g_tokens_today) + "（内容里写 %token 会替换成这个数）。\n";
    if (config.attach_sticker_list) {
        string list;
        int n = 0;
        {
            lock_guard<recursive_mutex> lock(data_mutex);
            for (const auto& s : stickers) {
                if (s.url.empty()) continue;
                if (n++) list += "、";
                string label = !s.note.empty() ? s.note : (!s.description.empty() ? s.description : s.name);
                if (label.empty()) label = "表情";
                list += to_string((long long)s.id) + "=" + label;
                if (n >= 40) break;
            }
        }
        if (n > 0) sys += "可用表情包编号（" + to_string(n) + " 个）：" + list + "。\n";
        else sys += "可用表情包：暂无（不要输出 {image:...}）。\n";
    }
    return sys;
}

// 原方案里的 call_snowluma_api：这里换成真实 HTTP 调用（智谱 GLM / OpenAI 兼容）
static bool call_ai(const string& prompt, const string& model_override, JValue& out, string& err) {
    err.clear();
    string key = config.key();
    if (key.empty()) { err = "未配置 API Key（config.json 的 api_url / api_key 字段）"; return false; }
    string model = model_override.empty() ? config.model : trim(model_override);
    if (model.empty()) { err = "需要输入模型名字（例如 glm-4-flash）"; return false; }
    if (config.base_url.empty()) { err = "未配置接口地址 base_url"; return false; }

    JValue body = JValue::mkObj();
    body.set("model", JValue::mkStr(model));
    JValue msgs = JValue::mkArr();
    if (config.attach_format_hint) {
        JValue sysm = JValue::mkObj();
        sysm.set("role", JValue::mkStr("system"));
        sysm.set("content", JValue::mkStr(build_system_prompt()));
        msgs.push(sysm);
    }
    JValue um = JValue::mkObj();
    um.set("role", JValue::mkStr("user"));
    um.set("content", JValue::mkStr(prompt));
    msgs.push(um);
    body.set("messages", msgs);
    body.set("temperature", JValue::mkNum(config.temperature));
    body.set("max_tokens", JValue::mkNum(config.max_tokens));
    body.set("stream", JValue::mkBool(false));

    string url = config.base_url;
    if (url.find("chat/completions") == string::npos) url = url_path_join(url, "chat/completions");
    vector<pair<string, string>> headers = {
        {"Content-Type", "application/json"}, {"Authorization", "Bearer " + key}};
    HttpResp r = http_call(url, "POST", body.dump(), headers, 90000);
    JValue resp = JValue::parse_or_null(r.body);
    if (!r.ok) {
        string msg = resp.find("error") ? resp.find("error")->sget("message") : "";
        err = "模型接口失败 HTTP " + to_string(r.status) + (msg.empty() ? (" " + r.err) : ("：" + msg));
        if (r.status == 401 || r.status == 403) err += "（API Key 无效，或该 Key 没有这个模型的权限）";
        if (r.status == 404) err += "（模型名字或接口地址不对）";
        return false;
    }
    if (!resp.isObj()) { err = "模型返回不是 JSON：" + r.body.substr(0, 200); return false; }
    if (resp.find("error")) {
        err = "模型报错：" + resp.find("error")->sget("message", "unknown");
        return false;
    }
    const JValue* choices = resp.find("choices");
    if (!choices || !choices->isArr() || choices->aval.empty()) {
        err = "模型返回缺少 choices：" + r.body.substr(0, 200);
        return false;
    }
    string text;
    const JValue& c0 = choices->aval[0];
    const JValue* msg = c0.find("message");
    if (msg) {
        const JValue* ct = msg->find("content");
        if (ct && ct->isStr()) text = ct->sval;
        else if (ct && ct->isArr()) { for (const auto& p : ct->aval) text += p.sget("text"); }
    }
    if (text.empty()) text = c0.sget("text");
    if (text.empty()) { err = "模型返回内容为空（可能是被安全策略拦截或 max_tokens 太小）"; return false; }
    out = JValue::mkObj();
    out.set("text", JValue::mkStr(text));
    out.set("model", JValue::mkStr(model));
    if (resp.find("usage")) {
        out.set("usage", *resp.find("usage"));
        account_tokens(*resp.find("usage"));   // 记进今日 token 用量
    }
    return true;
}

// ───────────────────────── OneBot ─────────────────────────

static bool parse_target(const string& target, string& kind, int64_t& id, string& err) {
    err.clear();
    size_t pos = target.find(':');
    string raw_kind, raw_id;
    if (pos == string::npos) { kind = "group"; raw_id = target; }
    else { raw_kind = target.substr(0, pos); raw_id = target.substr(pos + 1); }
    if (raw_kind.empty()) raw_kind = "group";
    kind = (raw_kind == "private" || raw_kind == "friend") ? "private" : "group";
    raw_id = trim(raw_id);
    if (raw_id.empty() || raw_id.find_first_not_of("0123456789") != string::npos) {
        err = "目标号不合法：" + target + "（应为 group:群号 或 private:QQ号）";
        return false;
    }
    try { id = stoll(raw_id); } catch (...) { err = "目标号不合法：" + target; return false; }
    return true;
}

// ── 表情压缩：ffmpeg 解码 → 等比缩到长边 sticker_long_edge → PNG / GIF（GIF 动图逐帧缩放、保留动画）──
// ffmpeg 位置：环境变量 QQAI_FFMPEG 优先，否则在 PATH 里搜 ffmpeg.exe；找不到时压缩失败、按原图发送。
static string find_ffmpeg() {
    const char* env = getenv("QQAI_FFMPEG");
    if (env && *env) {
        DWORD attr = GetFileAttributesA(env);
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) return env;
        return "";
    }
    char buf[MAX_PATH];
    if (SearchPathA(nullptr, "ffmpeg.exe", nullptr, MAX_PATH, buf, nullptr) > 0) return buf;
    return "";
}

static string make_temp_path(const string& suffix) {
    char dir[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) { dir[0] = '.'; dir[1] = '\0'; }
    static atomic<uint32_t> seq{0};
    char name[80];
    snprintf(name, sizeof(name), "qqai_face_%lu_%u%s",
             (unsigned long)GetCurrentProcessId(),
             (unsigned)(GetTickCount() ^ ((seq++) << 8)), suffix.c_str());
    return string(dir) + name;
}

static bool write_file_bytes(const string& path, const string& data) {
    ofstream f(fs::u8path(path), ios::binary | ios::trunc);
    if (!f.is_open()) return false;
    f.write(data.data(), (streamsize)data.size());
    return f.good();
}

static bool read_file_bytes(const string& path, string& data) {
    ifstream f(fs::u8path(path), ios::binary);
    if (!f.is_open()) return false;
    stringstream ss;
    ss << f.rdbuf();
    data = ss.str();
    return !data.empty();
}

static void remove_file_quiet(const string& path) {
    DeleteFileA(path.c_str());
}

// 运行 ffmpeg（stderr 重定向进管道），返回是否 exit code 0；err 给出失败原因。
static bool run_ffmpeg(const string& exe, const string& args, string& stderr_text, string& err) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { err = "创建管道失败"; return false; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nullptr;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    string cmdline = "\"" + exe + "\" " + args;
    vector<char> cmd(cmdline.begin(), cmdline.end());
    cmd.push_back('\0');
    BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        err = "启动 ffmpeg 失败（GetLastError " + to_string(GetLastError()) + "）";
        return false;
    }
    stderr_text.clear();
    char buf[4096];
    DWORD start_tick = GetTickCount();
    bool timed_out = false;
    auto drain = [&]() {
        DWORD avail = 0;
        while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD readn = 0;
            if (!ReadFile(rd, buf, min<DWORD>((DWORD)sizeof(buf), avail), &readn, nullptr) || readn == 0) break;
            stderr_text.append(buf, readn);
        }
    };
    for (;;) {
        drain();
        if (WaitForSingleObject(pi.hProcess, 100) != WAIT_TIMEOUT) { drain(); break; }
        if (GetTickCount() - start_tick > 20000) {
            timed_out = true;
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 3000);
            drain();
            break;
        }
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    if (timed_out) { err = "ffmpeg 执行超时"; return false; }
    if (code != 0) {
        string t = stderr_text;
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
        size_t pos = t.rfind('\n');
        string line = pos == string::npos ? t : t.substr(pos + 1);
        err = "ffmpeg 失败: " + line;
        return false;
    }
    return true;
}

// 从 ffmpeg 探测输出（stderr）里取最后一个 "宽x高"（输入流行里 hex tag 形如 0x31637661 会因宽为 0 被排除）。
static bool parse_dims(const string& text, int& w, int& h) {
    int best_w = 0, best_h = 0;
    size_t i = 0;
    while (i < text.size()) {
        if (isdigit((unsigned char)text[i])) {
            size_t j = i;
            while (j < text.size() && isdigit((unsigned char)text[j])) j++;
            if (j < text.size() && text[j] == 'x' && j + 1 < text.size() && isdigit((unsigned char)text[j + 1])) {
                size_t k = j + 1;
                while (k < text.size() && isdigit((unsigned char)text[k])) k++;
                int tw = atoi(text.substr(i, j - i).c_str());
                int th = atoi(text.substr(j + 1, k - j - 1).c_str());
                if (tw > 0 && th > 0) { best_w = tw; best_h = th; }
                i = k;
                continue;
            }
        }
        i++;
    }
    if (!best_w || !best_h) return false;
    w = best_w;
    h = best_h;
    return true;
}

static bool compress_image_bytes(const string& in_bytes, int long_edge, string& out_bytes, string& err) {
    err.clear();
    out_bytes.clear();
    if (in_bytes.empty()) { err = "空图片"; return false; }
    if (long_edge <= 0) { err = "未启用压缩"; return false; }
    string ffmpeg = find_ffmpeg();
    if (ffmpeg.empty()) { err = "找不到 ffmpeg，请加入 PATH 或设置环境变量 QQAI_FFMPEG"; return false; }

    bool is_gif = in_bytes.size() > 3 && in_bytes[0] == 'G' && in_bytes[1] == 'I' && in_bytes[2] == 'F';
    string in_path = make_temp_path(is_gif ? ".gif" : ".img");
    string out_path = make_temp_path(is_gif ? ".gif" : ".png");
    if (!write_file_bytes(in_path, in_bytes)) { err = "写临时文件失败"; return false; }

    // 1) 探测原始尺寸（长边没超过目标就不动原图）
    string probe_err, probe_out;
    int w = 0, h = 0;
    bool probed = run_ffmpeg(ffmpeg, "-hide_banner -nostdin -nostats -i \"" + in_path + "\" -f null -",
                             probe_out, probe_err) && parse_dims(probe_out, w, h);
    if (probed) {
        int long_side = max(w, h);
        if (long_side <= long_edge) {
            remove_file_quiet(in_path);
            err = "no-resize";
            return false;
        }
    }

    // 2) 等比缩到长边 = long_edge（GIF 输出 .gif 保留全部帧；其余转 PNG 保透明）
    int nw = long_edge, nh = long_edge;
    if (probed) {
        double k = (double)long_edge / (double)max(w, h);
        nw = (int)max(1.0, w * k);
        nh = (int)max(1.0, h * k);
    }
    string conv_err;
    bool ok = run_ffmpeg(ffmpeg,
                         "-hide_banner -loglevel error -nostdin -y -i \"" + in_path +
                             "\" -vf scale=" + to_string(nw) + ":" + to_string(nh) + " -an \"" + out_path + "\"",
                         probe_out, conv_err);
    if (ok) ok = read_file_bytes(out_path, out_bytes);
    remove_file_quiet(in_path);
    remove_file_quiet(out_path);
    if (!ok) {
        err = conv_err.empty() ? "ffmpeg 输出为空" : conv_err;
        return false;
    }
    return true;
}

// 原始字节：http(s) 下载；本地路径（/static/... 或 file://）直接读文件
static bool fetch_image_bytes(const string& url, string& bytes, string& err) {
    err.clear();
    if (starts_with(url, "http://") || starts_with(url, "https://")) {
        HttpResp r = http_call(url, "GET", "", {}, 20000);
        if (!r.ok || r.body.empty() || r.body.size() > 8 * 1024 * 1024) {
            err = "图片下载失败";
            return false;
        }
        bytes = r.body;
        return true;
    }
    string path = url;
    if (starts_with(path, "file://")) path = path.substr(7);
    if (!path.empty() && path[0] == '/') path = join_path(app_dir, path);
    ifstream f(fs::u8path(path), ios::binary);
    if (!f.is_open()) { err = "本地图片打不开: " + path; return false; }
    stringstream ss;
    ss << f.rdbuf();
    bytes = ss.str();
    if (bytes.empty() || bytes.size() > 8 * 1024 * 1024) { err = "本地图片为空或过大"; return false; }
    return true;
}

// 图片：先自己下载再 base64 内联（SnowLuma 不会替你抓取 URL）；发表情前把长边压到 sticker_long_edge
static string image_file_field(const string& url) {
    int edge = config.sticker_long_edge;
    if (edge == 0) {
        // 关闭压缩：维持原行为
        if (!starts_with(url, "http://") && !starts_with(url, "https://")) return url;
        HttpResp r = http_call(url, "GET", "", {}, 20000);
        if (!r.ok || r.body.empty() || r.body.size() > 8 * 1024 * 1024) return url;
        return "base64://" + base64_encode(r.body);
    }
    string bytes, err;
    if (!fetch_image_bytes(url, bytes, err)) {
        cout << "[warn] " << err << "，按原地址发送" << endl;
        return url;
    }
    string out;
    if (compress_image_bytes(bytes, edge, out, err)) {
        bool out_gif = out.size() > 3 && out[0] == 'G' && out[1] == 'I' && out[2] == 'F';
        cout << "[表情] 已压缩到长边 " << edge << "px（" << (out_gif ? "GIF" : "PNG") << " " << out.size() << " 字节）" << endl;
        return "base64://" + base64_encode(out);
    }
    if (err != "no-resize") cout << "[warn] 表情压缩失败（" << err << "），按原图发送" << endl;
    return "base64://" + base64_encode(bytes);
}

static bool send_to_qq(const Message& msg, string& err, int64_t* onebot_msg_id = nullptr) {
    err.clear();
    if (using_simulate()) return true;  // 本地模拟：只记录，不触达 QQ
    string kind;
    int64_t id = 0;
    if (!parse_target(msg.target, kind, id, err)) return false;

    JValue segs = JValue::mkArr();
    JValue seg = JValue::mkObj();
    JValue data = JValue::mkObj();
    if (msg.type == "image") {
        seg.set("type", JValue::mkStr("image"));
        data.set("file", JValue::mkStr(image_file_field(msg.content)));
    } else {
        // %id 展开出来的 @：单独作为一个 at 段放在文本前，QQ 里会显示成真正的 @人
        if (!msg.at_id.empty() && kind == "group") {
            JValue at_seg = JValue::mkObj();
            JValue at_data = JValue::mkObj();
            at_data.set("qq", JValue::mkStr(msg.at_id));
            at_seg.set("type", JValue::mkStr("at"));
            at_seg.set("data", at_data);
            segs.push(at_seg);
        }
        seg.set("type", JValue::mkStr("text"));
        data.set("text", JValue::mkStr(msg.content));
    }
    seg.set("data", data);
    segs.push(seg);

    JValue body = JValue::mkObj();
    if (kind == "private") body.set("user_id", JValue::mkNum((double)id));
    else body.set("group_id", JValue::mkNum((double)id));
    body.set("message", segs);

    string action = (kind == "private") ? "/send_private_msg" : "/send_group_msg";
    vector<pair<string, string>> headers = {{"Content-Type", "application/json"}};
    if (!config.onebot_token.empty()) headers.push_back({"Authorization", "Bearer " + config.onebot_token});
    HttpResp r = http_call(url_path_join(config.onebot_url, action), "POST", body.dump(), headers, 25000);
    JValue resp = JValue::parse_or_null(r.body);
    if (!r.ok && resp.isNull()) {
        err = "OneBot 请求失败：" + (r.err.empty() ? to_string(r.status) : r.err);
        if (r.status == 426) err += "（426：onebot_url 要填 OneBot HTTP API 端口，不是 WS 端口）";
        return false;
    }
    if (resp.sget("status") != "ok" || resp.iget("retcode") != 0) {
        err = "OneBot " + action + " 失败：" +
              resp.sget("wording", "retcode=" + to_string((long long)resp.iget("retcode", -1)));
        return false;
    }
    if (onebot_msg_id && resp.find("data"))
        *onebot_msg_id = resp.find("data")->iget("message_id");
    return true;
}

static bool check_onebot(string& err) {
    if (config.onebot_url.empty()) { err = "未配置 onebot_url"; g_onebot_up = false; return false; }
    vector<pair<string, string>> headers = {{"Content-Type", "application/json"}};
    if (!config.onebot_token.empty()) headers.push_back({"Authorization", "Bearer " + config.onebot_token});
    HttpResp r = http_call(url_path_join(config.onebot_url, "/get_login_info"), "POST", "{}", headers, 6000);
    JValue resp = JValue::parse_or_null(r.body);
    if (!r.ok || resp.sget("status") != "ok") {
        string e = resp.sget("wording");
        if (e.empty()) e = r.err.empty() ? ("HTTP " + to_string(r.status)) : r.err;
        if (e == "unauthorized" || r.status == 401 || r.status == 403)
            e += "（accessToken 不对或没填：到配置里填 OneBot accessToken）";
        if (r.status == 426) e += "（426：onebot_url 要填 OneBot HTTP API 端口，不是 WS 端口）";
        err = e;
        g_onebot_up = false;
        return false;
    }
    if (resp.find("data")) {
        g_self_nickname = resp.find("data")->sget("nickname");
        g_self_uin = resp.find("data")->iget("user_id");
    }
    g_onebot_up = true;
    err.clear();
    return true;
}

// ───────────────────────── 发送队列 ─────────────────────────

// 一个批次的群聊消息超过 3 条时：先逐条发给自己（私聊自己，拿到真实消息 id），
// 再用 send_group_forward_msg 以「引用消息 id」的节点合并转发到群里。
// 表情包消息也能正确进转发节点（图是真实消息里的图）。任何一条发给自己失败都返回 false，由调用方回退逐条发送。
static bool send_batch_as_forward(const vector<Message>& msgs, string& err) {
    err.clear();
    if (msgs.empty()) return false;
    int64_t self_uin = g_self_uin;
    if (self_uin <= 0) {
        string e;
        if (!check_onebot(e)) { err = "拿不到自己的 QQ 号：" + e; return false; }
        self_uin = g_self_uin;
    }
    if (self_uin <= 0) { err = "拿不到自己的 QQ 号"; return false; }
    if (msgs.front().target.rfind("group:", 0) != 0) { err = "合并转发只支持群聊"; return false; }

    // 1) 逐条发给自己
    vector<string> node_ids;
    for (const auto& src : msgs) {
        Message m = src;
        m.target = "private:" + to_string(self_uin);
        m.target_name = "我（自己）";
        string e;
        int64_t mid = 0;
        if (!send_to_qq(m, e, &mid) || mid == 0) {
            string what = m.type == "image" ? "表情#" + to_string((long long)m.sticker_id) : m.content.substr(0, 40);
            err = "发给自己失败（" + what + "）：" + e;
            return false;
        }
        node_ids.push_back(to_string(mid));
        this_thread::sleep_for(chrono::milliseconds(300));  // 保持顺序，别打太快
    }

    // 2) 合并转发到群里
    JValue nodes = JValue::mkArr();
    for (const auto& id : node_ids) {
        JValue node = JValue::mkObj();
        node.set("type", JValue::mkStr("node"));
        JValue d = JValue::mkObj();
        d.set("id", JValue::mkStr(id));
        node.set("data", d);
        nodes.push(node);
    }
    int64_t gid = 0;
    try { gid = stoll(msgs.front().target.substr(6)); } catch (...) { err = "群号不合法：" + msgs.front().target; return false; }
    JValue params = JValue::mkObj();
    params.set("group_id", JValue::mkNum((double)gid));
    params.set("message", nodes);
    vector<pair<string, string>> headers = {{"Content-Type", "application/json"}};
    if (!config.onebot_token.empty()) headers.push_back({"Authorization", "Bearer " + config.onebot_token});
    HttpResp r = http_call(url_path_join(config.onebot_url, "/send_group_forward_msg"), "POST", params.dump(), headers, 30000);
    JValue resp = JValue::parse_or_null(r.body);
    if (!r.ok || resp.sget("status") != "ok") {
        string e = resp.sget("wording");
        if (e.empty()) e = r.err.empty() ? ("HTTP " + to_string(r.status)) : r.err;
        err = "合并转发失败: " + e;
        return false;
    }
    cout << "[转发] " << msgs.front().target << " 批次 " << msgs.front().batch
         << " 共 " << msgs.size() << " 条，已发给自己并合并转发到群里" << endl;
    return true;
}

static void process_pending_messages() {
    vector<Message> due;
    {
        lock_guard<recursive_mutex> lock(data_mutex);
        int64_t n = now_ms();
        for (const auto& m : message_list) if (m.status == "queued" && m.epoch <= n) due.push_back(m);
        stable_sort(due.begin(), due.end(), [](const Message& a, const Message& b) {
            return a.epoch < b.epoch;
        });
    }

    // 超过 3 条的群聊批次（且该批次没有未来的定时条目）→ 发给自己再合并转发，不走逐条
    vector<char> handled(due.size(), 0);
    if (!using_simulate()) {
        map<string, size_t> total_queued, due_count;
        {
            lock_guard<recursive_mutex> lock(data_mutex);
            for (const auto& m : message_list)
                if (m.status == "queued" && !m.batch.empty())
                    total_queued[m.batch + "|" + m.target]++;
        }
        for (const auto& m : due)
            if (!m.batch.empty()) due_count[m.batch + "|" + m.target]++;
        for (const auto& kv : due_count) {
            if (kv.second <= 3 || total_queued[kv.first] != kv.second) continue;  // 不超过 3 条 / 还有定时条目没到期
            vector<Message> group;
            vector<size_t> idxs;
            for (size_t i = 0; i < due.size(); i++) {
                if (!due[i].batch.empty() && due[i].batch + "|" + due[i].target == kv.first) {
                    group.push_back(due[i]);
                    idxs.push_back(i);
                }
            }
            if (group.empty() || group.front().target.rfind("group:", 0) != 0) continue;
            string err;
            if (!send_batch_as_forward(group, err)) {
                cout << "[warn] 批次 " << group.front().batch << " 合并转发未成功（" << err << "），回退逐条发送" << endl;
                continue;
            }
            for (size_t i : idxs) handled[i] = 1;
            lock_guard<recursive_mutex> lock(data_mutex);
            for (size_t i : idxs) {
                for (auto& stored : message_list) {
                    if (stored.id == due[i].id) {
                        stored.status = "sent";
                        stored.epoch = now_ms();
                        break;
                    }
                }
            }
            save_messages();
        }
    }

    for (size_t k = 0; k < due.size(); k++) {
        if (handled[k]) continue;
        auto& m = due[k];
        int64_t wait = g_next_send_at - now_ms();
        if (wait > 0) this_thread::sleep_for(chrono::milliseconds(min<int64_t>(wait, 600000)));
        string err;
        int64_t ob_msg_id = 0;
        bool ok = send_to_qq(m, err, &ob_msg_id);
        g_next_send_at = now_ms() + max<int64_t>(0, config.send_interval_ms);
        lock_guard<recursive_mutex> lock(data_mutex);
        for (auto& stored : message_list) {
            if (stored.id == m.id) {
                stored.status = ok ? "sent" : "failed";
                stored.error = err;
                if (ok) stored.epoch = now_ms();
                if (ok && ob_msg_id) stored.qq_msg_id = to_string(ob_msg_id);  // 留着以后撤回用
                break;
            }
        }
        save_messages();
        cout << (ok ? "[已发] " : "[失败] ") << m.target << " " << m.type << " "
             << (m.type == "image" ? ("表情#" + to_string((long long)m.sticker_id)) : m.content);
        if (!ok) cout << "  -> " << err;
        cout << endl;
    }
}

// 询问“还发不发”：send=true 入队，false 取消。定时条目若已过期直接标失败并回报。
static size_t confirm_batch(const string& batch_id, bool send, vector<string>& expired_errs) {
    size_t n = 0;
    lock_guard<recursive_mutex> lock(data_mutex);
    for (auto& m : message_list) {
        if (m.batch != batch_id || m.status != "pending") continue;
        if (!send) { m.status = "cancelled"; n++; continue; }
        if (!m.time_raw.empty() && m.epoch <= now_ms()) {
            string e = "定时时间 " + m.time_raw + "（" + fmt_time(m.epoch) + "）已经早于当前时间，该条未发送";
            m.status = "failed";
            m.error = e;
            expired_errs.push_back(e);
            n++;
            continue;
        }
        if (m.epoch < now_ms()) m.epoch = now_ms();
        m.status = "queued";
        n++;
    }
    save_messages();
    return n;
}

// ───────────────────────── 自动收信回复 ─────────────────────────

static string current_target_name(const string& target);  // 定义在 HTTP 服务一节

struct QQMsg {
    int64_t msg_id = 0;
    int64_t time_sec = 0;
    string sender_id;
    string sender_name;
    string text;
    bool at_me = false;
};

static void save_poll_state() {
    JValue j = JValue::mkObj();
    for (const auto& kv : g_poll_state) {
        JValue s = JValue::mkObj();
        s.set("lastTime", JValue::mkNum((double)kv.second.last_time));
        s.set("lastMsgId", JValue::mkStr(kv.second.last_msg_id));
        s.set("lastCheck", JValue::mkNum((double)kv.second.last_check));
        s.set("lastReplyAt", JValue::mkNum((double)kv.second.last_reply_at));
        j.set(kv.first, s);
    }
    write_text_file(poll_state_file, j.dump());
}

static void load_poll_state() {
    lock_guard<recursive_mutex> lock(poll_mutex);
    string text;
    if (!read_text_file(poll_state_file, text)) return;
    JValue j = JValue::parse_or_null(text);
    if (!j.isObj()) return;
    for (const auto& kv : j.oval) {
        PollState s;
        s.last_time = kv.second.iget("lastTime");
        s.last_msg_id = kv.second.sget("lastMsgId");
        s.last_check = kv.second.iget("lastCheck");
        s.last_reply_at = kv.second.iget("lastReplyAt");
        g_poll_state[kv.first] = s;
    }
}

// ── 黑名单 / 白名单（按 QQ 号）──

static bool user_in_list(const vector<string>& list, const string& id) {
    return !id.empty() && find(list.begin(), list.end(), id) != list.end();
}

static void load_user_lists() {
    lock_guard<recursive_mutex> lock(data_mutex);
    string text;
    if (!read_text_file(user_lists_file, text)) return;
    JValue j = JValue::parse_or_null(text);
    if (!j.isObj()) return;
    user_black.clear();
    user_white.clear();
    const JValue* b = j.find("blacklist");
    if (b && b->isArr()) for (const auto& v : b->aval) if (v.isStr() && !v.sval.empty()) user_black.push_back(v.sval);
    const JValue* w = j.find("whitelist");
    if (w && w->isArr()) for (const auto& v : w->aval) if (v.isStr() && !v.sval.empty()) user_white.push_back(v.sval);
}

static void save_user_lists() {
    lock_guard<recursive_mutex> lock(data_mutex);
    JValue j = JValue::mkObj();
    JValue b = JValue::mkArr(), w = JValue::mkArr();
    for (const auto& s : user_black) b.push(JValue::mkStr(s));
    for (const auto& s : user_white) w.push(JValue::mkStr(s));
    j.set("blacklist", b);
    j.set("whitelist", w);
    write_text_file(user_lists_file, j.dump());
}

// 把 OneBot 消息段拼成纯文本（@机器人标记成 @我，图片/语音等用占位）
static string seg_text_from_message(const JValue& m, bool& at_me) {
    string out;
    const JValue* msg = m.find("message");
    if (msg && msg->isArr()) {
        for (const auto& seg : msg->aval) {
            string t = to_lower(seg.sget("type"));
            const JValue* d = seg.find("data");
            if (t == "text") {
                out += d ? d->sget("text") : "";
            } else if (t == "at") {
                string qq = d ? d->sget("qq") : "";
                if (g_self_uin && !qq.empty() && qq == to_string(g_self_uin)) { out += "@我"; at_me = true; }
                else if (!qq.empty()) out += "@" + qq;
            } else if (t == "image") out += "[图片]";
            else if (t == "face" || t == "mface") out += "[表情]";
            else if (t == "record") out += "[语音]";
            else if (t == "video") out += "[视频]";
            else if (t == "reply") out += "[引用]";
            else if (t == "json" || t == "xml") out += "[卡片]";
            else if (t == "forward") out += "[合并转发]";
            else if (!t.empty()) out += "[" + t + "]";
        }
    } else {
        out = m.sget("raw_message");
        if (out.empty() && msg && msg->isStr()) out = msg->sval;
        if (out.find("[CQ:at,qq=" + to_string(g_self_uin)) != string::npos) at_me = true;
    }
    return trim(out);
}

// 拉取群/私聊历史；兼容 data 为数组或 data.messages 两种形状
static bool fetch_history(const string& kind, int64_t id, vector<QQMsg>& out, string& err) {
    err.clear();
    bool is_group = (kind == "group");
    string action = is_group ? "/get_group_msg_history" : "/get_friend_msg_history";
    JValue body = JValue::mkObj();
    body.set(is_group ? "group_id" : "user_id", JValue::mkNum((double)id));
    body.set("count", JValue::mkNum(max(20, config.context_window * 2)));
    vector<pair<string, string>> headers = {{"Content-Type", "application/json"}};
    if (!config.onebot_token.empty()) headers.push_back({"Authorization", "Bearer " + config.onebot_token});
    HttpResp r = http_call(url_path_join(config.onebot_url, action), "POST", body.dump(), headers, 12000);
    JValue resp = JValue::parse_or_null(r.body);
    if (!r.ok && resp.isNull()) {
        err = action + " 请求失败：" + (r.err.empty() ? to_string(r.status) : r.err);
        return false;
    }
    if (resp.sget("status") != "ok") {
        err = action + " 失败：" + resp.sget("wording", resp.sget("status", "非 JSON"));
        return false;
    }
    const JValue* data = resp.find("data");
    const JValue* arr = nullptr;
    if (data) {
        arr = data->find("messages");
        if (!arr && data->isArr()) arr = data;
    }
    if (!arr || !arr->isArr()) {
        err = action + " 返回里没有消息数组（SnowLuma 可能不支持该接口）";
        return false;
    }
    for (const auto& m : arr->aval) {
        QQMsg q;
        int64_t mid = m.iget("message_id", m.iget("message_seq"));
        q.msg_id = mid;
        q.time_sec = m.iget("time");
        const JValue* s = m.find("sender");
        if (s && s->isObj()) {
            q.sender_name = s->sget("card");
            if (q.sender_name.empty()) q.sender_name = s->sget("nickname");
            q.sender_id = s->sget("user_id");
        }
        if (q.sender_name.empty()) q.sender_name = m.sget("user_id");
        if (q.sender_id.empty()) q.sender_id = q.sender_name;
        q.text = seg_text_from_message(m, q.at_me);
        if (q.msg_id == 0 && q.text.empty()) continue;
        out.push_back(q);
    }
    stable_sort(out.begin(), out.end(), [](const QQMsg& a, const QQMsg& b) {
        if (a.time_sec != b.time_sec) return a.time_sec < b.time_sec;
        return a.msg_id < b.msg_id;
    });
    return true;
}

// 新消息落进消息流（按 qq_msg_id 去重），返回真正的新消息
static vector<QQMsg> append_incoming(const string& target, const vector<QQMsg>& msgs) {
    vector<QQMsg> fresh;
    string tname;
    {
        lock_guard<recursive_mutex> lock(data_mutex);
        tname = current_target_name(target);
        int64_t now = now_ms();
        for (const auto& q : msgs) {
            if (q.text.empty() || q.msg_id == 0) continue;
            string mid = to_string(q.msg_id);
            bool dup = false;
            for (const auto& m : message_list) {
                if (m.target == target && m.qq_msg_id == mid) { dup = true; break; }
            }
            if (dup) continue;
            Message m;
            m.id = make_id("msg");
            m.target = target;
            m.target_name = tname;
            m.dir = "in";
            m.sender = q.sender_name;
            m.sender_id = q.sender_id;
            m.type = "send";
            m.content = q.text;
            m.epoch = q.time_sec ? q.time_sec * 1000 : now;
            m.status = "sent";
            m.created_at = now;
            m.qq_msg_id = mid;
            message_list.push_back(m);
            fresh.push_back(q);
        }
        if (!fresh.empty()) save_messages();
    }
    return fresh;
}

// 触发判定 + 调模型 + （可选）直接确认发送
struct AutoResult {
    bool considered = false;   // 是否走到了“调模型”这一步
    bool spoke = false;        // 模型是否真的要说话
    string info;               // 给前端/日志看的说明
    JValue reply;              // chat 结果（considered 时有值）
};

static AutoResult auto_consider(const string& target, const vector<QQMsg>& ctx, const QQMsg& newest) {
    AutoResult res;
    bool is_private = starts_with(target, "private:");
    string err;

    // 黑名单 / 白名单（黑名单优先）：名单里的人不发消息、不触发回复
    {
        lock_guard<recursive_mutex> lock(data_mutex);
        if (user_in_list(user_black, newest.sender_id)) {
            res.info = "黑名单用户 " + newest.sender_id + "（" + newest.sender_name + "），不回复";
            return res;
        }
        if (!user_white.empty() && !user_in_list(user_white, newest.sender_id)) {
            res.info = "白名单已启用且 " + newest.sender_id + "（" + newest.sender_name + "）不在其中，不回复";
            return res;
        }
    }

    // 冷却：两次触发模型之间至少隔 reply_cooldown_ms
    int64_t last_reply = 0;
    {
        lock_guard<recursive_mutex> lock(poll_mutex);
        last_reply = g_poll_state[target].last_reply_at;
    }
    int64_t now = now_ms();
    if (now - last_reply < config.reply_cooldown_ms) {
        res.info = "冷却中，还剩 " + to_string((long long)(config.reply_cooldown_ms - (now - last_reply)) / 1000) + " 秒";
        return res;
    }

    // 必回条件：@我 / 命中关键词 / 私聊
    bool must = false;
    if (is_private && config.private_always_reply) must = true;
    if (newest.at_me) must = true;
    for (const auto& kw : config.must_reply_keywords) {
        if (!kw.empty() && newest.text.find(kw) != string::npos) { must = true; break; }
    }
    if (!must && (rand() % 100) >= config.reply_probability) {
        res.info = "新消息但不值得接话（概率 " + to_string(config.reply_probability) + "% 没中）";
        return res;
    }

    // 上下文：最近 N 条（旧→新）
    string context;
    size_t from = ctx.size() > (size_t)config.context_window ? ctx.size() - config.context_window : 0;
    for (size_t k = from; k < ctx.size(); k++) {
        context += ctx[k].sender_name + ": " + ctx[k].text + "\n";
    }

    string prompt = process_message_template(config.prompt_template, newest.text,
                                             newest.sender_name, newest.sender_id, target);
    if (!context.empty())
        prompt += "\n\n【这个群最近的聊天记录（旧→新），最后一条是刚发生的】\n" + context;
    prompt += "\n【注意】你是被动旁观的群友：先判断这条/这段对话值不值得你接话，"
              "不值得就只输出 [SILENT]，不要为了回而回。";

    res.considered = true;
    {
        lock_guard<recursive_mutex> lock(poll_mutex);
        g_poll_state[target].last_reply_at = now;  // 无论说不说，都进入冷却
    }
    JValue out;
    cout << "[思考] " << target << "（" << newest.sender_name << " 的消息）正在思考…" << endl;
    if (!call_ai(prompt, "", out, err)) {
        res.info = "模型调用失败：" + err;
        return res;
    }
    string reply_text = out.sget("text");
    {
        string plain = strip_directives(reply_text);
        cout << "[AI 完整回复] " << target << (plain.empty() ? "：（只有 send/image 指令）" : ("：" + plain)) << endl;
    }
    vector<string> errors;
    vector<ParsedItem> items = parse_message_content(reply_text, errors);
    expand_output_placeholders(items, target, newest.sender_id);  // %token / %gname / %id
    string tname;
    {
        lock_guard<recursive_mutex> lock(data_mutex);
        tname = current_target_name(target);
    }
    if (items.empty()) {
        res.info = "模型选择不说话" + (reply_text.empty() ? "" : ("（" + reply_text.substr(0, 60) + "）"));
        JValue j = JValue::mkObj();
        j.set("reply", JValue::mkStr(reply_text));
        j.set("silent", JValue::mkBool(true));
        JValue errs = JValue::mkArr();
        for (const auto& e : errors) errs.push(JValue::mkStr(e));
        j.set("errors", errs);
        res.reply = j;
        return res;
    }
    string batch = enqueue_pending(items, target, tname, config.persona.empty() ? "AI" : config.persona);
    if (config.auto_confirm) {
        vector<string> expired;
        confirm_batch(batch, true, expired);
        for (const auto& e : expired) cout << "[自动] " << e << endl;
    }
    res.spoke = true;
    res.info = "回复 " + to_string((long long)items.size()) + " 条" +
               (config.auto_confirm ? "（已直接入队发送）" : "（等待确认）");
    JValue j = JValue::mkObj();
    j.set("reply", JValue::mkStr(reply_text));
    j.set("batchId", JValue::mkStr(batch));
    j.set("ask", JValue::mkBool(!config.auto_confirm));
    j.set("silent", JValue::mkBool(false));
    JValue errs = JValue::mkArr();
    for (const auto& e : errors) errs.push(JValue::mkStr(e));
    j.set("errors", errs);
    res.reply = j;
    return res;
}

// 对一个会话跑一轮：拉历史 → 落消息流 → 判定 → 可能回复
static void auto_check_target(const string& target) {
    string kind;
    int64_t id = 0;
    string e;
    if (!parse_target(target, kind, id, e)) {
        g_last_auto_info = target + "：" + e;
        return;
    }
    vector<QQMsg> msgs;
    if (!fetch_history(kind, id, msgs, e)) {
        g_last_auto_info = target + " 拉取失败：" + e;
        cout << "[自动] " << g_last_auto_info << endl;
        return;
    }
    vector<QQMsg> fresh = append_incoming(target, msgs);
    {
        lock_guard<recursive_mutex> lock(poll_mutex);
        PollState& st = g_poll_state[target];
        st.last_check = now_ms();
        if (!msgs.empty()) {
            st.last_time = msgs.back().time_sec;
            st.last_msg_id = to_string(msgs.back().msg_id);
        }
        save_poll_state();
    }
    if (fresh.empty()) { g_last_auto_info = target + " 没有新消息"; return; }
    cout << "[自动] " << target << " 有 " << fresh.size() << " 条新消息，最新："
         << fresh.back().sender_name << ": " << fresh.back().text.substr(0, 50) << endl;
    AutoResult r = auto_consider(target, msgs, fresh.back());
    g_last_auto_info = target + " " + r.info;
    cout << "[自动] " << g_last_auto_info << endl;
}

static void auto_check_tick() {
    if (!config.auto_reply || g_auto_busy.exchange(true)) return;
    struct Guard {
        atomic<bool>& f;
        ~Guard() { f = false; }
    } guard{g_auto_busy};

    if (!g_onebot_up) { g_last_auto_info = "OneBot 不在线，收不了消息（send_mode=" + config.send_mode + "）"; return; }
    vector<string> targets;
    {
        lock_guard<recursive_mutex> lock(data_mutex);
        for (const auto& g : groups) if (g.enabled) targets.push_back("group:" + g.id);
        for (const auto& p : private_chats) if (p.enabled) targets.push_back("private:" + p.id);
    }
    for (const auto& t : targets) {
        if (!running) break;
        auto_check_target(t);
    }
}

// 供前端/测试用：不经 OneBot，直接把一条“群友说的话”喂进同一条判定管道
static void main_loop() {
    int64_t last_probe = 0;
    int64_t last_auto_check = 0;
    bool probe_logged = false;
    srand((unsigned)time(nullptr));
    while (running) {
        try {
            // OneBot 探活（只读，无论发送模式如何，自动收信都依赖它）
            if (config.send_mode != "simulate" && now_ms() - last_probe > 15000) {
                last_probe = now_ms();
                string e;
                bool up = check_onebot(e);
                if (up && !probe_logged) {
                    cout << "[QQ] OneBot 在线：" << (g_self_nickname.empty() ? "(未取到昵称)" : g_self_nickname) << endl;
                    probe_logged = true;
                } else if (!up && probe_logged) {
                    cout << "[QQ] OneBot 掉线：" << e << endl;
                    probe_logged = false;
                } else if (!up && config.send_mode == "real" && !probe_logged) {
                    cout << "[QQ] OneBot 不可用：" << e << "（send_mode=real，发送会失败）" << endl;
                    probe_logged = true;  // 不刷屏
                }
            }
            // 自动收信回复
            if (config.auto_reply && now_ms() - last_auto_check >= config.auto_check_ms) {
                last_auto_check = now_ms();
                auto_check_tick();
            }
            process_pending_messages();
        } catch (const exception& e) {
            cerr << "主循环错误: " << e.what() << endl;
        }
        for (int i = 0; i < 4 && running; i++) this_thread::sleep_for(chrono::milliseconds(50));
    }
}

// ───────────────────────── HTTP 服务 ─────────────────────────

struct Request {
    string method, path, target, query, body;
    map<string, string> headers;
    map<string, string> params;
    JValue json;
};

static string url_decode(const string& s) {
    string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') out += ' ';
        else if (s[i] == '%' && i + 2 < s.size()) {
            auto hv = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return 0;
            };
            out += (char)(hv(s[i + 1]) * 16 + hv(s[i + 2]));
            i += 2;
        } else out += s[i];
    }
    return out;
}

static map<string, string> parse_query(const string& q) {
    map<string, string> out;
    size_t i = 0;
    while (i < q.size()) {
        size_t amp = q.find('&', i);
        string kv = q.substr(i, (amp == string::npos ? q.size() : amp) - i);
        size_t eq = kv.find('=');
        if (eq != string::npos) out[url_decode(kv.substr(0, eq))] = url_decode(kv.substr(eq + 1));
        else if (!kv.empty()) out[url_decode(kv)] = "";
        if (amp == string::npos) break;
        i = amp + 1;
    }
    return out;
}

static string param_str(const Request& req, const string& key, const string& def = "") {
    const JValue* v = req.json.find(key);
    if (v) {
        if (v->isStr()) return v->sval;
        if (v->type == JValue::TNum) { char b[32]; snprintf(b, sizeof(b), "%lld", (long long)v->nval); return b; }
        if (v->type == JValue::TBool) return v->bval ? "true" : "false";
    }
    auto it = req.params.find(key);
    if (it != req.params.end()) return it->second;
    return def;
}

struct Response {
    int status = 200;
    string body;
    string content_type = "application/json; charset=utf-8";
};

static Response json_resp(const JValue& v, int status = 200) {
    Response r;
    r.status = status;
    r.body = v.dump();
    return r;
}

static Response ok_resp(const JValue& extra = JValue()) {
    JValue j = JValue::mkObj();
    j.set("ok", JValue::mkBool(true));
    if (extra.isObj()) for (const auto& p : extra.oval) j.set(p.first, p.second);
    return json_resp(j);
}

static Response err_resp(const string& msg, int status = 400) {
    JValue j = JValue::mkObj();
    j.set("ok", JValue::mkBool(false));
    j.set("error", JValue::mkStr(msg));
    return json_resp(j, status);
}

static string guess_mime(const string& path) {
    string p = to_lower(path);
    if (p.size() >= 5 && p.substr(p.size() - 5) == ".html") return "text/html; charset=utf-8";
    if (p.size() >= 4 && p.substr(p.size() - 4) == ".css") return "text/css; charset=utf-8";
    if (p.size() >= 3 && p.substr(p.size() - 3) == ".js") return "application/javascript; charset=utf-8";
    if (p.size() >= 5 && p.substr(p.size() - 5) == ".json") return "application/json; charset=utf-8";
    if (p.size() >= 4 && p.substr(p.size() - 4) == ".png") return "image/png";
    if (p.size() >= 5 && p.substr(p.size() - 5) == ".jpg") return "image/jpeg";
    if (p.size() >= 4 && p.substr(p.size() - 4) == ".gif") return "image/gif";
    if (p.size() >= 5 && p.substr(p.size() - 5) == ".webp") return "image/webp";
    if (p.size() >= 4 && p.substr(p.size() - 4) == ".svg") return "image/svg+xml";
    if (p.size() >= 4 && p.substr(p.size() - 4) == ".ico") return "image/x-icon";
    return "application/octet-stream";
}

static string current_target_name(const string& target) {
    string kind;
    int64_t id = 0;
    string e;
    if (!parse_target(target, kind, id, e)) return target;
    string sid = to_string(id);
    lock_guard<recursive_mutex> lock(data_mutex);
    if (kind == "group") {
        for (const auto& g : groups) if (g.id == sid) return g.name;
    } else {
        for (const auto& p : private_chats) if (p.id == sid) return p.name;
    }
    return (kind == "group" ? "群 " : "私聊 ") + sid;
}

static JValue status_json() {
    JValue j = JValue::mkObj();
    j.set("app", JValue::mkStr(APP_NAME));
    j.set("version", JValue::mkStr(APP_VERSION));
    j.set("now", JValue::mkStr(now_string()));
    j.set("model", JValue::mkStr(config.model));
    j.set("hasKey", JValue::mkBool(!config.key().empty()));
    j.set("sendMode", JValue::mkStr(config.send_mode));
    j.set("simulating", JValue::mkBool(using_simulate()));
    j.set("onebotUrl", JValue::mkStr(config.onebot_url));
    j.set("onebotUp", JValue::mkBool(g_onebot_up.load()));
    j.set("onebotNick", JValue::mkStr(g_self_nickname));
    j.set("stickerCount", JValue::mkNum((int)stickers.size()));
    j.set("groupCount", JValue::mkNum((int)groups.size()));
    j.set("privateCount", JValue::mkNum((int)private_chats.size()));
    size_t pending = 0, queued = 0;
    for (const auto& m : message_list) {
        if (m.status == "pending") pending++;
        else if (m.status == "queued") queued++;
    }
    j.set("pending", JValue::mkNum((double)pending));
    j.set("queued", JValue::mkNum((double)queued));
    j.set("sendIntervalMs", JValue::mkNum((double)config.send_interval_ms));
    j.set("autoReply", JValue::mkBool(config.auto_reply));
    j.set("autoCheckMs", JValue::mkNum(config.auto_check_ms));
    j.set("replyProbability", JValue::mkNum(config.reply_probability));
    j.set("autoConfirm", JValue::mkBool(config.auto_confirm));
    j.set("lastAutoInfo", JValue::mkStr(g_last_auto_info));
    j.set("tokenDate", JValue::mkStr(g_token_date.empty() ? today_date_str() : g_token_date));
    j.set("tokensToday", JValue::mkNum((double)g_tokens_today));
    j.set("promptTokensToday", JValue::mkNum((double)g_prompt_today));
    j.set("completionTokensToday", JValue::mkNum((double)g_completion_today));
    return j;
}

static Response auto_inject(const Request& req) {
    string target = trim(param_str(req, "target"));
    string text = param_str(req, "text");
    string name = trim(param_str(req, "name"));
    string sid = trim(param_str(req, "id"));
    bool force = req.json.bget("force", false);  // true=无视概率直接问模型
    if (target.empty()) return err_resp("需要 target（group:群号 或 private:QQ号）");
    if (text.empty()) return err_resp("需要 text");
    if (name.empty()) name = "群友";
    QQMsg q;
    q.msg_id = (int64_t)(now_ms() % 1000000000);
    q.time_sec = now_ms() / 1000;
    q.sender_name = name;
    q.sender_id = sid.empty() ? "10000" : sid;
    q.text = text;
    q.at_me = text.find("@我") != string::npos;
    vector<QQMsg> fresh = append_incoming(target, {q});
    if (fresh.empty()) return ok_resp();
    AutoResult r;
    if (force) {
        // 强制模式：跳过概率，但仍尊重冷却与必回以外的流程
        lock_guard<recursive_mutex> lock(poll_mutex);
        g_poll_state[target].last_reply_at = 0;
        r = auto_consider(target, {q}, q);
    } else {
        r = auto_consider(target, {q}, q);
    }
    JValue j = JValue::mkObj();
    j.set("considered", JValue::mkBool(r.considered));
    j.set("spoke", JValue::mkBool(r.spoke));
    j.set("info", JValue::mkStr(r.info));
    if (r.reply.isObj()) for (const auto& p : r.reply.oval) j.set(p.first, p.second);
    return ok_resp(j);
}

static Response handle_api(const Request& req) {
    string path = req.path;

    // ── 状态 / 配置 ──
    if (path == "/api/status") return ok_resp(status_json());
    if (path == "/api/config" && req.method == "GET") {
        JValue j = JValue::mkObj();
        j.set("config", config_to_json());
        return ok_resp(j);
    }
    if (path == "/api/config" && req.method == "POST") {
        apply_config_json(req.json);
        if (!save_config()) return err_resp("保存 config.json 失败");
        string e;
        if (config.send_mode != "simulate") check_onebot(e);
        JValue j = JValue::mkObj();
        j.set("config", config_to_json());
        j.set("status", status_json());
        j.set("restarting", JValue::mkBool(true));
        thread(hot_restart_worker).detach();  // 响应先发完，随后自动热重启
        return ok_resp(j);
    }

    // ── 群聊 / 私聊管理 ──
    if (path == "/api/groups" && req.method == "GET") {
        JValue arr = JValue::mkArr();
        lock_guard<recursive_mutex> lock(data_mutex);
        for (const auto& g : groups) arr.push(group_to_json(g));
        JValue j = JValue::mkObj();
        j.set("groups", arr);
        return ok_resp(j);
    }
    if (path == "/api/groups" && req.method == "POST") {
        string id = trim(param_str(req, "id"));
        if (id.empty()) return err_resp("群号不能为空");
        if (id.find_first_not_of("0123456789") != string::npos) return err_resp("群号只能是数字：" + id);
        string name = trim(param_str(req, "name"));
        Group* found;
        {
            lock_guard<recursive_mutex> lock(data_mutex);
            found = nullptr;
            for (auto& g : groups) if (g.id == id) { found = &g; break; }
            if (!found) { Group g; g.id = id; g.name = name.empty() ? ("群 " + id) : name; groups.push_back(g); found = &groups.back(); }
            if (!name.empty()) found->name = name;
            if (req.json.has("enabled")) found->enabled = req.json.bget("enabled", found->enabled);
            const JValue* mem = req.json.find("selectedMembers");
            if (mem && mem->isArr()) { found->selected_members.clear(); for (const auto& v : mem->aval) if (v.isStr()) found->selected_members.push_back(v.sval); }
            save_groups();
        }
        return ok_resp();
    }
    if (path == "/api/groups/delete" && req.method == "POST") {
        string id = trim(param_str(req, "id"));
        lock_guard<recursive_mutex> lock(data_mutex);
        groups.erase(remove_if(groups.begin(), groups.end(),
                               [&](const Group& g) { return g.id == id; }), groups.end());
        save_groups();
        return ok_resp();
    }
    // 从 OneBot 拉真实群列表（get_group_list）
    if (path == "/api/groups/sync" && req.method == "POST") {
        vector<pair<string, string>> headers = {{"Content-Type", "application/json"}};
        if (!config.onebot_token.empty()) headers.push_back({"Authorization", "Bearer " + config.onebot_token});
        HttpResp r = http_call(url_path_join(config.onebot_url, "/get_group_list"), "POST", "{}", headers, 20000);
        JValue resp = JValue::parse_or_null(r.body);
        if (!r.ok || resp.sget("status") != "ok") {
            string e = r.err.empty() ? ("HTTP " + to_string(r.status)) : r.err;
            return err_resp("get_group_list 失败：" + (resp.sget("wording", e)));
        }
        const JValue* data = resp.find("data");
        if (!data || !data->isArr()) return err_resp("OneBot 没返回群数组");
        int added = 0, total = 0;
        lock_guard<recursive_mutex> lock(data_mutex);
        for (const auto& g : data->aval) {
            string gid = g.sget("group_id");
            if (gid.empty() || gid.find_first_not_of("0123456789") != string::npos)
                gid = to_string((long long)g.iget("group_id"));
            if (gid.empty() || gid == "0") continue;
            string gname = g.sget("group_name");
            int mc = (int)g.iget("member_count");
            Group* found = nullptr;
            for (auto& x : groups) if (x.id == gid) { found = &x; break; }
            if (!found) {
                Group x;
                x.id = gid;
                x.name = gname.empty() ? ("群 " + gid) : gname;
                x.members = mc;
                groups.push_back(x);
                added++;
            } else {
                if (!gname.empty()) found->name = gname;
                if (mc) found->members = mc;
            }
        }
        total = (int)groups.size();
        save_groups();
        JValue j = JValue::mkObj();
        j.set("added", JValue::mkNum(added));
        j.set("total", JValue::mkNum(total));
        cout << "[群] 同步 OneBot 群列表：新增 " << added << " 个，共 " << total << " 个" << endl;
        return ok_resp(j);
    }
    if (path == "/api/private" && req.method == "GET") {
        JValue arr = JValue::mkArr();
        lock_guard<recursive_mutex> lock(data_mutex);
        for (const auto& p : private_chats) arr.push(private_to_json(p));
        JValue j = JValue::mkObj();
        j.set("private", arr);
        return ok_resp(j);
    }
    if (path == "/api/private" && req.method == "POST") {
        string id = trim(param_str(req, "id"));
        if (id.empty()) return err_resp("QQ 号不能为空");
        if (id.find_first_not_of("0123456789") != string::npos) return err_resp("QQ 号只能是数字：" + id);
        string name = trim(param_str(req, "name"));
        {
            lock_guard<recursive_mutex> lock(data_mutex);
            PrivateChat* found = nullptr;
            for (auto& p : private_chats) if (p.id == id) { found = &p; break; }
            if (!found) { PrivateChat p; p.id = id; p.name = name.empty() ? id : name; private_chats.push_back(p); found = &private_chats.back(); }
            if (!name.empty()) found->name = name;
            if (req.json.has("enabled")) found->enabled = req.json.bget("enabled", found->enabled);
            save_privates();
        }
        return ok_resp();
    }
    if (path == "/api/private/delete" && req.method == "POST") {
        string id = trim(param_str(req, "id"));
        lock_guard<recursive_mutex> lock(data_mutex);
        private_chats.erase(remove_if(private_chats.begin(), private_chats.end(),
                                      [&](const PrivateChat& p) { return p.id == id; }), private_chats.end());
        save_privates();
        return ok_resp();
    }
    // 从 OneBot 拉好友列表（get_friend_list）
    if (path == "/api/private/sync" && req.method == "POST") {
        vector<pair<string, string>> headers = {{"Content-Type", "application/json"}};
        if (!config.onebot_token.empty()) headers.push_back({"Authorization", "Bearer " + config.onebot_token});
        HttpResp r = http_call(url_path_join(config.onebot_url, "/get_friend_list"), "POST", "{}", headers, 20000);
        JValue resp = JValue::parse_or_null(r.body);
        if (!r.ok || resp.sget("status") != "ok") {
            string e = r.err.empty() ? ("HTTP " + to_string(r.status)) : r.err;
            return err_resp("get_friend_list 失败：" + (resp.sget("wording", e)));
        }
        const JValue* data = resp.find("data");
        if (!data || !data->isArr()) return err_resp("OneBot 没返回好友数组");
        int added = 0, total = 0;
        lock_guard<recursive_mutex> lock(data_mutex);
        for (const auto& f : data->aval) {
            string uid = f.sget("user_id");
            if (uid.empty() || uid.find_first_not_of("0123456789") != string::npos)
                uid = to_string((long long)f.iget("user_id"));
            if (uid.empty() || uid == "0") continue;
            string nick = f.sget("nickname");
            if (nick.empty()) nick = f.sget("card");
            PrivateChat* found = nullptr;
            for (auto& x : private_chats) if (x.id == uid) { found = &x; break; }
            if (!found) {
                PrivateChat x;
                x.id = uid;
                x.name = nick.empty() ? uid : nick;
                private_chats.push_back(x);
                added++;
            } else if (!nick.empty()) {
                found->name = nick;
            }
        }
        total = (int)private_chats.size();
        save_privates();
        JValue j = JValue::mkObj();
        j.set("added", JValue::mkNum(added));
        j.set("total", JValue::mkNum(total));
        cout << "[私聊] 同步 OneBot 好友列表：新增 " << added << " 个，共 " << total << " 个" << endl;
        return ok_resp(j);
    }

    // ── 表情包 ──
    if (path == "/api/stickers" && req.method == "GET") {
        JValue j = JValue::mkObj();
        {
            lock_guard<recursive_mutex> lock(data_mutex);
            JValue arr = JValue::mkArr();
            for (const auto& s : stickers) arr.push(sticker_to_json(s));
            j.set("stickers", arr);
        }
        j.set("count", JValue::mkNum((int)stickers.size()));
        return ok_resp(j);
    }
    if (path == "/api/stickers/sync" && req.method == "POST") {
        string err;
        int added = 0, total = 0;
        if (!sync_stickers_from_qq(err, added, total)) return err_resp(err);
        JValue j = JValue::mkObj();
        j.set("added", JValue::mkNum(added));
        j.set("total", JValue::mkNum(total));
        JValue arr = JValue::mkArr();
        {
            lock_guard<recursive_mutex> lock(data_mutex);
            for (const auto& s : stickers) arr.push(sticker_to_json(s));
        }
        j.set("stickers", arr);
        cout << "[表情] 同步 QQ 收藏表情：新增 " << added << " 个，共 " << total << " 个" << endl;
        return ok_resp(j);
    }
    if (path == "/api/stickers" && req.method == "POST") {
        string url = trim(param_str(req, "url"));
        if (url.empty()) return err_resp("图片地址不能为空");
        int64_t sid = req.json.iget("id");
        lock_guard<recursive_mutex> lock(data_mutex);
        Sticker* found = sid ? find_sticker(sid) : nullptr;
        if (!found) {
            Sticker s;
            s.id = next_sticker_id();
            s.url = url;
            s.name = trim(param_str(req, "name"));
            if (s.name.empty()) s.name = "表情" + to_string((long long)s.id);
            s.description = trim(param_str(req, "description"));
            s.note = trim(param_str(req, "note"));
            s.source = "manual";
            stickers.push_back(s);
            sid = s.id;
        } else {
            found->url = url;
            if (req.json.has("name")) found->name = trim(param_str(req, "name"));
            if (req.json.has("description")) found->description = trim(param_str(req, "description"));
            if (req.json.has("note")) found->note = trim(param_str(req, "note"));
        }
        save_stickers();
        JValue j = JValue::mkObj();
        j.set("id", JValue::mkNum((double)sid));
        return ok_resp(j);
    }
    if (path == "/api/stickers/delete" && req.method == "POST") {
        int64_t sid = req.json.iget("id");
        lock_guard<recursive_mutex> lock(data_mutex);
        stickers.erase(remove_if(stickers.begin(), stickers.end(),
                                 [&](const Sticker& s) { return s.id == sid; }), stickers.end());
        save_stickers();
        return ok_resp();
    }
    if (path == "/api/stickers/favorite" && req.method == "POST") {
        int64_t sid = req.json.iget("id");
        lock_guard<recursive_mutex> lock(data_mutex);
        Sticker* s = find_sticker(sid);
        if (!s) return err_resp("找不到表情包编号 " + to_string((long long)sid));
        s->favorite = !s->favorite;
        if (req.json.has("note")) s->note = trim(param_str(req, "note"));
        save_stickers();
        JValue j = JValue::mkObj();
        j.set("favorite", JValue::mkBool(s->favorite));
        return ok_resp(j);
    }

    // 预览压缩效果：把编号 N 的表情按当前设置压一遍，返回 dataURL（前端可直接显示）
    if (path == "/api/stickers/preview" && req.method == "GET") {
        int64_t sid = 0;
        if (req.params.count("id")) { try { sid = stoll(req.params.at("id")); } catch (...) { sid = 0; } }
        string url;
        {
            lock_guard<recursive_mutex> lock(data_mutex);
            Sticker* s = find_sticker(sid);
            if (!s || s->url.empty()) return err_resp("未找到表情包编号 " + to_string((long long)sid));
            url = s->url;
        }
        string bytes, err;
        if (!fetch_image_bytes(url, bytes, err)) return err_resp(err);
        int edge = config.sticker_long_edge;
        string png, cerr2;
        bool compressed = compress_image_bytes(bytes, edge, png, cerr2);
        const string& payload = compressed ? png : bytes;
        string mime = "image/png";
        if (compressed) {
            if (payload.size() > 3 && payload[0] == 'G' && payload[1] == 'I' && payload[2] == 'F') mime = "image/gif";
        } else {
            if (bytes.size() > 3 && bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F') mime = "image/gif";
            else if (bytes.size() > 2 && (unsigned char)bytes[0] == 0xFF && (unsigned char)bytes[1] == 0xD8) mime = "image/jpeg";
        }
        JValue j = JValue::mkObj();
        j.set("id", JValue::mkNum((double)sid));
        j.set("edge", JValue::mkNum(edge));
        j.set("compressed", JValue::mkBool(compressed));
        j.set("originBytes", JValue::mkNum((double)bytes.size()));
        j.set("sentBytes", JValue::mkNum((double)payload.size()));
        j.set("dataUrl", JValue::mkStr("data:" + mime + ";base64," + base64_encode(payload)));
        j.set("note", JValue::mkStr(compressed ? ("长边已压到 " + to_string(edge) + "px（动图保留动画）")
                                               : (cerr2 == "no-resize" ? "原图没超过长边，按原样发送" : "压缩失败，按原图发送")));
        return ok_resp(j);
    }

    // ── 解析预览 ──
    if (path == "/api/parse" && req.method == "POST") {
        string text = param_str(req, "text");
        vector<string> errors;
        auto items = parse_message_content(text, errors);
        return ok_resp(parsed_items_json(items, errors));
    }

    // ── 让模型生成（并把结果落成待确认批次：询问还发不发） ──
    if (path == "/api/chat" && req.method == "POST") {
        string target = trim(param_str(req, "target"));
        string text = param_str(req, "text");
        string name = trim(param_str(req, "name"));
        string id = trim(param_str(req, "id"));
        string model = trim(param_str(req, "model"));
        if (target.empty()) return err_resp("需要先选择发送目标（群号或 QQ 号）");
        if (text.empty()) return err_resp("消息内容不能为空");
        if (name.empty()) name = "群友";
        string tname = current_target_name(target);
        // 先把这条“别人说的话”记进气泡流
        {
            lock_guard<recursive_mutex> lock(data_mutex);
            Message m;
            m.id = make_id("msg");
            m.batch = "";
            m.target = target;
            m.target_name = tname;
            m.dir = "in";
            m.sender = name;
            m.type = "send";
            m.content = text;
            m.sender_id = id;
            m.epoch = now_ms();
            m.status = "sent";
            m.created_at = now_ms();
            message_list.push_back(m);
            save_messages();
        }
        string prompt = process_message_template(config.prompt_template, text, name, id, target);
        JValue ai_out;
        string err;
        cout << "[思考] " << target << "（" << name << " 的消息）正在思考…" << endl;
        if (!call_ai(prompt, model, ai_out, err)) {
            JValue j = JValue::mkObj();
            j.set("prompt", JValue::mkStr(prompt));
            Response r = err_resp(err);
            JValue merged = JValue::parse_or_null(r.body);
            for (const auto& p : ai_out.oval) merged.set(p.first, p.second);
            for (const auto& p : j.oval) merged.set(p.first, p.second);
            return json_resp(merged);
        }
        string reply = ai_out.sget("text");
        {
            string plain = strip_directives(reply);
            cout << "[AI 完整回复] " << target << (plain.empty() ? "：（只有 send/image 指令）" : ("：" + plain)) << endl;
        }
        vector<string> errors;
        auto items = parse_message_content(reply, errors);
        expand_output_placeholders(items, target, id);  // %token / %gname / %id（@这个回话的人）
        string batch;
        if (!items.empty()) batch = enqueue_pending(items, target, tname, config.persona.empty() ? "AI" : config.persona);
        JValue j = parsed_items_json(items, errors);
        j.set("reply", JValue::mkStr(reply));
        j.set("prompt", JValue::mkStr(prompt));
        j.set("model", JValue::mkStr(ai_out.sget("model")));
        j.set("batchId", JValue::mkStr(batch));
        j.set("ask", JValue::mkBool(!items.empty()));   // 有 send/image 才询问还发不发
        j.set("silent", JValue::mkBool(items.empty())); // 没写 {send:} = 不发消息
        if (ai_out.find("usage")) j.set("usage", *ai_out.find("usage"));
        cout << "[AI] " << (items.empty() ? "本次不发消息" : to_string((long long)items.size()) + " 条待发，等待确认")
             << " (" << ai_out.sget("model") << ")" << endl;
        return ok_resp(j);
    }

    // ── 手动把一段文本按格式排队（同样先询问还发不发） ──
    if (path == "/api/enqueue" && req.method == "POST") {
        string target = trim(param_str(req, "target"));
        string text = param_str(req, "text");
        if (target.empty()) return err_resp("需要先选择发送目标");
        vector<string> errors;
        auto items = parse_message_content(text, errors);
        if (items.empty()) return err_resp(errors.empty() ? "没有解析出 {send:...} 或 {image:...}，本次不发消息"
                                                          : errors[0]);
        string batch = enqueue_pending(items, target, current_target_name(target),
                                       config.persona.empty() ? "AI" : config.persona);
        JValue j = parsed_items_json(items, errors);
        j.set("batchId", JValue::mkStr(batch));
        j.set("ask", JValue::mkBool(true));
        return ok_resp(j);
    }

    // ── 还发不发 ──
    if (path == "/api/pending" && req.method == "GET") {
        JValue j = JValue::mkObj();
        lock_guard<recursive_mutex> lock(data_mutex);
        j.set("batches", pending_batches_json());
        return ok_resp(j);
    }
    if (path == "/api/confirm" && req.method == "POST") {
        string batch = trim(param_str(req, "batchId"));
        if (batch.empty()) return err_resp("缺少 batchId");
        string action = param_str(req, "action", "send");
        vector<string> expired;
        size_t n;
        {
            lock_guard<recursive_mutex> lock(data_mutex);
            n = confirm_batch(batch, action != "cancel", expired);
        }
        if (n == 0) return err_resp("找不到待确认批次：" + batch);
        JValue j = JValue::mkObj();
        j.set("batchId", JValue::mkStr(batch));
        j.set("count", JValue::mkNum((double)n));
        j.set("action", JValue::mkStr(action == "cancel" ? "cancel" : "send"));
        JValue ex = JValue::mkArr();
        for (const auto& e : expired) ex.push(JValue::mkStr(e));
        j.set("expired", ex);
        cout << "[确认] " << (action == "cancel" ? "不发" : "发送") << " 批次 " << batch
             << "（" << n << " 条）" << endl;
        return ok_resp(j);
    }
    if (path == "/api/cancel-all" && req.method == "POST") {
        size_t total = 0;
        lock_guard<recursive_mutex> lock(data_mutex);
        vector<string> batches;
        for (const auto& m : message_list)
            if (m.status == "pending" && find(batches.begin(), batches.end(), m.batch) == batches.end())
                batches.push_back(m.batch);
        vector<string> e;
        for (const auto& b : batches) total += confirm_batch(b, false, e);
        JValue j = JValue::mkObj();
        j.set("count", JValue::mkNum((double)total));
        return ok_resp(j);
    }
    if (path == "/api/queue/delete" && req.method == "POST") {
        string mid = trim(param_str(req, "id"));
        lock_guard<recursive_mutex> lock(data_mutex);
        for (auto& m : message_list)
            if (m.id == mid && (m.status == "pending" || m.status == "queued")) { m.status = "cancelled"; break; }
        save_messages();
        return ok_resp();
    }

    // ── 直接发送（不走模型，不经确认；进队列：空闲时第一条瞬间发，第二条起按「队列间隔」） ──
    if (path == "/api/send" && req.method == "POST") {
        string target = trim(param_str(req, "target"));
        string type = param_str(req, "type", "send");
        string content = param_str(req, "content");
        int64_t sid = req.json.iget("stickerId");
        if (target.empty()) return err_resp("需要先选择发送目标");
        Message m;
        m.id = make_id("msg");
        m.target = target;
        m.target_name = current_target_name(target);
        m.dir = "out";
        m.sender = config.persona.empty() ? "我" : config.persona;
        m.type = (type == "image") ? "image" : "send";
        if (m.type == "image") {
            lock_guard<recursive_mutex> lock(data_mutex);
            Sticker* s = find_sticker(sid);
            if (!s || s->url.empty()) return err_resp("未找到表情包编号 " + to_string((long long)sid));
            m.content = s->url;
            m.sticker_id = s->id;
        } else {
            if (content.empty()) return err_resp("内容不能为空");
            m.content = content;
        }
        m.epoch = now_ms();
        m.created_at = now_ms();
        m.status = "queued";
        {
            lock_guard<recursive_mutex> lock(data_mutex);
            message_list.push_back(m);
            save_messages();
        }
        JValue j = JValue::mkObj();
        j.set("id", JValue::mkStr(m.id));
        j.set("queued", JValue::mkBool(true));
        j.set("simulated", JValue::mkBool(using_simulate()));
        return ok_resp(j);
    }

    // ── 消息记录 ──
    if (path == "/api/messages" && req.method == "GET") {
        string filter = req.params.count("target") ? req.params.at("target") : "";
        int limit = 400;
        if (req.params.count("limit")) { try { limit = stoi(req.params.at("limit")); } catch (...) {} }
        JValue arr = JValue::mkArr();
        lock_guard<recursive_mutex> lock(data_mutex);
        vector<const Message*> picked;
        for (const auto& m : message_list) {
            if (!filter.empty() && m.target != filter) continue;
            picked.push_back(&m);
        }
        if (limit > 0 && picked.size() > (size_t)limit)
            picked.erase(picked.begin(), picked.begin() + (picked.size() - limit));
        for (const auto* m : picked) arr.push(message_to_json(*m));
        JValue j = JValue::mkObj();
        j.set("messages", arr);
        j.set("total", JValue::mkNum((int)message_list.size()));
        return ok_resp(j);
    }
    if (path == "/api/messages/clear" && req.method == "POST") {
        string target = param_str(req, "target");
        lock_guard<recursive_mutex> lock(data_mutex);
        if (target.empty()) message_list.clear();
        else message_list.erase(remove_if(message_list.begin(), message_list.end(),
                                      [&](const Message& m) { return m.target == target; }), message_list.end());
        save_messages();
        return ok_resp();
    }

    // ── 黑名单 / 白名单（存 QQ 号；黑名单优先于白名单） ──
    if (path == "/api/userlists" && req.method == "GET") {
        JValue j = JValue::mkObj();
        lock_guard<recursive_mutex> lock(data_mutex);
        JValue b = JValue::mkArr(), w = JValue::mkArr();
        for (const auto& s : user_black) b.push(JValue::mkStr(s));
        for (const auto& s : user_white) w.push(JValue::mkStr(s));
        j.set("blacklist", b);
        j.set("whitelist", w);
        return ok_resp(j);
    }
    if (path == "/api/userlists" && req.method == "POST") {
        auto read_list = [&](const char* key, vector<string>& dst) {
            const JValue* v = req.json.find(key);
            if (!v) return;
            dst.clear();
            auto push_one = [&](const string& s) {
                string one = trim(s);
                if (!one.empty()) dst.push_back(one);
            };
            if (v->isArr())
                for (const auto& x : v->aval) {
                    if (x.isStr()) push_one(x.sval);
                    else if (x.type == JValue::TNum && x.nval == (double)(long long)x.nval)
                        push_one(to_string((long long)x.nval));
                }
            else if (v->isStr()) {
                string s = v->sval;
                size_t pos = 0;
                while (pos <= s.size()) {
                    size_t c = s.find_first_of(",，\n ", pos);
                    if (c == string::npos) c = s.size();
                    push_one(s.substr(pos, c - pos));
                    if (c == s.size()) break;
                    pos = c + 1;
                }
            }
        };
        {
            lock_guard<recursive_mutex> lock(data_mutex);
            read_list("blacklist", user_black);
            read_list("whitelist", user_white);
            save_user_lists();
        }
        return ok_resp();
    }
    // 右键头像用：在名单里则移除，不在则加入（幂等）
    if (path == "/api/userlists/toggle" && req.method == "POST") {
        string list_name = param_str(req, "list", "black");
        string id = trim(param_str(req, "id"));
        if (id.empty()) return err_resp("需要 QQ 号 id");
        if (id.find_first_not_of("0123456789") != string::npos) return err_resp("QQ 号只能是数字：" + id);
        if (list_name != "black" && list_name != "white") return err_resp("list 应为 black 或 white");
        bool in_list;
        {
            lock_guard<recursive_mutex> lock(data_mutex);
            vector<string>& dst = (list_name == "black") ? user_black : user_white;
            auto it = find(dst.begin(), dst.end(), id);
            if (it != dst.end()) { dst.erase(it); in_list = false; }
            else { dst.push_back(id); in_list = true; }
            save_user_lists();
        }
        JValue j = JValue::mkObj();
        j.set("inList", JValue::mkBool(in_list));
        j.set("list", JValue::mkStr(list_name));
        j.set("id", JValue::mkStr(id));
        return ok_resp(j);
    }
    // 申请好友（仅限陌生人）：OneBot v11 / SnowLuma 没有主动加好友动作，尽力尝试并如实回报
    if (path == "/api/friend/add" && req.method == "POST") {
        string id = trim(param_str(req, "id"));
        if (id.empty() || id.find_first_not_of("0123456789") != string::npos)
            return err_resp("QQ 号不合法：" + id);
        if (using_simulate()) {
            JValue j = JValue::mkObj();
            j.set("simulated", JValue::mkBool(true));
            j.set("note", JValue::mkStr("当前是本地模拟，没有真的发好友申请"));
            return ok_resp(j);
        }
        JValue body = JValue::mkObj();
        body.set("user_id", JValue::mkStr(id));
        string remark = trim(param_str(req, "remark"));
        if (!remark.empty()) body.set("remark", JValue::mkStr(remark));
        vector<pair<string, string>> headers = {{"Content-Type", "application/json"}};
        if (!config.onebot_token.empty()) headers.push_back({"Authorization", "Bearer " + config.onebot_token});
        HttpResp r = http_call(url_path_join(config.onebot_url, "/add_friend"), "POST", body.dump(), headers, 15000);
        JValue resp = JValue::parse_or_null(r.body);
        if (!r.ok || resp.sget("status") != "ok") {
            string detail = resp.sget("wording", resp.sget("status", "HTTP " + to_string(r.status)));
            return err_resp("OneBot 拒绝了 add_friend：" + detail +
                            "（OneBot v11 / SnowLuma 通常不支持主动加好友，只能处理别人发来的好友申请；"
                            "可以让对方先加你，然后在 SnowLuma 里通过）");
        }
        return ok_resp();
    }

    // ── 黑名单 / 白名单结束 ──

    if (path == "/api/auto/status" && req.method == "GET") {
        JValue j = JValue::mkObj();
        lock_guard<recursive_mutex> lock(poll_mutex);
        JValue arr = JValue::mkArr();
        for (const auto& kv : g_poll_state) {
            JValue s = JValue::mkObj();
            s.set("target", JValue::mkStr(kv.first));
            s.set("lastCheck", JValue::mkNum((double)kv.second.last_check));
            s.set("lastCheckText", JValue::mkStr(fmt_time(kv.second.last_check)));
            s.set("lastReplyAt", JValue::mkNum((double)kv.second.last_reply_at));
            s.set("cooldownLeftMs", JValue::mkNum((double)max<int64_t>(
                0, config.reply_cooldown_ms - (now_ms() - kv.second.last_reply_at))));
            arr.push(s);
        }
        j.set("targets", arr);
        j.set("busy", JValue::mkBool(g_auto_busy.load()));
        j.set("info", JValue::mkStr(g_last_auto_info));
        return ok_resp(j);
    }
    // 不经 OneBot 直接喂一条消息进同一条“判定→调模型→发送”管道（调试/演示用）
    if (path == "/api/auto/inject" && req.method == "POST") {
        return auto_inject(req);
    }
    // ── 连通性自检 ──
    if (path == "/api/test/ai" && req.method == "POST") {
        string model = trim(param_str(req, "model"));
        string prompt = param_str(req, "prompt", "只回复两个字：收到");
        JValue out;
        string err;
        int64_t t0 = now_ms();
        if (!call_ai(prompt, model, out, err)) return err_resp(err);
        JValue j = JValue::mkObj();
        j.set("model", JValue::mkStr(out.sget("model")));
        j.set("reply", JValue::mkStr(out.sget("text")));
        j.set("latencyMs", JValue::mkNum((double)(now_ms() - t0)));
        return ok_resp(j);
    }
    if (path == "/api/test/qq" && req.method == "POST") {
        string err;
        if (!check_onebot(err)) return err_resp(err);
        JValue j = JValue::mkObj();
        j.set("nickname", JValue::mkStr(g_self_nickname));
        j.set("url", JValue::mkStr(config.onebot_url));
        return ok_resp(j);
    }

    return err_resp("接口不存在：" + path, 404);
}

static Response handle_request(const Request& req) {
    if (req.method == "OPTIONS") { Response r; r.status = 204; r.body = ""; return r; }
    if (starts_with(req.path, "/api/")) return handle_api(req);

    // 静态文件：/ -> index.html，其余按路径取（禁止越出 app_dir）
    string rel = req.path;
    if (rel.empty() || rel == "/") rel = "/index.html";
    rel = url_decode(rel);
    if (starts_with(rel, "/file/")) rel = rel.substr(5);
    string clean = replace_all(rel, "..", "");
    while (!clean.empty() && (clean[0] == '/' || clean[0] == '\\')) clean.erase(clean.begin());
    fs::path full = fs::u8path(join_path(app_dir, clean));
    error_code ec;
    if (!fs::exists(full, ec) || fs::is_directory(full, ec)) {
        Response r;
        r.status = 404;
        r.content_type = "text/plain; charset=utf-8";
        r.body = "404 Not Found: " + clean;
        return r;
    }
    string text;
    if (!read_text_file(full.string(), text)) {
        Response r;
        r.status = 500;
        r.content_type = "text/plain; charset=utf-8";
        r.body = "读取文件失败: " + clean;
        return r;
    }
    Response r;
    r.body = text;
    r.content_type = guess_mime(clean);
    return r;
}

static void send_all(SOCKET s, const string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = send(s, data.data() + sent, (int)min<size_t>(data.size() - sent, 65536), 0);
        if (n <= 0) return;
        sent += (size_t)n;
    }
}

static void handle_client(SOCKET client) {
    string raw;
    char buf[8192];
    size_t header_end = string::npos;
    size_t content_len = 0;
    // 读请求头
    while (true) {
        int n = recv(client, buf, sizeof(buf), 0);
        if (n <= 0) break;
        raw.append(buf, (size_t)n);
        header_end = raw.find("\r\n\r\n");
        if (header_end != string::npos) break;
        if (raw.size() > 262144) break;
    }
    if (header_end == string::npos) { closesocket(client); return; }
    string head_part = raw.substr(0, header_end);
    string body = raw.substr(header_end + 4);

    Request req;
    {
        istringstream hs(head_part);
        string line;
        if (getline(hs, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t sp1 = line.find(' ');
            size_t sp2 = sp1 == string::npos ? string::npos : line.find(' ', sp1 + 1);
            if (sp1 != string::npos) {
                req.method = line.substr(0, sp1);
                req.target = sp2 == string::npos ? line.substr(sp1 + 1) : line.substr(sp1 + 1, sp2 - sp1 - 1);
            }
        }
        while (getline(hs, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            size_t colon = line.find(':');
            if (colon == string::npos) continue;
            req.headers[to_lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
        }
    }
    auto it_cl = req.headers.find("content-length");
    if (it_cl != req.headers.end()) { try { content_len = (size_t)stoul(it_cl->second); } catch (...) { content_len = 0; } }
    while (body.size() < content_len) {
        int n = recv(client, buf, sizeof(buf), 0);
        if (n <= 0) break;
        body.append(buf, (size_t)n);
    }
    req.body = body.substr(0, min<size_t>(body.size(), content_len));

    size_t q = req.target.find('?');
    req.path = q == string::npos ? req.target : req.target.substr(0, q);
    req.query = q == string::npos ? "" : req.target.substr(q + 1);
    req.params = parse_query(req.query);
    if (!req.body.empty()) req.json = JValue::parse_or_null(req.body);
    if (req.json.isNull()) req.json = JValue::mkObj();

    Response res;
    try {
        res = handle_request(req);
    } catch (const exception& e) {
        res = err_resp(string("后端异常：") + e.what(), 500);
    } catch (...) {
        res = err_resp("后端未知异常", 500);
    }

    ostringstream out;
    out << "HTTP/1.1 " << res.status << " " << (res.status == 204 ? "No Content" : "OK") << "\r\n";
    out << "Content-Type: " << res.content_type << "\r\n";
    out << "Content-Length: " << res.body.size() << "\r\n";
    out << "Cache-Control: no-store\r\n";
    out << "Access-Control-Allow-Origin: *\r\n";
    out << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    out << "Access-Control-Allow-Headers: content-type\r\n";
    out << "Connection: close\r\n\r\n";
    string head = out.str();
    send_all(client, head);
    if (!res.body.empty()) send_all(client, res.body);
    shutdown(client, SD_BOTH);
    closesocket(client);
}

static BOOL WINAPI ctrl_handler(DWORD) {
    running = false;
    return TRUE;
}

static int run_server() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        cerr << "WSAStartup 失败" << endl;
        return 1;
    }
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) { cerr << "创建 socket 失败" << endl; return 1; }
    BOOL reuse = TRUE;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons((u_short)config.port);
    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        cerr << "端口 " << config.port << " 绑定失败（错误码 " << WSAGetLastError()
             << "）：端口被占用，或用 --port 换一个端口" << endl;
        return 1;
    }
    if (listen(srv, 16) == SOCKET_ERROR) { cerr << "listen 失败" << endl; return 1; }
    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    string err;
    check_onebot(err);  // 探活一次，决定 auto 模式下的初始发送通道

    cout << "==================================================\n";
    cout << APP_NAME << " v" << APP_VERSION << " 后端已启动\n";
    cout << "  前端地址: http://127.0.0.1:" << config.port << "/index.html\n";
    cout << "  模型名字: " << config.model << "\n";
    cout << "  接口地址: " << config.base_url << "\n";
    cout << "  API Key : " << (config.key().empty() ? "未配置" : "已配置") << "\n";
    cout << "  OneBot  : " << config.onebot_url << (g_onebot_up ? "（在线）" : ("（不可用：" + err + "）")) << "\n";
    cout << "  发送通道: " << (using_simulate() ? "本地模拟" : "真实 QQ")
         << "（send_mode=" << config.send_mode << "）\n";
    cout << "  表情包  : " << stickers.size() << " 个\n";
    cout << "  Ctrl+C 退出\n==================================================\n";

    thread worker(main_loop);
    while (running) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(srv, &rf);
        timeval tv = {0, 200000};
        int sel = select(0, &rf, nullptr, nullptr, &tv);
        if (sel <= 0) continue;
        SOCKET client = accept(srv, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        DWORD tv2 = 60000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv2, sizeof(tv2));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv2, sizeof(tv2));
        try {
            thread t(handle_client, client);
            t.detach();
        } catch (...) {
            closesocket(client);
        }
    }
    running = false;
    if (worker.joinable()) worker.join();
    closesocket(srv);
    WSACleanup();
    cout << "后端已退出" << endl;
    return 0;
}

// ───────────────────────── 命令行菜单（保留原方案入口） ─────────────────────────

static void show_menu() {
    cout << "\n=== " << APP_NAME << " ===\n"
         << "1. 查看配置\n"
         << "2. 设置模型名字\n"
         << "3. 设置 API Key / 接口地址\n"
         << "4. 设置人设 / 提示词\n"
         << "5. 测试模型连通性\n"
         << "6. 测试 OneBot 连通性\n"
         << "7. 同步 QQ 收藏表情\n"
         << "8. 查看消息记录\n"
         << "9. 启动服务（默认动作）\n"
         << "0. 退出\n"
         << "请选择: ";
}

static string ask_line(const string& label, const string& cur) {
    cout << label << (cur.empty() ? "" : (" [当前: " + cur + "]")) << ": ";
    string s;
    getline(cin, s);
    s = trim(s);
    return s.empty() ? cur : s;
}

static int run_menu() {
    while (true) {
        show_menu();
        string choice;
        if (!getline(cin, choice)) break;
        choice = trim(choice);
        if (choice == "0") break;
        if (choice == "1") { cout << config_to_json().dump() << endl; }
        else if (choice == "2") { config.model = ask_line("模型名字", config.model); save_config(); }
        else if (choice == "3") {
            config.api_key = ask_line("API Key", config.key());
            config.api_url = config.api_key;
            config.base_url = ask_line("接口地址 base_url", config.base_url);
            save_config();
        } else if (choice == "4") {
            config.persona = ask_line("AI 人设", config.persona);
            config.prompt_template = ask_line("提示词模板", config.prompt_template);
            save_config();
        } else if (choice == "5") {
            JValue out;
            string err;
            if (call_ai("只回复两个字：收到", "", out, err))
                cout << "模型 OK: " << out.sget("text") << endl;
            else cout << "模型失败: " << err << endl;
        } else if (choice == "6") {
            string err;
            if (check_onebot(err)) cout << "OneBot OK，昵称: " << g_self_nickname << endl;
            else cout << "OneBot 不可用: " << err << endl;
        } else if (choice == "7") {
            string err;
            int added = 0, total = 0;
            if (sync_stickers_from_qq(err, added, total))
                cout << "同步完成：新增 " << added << "，共 " << total << " 个表情" << endl;
            else cout << "同步失败: " << err << endl;
        } else if (choice == "8") {
            lock_guard<recursive_mutex> lock(data_mutex);
            size_t from = message_list.size() > 30 ? message_list.size() - 30 : 0;
            for (size_t k = from; k < message_list.size(); k++) {
                const Message& m = message_list[k];
                cout << "[" << fmt_time(m.epoch) << "] " << m.target << " " << m.dir << " " << m.sender
                     << " (" << m.type << "/" << m.status << "): "
                     << (m.type == "image" ? ("表情#" + to_string((long long)m.sticker_id)) : m.content);
                if (!m.error.empty()) cout << " !! " << m.error;
                cout << endl;
            }
        } else if (choice == "9" || choice.empty()) {
            return run_server();
        } else cout << "无效选择" << endl;
    }
    return 0;
}

// ── 热重启：保存配置后自动重启进程（前端无需手动重启）──
static int g_argc = 0;
static char** g_argv = nullptr;

static void restart_self() {
    char exe[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    string args;
    for (int i = 1; i < g_argc; i++) {
        args += " \"";
        args += g_argv[i];
        args += "\"";
    }
    // ping 起延时作用：等旧进程退出、释放端口后再拉起新实例
    string cmd = "cmd /c ping -n 3 127.0.0.1 > nul & start \"\" \"" + string(exe) + "\"" + args;
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    vector<char> cmdv(cmd.begin(), cmd.end());
    cmdv.push_back('\0');
    if (!CreateProcessA(nullptr, cmdv.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        cout << "[warn] 热重启失败（GetLastError " << GetLastError() << "），请手动重启" << endl;
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

static void hot_restart_worker() {
    // 先让 HTTP 响应发完，再退出旧进程；新实例由 cmd 延时拉起
    this_thread::sleep_for(chrono::milliseconds(1200));
    cout << "[重启] 配置已保存，热重启程序…" << endl;
    restart_self();
    this_thread::sleep_for(chrono::milliseconds(300));
    exit(0);
}

int main(int argc, char** argv) {
    g_argc = argc;
    g_argv = argv;
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    // exe 所在目录优先作为工作目录（双击启动时 cwd 可能是 system32）
    char exe[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        fs::path p = fs::u8path(exe).parent_path();
        if (!p.empty()) { app_dir = p.string(); fs::current_path(p); }
    }
    int menu_mode = 0;
    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        if (a == "--port" && i + 1 < argc) { try { config.port = stoi(argv[++i]); } catch (...) {} }
        else if (a == "--dir" && i + 1 < argc) app_dir = fs::u8path(argv[++i]).string();
        else if (a == "--menu") menu_mode = 1;
        else if (a == "--server") menu_mode = 2;
        else if (a == "--version") { cout << APP_NAME << " " << APP_VERSION << endl; return 0; }
        else if (a == "-h" || a == "--help") {
            cout << "用法: qqai_backend [--port 8788] [--dir 项目目录] [--menu|--server]\n"
                 << "  默认直接启动服务，浏览器打开 http://127.0.0.1:<port>/index.html\n"
                 << "  --menu 进入命令行菜单；--server 强制服务模式\n";
            return 0;
        }
    }
    init_data_directory();
    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        if (a == "--port" && i + 1 < argc) { config.port = atoi(argv[i + 1]); break; }
    }
    load_config();
    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        if (a == "--port" && i + 1 < argc) { try { config.port = stoi(argv[++i]); } catch (...) {} break; }
    }
    load_all_data();
    load_poll_state();
    load_user_lists();
    load_token_usage();
    if (menu_mode == 1) return run_menu();
    return run_server();
}
