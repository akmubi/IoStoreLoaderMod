#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <UE4SSProgram.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/World.hpp>
#include <Unreal/Hooks.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/FField.hpp>

#include <windows.h>
#include <MinHook.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

#define LOG_NOTICE(...) Output::send<LogLevel::Verbose>(STR("[IoStoreLoaderMod] ") __VA_ARGS__)
#define LOG_INFO(...)   Output::send<LogLevel::Normal>(STR("[IoStoreLoaderMod] ") __VA_ARGS__)
#define LOG_WARN(...)   Output::send<LogLevel::Warning>(STR("[IoStoreLoaderMod] ") __VA_ARGS__)
#define LOG_ERROR(...)  Output::send<LogLevel::Error>(STR("[IoStoreLoaderMod] ") __VA_ARGS__)

using namespace RC;
using namespace RC::Unreal;

static const std::wstring kModName = STR("IoStoreLoaderMod");
static const int          kBaseOrder = 200;

namespace sigscan
{
    struct Span {
        const uint8_t* base{};
        size_t size{};
        char name[9]{};
    };

    static inline int
    hex_val(int c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    }

    int
    exec_spans(HMODULE module, Span* out_spans, int max_spans)
    {
        if (!module || !out_spans || max_spans <= 0) {
            return 0;
        }

        auto* base = reinterpret_cast<uint8_t*>(module);
        auto* dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) {
            return 0;
        }

        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (!nt || nt->Signature != IMAGE_NT_SIGNATURE) {
            return 0;
        }

        auto* sec   = IMAGE_FIRST_SECTION(nt);
        WORD  n_sec = nt->FileHeader.NumberOfSections;

        int n_spans = 0;
        for (WORD i = 0; i < n_sec && n_spans < max_spans; ++i) {
            DWORD ch = sec[i].Characteristics;
            if ((ch & IMAGE_SCN_MEM_EXECUTE) == 0) {
                continue;
            }

            Span& s = out_spans[n_spans];
            std::memset(s.name, 0, sizeof(s.name));
            std::memcpy(s.name, sec[i].Name, IMAGE_SIZEOF_SHORT_NAME);

            uint64_t vsz = sec[i].Misc.VirtualSize;
            uint64_t rsz = sec[i].SizeOfRawData;
            s.base = base + sec[i].VirtualAddress;
            s.size = static_cast<size_t>((vsz > rsz) ? vsz : rsz);

            ++n_spans;
        }
        return n_spans;
    }

    size_t
    parse_pattern(const char* pattern, uint8_t* out_bytes, char* out_mask, size_t max_len)
    {
        if (!pattern || !out_bytes || !out_mask || max_len == 0) {
            return 0;
        }

        size_t      n = 0;
        const char* p = pattern;

        auto skip_ws = [&]() {
            while (*p == ' ' || *p == '\t' || *p == ',' || *p == '\r' || *p == '\n') {
                ++p;
            }
        };

        skip_ws();
        while (*p && n < max_len) {
            skip_ws();
            if (!*p) {
                break;
            }

            if (*p == '?') {
                out_bytes[n] = 0;
                out_mask[n]  = '?';
                
                ++n;
                ++p;

                if (*p == '?') {
                    ++p;
                }
            } else {
                int hi = hex_val(p[0]);
                int lo = hex_val(p[1]);
                if (hi < 0 || lo < 0) {
                    return 0;
                }

                out_bytes[n] = static_cast<uint8_t>((hi << 4) | lo);
                out_mask[n]  = 'x';
                ++n;
                p += 2;
            }
            skip_ws();
        }

        if (n < max_len) {
            out_mask[n] = '\0';
        }
        return n;
    }

    static inline bool
    match_at(const uint8_t* a, const uint8_t* b, const char* mask, size_t len)
    {
        for (size_t i = 0; i < len; ++i) {
            if (mask[i] != '?' && a[i] != b[i]) {
                return false;
            }
        }
        return true;
    }

    const uint8_t*
    find(const uint8_t* hay, size_t hay_len, const uint8_t* sig, const char* mask, size_t sig_len)
    {
        if (!hay || !sig || !mask || sig_len == 0 || hay_len < sig_len) {
            return nullptr;
        }

        for (size_t i = 0; i + sig_len <= hay_len; ++i) {
            const uint8_t* p = hay + i;
            if (match_at(p, sig, mask, sig_len)) {
                return p;
            }
        }
        return nullptr;
    }

    const uint8_t*
    scan_exec(HMODULE module, const char* pattern)
    {
        uint8_t bytes[1024];
        char mask[1024];

        size_t len = parse_pattern(pattern, bytes, mask, sizeof(bytes));
        if (len == 0) {
            return nullptr;
        }

        Span spans[32];
        int n_spans = exec_spans(module, spans, 32);
        if (n_spans <= 0) {
            return nullptr;
        }

        for (int i = 0; i < n_spans; ++i) {
            const Span&    s = spans[i];
            const uint8_t* m = find(s.base, s.size, bytes, mask, len);
            if (m) {
                return m;
            }
        }
        return nullptr;
    }
}

