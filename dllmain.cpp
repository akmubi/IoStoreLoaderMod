#include <polyhook2/Detour/x64Detour.hpp>

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <UE4SSProgram.hpp>

#include "unreal_types.hpp"
#include "sigscan.hpp"
#include "parser.hpp"

#define LOG_NOTICE(...) Output::send<LogLevel::Verbose>(__VA_ARGS__)
#define LOG_INFO(...)   Output::send<LogLevel::Normal>(__VA_ARGS__)
#define LOG_WARN(...)   Output::send<LogLevel::Warning>(__VA_ARGS__)
#define LOG_ERROR(...)  Output::send<LogLevel::Error>(__VA_ARGS__)

using namespace RC;
using namespace RC::Unreal;

static const std::wstring mod_name   = STR("IoStoreLoaderMod");
static const int          base_order = 200;

using FIoDispatcherMountFunc               = FIoStatus * (__fastcall*)(FIoDispatcherImpl* self, FIoStatus* status, FIoEnvironment* env, FGuid* guid, FAES* key);
using FPakPlatformFileMountFunc            = bool(__fastcall*)(void *self, const wchar_t *pak_filename, int pak_order, const wchar_t *path, bool load_index);
using FPakPlatformFileMountAllPakFilesFunc = int(__fastcall*)(void* self, TArray<FString>* pak_folders, FString* wildcard);

static FIoDispatcherMountFunc               g_real_io_dispatcher_mount                   = nullptr;
static FPakPlatformFileMountFunc            g_real_pak_platform_file_mount               = nullptr;
static FPakPlatformFileMountAllPakFilesFunc g_real_pak_platform_file_mount_all_pak_files = nullptr;

static FIoDispatcherImpl* g_io_dispatcher     = nullptr;
static void             * g_pak_platform_file = nullptr;

static bool all_paks_mounted = false;

static std::unique_ptr<PLH::x64Detour> g_io_dispatcher_mount_detour;
static std::unique_ptr<PLH::x64Detour> g_pak_platform_file_mount_detour;
static std::unique_ptr<PLH::x64Detour> g_pak_platform_file_mount_all_pak_files_detour;

static const wchar_t *
fstring_to_wchar(const FString* s)
{
    if (!s) {
        return L"<null>";
    }
    return s->GetCharArray();
}

static inline std::wstring
widen_ascii(const std::string& s)
{
    return std::wstring(s.begin(), s.end());
}

static bool
mount_container_best(const std::string& base_no_ext_utf8, int order)
{
    fs::path base = fs::path(base_no_ext_utf8).lexically_normal();

    // prefer .pak mount
    fs::path pak = base_to_ext(base, L".pak");
    if (file_exists(pak)) {
        if (!g_pak_platform_file || !g_real_pak_platform_file_mount) {
            LOG_WARN(STR("[{}] Pak mount unavailable (self={:p}, fn={:p})\n"),
                mod_name, g_pak_platform_file, (void*)g_real_pak_platform_file_mount);
            return false;
        }

        std::wstring pak_w = pak.wstring();
        bool ok = g_real_pak_platform_file_mount(
            g_pak_platform_file,
            pak_w.c_str(),
            order,
            nullptr,
            true
        );
        return ok;
    }

    // fallback: direct FIoDispatcher::Mount
    fs::path utoc = base_to_ext(base, L".utoc");
    if (!file_exists(utoc) || !has_ucas_any(base)) {
        LOG_WARN(STR("[{}] Missing IoStore pair for base: {}\n"), mod_name, widen_ascii(base_no_ext_utf8));
        return false;
    }

    if (!g_io_dispatcher || !g_real_io_dispatcher_mount) {
        LOG_WARN(STR("[{}] Io mount unavailable (dispatcher={:p}, fn={:p})\n"),
            mod_name, (void*)g_io_dispatcher, (void*)g_real_io_dispatcher_mount);
        return false;
    }

    const std::string base_generic = base.generic_string();
    FIoEnvironment env(base_generic, order);
    FIoStatus      status{};
    FGuid          guid{};
    FAES           key{};

    FIoStatus* s = g_real_io_dispatcher_mount(g_io_dispatcher, &status, &env, &guid, &key);
    return (s && s->ErrorCode == EIoErrorCode::Ok);
}

