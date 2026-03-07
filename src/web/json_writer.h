#ifndef BDTRACE_JSON_WRITER_H
#define BDTRACE_JSON_WRITER_H

#include <string>
#include <vector>
#include <cstdio>
#include <stdint.h>

namespace bdtrace {

class JsonWriter {
public:
    JsonWriter() : need_comma_(false) {}

    JsonWriter& beginObject() {
        comma_();
        buf_ += '{';
        stack_.push_back(false);
        need_comma_ = false;
        return *this;
    }

    JsonWriter& endObject() {
        buf_ += '}';
        stack_.pop_back();
        need_comma_ = true;
        return *this;
    }

    JsonWriter& beginArray() {
        comma_();
        buf_ += '[';
        stack_.push_back(false);
        need_comma_ = false;
        return *this;
    }

    JsonWriter& endArray() {
        buf_ += ']';
        stack_.pop_back();
        need_comma_ = true;
        return *this;
    }

    JsonWriter& key(const char* k) {
        comma_();
        escape_(k);
        buf_ += ':';
        need_comma_ = false;
        return *this;
    }

    JsonWriter& val(const std::string& s) {
        comma_();
        escape_(s.c_str());
        need_comma_ = true;
        return *this;
    }

    JsonWriter& val(const char* s) {
        comma_();
        escape_(s);
        need_comma_ = true;
        return *this;
    }

    JsonWriter& val(int v) {
        comma_();
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d", v);
        buf_ += buf;
        need_comma_ = true;
        return *this;
    }

    JsonWriter& val(int64_t v) {
        comma_();
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", (long long)v);
        buf_ += buf;
        need_comma_ = true;
        return *this;
    }

    JsonWriter& val(double v) {
        comma_();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.6g", v);
        buf_ += buf;
        need_comma_ = true;
        return *this;
    }

    JsonWriter& val(bool v) {
        comma_();
        buf_ += v ? "true" : "false";
        need_comma_ = true;
        return *this;
    }

    JsonWriter& null_val() {
        comma_();
        buf_ += "null";
        need_comma_ = true;
        return *this;
    }

    const std::string& str() const { return buf_; }

private:
    void comma_() {
        if (need_comma_) buf_ += ',';
    }

    void escape_(const char* s) {
        buf_ += '"';
        for (; *s; ++s) {
            switch (*s) {
                case '"':  buf_ += "\\\""; break;
                case '\\': buf_ += "\\\\"; break;
                case '\b': buf_ += "\\b";  break;
                case '\f': buf_ += "\\f";  break;
                case '\n': buf_ += "\\n";  break;
                case '\r': buf_ += "\\r";  break;
                case '\t': buf_ += "\\t";  break;
                default:
                    if ((unsigned char)*s < 0x20) {
                        char esc[8];
                        std::snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)*s);
                        buf_ += esc;
                    } else {
                        buf_ += *s;
                    }
            }
        }
        buf_ += '"';
    }

    std::string buf_;
    std::vector<bool> stack_;
    bool need_comma_;
};

} // namespace bdtrace

#endif // BDTRACE_JSON_WRITER_H