static inline std::wstring
widen_ascii(const std::string& s)
{
    return std::wstring(s.begin(), s.end());
}

static inline fs::path
loader_root()
{
    return (fs::current_path() / "Mods" / kModName).lexically_normal();
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

static inline std::vector<fs::path>
list_files_ext_sorted(const fs::path& dir, std::wstring_view ext)
{
    std::vector<fs::path> out;
    std::error_code ec;

    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
        return out;
    }

    for (auto& it : fs::directory_iterator(dir, ec)) {
        if (ec) {
            break;
        }

        if (!it.is_regular_file(ec)) {
            continue;
        }

        auto p = it.path();
        if (p.extension() == ext) {
            out.push_back(p.lexically_normal());
        }
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

static inline std::wstring
to_game_relative_path(const fs::path& absolute_path)
{
    fs::path working_dir = fs::current_path();

    std::error_code ec;
    fs::path rel_path = fs::relative(absolute_path, working_dir, ec);

    if (ec || rel_path.empty()) {
        LOG_WARN(STR("Could not compute relative path for: {}\n"), absolute_path.wstring());
        return absolute_path;
    }

    std::wstring result = rel_path.wstring();
    std::replace(result.begin(), result.end(), L'\\', L'/');

    return result;
}

static inline std::vector<fs::path>
discover_mod_dirs()
{
    std::vector<fs::path> dirs;
    std::error_code ec;

    fs::path root = loader_root();

    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        return dirs;
    }

    for (auto& it : fs::directory_iterator(root, ec)) {
        if (ec) {
            break;
        }

        if (!it.is_directory(ec)) {
            continue;
        }

        fs::path     p     = it.path().lexically_normal();
        std::wstring fname = p.filename().wstring();

        if (_wcsicmp(fname.c_str(), L"dlls")     == 0 ||
            _wcsicmp(fname.c_str(), L"scripts")  == 0 ||
            _wcsicmp(fname.c_str(), L"disabled") == 0) {
            continue;
        }

        dirs.push_back(p);
    }

    std::sort(dirs.begin(), dirs.end());
    return dirs;
}

static inline void*
resolve_rel32_call_target(const uint8_t* call_insn /*E8*/)
{
    int32_t rel = *reinterpret_cast<const int32_t*>(call_insn + 1);
    return (void*)(call_insn + 5 + rel);
}

static inline void*
resolve_rip_rel32_mov_target(const uint8_t* mov_inst /*48*/)
{
    int32_t rel = *reinterpret_cast<const int32_t*>(mov_inst + 3);
	return (void*)(mov_inst + 7 + rel);
}

namespace POD
{
    enum class EIoErrorCode {
        Ok,
        Unknown,
        InvalidCode,
        Cancelled,
        FileOpenFailed,
        FileNotOpen,
        ReadError,
        WriteError,
        NotFound,
        CorruptToc,
        UnknownChunkID,
        InvalidParameter,
        SignatureError,
        InvalidEncryptionKey,
    };

    struct FIoStatus {
        EIoErrorCode ErrorCode;
        wchar_t      ErrorMessage[128];
    };

    template <typename T>
    struct TArray
    {
        T* Data;
        int32_t Num;
        int32_t Max;
    };

    struct FString
    {
        TArray<TCHAR> Data{ nullptr, 0, 0 };

        FString() = default;
        FString(const wchar_t* src, int32_t len, int32_t slack = 256)
        {
            const int32_t capacity = len + 1 + slack;

            TCHAR* buffer = static_cast<TCHAR*>(std::malloc(sizeof(TCHAR) * capacity));
            if (!buffer) {
                Data = {};
                return;
            }

            std::memcpy(buffer, src, sizeof(TCHAR) * len);
            buffer[len] = L'\0';

            Data.Data = buffer;
            Data.Num = len + 1;
            Data.Max = capacity;
        }

        ~FString()
        {
            if (Data.Data) {
                std::free(Data.Data);
                Data = {};
            }
        }

        // disable copy
        FString(const FString&) = delete;
        FString& operator=(const FString&) = delete;

        // allow move
        FString(FString&& other) noexcept
        {
            Data = other.Data;
            other.Data = {};
        }

        FString& operator=(FString&& other) noexcept
        {
            if (this != &other) {
                if (Data.Data) {
                    std::free(Data.Data);
                }
                Data = other.Data;
                other.Data = {};
            }
            return *this;
        }
    };

    struct FIoEnvironment {
        FString Path;
        int32_t   Order = 0;

        FIoEnvironment(const std::wstring& path, int order)
            : Path(path.c_str(), static_cast<int32_t>(path.size()), 512), Order(order)
        {
        }
    };

    struct FGuid {
        uint32_t A;
        uint32_t B;
        uint32_t C;
        uint32_t D;
    };

    struct FAES {
        uint8_t Key[32];
    };
};

using FIoDispatcherMountFunc               = POD::FIoStatus* (__fastcall*)(void* self, POD::FIoStatus* status, POD::FIoEnvironment* env, POD::FGuid* guid, POD::FAES* key);
using FPakPlatformFileMountFunc            = bool (__fastcall*)(void* self, const wchar_t* pak_filename, int pak_order, const wchar_t* path, bool load_index);
using FPakPlatformFileMountAllPakFilesFunc = int (__fastcall*)(void* self, TArray<FString>* pak_folders, FString* wildcard);
using StaticLoadClassFunc                  = UClass* (__fastcall*)(UClass*, UObject*, const wchar_t*, const wchar_t*, uint32_t);

static FIoDispatcherMountFunc               g_real_io_mount  = nullptr;
static FPakPlatformFileMountFunc            g_real_pak_mount = nullptr;
static FPakPlatformFileMountAllPakFilesFunc g_real_mount_all = nullptr;

static StaticLoadClassFunc static_load_class = nullptr;

static std::vector<std::wstring> g_pending_mod_actor_classes;

static void* g_io_dispatcher     = nullptr;
static void* g_pak_platform_file = nullptr;

static bool g_mount_hook_installed = false;
static bool g_user_mounted_once    = false;
static bool g_spawn_hook_installed = false;

static POD::FIoStatus* __fastcall
io_mount_hook(void* self, POD::FIoStatus* status, POD::FIoEnvironment* env, POD::FGuid* guid, POD::FAES* key);
static bool __fastcall
pak_mount_hook(void* self, const wchar_t* pak_filename, int pak_order, const wchar_t* path, bool load_index);
static int __fastcall
mount_all_hook(void* self, TArray<FString>* pak_folders, FString* wildcard);

static bool
patch_memory(void* address, const void* data, size_t size)
{
    if (!address || !data || size == 0) {
        return false;
    }

    DWORD oldp = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &oldp)) {
        return false;
    }

    std::memcpy(address, data, size);
    FlushInstructionCache(GetCurrentProcess(), address, size);

    DWORD tmp = 0;
    VirtualProtect(address, size, oldp, &tmp);
    return true;
}