static FIoStatus* __fastcall
io_dispatcher_mount_hook(FIoDispatcherImpl* self, FIoStatus* status, FIoEnvironment* env, FGuid* guid, FAES* key)
{
    if (!g_io_dispatcher) {
        g_io_dispatcher = self;
    }

    FIoStatus *ret = g_real_io_dispatcher_mount(self, status, env, guid, key);

    LOG_NOTICE(STR("[{}] Path:   {}\n"),          mod_name, fstring_to_wchar(&env->Path));
    LOG_NOTICE(STR("[{}] Order:  {}\n"),          mod_name, env->Order);
    LOG_NOTICE(STR("[{}] GUID:   {}.{}.{}.{}\n"), mod_name, guid->A, guid->B, guid->C, guid->D);
    LOG_NOTICE(STR("[{}] Status: {}\n"),          mod_name, ret->ErrorMessage);

    return ret;
}

static bool __fastcall
pak_platform_file_mount_hook(void* self, const wchar_t* pak_filename, int pak_order, const wchar_t* path, bool load_index)
{
    LOG_NOTICE(STR("[{}] Pak Filename: {}\n"), mod_name, pak_filename);
    LOG_NOTICE(STR("[{}] Pak Order:    {}\n"), mod_name, pak_order);
    LOG_NOTICE(STR("[{}] Path:         {}\n"), mod_name, (path) ? path : L"<null>");
    LOG_NOTICE(STR("[{}] Load Index:   {}\n"), mod_name, load_index);

    if (!self || !g_real_pak_platform_file_mount) {
        LOG_WARN(STR("[{}] FPakPlatformFile::Mount hook: self: {:p}, FPakPlatformFile::Mount original: {:p}\n"), self, (void*)g_real_pak_platform_file_mount);
        return false;
    }

    bool ok = g_real_pak_platform_file_mount(self, pak_filename, pak_order, path, load_index);
    LOG_NOTICE(STR("[{}] OK:           {}\n"), mod_name, ok);
    return ok;
}

static uint64_t
resolve_rel32_call_target(const uint8_t* call_insn /* points at 0xE8 */)
{
    int32_t rel = *reinterpret_cast<const int32_t*>(call_insn + 1);
    return reinterpret_cast<uint64_t>(call_insn + 5 + rel);
}

static int __fastcall
pak_platform_file_mount_all_pak_files_hook(void* self, TArray<FString>* pak_folders, FString* wildcard)
{
    if (!g_pak_platform_file) {
        g_pak_platform_file = self;
    }

    if (!g_pak_platform_file_mount_detour) {
        HMODULE exe = GetModuleHandleW(nullptr);
        const uint8_t* match = sigscan::scan_exec(exe, "E8 ? ? ? ? 84 C0 74 ? 41 FF C5 FF C6");
        if (!match) {
            LOG_ERROR(STR("[{}] FPakPlatformFile::Mount call not found\n"), mod_name);
        }
        else {
            uint64_t addr = resolve_rel32_call_target(match);
            uint64_t hook_addr = reinterpret_cast<uint64_t>(&pak_platform_file_mount_hook);

            g_pak_platform_file_mount_detour = std::make_unique<PLH::x64Detour>(addr, hook_addr, (uint64_t*)&g_real_pak_platform_file_mount);
            if (!g_pak_platform_file_mount_detour->hook()) {
                LOG_ERROR(STR("[{}] Failed to install FPakPlatformFile::Mount call hook at {:p}\n"), mod_name, (void*)addr);
            }
            else {
                LOG_INFO(STR("[{}] Installed FPakPlatformFile::Mount call hook at {:p}\n"), mod_name, (void*)addr);
            }
        }
    }

    int result = g_real_pak_platform_file_mount_all_pak_files(self, pak_folders, wildcard);
    if (!all_paks_mounted) {
        all_paks_mounted = true;

        fs::path mod_root = fs::current_path() / "Mods" / mod_name;
        fs::path load_order_filepath = (mod_root / "load_order.txt").lexically_normal();

        auto entries = parse_load_order(load_order_filepath);
        if (entries.empty()) {
            LOG_WARN(STR("[{}]: no {}\n"), mod_name, load_order_filepath.wstring());
        } else {
            for (size_t i = 0; i < entries.size(); ++i) {
                auto& e = entries[i];

                auto bases = generate_mount_bases(mod_root / e.rel_path);
                if (bases.empty()) {
                    LOG_WARN(STR("[{}] {}/{}: {}: no .utoc/.ucas pairs found\n"), mod_name, i + 1, entries.size(), widen_ascii(e.rel_path));
                    continue;
                }

                for (const auto& base : bases) {
                    mount_container_best(base, base_order + e.priority);
                }
            }
        }
    }

    return result;
}

