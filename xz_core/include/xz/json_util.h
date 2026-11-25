// 与固件 application.cc 里的 JsonEscape 同源：只转义、不加引号
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace xz {

std::string JsonEscape(const std::string& in);
// 转义并加引号，便于直接拼 JSON
std::string Quote(const std::string& in);

// 扁平 JSON 对象解析（args 只有 string/number/bool，够工具参数用）
class Args {
 public:
    bool Parse(const std::string& json);
    bool Has(const std::string& k) const;
    std::string Str(const std::string& k, const std::string& def = "") const;
    long long Int(const std::string& k, long long def = 0) const;
    bool Bool(const std::string& k, bool def = false) const;

 private:
    struct Val { std::string raw; bool is_str = false; };
    std::string Get(const std::string& k) const;
    std::vector<std::pair<std::string, Val>> kv_;
};

// 从一个 JSON 文本里取某个键的原始值（字符串去引号、对象/数组取原文）
bool JsonRawValue(const std::string& json, const std::string& key, std::string* out, bool* was_string);

}  // namespace xz