static bool
patch_get_pak_signkey_helper(void)
{
    HMODULE exe = GetModuleHandleW(nullptr);
    const uint8_t* match = sigscan::scan_exec(exe, "48 83 EC ? E8 ? ? ? ? 83 78 ? 00");
    if (!match) {
        LOG_ERROR(STR("GetPakSigningKeysHelper not found\n"));
        return false;
    }

    uint8_t patch[] = {
        0x31, 0xC0, // xor eax, eax
        0xC3,       // ret
    };

    if (!patch_memory((void*)match, patch, sizeof(patch))) {
        LOG_ERROR(STR("Failed to patch GetPakSigningKeysHelper\n"));
        return false;
    }

    LOG_INFO(STR("Patched GetPakSigningKeysHelper\n"));
    return true;
}

static void
mount_one_pak(const fs::path& pak_path, int order)
{
    if (!g_pak_platform_file || !g_real_pak_mount) {
        LOG_WARN(STR("Pak mount unavailable (self={:p}, fn={:p})\n"), g_pak_platform_file, (void*)g_real_pak_mount);
        return;
    }

    std::wstring game_rel_path = to_game_relative_path(pak_path);

    pak_mount_hook(
        g_pak_platform_file,
        game_rel_path.c_str(),
        order,
        nullptr,
        true
    );
}

