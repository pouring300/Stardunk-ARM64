#include "guest.h"

#include <cstdio>
#include <cwchar>

// Formats a guest printf-style string. Guest 'long' and pointers are 32-bit,
// 'long long'/'j' are 64-bit, 'L' (long double) is a 64-bit double on ARM32.
std::string guest_format(const char* fmt, VarArgs& va) {
    std::string out;
    char spec[64], buf[512];
    for (const char* p = fmt; *p;) {
        if (*p != '%') { out += *p++; continue; }
        if (p[1] == '%') { out += '%'; p += 2; continue; }
        const char* start = p++;
        std::string flags;
        while (*p && strchr("-+ #0'", *p)) flags += *p++;
        std::string width, prec;
        if (*p == '*') { width = std::to_string((int32_t)va.next32()); p++; }
        else while (*p >= '0' && *p <= '9') width += *p++;
        bool has_prec = false;
        if (*p == '.') {
            has_prec = true;
            p++;
            if (*p == '*') { prec = std::to_string((int32_t)va.next32()); p++; }
            else while (*p >= '0' && *p <= '9') prec += *p++;
        }
        int lng = 0;  // 0 = 32-bit, 2 = 64-bit
        bool half = false, wide = false;
        for (;;) {
            if (*p == 'h') { half = true; p++; }
            else if (*p == 'l') { if (p[1] == 'l') { lng = 2; p += 2; } else { wide = true; p++; } }
            else if (*p == 'q' || *p == 'j' || *p == 'L') { lng = 2; p++; }
            else if (*p == 'z' || *p == 't') { p++; }
            else break;
        }
        char conv = *p ? *p++ : 0;
        std::string base = "%" + flags + width + (has_prec ? "." + prec : "");
        switch (conv) {
            case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c': {
                if (conv == 'c' && wide) {
                    wchar_t wc = (wchar_t)va.next32();
                    snprintf(spec, sizeof spec, "%slc", base.c_str());
                    snprintf(buf, sizeof buf, spec, wc);
                } else if (lng == 2) {
                    snprintf(spec, sizeof spec, "%sll%c", base.c_str(), conv);
                    snprintf(buf, sizeof buf, spec, (long long)va.next64());
                } else {
                    uint32_t v = va.next32();
                    if (half && conv != 'c') v = (conv == 'd' || conv == 'i') ? (uint32_t)(int32_t)(int16_t)v : (uint16_t)v;
                    snprintf(spec, sizeof spec, "%s%c", base.c_str(), conv);
                    snprintf(buf, sizeof buf, spec, (int)v);
                }
                out += buf;
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                snprintf(spec, sizeof spec, "%s%c", base.c_str(), conv);
                snprintf(buf, sizeof buf, spec, va.nextd());
                out += buf;
                break;
            }
            case 'p': {
                snprintf(buf, sizeof buf, "0x%x", va.next32());
                out += buf;
                break;
            }
            case 's': {
                uint32_t a = va.next32();
                if (wide) {
                    const wchar_t* ws = a ? (const wchar_t*)g2h(a) : L"(null)";
                    snprintf(spec, sizeof spec, "%sls", base.c_str());
                    std::string tmp(4096, '\0');
                    int n = snprintf(tmp.data(), tmp.size(), spec, ws);
                    if (n > 0) out.append(tmp.data(), std::min<size_t>(n, tmp.size() - 1));
                } else {
                    const char* s = a ? gstr(a) : "(null)";
                    if (flags.empty() && width.empty() && !has_prec) { out += s; break; }
                    snprintf(spec, sizeof spec, "%ss", base.c_str());
                    int n = snprintf(nullptr, 0, spec, s);
                    std::string tmp(n + 1, '\0');
                    snprintf(tmp.data(), tmp.size(), spec, s);
                    out.append(tmp.data(), n);
                }
                break;
            }
            case 'n': {
                uint32_t a = va.next32();
                if (a) wr32(a, (uint32_t)out.size());
                break;
            }
            default:
                out.append(start, p - start);
        }
    }
    return out;
}

// sscanf/fscanf: rewrites the format for host type sizes and passes host pointers.
int guest_scan(const char* input, const char* fmt, VarArgs& va, void* file) {
    std::string hfmt;
    void* ptrs[24] = {};
    int np = 0;
    for (const char* p = fmt; *p;) {
        if (*p != '%') { hfmt += *p++; continue; }
        if (p[1] == '%') { hfmt += "%%"; p += 2; continue; }
        hfmt += *p++;
        bool suppress = false;
        if (*p == '*') { suppress = true; hfmt += *p++; }
        while (*p >= '0' && *p <= '9') hfmt += *p++;
        std::string len;
        for (;;) {
            if (*p == 'h' || *p == 'l' || *p == 'L' || *p == 'q' || *p == 'j' || *p == 'z' || *p == 't') len += *p++;
            else break;
        }
        char conv = *p;
        if (!conv) break;
        p++;
        bool is_int = strchr("diouxXn", conv) != nullptr;
        bool is_flt = strchr("eEfFgGaA", conv) != nullptr;
        std::string hlen;
        if (is_int) {
            if (len == "ll" || len == "q" || len == "j" || len == "L") hlen = "ll";
            else if (len == "h" || len == "hh") hlen = len;
        } else if (is_flt) {
            if (len == "l" || len == "L" || len == "ll") hlen = "l";
        } else if ((conv == 's' || conv == 'c' || conv == '[') && len == "l") {
            hlen = "l";
        }
        if (conv == 'p') { conv = 'x'; }
        hfmt += hlen;
        hfmt += conv;
        if (conv == '[') {
            if (*p == '^') hfmt += *p++;
            if (*p == ']') hfmt += *p++;
            while (*p && *p != ']') hfmt += *p++;
            if (*p) hfmt += *p++;
        }
        if (!suppress) {
            if (np >= 24) break;
            ptrs[np++] = g2h(va.next32());
        }
    }
#define P ptrs[0], ptrs[1], ptrs[2], ptrs[3], ptrs[4], ptrs[5], ptrs[6], ptrs[7], ptrs[8], ptrs[9], ptrs[10], ptrs[11], \
          ptrs[12], ptrs[13], ptrs[14], ptrs[15], ptrs[16], ptrs[17], ptrs[18], ptrs[19], ptrs[20], ptrs[21], ptrs[22], ptrs[23]
    if (file) return fscanf((FILE*)file, hfmt.c_str(), P);
    return sscanf(input, hfmt.c_str(), P);
#undef P
}
