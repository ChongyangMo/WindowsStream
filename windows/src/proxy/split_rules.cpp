#include "split_rules.h"
#include <sstream>

namespace proxy {
namespace {
std::string Quote(const std::string& value) {
    std::string result = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 32 || c >= 127) {
            constexpr char digits[] = "0123456789abcdef";
            result += "\\u00"; result += digits[c >> 4]; result += digits[c & 15];
        } else result += static_cast<char>(c);
    }
    return result + "\"";
}
std::vector<std::string> Parts(std::string value) {
    std::vector<std::string> result;
    for (char& c : value) if (c == ',' || c == ';' || c == '\t' || c == '\n') c = ' ';
    std::istringstream items(value); std::string item;
    while (items >> item) result.push_back(item);
    return result;
}
bool Read(const std::wstring& ini, const wchar_t* key, const wchar_t* fallback, std::string& result) {
    wchar_t value[8192]{};
    const auto count = GetPrivateProfileStringW(L"intranet", key, fallback, value, 8192, ini.c_str());
    if (count >= 8191) return false;
    result = WideToUtf8(value); return true;
}
bool Ipv4(const std::string& text, uint32_t& value) {
    IN_ADDR address{};
    if (inet_pton(AF_INET, text.c_str(), &address) != 1) return false;
    value = ntohl(address.s_addr); return true;
}
std::string Fallback(const SystemSettings& original) {
    if (!(original.flags & 2)) return "function wsFallback(url,host){return 'DIRECT';}\n";
    std::string result = "function wsFallback(url,host){\n";
    for (const auto& bypass : Parts(WideToUtf8(original.bypass))) {
        if (bypass == "<local>") result += "if(isPlainHostName(host))return 'DIRECT';\n";
        else result += "if(shExpMatch(host," + Quote(bypass) + "))return 'DIRECT';\n";
    }
    std::string http = "DIRECT", https = "DIRECT", generic, socks;
    for (const auto& entry : Parts(WideToUtf8(original.server))) {
        const auto equal = entry.find('=');
        if (equal == std::string::npos) { generic = "PROXY " + entry; continue; }
        auto name = entry.substr(0, equal);
        for (char& c : name) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        auto server = entry.substr(equal + 1);
        if (name == "http") http = "PROXY " + server;
        if (name == "https") https = "PROXY " + server;
        if (name == "socks") socks = "SOCKS " + server;
    }
    if (http == "DIRECT" && !socks.empty()) http = socks;
    if (https == "DIRECT" && !socks.empty()) https = socks;
    if (!generic.empty()) http = https = generic;
    result += "return url.substring(0,6).toLowerCase()==='https:'?" + Quote(https) + ":" + Quote(http) + ";}\n";
    return result;
}
}
bool BuildSplitPac(const std::wstring& ini, uint16_t port, const SystemSettings& original, std::string& script) {
    script.clear();
    if (!port) return false;
    // 无法可靠嵌套任意外部 PAC/WPAD 时保留用户原配置，避免悄悄改变外网出口。
    if (original.flags & (4 | 8)) {
        LogLine("原网络已启用自动代理脚本或自动发现，无法安全合并内网分流，保留原代理设置"); return false;
    }
    const auto endpoint = "127.0.0.1:" + std::to_string(port);
    if ((original.flags & 2) && WideToUtf8(original.server).find(endpoint) != std::string::npos) {
        LogLine("原系统代理指向当前内网转发端口，请先关闭旧版内网访问再启动新版"); return false;
    }
    std::string networks, domains;
    if (!Read(ini, L"networks", L"10.18.0.0/16", networks) ||
        !Read(ini, L"domains", L"sdserver.luxvisions-inno.com", domains)) {
        LogLine("内网分流规则过长，请检查配置"); return false;
    }
    std::string ranges, matches;
    for (const auto& cidr : Parts(networks)) {
        const auto slash = cidr.find('/');
        uint32_t network = 0;
        const auto prefix = slash == std::string::npos ? "" : cidr.substr(slash + 1);
        if (prefix.empty() || prefix.size() > 2 || prefix.find_first_not_of("0123456789") != std::string::npos ||
            !Ipv4(cidr.substr(0, slash), network) || strtoul(prefix.c_str(), nullptr, 10) > 32) {
            LogLine("内网网段配置无效：%s", cidr.c_str()); return false;
        }
        const unsigned bits = static_cast<unsigned>(strtoul(prefix.c_str(), nullptr, 10));
        const uint32_t mask = bits ? 0xFFFFFFFFu << (32 - bits) : 0;
        ranges += "if(((n&" + std::to_string(mask) + ")>>>0)===" + std::to_string(network & mask) + ")return true;\n";
    }
    for (auto domain : Parts(domains)) {
        bool suffix = domain.starts_with("*.");
        if (suffix) domain.erase(0, 2);
        for (char& c : domain) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (domain.empty() || domain.size() > 253 || domain.front() == '.' || domain.back() == '.' ||
            domain.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-.") != std::string::npos) {
            LogLine("内网域名配置无效：%s", domain.c_str()); return false;
        }
        auto condition = "host===" + Quote(domain);
        if (suffix) condition += "||dnsDomainIs(host," + Quote("." + domain) + ")";
        matches += "if(" + condition + ")return " + Quote("PROXY " + endpoint) + ";\n";
    }
    if (ranges.empty() && matches.empty()) { LogLine("内网分流规则为空，未启用代理"); return false; }
    script = "function wsIPv4(ip){if(!ip)return null;var a=ip.split('.');if(a.length!==4)return null;var n=0;"
        "for(var i=0;i<4;i++){if(!/^[0-9]{1,3}$/.test(a[i])||Number(a[i])>255)return null;n=n*256+Number(a[i]);}return n>>>0;}\n"
        "function wsInternal(n){" + ranges + "return false;}\n" + Fallback(original) +
        "function FindProxyForURL(url,host){host=host.toLowerCase().replace(/\\.$/,'');\n" + matches +
        "var n=wsIPv4(host);if(n!==null){if(wsInternal(n))return " + Quote("PROXY " + endpoint) +
        ";return wsFallback(url,host);}\n"
        "if(host.indexOf(':')<0&&host!=='localhost'){var ip=dnsResolve(host);n=wsIPv4(ip);"
        "if(n!==null&&wsInternal(n))return " + Quote("PROXY " + endpoint) + ";}\nreturn wsFallback(url,host);}\n";
    return true;
}
}