static void
mount_one_utoc_ucas(const fs::path& path, int order)
{
    if (!g_io_dispatcher || !g_real_io_mount) {
        LOG_WARN(STR("IoStore mount unavailable (self={:p}, fn={:p})\n"), g_io_dispatcher, (void*)g_real_io_mount);
        return;
    }

    fs::path base = path;
    base.replace_extension(L"");

    if (!file_exists(base_to_ext(base, L".utoc")) || !has_ucas_any(base)) {
        LOG_WARN(STR("Missing IoStore pair for base: {}\n"), base.wstring());
        return;
    }

    std::wstring game_rel_path = to_game_relative_path(base);

    POD::FIoEnvironment env(game_rel_path, order);
    POD::FIoStatus      status{};
    POD::FGuid          guid{};
    POD::FAES           key{};

    io_mount_hook(g_io_dispatcher, &status, &env, &guid, &key);
}

static void
queue_mod_actor_spawn(const std::wstring& mod_name)
{
    // /Game/Mods/<ExampleMod>/ModActor.ModActor_C
    std::wstring path = L"/Game/Mods/" + mod_name + L"/ModActor.ModActor_C";
    g_pending_mod_actor_classes.push_back(std::move(path));
}

static void
mount_mod_folder_only_pak(const fs::path& mod_dir, int order_base)
{
    // preferred: mount all .pak files found in the mod folder
    auto paks = list_files_ext_sorted(mod_dir, L".pak");
    if (!paks.empty()) {
        for (size_t i = 0; i < paks.size(); ++i) {
            mount_one_pak(paks[i], order_base + (int)i);
            
            auto base_name = paks[i].filename().replace_extension("").wstring();
			queue_mod_actor_spawn(base_name);
        }
        return;
    }

    // if user supplied only IoStore (.utoc/.ucas) but no .pak
    // we cannot mount it without calling IoDispatcher.
    auto utocs = list_files_ext_sorted(mod_dir, L".utoc");
    if (!utocs.empty()) {
        for (size_t i = 0; i < utocs.size(); ++i) {
			mount_one_utoc_ucas(utocs[i], order_base + (int)i);

            auto base_name = utocs[i].filename().replace_extension("").wstring();
            queue_mod_actor_spawn(base_name);
        }
        return;
    }

    LOG_WARN(STR("No .pak or .utoc/.ucas found in {}\n"), mod_dir.wstring());
}

