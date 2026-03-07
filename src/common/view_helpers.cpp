#include "view_helpers.h"

namespace bdtrace {

const char* mode_str(int mode) {
    switch (mode) {
        case FA_READ:           return "R";
        case FA_WRITE:          return "W";
        case FA_RDWR:           return "RW";
        case FA_STAT:           return "ST";
        case FA_ACCESS:         return "AC";
        case FA_EXEC:           return "EX";
        case FA_UNLINK:         return "UL";
        case FA_RENAME_SRC:     return "RS";
        case FA_RENAME_DST:     return "RD";
        case FA_READLINK:       return "RL";
        case FA_MKDIR:          return "MD";
        case FA_LINK_SRC:       return "LS";
        case FA_LINK_DST:       return "LD";
        case FA_SYMLINK_TARGET: return "SY";
        case FA_SYMLINK_LINK:   return "SL";
        case FA_CHMOD:          return "CM";
        case FA_CHOWN:          return "CO";
        case FA_TRUNCATE:       return "TR";
        case FA_MKNOD:          return "MN";
        case FA_UTIMENS:        return "UT";
        case FA_CHDIR:          return "CD";
        default:                return "?";
    }
}

bool is_input_mode(int mode) {
    return mode == FA_READ || mode == FA_RDWR || mode == FA_EXEC
        || mode == FA_STAT || mode == FA_ACCESS || mode == FA_READLINK;
}

bool is_output_mode(int mode) {
    return mode == FA_WRITE || mode == FA_RDWR || mode == FA_RENAME_DST
        || mode == FA_LINK_DST || mode == FA_SYMLINK_LINK
        || mode == FA_TRUNCATE || mode == FA_MKNOD || mode == FA_MKDIR;
}

std::string cmd_name(const std::string& cmdline) {
    std::string first = cmdline;
    size_t sp = first.find(' ');
    if (sp != std::string::npos) first = first.substr(0, sp);
    size_t sl = first.rfind('/');
    if (sl != std::string::npos) first = first.substr(sl + 1);
    return first;
}

std::string shorten_cmd(const std::string& cmdline, size_t max_len) {
    if (cmdline.size() <= max_len) return cmdline;
    if (max_len <= 3) return cmdline.substr(0, max_len);
    return cmdline.substr(0, max_len - 3) + "...";
}

const char* errno_name(int e) {
    switch (e) {
        case 1:  return "EPERM";
        case 2:  return "ENOENT";
        case 13: return "EACCES";
        case 17: return "EEXIST";
        case 20: return "ENOTDIR";
        case 21: return "EISDIR";
        case 22: return "EINVAL";
        case 36: return "ENAMETOOLONG";
        case 40: return "ELOOP";
        default: return "";
    }
}

} // namespace bdtrace
