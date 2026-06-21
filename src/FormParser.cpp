#include "FormParser.h"

using namespace std;

string url_decode(const string& s) {
    string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size()) {
            int hi = s[i + 1], lo = s[i + 2];
            auto hex = [](int c) {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            };
            int h = hex(hi), l = hex(lo);
            if (h >= 0 && l >= 0) {
                out += static_cast<char>((h << 4) | l);
                i += 2;
            } else {
                out += s[i];
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

map<string, string> parse_form_urlencoded(const string& body) {
    map<string, string> m;
    size_t start = 0;
    while (start < body.size()) {
        size_t amp = body.find('&', start);
        string pair = body.substr(start,
            amp == string::npos ? string::npos : amp - start);
        size_t eq = pair.find('=');
        if (eq != string::npos)
            m[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
        if (amp == string::npos) break;
        start = amp + 1;
    }
    return m;
}

string escape_sql(const string& s) {
    string out;
    for (char c : s) {
        if (c == '\'') out += "''";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}