static void
mount_all_user_mods_once(void)
{
    auto mods = discover_mod_dirs();
    if (mods.empty()) {
        LOG_INFO(STR("No user mods found under {}\n"), loader_root().wstring());
        return;
    }

    LOG_INFO(STR("Found {} user mod folder(s)\n"), (int)mods.size());

    for (size_t mod_index = 0; mod_index < mods.size(); ++mod_index) {
        const fs::path& dir   = mods[mod_index];
        int             order = kBaseOrder + (int)(mod_index);

        LOG_INFO(STR("Mounting mod folder: {} (order {})\n"), dir.filename().wstring(), order);
        mount_mod_folder_only_pak(dir, order);
    }
}

static bool
install_io_mount_hook(void)
{
    const uint8_t* target = sigscan::scan_exec(GetModuleHandleW(nullptr), "40 53 41 55 41 57 48 81 EC ? ? ? ? 48 8B 05");
    if (!target) {
        LOG_ERROR(STR("FIoDispatcherImpl::Mount call not found\n"));
        return false;
    }

    MH_STATUS s = MH_CreateHook((LPVOID)target, (LPVOID)io_mount_hook, (LPVOID*)&g_real_io_mount);
    if (s != MH_OK) {
        LOG_ERROR(STR("Failed to create FIoDispatcherImpl::Mount hook: {}\n"), widen_ascii(MH_StatusToString(s)));
        return false;
    }

    s = MH_EnableHook((LPVOID)target);
    if (s != MH_OK) {
        LOG_ERROR(STR("Failed to enable FIoDispatcherImpl::Mount hook: {}\n"), widen_ascii(MH_StatusToString(s)));
        return false;
    }

    LOG_INFO(STR("Installed FIoDispatcherImpl::Mount hook at {:p}\n"), (LPVOID)target);
    return true;
}

static bool
install_pak_mount_hook(void)
{
    const uint8_t* callsite = sigscan::scan_exec(GetModuleHandleW(nullptr), "E8 ? ? ? ? 84 C0 74 ? 41 FF C5 FF C6");
    if (!callsite) {
        LOG_ERROR(STR("FPakPlatformFile::Mount call not found\n"));
        return false;
    }

    void* target = resolve_rel32_call_target(callsite);

    MH_STATUS s = MH_CreateHook(target, (LPVOID)pak_mount_hook, (LPVOID*)&g_real_pak_mount);
    if (s != MH_OK) {
        LOG_ERROR(STR("Failed to create FPakPlatformFile::Mount hook: {}\n"), widen_ascii(MH_StatusToString(s)));
        return false;
    }

    s = MH_EnableHook(target);
    if (s != MH_OK) {
        LOG_ERROR(STR("Failed to enable FPakPlatformFile::Mount hook: {}\n"), widen_ascii(MH_StatusToString(s)));
        return false;
    }

    LOG_INFO(STR("Installed FPakPlatformFile::Mount hook at {:p}\n"), target);
    return true;
}

static bool
install_mount_all_hook(void)
{
    const uint8_t* target = sigscan::scan_exec(
        GetModuleHandleW(nullptr),
        "48 89 5C 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 45 ? 33 FF 48 89 4D"
    );

    if (!target) {
        LOG_ERROR(STR("FPakPlatformFile::MountAllPakFiles not found\n"));
        return false;
    }

    MH_STATUS s = MH_CreateHook((LPVOID)target, (LPVOID)mount_all_hook, (LPVOID*)&g_real_mount_all);
    if (s != MH_OK) {
        LOG_ERROR(STR("Failed to create MountAllPakFiles hook: {}\n"), widen_ascii(MH_StatusToString(s)));
        return false;
    }

    s = MH_EnableHook((LPVOID)target);
    if (s != MH_OK) {
        LOG_ERROR(STR("Failed to enable MountAllPakFiles hook: {}\n"), widen_ascii(MH_StatusToString(s)));
        return false;
    }

    LOG_INFO(STR("Installed FPakPlatformFile::MountAllPakFiles hook at {:p}\n"), (void*)target);
    return true;
}

