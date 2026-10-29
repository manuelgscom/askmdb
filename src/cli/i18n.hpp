#pragma once
// UI strings in Spanish and English.

#include <initializer_list>
#include <string>
#include <string_view>

#include "config/config.hpp"

namespace askmdb::cli {

enum class UiLang { Es, En };

enum class Msg {
    Help,
    UnknownFlag,         // + flag
    MissingValue,        // + flag
    BadValue,            // {} flag, {} value
    Conflict,            // + flags
    QuestionTooLong,     // {} = max bytes
    NoUri,
    InvalidUri,          // + libmongoc's reason
    UriPasswordWarning,
    NoDb,
    DbBlocked,           // {} db, {} reason
    TooManyCollections,  // {} count, {} max
    SchemaTooLarge,
    CostLine,            // {} collection, {} tokens
    CollectionsHint,
    CacheWrite,          // + reason
    SchemaFromRead,      // {} file, {} reason
    SchemaFromTooLarge,  // {} file, {} max bytes
    SchemaFromInvalid,   // {} file, {} parser's reason
    SchemaFromRender,    // {} file
    MongoConnect,        // + message
    MongoAuth,           // + message
    MongoTimeout,        // + message
    MongoServer,         // + message
    ModelMissing,        // + path
    NoModelsDir,
    DownloadOffer,       // {} file, {} MB, {} URL
    DownloadPrompt,
    DownloadDeclined,
    DownloadNoTty,       // {} file, {} MB, {} URL
    DownloadDone,        // + path
    EngineError,         // + unknown error code
    InvalidResponse,
    Incomplete,          // appended to InvalidResponse
    Timeout,
    LoadFailed,          // + path
    Refuse,              // + explanation
    Unsure,              // + explanation
    LevelRead,
    LevelWarn,
    LevelBlocked,
    NotExecuted,
    PromptRead,
    PromptWarn,
    Cancelled,
    Copied,
    CopyFailed,
    MoreRows,            // {} = max rows
    NoRows,
    DetectQuestion,
    GpuUnavailable,
    HwHeader,
    HwVulkanYes,
    HwVulkanNo,
    HwVulkanMissing,     // built with Vulkan, but libvulkan.so.1 is not on the system
    HwGpu,               // {} id, {} name, {} kind, {} free MiB, {} total MiB
    HwNoGpu,
    HwRam,               // {} available MiB, {} total MiB, {} cores
    KindDiscrete,
    KindIntegrated,
    KindSoftware,
    OnCpu,
    OnGpu,               // {} GPU name
    Proposal,            // {} model, {} OnCpu/OnGpu, {} MB
    ChoiceLine,          // {} model, {} OnCpu/OnGpu
    GpuPickHeader,       // {} GPU name
    GpuPickOption,       // {} number, {} model, {} MB, {} GpuPick4b/GpuPick9b
    GpuPick4b,
    GpuPick9b,
    GpuPickPrompt,
    ModelSource,         // {} file, {} URL
    ModelPresent,        // {} path
    UsePrompt,
    OfferCpu,            // {} model, {} MB
    FirstStartDeclined,
    SelfCheck,           // {} OnCpu/OnGpu
    SelfCheckGpuFailed,  // {} error code
    SelfCheckFailed,     // {} error code
    ChoiceSaved,         // {} model, {} OnCpu/OnGpu
    GpuWarmup,
    SelfCheckGpuNote,
    ChoiceNotSaved,      // + reason
    ConfigOverrides,
    GpuFallback,         // {} GPU, {} model
    Count_
};

std::string_view tr(UiLang lang, Msg msg);

// Replaces the first "{}" in pattern with value.
std::string fill(std::string_view pattern, std::string_view value);

// Replaces each "{}" in pattern, in order, with the matching value (extra ones get "").
std::string fill_all(std::string_view pattern, std::initializer_list<std::string_view> values);

// lang from --lang or the config unless Auto; otherwise the first non-empty of LC_ALL,
// LC_MESSAGES and LANG (nullptr = unset): "es…" => Spanish; anything else => English.
UiLang ui_lang(config::Lang lang, const char* lc_all, const char* lc_messages, const char* lang_env);

// Same, reading the real environment.
UiLang ui_lang_from_env(config::Lang lang);

}  // namespace askmdb::cli
