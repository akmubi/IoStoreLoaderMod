#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <UE4SSProgram.hpp>

#include <MinHook.h>

#include "unreal_types.hpp"
#include "sigscan.hpp"
#include "parser.hpp"

#define LOG_NOTICE(...) Output::send<LogLevel::Verbose>(STR("[IoStoreLoaderMod] ") __VA_ARGS__)
#define LOG_INFO(...)   Output::send<LogLevel::Normal>(STR("[IoStoreLoaderMod] ") __VA_ARGS__)
#define LOG_WARN(...)   Output::send<LogLevel::Warning>(STR("[IoStoreLoaderMod] ") __VA_ARGS__)
#define LOG_ERROR(...)  Output::send<LogLevel::Error>(STR("[IoStoreLoaderMod] ") __VA_ARGS__)

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

static bool all_paks_mounted               = false;
static bool pak_platform_file_mount_hooked = false;

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

static inline LPVOID
resolve_rel32_call_target(uint8_t* call_insn /* points at 0xE8 */)
{
    int32_t rel = *reinterpret_cast<const int32_t*>(call_insn + 1);
    return reinterpret_cast<LPVOID>(call_insn + 5 + rel);
}

static bool
patch_memory(void* address, const void* data, size_t size);
static bool
patch_get_pak_signkey_helper(void);

static bool
install_pak_platform_file_hook(void);
static bool
install_dispatcher_mount_hook(void);
static bool
install_pak_platform_file_mount_all_pak_files_hook(void);

static FIoStatus* __fastcall
io_dispatcher_mount_hook(FIoDispatcherImpl* self, FIoStatus* status, FIoEnvironment* env, FGuid* guid, FAES* key);
static bool __fastcall
pak_platform_file_mount_hook(void* self, const wchar_t* pak_filename, int pak_order, const wchar_t* path, bool load_index);
static int __fastcall
pak_platform_file_mount_all_pak_files_hook(void* self, TArray<FString>* pak_folders, FString* wildcard);

static bool
mount_container_best(const std::string& base_no_ext_utf8, int order);

class IOStoreLoaderMod : public RC::CppUserModBase
{
public:
    IOStoreLoaderMod() : CppUserModBase()
    {
        ModName        = mod_name;
        ModVersion     = STR("1.1");
        ModDescription = STR("Loads Pak/IoStore mods (.pak/.utoc/.ucas)");
        ModAuthors     = STR("akmubi");

        MH_STATUS s = MH_Initialize();
        if (s != MH_OK) {
            LOG_ERROR(STR("Failed to initialize MinHook: {}\n"), widen_ascii(MH_StatusToString(s)));
            return;
        }

        if (patch_get_pak_signkey_helper() &&
            install_dispatcher_mount_hook() &&
            install_pak_platform_file_mount_all_pak_files_hook()) {
            LOG_NOTICE(STR("Initialized!\n"));
        }
    }