static bool
resolve_static_load_class(void)
{
    const uint8_t* target = sigscan::scan_exec(
        GetModuleHandleW(nullptr),
        "40 55 53 57 41 56 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 8B BD"
    );

    if (!target) {
        LOG_ERROR(STR("StaticLoadClass not found\n"));
        return false;
    }

	static_load_class = reinterpret_cast<StaticLoadClassFunc>(target);

    LOG_INFO(STR("Found StaticLoadClass at {:p}\n"), (void*)target);
    return true;
}

POD::FIoStatus* __fastcall
io_mount_hook(void* self, POD::FIoStatus* status, POD::FIoEnvironment* env, POD::FGuid* guid, POD::FAES* key)
{
    if (!g_io_dispatcher) {
        g_io_dispatcher = self;
    }

    POD::FIoStatus* ret = g_real_io_mount(self, status, env, guid, key);

    LOG_INFO(STR("Path:   {}\n"), env->Path.Data.Data);
    LOG_INFO(STR("Order:  {}\n"), env->Order);
    LOG_INFO(STR("GUID:   {}.{}.{}.{}\n"), guid->A, guid->B, guid->C, guid->D);
    LOG_INFO(STR("Status: {}\n"), ret->ErrorMessage);

    return ret;
}

bool __fastcall
pak_mount_hook(void* self, const wchar_t* pak_filename, int pak_order, const wchar_t* path, bool load_index)
{
    LOG_INFO(STR("Pak Filename: {}\n"), pak_filename ? pak_filename : L"<null>");
    LOG_INFO(STR("Pak Order:    {}\n"), pak_order);
    LOG_INFO(STR("Path:         {}\n"), path ? path : L"<null>");
    LOG_INFO(STR("Load Index:   {}\n"), load_index);

    if (!self || !g_real_pak_mount) {
        LOG_WARN(STR("FPakPlatformFile::Mount hook: self={:p}, original={:p}\n"), self, (void*)g_real_pak_mount);
        return false;
    }

    bool ok = g_real_pak_mount(self, pak_filename, pak_order, path, load_index);
    LOG_INFO(STR("OK:           {}\n"), ok);
    return ok;
}


int __fastcall
mount_all_hook(void* self, TArray<FString>* pak_folders, FString* wildcard)
{
    if (!g_pak_platform_file) {
        g_pak_platform_file = self;
    }

    int result = g_real_mount_all(self, pak_folders, wildcard);

    if (!g_user_mounted_once) {
        g_user_mounted_once = true;
        mount_all_user_mods_once();
    }

    return result;
}

static UClass*
load_bp_actor_class(const std::wstring& class_path)
{
    if (!static_load_class) {
        return nullptr;
    }

    {
        std::wstring wrapped = L"BlueprintGeneratedClass'" + class_path + L"'";
        UClass* c = static_load_class(UObject::StaticClass(), nullptr, wrapped.c_str(), nullptr, 0);
        if (c) {
            return c;
        }
    }
    {
        std::wstring wrapped = L"Class'" + class_path + L"'";
        UClass* c = static_load_class(UObject::StaticClass(), nullptr, wrapped.c_str(), nullptr, 0);
        if (c) {
            return c;
        }
    }
    {
        UClass* c = static_load_class(UObject::StaticClass(), nullptr, class_path.c_str(), nullptr, 0);
        if (c) {
            return c;
        }
    }

    return nullptr;
}

