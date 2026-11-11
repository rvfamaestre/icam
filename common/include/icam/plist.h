#pragma once

// Minimal XML property-list reader/writer: just enough for usbmuxd (dict, array, string,
// integer, true/false; data/real/date are kept as raw text). No external dependency.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace icam::plist {

struct Node {
    enum class Kind { Dict, Array, String, Integer, Bool, Other } kind = Kind::Other;
    std::map<std::string, Node> dict;
    std::vector<Node> array;
    std::string text;
    int64_t integer = 0;
    bool boolean = false;

    const Node* Get(const std::string& key) const {
        auto it = dict.find(key);
        return it == dict.end() ? nullptr : &it->second;
    }
    std::string GetString(const std::string& key, const std::string& def = {}) const {
        const Node* n = Get(key);
        return n && n->kind == Kind::String ? n->text : def;
    }
    int64_t GetInt(const std::string& key, int64_t def = -1) const {
        const Node* n = Get(key);
        return n && n->kind == Kind::Integer ? n->integer : def;
    }
};

// Builds `<dict>` with string and integer values, in insertion order.
class DictWriter {
public:
    DictWriter& String(const std::string& key, const std::string& value);
    DictWriter& Integer(const std::string& key, int64_t value);
    std::string Xml() const;

private:
    std::string body_;
};

// Parses an XML plist document. Returns false on malformed input (depth- and size-limited).
bool Parse(const std::string& xml, Node& out);

std::string Escape(const std::string& s);

}  // namespace icam::plist