    ~IOStoreLoaderMod() override
    {
        MH_Uninitialize();
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

bool
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

bool
patch_get_pak_signkey_helper(void)
{
    HMODULE exe = GetModuleHandleW(nullptr);

    const uint8_t* match = sigscan::scan_exec(exe, "48 83 EC ? E8 ? ? ? ? 83 78 ? 00");
    if (!match) {
        LOG_ERROR(STR("GetPakSigningKeysHelper not found\n"));
        return false;
    }

    // early return with FALSE i.e. IsSigningEnabled() ==> FALSE
    uint8_t patch[] = {
      0x31,
      0xC0, // xor eax, eax
      0xC3, // ret
    };

    if (!patch_memory((void*)match, patch, sizeof(patch))) {
        LOG_ERROR(STR("Failed to patch GetPakSigningKeysHelper\n"));
        return false;
    }

    LOG_INFO(STR("Successfully patched GetPakSigningKeysHelper\n"));
    return true;
}

bool
install_dispatcher_mount_hook(void)
{
    LPVOID target = (LPVOID)sigscan::scan_exec(GetModuleHandleW(nullptr), "40 53 41 55 41 57 48 81 EC ? ? ? ? 48 8B 05");
    if (!target) {
        LOG_ERROR(STR("FIoDispatcherImpl::Mount not found\n"));
        return false;
    }

    LOG_INFO(STR("Creating FIoDispatcherImpl::Mount hook at {:p}\n"), target);
    MH_STATUS s = MH_CreateHook(target, (LPVOID)io_dispatcher_mount_hook, (LPVOID*)&g_real_io_dispatcher_mount);
    if (s != MH_OK) {
        LOG_ERROR(STR("Failed to create FIoDispatcherImpl::Mount hook at {:p}: {}\n"),
            target, widen_ascii(MH_StatusToString(s)));
        return false;
    }

    LOG_INFO(STR("Enabling FIoDispatcherImpl::Mount hook\n"));
    s = MH_EnableHook(target);
    if (s != MH_OK) {
        LOG_ERROR(STR("Failed to enable FIoDispatcherImpl::Mount hook at {:p}: {}\n"),
            target, widen_ascii(MH_StatusToString(s)));
        return false;
    }

    LOG_INFO(STR("Installed FIoDispatcherImpl::Mount hook at {:p}\n"), target);
    return true;
}

bool
install_pak_platform_file_hook(void)
{
    LPVOID target = (LPVOID)sigscan::scan_exec(GetModuleHandleW(nullptr), "E8 ? ? ? ? 84 C0 74 ? 41 FF C5 FF C6");
    if (!target) {
        LOG_ERROR(STR("FPakPlatformFile::Mount call not found\n"));
        return false;
    }

    target = resolve_rel32_call_target((uint8_t*)target);

    LOG_INFO(STR("Creating FPakPlatformFile::Mount hook at {:p}\n"), target);
    MH_STATUS s = MH_CreateHook(target, (LPVOID)pak_platform_file_mount_hook, (LPVOID*)&g_real_pak_platform_file_mount);
    if (s != MH_OK) {
        LOG_ERROR(STR("Failed to create FPakPlatformFile::Mount hook at {:p}: {}\n"),
            target, widen_ascii(MH_StatusToString(s)));
        return false;
    }

    LOG_INFO(STR("Enabling FPakPlatformFile::Mount hook\n"));
    s = MH_EnableHook(target);
    if (s != MH_OK) {
        LOG_ERROR(STR("Failed to enable FPakPlatformFile::Mount hook at {:p}: {}\n"),
            target, widen_ascii(MH_StatusToString(s)));
        return false;
    }

    LOG_INFO(STR("Installed FPakPlatformFile::Mount hook at {:p}\n"), target);
    return true;
}

bool
install_pak_platform_file_mount_all_pak_files_hook(void)
{
    LPVOID target = (LPVOID)sigscan::scan_exec(GetModuleHandleW(nullptr), "48 89 5C 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 45 ? 33 FF 48 89 4D");
    if (!target) {
        LOG_ERROR(STR("FPakPlatformFile::MountAllPakFiles not found\n"));
        return false;
    }

    LOG_INFO(STR("Creating FPakPlatformFile::MountAllPakFiles hook at {:p}\n"), target);
    MH_STATUS s = MH_CreateHook(target, (LPVOID)pak_platform_file_mount_all_pak_files_hook, (LPVOID*)&g_real_pak_platform_file_mount_all_pak_files);
    if (s != MH_OK) {
        LOG_ERROR(STR("Failed to create FPakPlatformFile::MountAllPakFiles hook at {:p}: {}\n"),
            target, widen_ascii(MH_StatusToString(s)));
        return false;
    }

    LOG_INFO(STR("Enabling FPakPlatformFile::MountAllPakFiles hook\n"));
    s = MH_EnableHook(target);
    if (s != MH_OK) {
        LOG_ERROR(STR("Failed to enable FPakPlatformFile::MountAllPakFiles hook at {:p}: {}\n"),
            target, widen_ascii(MH_StatusToString(s)));
        return false;
    }

    LOG_INFO(STR("Installed FPakPlatformFile::MountAllPakFiles hook at {:p}\n"), target);
    return true;
}

FIoStatus* __fastcall
io_dispatcher_mount_hook(FIoDispatcherImpl* self, FIoStatus* status, FIoEnvironment* env, FGuid* guid, FAES* key)
{
    if (!g_io_dispatcher) {
        g_io_dispatcher = self;
    }

    FIoStatus* ret = g_real_io_dispatcher_mount(self, status, env, guid, key);

    LOG_NOTICE(STR("Path:   {}\n"), fstring_to_wchar(&env->Path));
    LOG_NOTICE(STR("Order:  {}\n"), env->Order);
    LOG_NOTICE(STR("GUID:   {}.{}.{}.{}\n"), guid->A, guid->B, guid->C, guid->D);
    LOG_NOTICE(STR("Status: {}\n"), ret->ErrorMessage);

    return ret;
}

bool __fastcall
pak_platform_file_mount_hook(void* self, const wchar_t* pak_filename, int pak_order, const wchar_t* path, bool load_index)
{
    LOG_NOTICE(STR("Pak Filename: {}\n"), pak_filename);
    LOG_NOTICE(STR("Pak Order:    {}\n"), pak_order);
    LOG_NOTICE(STR("Path:         {}\n"), (path) ? path : L"<null>");
    LOG_NOTICE(STR("Load Index:   {}\n"), load_index);

    if (!self || !g_real_pak_platform_file_mount) {
        LOG_WARN(STR("FPakPlatformFile::Mount hook: self: {:p}, FPakPlatformFile::Mount original: {:p}\n"), self, (void*)g_real_pak_platform_file_mount);
        return false;
    }

    bool ok = g_real_pak_platform_file_mount(self, pak_filename, pak_order, path, load_index);
    LOG_NOTICE(STR("OK:           {}\n"), ok);
    return ok;
}

int __fastcall
pak_platform_file_mount_all_pak_files_hook(void* self, TArray<FString>* pak_folders, FString* wildcard)
{
    if (!g_pak_platform_file) {
        g_pak_platform_file = self;
    }

    if (!pak_platform_file_mount_hooked) {
        install_pak_platform_file_hook();
        pak_platform_file_mount_hooked = true;
    }

    int result = g_real_pak_platform_file_mount_all_pak_files(self, pak_folders, wildcard);
    if (!all_paks_mounted) {
        all_paks_mounted = true;

        fs::path mod_root = fs::current_path() / "Mods" / mod_name;
        fs::path load_order_filepath = (mod_root / "load_order.txt").lexically_normal();

        auto entries = parse_load_order(load_order_filepath);
        if (entries.empty()) {
            LOG_WARN(STR("No {}\n"), load_order_filepath.wstring());
        } else {
            for (size_t i = 0; i < entries.size(); ++i) {
                auto& e = entries[i];

                auto bases = generate_mount_bases(mod_root / e.rel_path);
                if (bases.empty()) {
                    LOG_WARN(STR("{}/{}: {}: no .utoc/.ucas pairs found\n"), i + 1, entries.size(), widen_ascii(e.rel_path));
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

bool
mount_container_best(const std::string& base_no_ext_utf8, int order)
{
    fs::path base = fs::path(base_no_ext_utf8).lexically_normal();

    // prefer .pak mount
    fs::path pak = base_to_ext(base, L".pak");
    if (file_exists(pak)) {
        if (!g_pak_platform_file || !g_real_pak_platform_file_mount) {
            LOG_WARN(STR("Pak mount unavailable (self={:p}, fn={:p})\n"), g_pak_platform_file, (void*)g_real_pak_platform_file_mount);
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
        LOG_WARN(STR("Missing IoStore pair for base: {}\n"), widen_ascii(base_no_ext_utf8));
        return false;
    }

    if (!g_io_dispatcher || !g_real_io_dispatcher_mount) {
        LOG_WARN(STR("Io mount unavailable (dispatcher={:p}, fn={:p})\n"), (void*)g_io_dispatcher, (void*)g_real_io_dispatcher_mount);
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