static bool
install_dispatcher_mount_hook(void)
{
    HMODULE exe = GetModuleHandleW(nullptr);

    const uint8_t* match = sigscan::scan_exec(exe, "40 53 41 55 41 57 48 81 EC ? ? ? ? 48 8B 05");
    if (!match) {
        LOG_ERROR(STR("[{}] FIoDispatcherImpl::Mount not found\n"), mod_name);
        return false;
    }

    uint64_t addr      = reinterpret_cast<uint64_t>(match);
    uint64_t hook_addr = reinterpret_cast<uint64_t>(&io_dispatcher_mount_hook);

    g_io_dispatcher_mount_detour = std::make_unique<PLH::x64Detour>(addr, hook_addr, (uint64_t*)&g_real_io_dispatcher_mount);
    if (!g_io_dispatcher_mount_detour->hook()) {
        LOG_ERROR(STR("[{}] Failed to install FIoDispatcherImpl::Mount hook at {:p}\n"), mod_name, (void*)addr);
        return false;
    }

    LOG_INFO(STR("[{}] Installed FIoDispatcherImpl::Mount hook at {:p}\n"), mod_name, (void*)addr);
    return true;
}

static bool
install_pak_platform_file_mount_all_pak_files_hook(void)
{
    HMODULE exe = GetModuleHandleW(nullptr);
    const uint8_t* match = sigscan::scan_exec(exe, "48 89 5C 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 45 ? 33 FF 48 89 4D");
    if (!match) {
        LOG_ERROR(STR("[{}] FPakPlatformFile::MountAllPakFiles not found\n"), mod_name);
        return false;
    }

    uint64_t addr = reinterpret_cast<uint64_t>(match);
    uint64_t hook_addr = reinterpret_cast<uint64_t>(&pak_platform_file_mount_all_pak_files_hook);

    g_pak_platform_file_mount_all_pak_files_detour = std::make_unique<PLH::x64Detour>(addr, hook_addr, (uint64_t*)&g_real_pak_platform_file_mount_all_pak_files);
    if (!g_pak_platform_file_mount_all_pak_files_detour->hook()) {
        LOG_ERROR(STR("[{}] Failed to install FPakPlatformFile::MountAllPakFiles hook at {:p}\n"), mod_name, (void*)addr);
        return false;
    }

    LOG_INFO(STR("[{}] Installed FPakPlatformFile::MountAllPakFiles hook at {:p}\n"), mod_name, (void*)addr);
    return true;
}

static bool
patch_memory(void* address, const void* data, size_t size)
{
    if (!address || !data || size == 0) {
        return false;
    }

    DWORD old_protect = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old_protect)) {
        return false;
    }

    std::memcpy(address, data, size);

    FlushInstructionCache(GetCurrentProcess(), address, size);

    DWORD tmp = 0;
    VirtualProtect(address, size, old_protect, &tmp);
    return true;
}

static bool
patch_get_pak_signkey_helper(void)
{
    HMODULE exe = GetModuleHandleW(nullptr);

    const uint8_t* match = sigscan::scan_exec(exe, "48 83 EC ? E8 ? ? ? ? 83 78 ? 00");
    if (!match) {
        LOG_ERROR(STR("[{}] GetPakSigningKeysHelper not found\n"), mod_name);
        return false;
    }

    // early return with FALSE i.e. IsSigningEnabled() ==> FALSE
    uint8_t patch[] = {
      0x31,
      0xC0, // xor eax, eax
      0xC3, // ret
    };

    if (!patch_memory((void*)match, patch, sizeof(patch))) {
        LOG_ERROR(STR("[{}] Failed to patch GetPakSigningKeysHelper\n"), mod_name);
        return false;
    }

    LOG_INFO(STR("[{}] Successfully patched GetPakSigningKeysHelper\n"), mod_name);
    return true;
}

class IOStoreLoaderMod : public RC::CppUserModBase
{
public:
    IOStoreLoaderMod() : CppUserModBase()
    {
        ModName        = mod_name;
        ModVersion     = STR("1.0");
        ModDescription = STR("Loads IoStore assets (.utoc/.ucas)");
        ModAuthors     = STR("akmubi");

        if (patch_get_pak_signkey_helper() &&
            install_dispatcher_mount_hook() &&
            install_pak_platform_file_mount_all_pak_files_hook()) {
            LOG_NOTICE(STR("[{}] Initialized!\n"), mod_name);
        }
    }
};

#define MOD_API __declspec(dllexport)
extern "C"
{
    MOD_API RC::CppUserModBase*
    start_mod()
    {
        return new IOStoreLoaderMod();
    }

    MOD_API void
    uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}
