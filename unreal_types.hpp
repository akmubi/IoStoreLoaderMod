#pragma once

#include <Unreal/FString.hpp>
#include <cstdint>

enum class EIoErrorCode
{
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
    InvalidEncryptionKey
};

struct FIoStatus
{
    EIoErrorCode ErrorCode;
	wchar_t      ErrorMessage[128];
};

struct FIoEnvironment
{
    RC::Unreal::FString Path;
    int32_t             Order = 0;

    FIoEnvironment(const std::string& path, int order)
    {
        std::wstring wpath = std::wstring(path.begin(), path.end());
        
        this->Path = RC::Unreal::FString(wpath.c_str());
        this->Order = order;
    }
};

struct FGuid
{
    uint32_t A;
    uint32_t B;
    uint32_t C;
    uint32_t D;
};

struct FAES
{
    uint8_t Key[32];
};

struct FIoDispatcherImpl
{
    bool IsMultithreaded;
    // lots of other fields
};
