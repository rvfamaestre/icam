#include "icam/plist.h"

#include <cctype>
#include <cstdlib>

namespace icam::plist {

std::string Escape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

DictWriter& DictWriter::String(const std::string& key, const std::string& value) {
    body_ += "\t<key>" + Escape(key) + "</key>\n\t<string>" + Escape(value) + "</string>\n";
    return *this;
}

DictWriter& DictWriter::Integer(const std::string& key, int64_t value) {
    body_ += "\t<key>" + Escape(key) + "</key>\n\t<integer>" + std::to_string(value) + "</integer>\n";
    return *this;
}

std::string DictWriter::Xml() const {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
           "<plist version=\"1.0\">\n<dict>\n" +
           body_ + "</dict>\n</plist>\n";
}

namespace {

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    bool Document(Node& out) {
        // Skip prolog, doctype and the <plist> element.
        for (;;) {
            SkipWs();
            if (pos_ >= s_.size()) return false;
            if (s_.compare(pos_, 5, "<?xml") == 0 || s_.compare(pos_, 2, "<!") == 0) {
                size_t e = s_.find('>', pos_);
                if (e == std::string::npos) return false;
                pos_ = e + 1;
                continue;
            }
            if (s_.compare(pos_, 6, "<plist") == 0) {
                size_t e = s_.find('>', pos_);
                if (e == std::string::npos) return false;
                pos_ = e + 1;
                if (!Value(out, 0)) return false;
                return true;
            }
            // Some producers omit <plist>.
            return Value(out, 0);
        }
    }

private:
    void SkipWs() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }

    // Reads "<tag>" or "<tag/>"; returns tag name and whether it was self-closing.
    bool OpenTag(std::string& name, bool& self_closing) {
        SkipWs();
        if (pos_ >= s_.size() || s_[pos_] != '<') return false;
        size_t e = s_.find('>', pos_);
        if (e == std::string::npos) return false;
        std::string inner = s_.substr(pos_ + 1, e - pos_ - 1);
        pos_ = e + 1;
        self_closing = !inner.empty() && inner.back() == '/';
        if (self_closing) inner.pop_back();
        size_t sp = inner.find_first_of(" \t\r\n");
        name = inner.substr(0, sp);
        return !name.empty() && name[0] != '/';
    }

    bool CloseTag(const std::string& name) {
        SkipWs();
        std::string want = "</" + name + ">";
        if (s_.compare(pos_, want.size(), want) != 0) return false;
        pos_ += want.size();
        return true;
    }

    bool PeekClose() {
        SkipWs();
        return s_.compare(pos_, 2, "</") == 0;
    }

    bool Text(const std::string& name, std::string& out) {
        size_t e = s_.find("</" + name + ">", pos_);
        if (e == std::string::npos) return false;
        out = Unescape(s_.substr(pos_, e - pos_));
        pos_ = e + name.size() + 3;
        return true;
    }

    static std::string Unescape(const std::string& t) {
        std::string o;
        o.reserve(t.size());
        for (size_t i = 0; i < t.size(); ++i) {
            if (t[i] != '&') {
                o += t[i];
                continue;
            }
            size_t semi = t.find(';', i);
            if (semi == std::string::npos) {
                o += t[i];
                continue;
            }
            std::string ent = t.substr(i + 1, semi - i - 1);
            if (ent == "amp") o += '&';
            else if (ent == "lt") o += '<';
            else if (ent == "gt") o += '>';
            else if (ent == "quot") o += '"';
            else if (ent == "apos") o += '\'';
            else o += "&" + ent + ";";
            i = semi;
        }
        return o;
    }

    bool Value(Node& out, int depth) {
        if (depth > 16) return false;
        std::string tag;
        bool sc = false;
        if (!OpenTag(tag, sc)) return false;
        if (tag == "dict") {
            out.kind = Node::Kind::Dict;
            if (sc) return true;
            while (!PeekClose()) {
                std::string ktag;
                bool ksc = false;
                if (!OpenTag(ktag, ksc) || ktag != "key" || ksc) return false;
                std::string key;
                if (!Text("key", key)) return false;
                Node v;
                if (!Value(v, depth + 1)) return false;
                out.dict[key] = std::move(v);
                if (out.dict.size() > 4096) return false;
            }
            return CloseTag("dict");
        }
        if (tag == "array") {
            out.kind = Node::Kind::Array;
            if (sc) return true;
            while (!PeekClose()) {
                Node v;
                if (!Value(v, depth + 1)) return false;
                out.array.push_back(std::move(v));
                if (out.array.size() > 4096) return false;
            }
            return CloseTag("array");
        }
        if (tag == "true" || tag == "false") {
            out.kind = Node::Kind::Bool;
            out.boolean = tag == "true";
            return sc || CloseTag(tag);
        }
        if (tag == "string") {
            out.kind = Node::Kind::String;
            return sc || Text(tag, out.text);
        }
        if (tag == "integer") {
            out.kind = Node::Kind::Integer;
            if (sc) return false;
            if (!Text(tag, out.text)) return false;
            out.integer = std::strtoll(out.text.c_str(), nullptr, 10);
            return true;
        }
        // data, real, date: keep text.
        out.kind = Node::Kind::Other;
        return sc || Text(tag, out.text);
    }

    const std::string& s_;
    size_t pos_ = 0;
};

}  // namespace

bool Parse(const std::string& xml, Node& out) {
    if (xml.size() > 4 * 1024 * 1024) return false;
    Parser p(xml);
    return p.Document(out);
}

}  // namespace icam::plist
