#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

struct LoadEntry {
    int32_t     priority{};
    std::string rel_path;     // e.g. "ExampleMod/"
    std::string spawn_class;  // empty => mount only
};

static inline void
trim_inplace(std::string& s)
{
    auto not_ws = [](unsigned char c) {
        return !std::isspace(c);
    };

    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_ws));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_ws).base(), s.end());
}

static inline std::vector<std::string>
split_ws(const std::string& line)
{
    std::istringstream iss(line);
    std::vector<std::string> out;
    std::string tok;
    while (iss >> tok) {
        out.push_back(tok);
    }
    return out;
}

static inline bool
starts_with(const std::string& s, const char* p)
{
    return s.rfind(p, 0) == 0;
}

static inline std::string
strip_quotes(std::string v)
{
    if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\''))) {
        return v.substr(1, v.size() - 2);
    }
    return v;
}

static std::vector<LoadEntry>
parse_load_order(fs::path filepath)
{
    std::vector<LoadEntry> out;

    std::ifstream f(filepath);
    if (!f.is_open()) {
        return out;
    }

    std::string line;
    while (std::getline(f, line)) {
        // remove comments
        if (auto pos = line.find('#'); pos != std::string::npos) {
            line = line.substr(0, pos);
        }

        trim_inplace(line);
        if (line.empty()) {
            continue;
        }

        auto toks = split_ws(line);
        if (toks.size() < 2) {
            continue;
        }

        LoadEntry e{};
        try {
            e.priority = std::stoi(toks[0]);
        } catch (...) {
            continue; // invalid priority
        }

        e.rel_path = toks[1];

        // options: key=value
        for (size_t i = 2; i < toks.size(); ++i) {
            auto& kv = toks[i];
            auto eq = kv.find('=');
            if (eq == std::string::npos) {
                continue;
            }

            std::string k = kv.substr(0, eq);
            std::string v = strip_quotes(kv.substr(eq + 1));

            if (k == "spawn") {
                e.spawn_class = v; // "/Game/Mods/ExampleMod/ModActor.ModActor_C"
            }
        }

        out.push_back(std::move(e));
    }

    std::stable_sort(out.begin(), out.end(), [](const LoadEntry& a, const LoadEntry& b){
        return a.priority < b.priority;
    });

    return out;
}

static inline bool
has_ext(const fs::path& p, const wchar_t* ext)
{
    return p.has_extension() && p.extension() == ext;
}

static inline bool
exists_pair_for_base(const fs::path& base_no_ext)
{
    // base_no_ext is like ".../ExampleMod/ExampleMod"
    return fs::exists(base_no_ext.wstring() + L".utoc") && fs::exists(base_no_ext.wstring() + L".ucas");
}

// Returns "IoStore mount base paths" (without extension)
static std::vector<std::string>
generate_mount_bases(fs::path filepath)
{
    std::vector<std::string> out;

    filepath = filepath.lexically_normal();

    // 1) rel is a directory => enumerate all *.utoc
    if (fs::is_directory(filepath)) {
        for (const auto& it : fs::directory_iterator(filepath)){
            if (!it.is_regular_file()) {
                continue;
            }

            const fs::path& p = it.path();
            if (!has_ext(p, L".utoc")) {
                continue;
            }

            fs::path base = p;
            base.replace_extension(L""); // remove ".utoc"
            if (!exists_pair_for_base(base)) {
                continue;
            }

            out.push_back(base.generic_string());
        }
    }
    // 2) rel is a file (.utoc or .ucas) => strip ext to base
    else if (has_ext(filepath, L".utoc") || has_ext(filepath, L".ucas")) {
        fs::path base = filepath;
        base.replace_extension(L"");
        if (exists_pair_for_base(base)) {
            out.push_back(base.generic_string());
        }
    }
    // 3) rel is already the base (no ext) => check base.utoc + base.ucas
    else if (exists_pair_for_base(filepath)) {
        out.push_back(filepath.generic_string());
    }

    std::sort(out.begin(), out.end());
    return out;
}

static inline bool
file_exists(const fs::path& p)
{
    std::error_code ec;
    return fs::exists(p, ec);
}

static inline fs::path
base_to_ext(const fs::path& base_no_ext, const wchar_t* ext)
{
    fs::path p = base_no_ext;
    p.replace_extension(ext);
    return p;
}

static bool
has_ucas_any(const fs::path& base_no_ext)
{
    if (file_exists(base_to_ext(base_no_ext, L".ucas"))) {
        return true;
    }

    for (int i = 1; i <= 16; ++i) {
        fs::path p = base_no_ext;
        p += L".ucas" + std::to_wstring(i);
        if (file_exists(p)) {
            return true;
        }
    }
    return false;
}