static bool
try_spawn_mod_actor(UWorld* world, const std::wstring& bp_class_path)
{
    if (!world) {
        return false;
    }

    UClass* cls = load_bp_actor_class(bp_class_path);
    if (!cls) {
        return false;
    }

    FVector  loc{ 0.f, 0.f, 0.f };
    FRotator rot{ 0.f, 0.f, 0.f };

    AActor* spawned = world->SpawnActor(cls, &loc, &rot);
	return (spawned != nullptr);
}

static inline std::wstring
to_lower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
        return (wchar_t)towlower(c);
    });
    return s;
}

static inline bool
ends_with_icase(const std::wstring& s, const std::wstring& suffix)
{
    if (s.size() < suffix.size()) {
        return false;
    }

    auto ls = to_lower(s);
    auto lf = to_lower(suffix);
    return ls.compare(ls.size() - lf.size(), lf.size(), lf) == 0;
}

static inline bool
starts_with_icase(const std::wstring& s, const std::wstring& prefix)
{
    if (s.size() < prefix.size()) {
        return false;
    }

    auto ls = to_lower(s);
    auto lp = to_lower(prefix);
    return ls.compare(0, lp.size(), lp) == 0;
}

static inline bool
contains_icase(const std::wstring& s, const std::wstring& needle)
{
    auto ls = to_lower(s);
    auto ln = to_lower(needle);
    return ls.find(ln) != std::wstring::npos;
}

static void
spawn_on_pc_beginplay(UObject* ctx, UFunction* func, void*)
{
    if (!ctx || !func) {
        return;
    }

    const std::wstring obj_full  = ctx->GetFullName();   // e.g. "HbkPlayerControllerBP_C /Game/..."
    const std::wstring func_full = func->GetFullName();  // e.g. "Function ...ReceiveBeginPlay"

    if (!contains_icase(obj_full, L"HbkPlayerControllerBP_C")) {
        return;
    }

    if (!ends_with_icase(func_full, L"ReceiveBeginPlay")) {
        return;
    }

    UWorld* world = reinterpret_cast<AActor*>(ctx)->GetWorld();
    if (!world) {
        LOG_WARN(STR("PC BeginPlay matched but world was null; giving up (no retry)\n"));
        return;
    }

    for (const auto& class_path : g_pending_mod_actor_classes) {
        if (try_spawn_mod_actor(world, class_path)) {
            LOG_INFO(STR("Spawned: {}\n"), class_path);
        }
    }
}

class IOStoreLoaderMod : public RC::CppUserModBase
{
public:
    IOStoreLoaderMod()
    {
        ModName        = kModName;
        ModVersion     = STR("0.3.0");
        ModDescription = STR("Loads Pak/IoStore mods (.pak/.utoc/.ucas)");
        ModAuthors     = STR("akmubi");

        MH_STATUS s = MH_Initialize();
        if (s != MH_OK) {
            LOG_ERROR(STR("MinHook init failed: {}\n"), widen_ascii(MH_StatusToString(s)));
            return;
        }

        if (patch_get_pak_signkey_helper() &&
            install_mount_all_hook() &&
            install_pak_mount_hook() &&
            install_io_mount_hook()) {
            LOG_NOTICE(STR("Initialized!\n"));
        }
    }

    ~IOStoreLoaderMod() override
    {
        MH_Uninitialize();
    }

    auto on_unreal_init() -> void override
    {
        resolve_static_load_class();
        if (!g_spawn_hook_installed) {
            g_spawn_hook_installed = true;
            Hook::RegisterProcessEventPreCallback(spawn_on_pc_beginplay);
            LOG_INFO(STR("Installed ProcessEvent PC BeginPlay listener\n"));
        }
    }
};

#define MOD_API __declspec(dllexport)
extern "C"
{
    MOD_API RC::CppUserModBase* start_mod()
    {
        return new IOStoreLoaderMod();
    }

    MOD_API void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}
