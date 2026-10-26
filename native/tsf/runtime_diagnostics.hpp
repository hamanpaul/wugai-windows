#ifndef BOSHIAMY_TSF_RUNTIME_DIAGNOSTICS_HPP
#define BOSHIAMY_TSF_RUNTIME_DIAGNOSTICS_HPP

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <msctf.h>
#include <textstor.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>

#include "activation_diagnostics.hpp"

namespace boshiamy::runtime_diagnostics {

inline constexpr wchar_t kMarkerName[] = L"tsf-diagnostics.enabled";
inline constexpr wchar_t kMarkerMagic[] = L"LIUKAI_TSF_DIAGNOSTICS_V1";
inline constexpr std::uint32_t kMaxEvents = 512;
inline constexpr std::int64_t kMaxMarkerLifetimeSeconds = 24 * 60 * 60;

enum class Event : std::uint32_t {
    Created = 1,
    ActivateBegin = 2,
    ActivateEnd = 3,
    KeySinkAdvice = 4,
    Focus = 5,
    TestDown = 6,
    KeyDown = 7,
    EditRequest = 8,
    EditApplied = 9,
    Deactivate = 10,
    ExtendedInterfaceQuery = 11
};

enum class HostKind : std::uint32_t {
    Other = 0,
    WindowsTerminal = 1,
    Conhost = 2,
    Code = 3
};

enum class WriteResult { Written, Disabled, LimitReached, IoError };

// Every field is numeric. Callers choose only fixed enum values; this record
// intentionally has no string, text, key, path, or window-title surface.
struct Record {
    Event event = Event::Created;
    HostKind host_kind = HostKind::Other;
    std::uint32_t activation_flags = 0;
    // Last activation call: 0=none, 1=legacy Activate, 2=ActivateEx.
    // Keep the callback argument separate from GetActiveFlags observations.
    std::uint32_t activation_entry_point = 0;
    std::uint32_t activation_argument_flags = 0;
    std::uint32_t context_dynamic_flags = 0;
    std::uint32_t context_static_flags = 0;
    std::uint32_t has_focus = 0;
    std::int32_t keyboard_disabled = -1;
    std::int32_t empty_context = -1;
    std::int32_t result_hr = static_cast<std::int32_t>(E_PENDING);
    // For EditRequest, result_hr/session_hr preserve the outer request and
    // edit-session callback results. Context observations stay separate.
    std::int32_t session_hr = static_cast<std::int32_t>(E_PENDING);
    std::int32_t context_status_hr = static_cast<std::int32_t>(E_PENDING);
    std::int32_t active_flags_hr = static_cast<std::int32_t>(E_PENDING);
    std::uint32_t engine_loaded = 0;
    std::uint32_t mode_english = 0;
    std::uint32_t composing = 0;
    std::uint32_t eaten = 0;
};

namespace detail {

inline bool decimal_i64(const wchar_t *text, std::int64_t *value) noexcept {
    if (!text || !*text || !value) return false;
    std::int64_t parsed = 0;
    for (const wchar_t *p = text; *p; ++p) {
        if (*p < L'0' || *p > L'9') return false;
        const int digit = static_cast<int>(*p - L'0');
        if (parsed > (INT64_MAX - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }
    *value = parsed;
    return true;
}

inline std::int64_t utc_now_seconds() noexcept {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = ft.dwLowDateTime;
    ticks.HighPart = ft.dwHighDateTime;
    constexpr std::uint64_t kUnixEpochTicks = 116444736000000000ULL;
    if (ticks.QuadPart < kUnixEpochTicks) return 0;
    return static_cast<std::int64_t>((ticks.QuadPart - kUnixEpochTicks) / 10000000ULL);
}

inline bool valid_marker(const std::wstring &directory, std::int64_t now,
    std::wstring *out_path) noexcept {
    try {
        const std::wstring path = directory + L"\\" + kMarkerName;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        std::array<char, 160> buffer{};
        DWORD bytes = 0;
        const BOOL ok = ReadFile(file, buffer.data(),
            static_cast<DWORD>(buffer.size() - 1), &bytes, nullptr);
        CloseHandle(file);
        if (!ok || bytes == 0) return false;
        std::wstring content;
        content.reserve(bytes);
        for (DWORD i = 0; i < bytes; ++i) {
            const unsigned char ch = static_cast<unsigned char>(buffer[i]);
            if (ch > 0x7f) return false;
            content.push_back(static_cast<wchar_t>(ch));
        }
        while (!content.empty() && (content.back() == L'\r' || content.back() == L'\n')) content.pop_back();
        const std::size_t newline = content.find_first_of(L"\r\n");
        if (newline == std::wstring::npos || content.substr(0, newline) != kMarkerMagic) return false;
        std::size_t expiry_start = newline;
        while (expiry_start < content.size() && (content[expiry_start] == L'\r' || content[expiry_start] == L'\n')) ++expiry_start;
        if (expiry_start == content.size()) return false;
        const std::wstring expiry_text = content.substr(expiry_start);
        if (expiry_text.find_first_of(L"\r\n") != std::wstring::npos) return false;
        std::int64_t expiry = 0;
        if (!decimal_i64(expiry_text.c_str(), &expiry) || expiry <= now ||
            expiry - now > kMaxMarkerLifetimeSeconds) return false;
        if (out_path) *out_path = path;
        return true;
    } catch (...) { return false; }
}

inline bool process_creation_stamp(std::uint64_t *stamp) noexcept {
    if (!stamp) return false;
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return false;
    ULARGE_INTEGER value{};
    value.LowPart = created.dwLowDateTime;
    value.HighPart = created.dwHighDateTime;
    *stamp = value.QuadPart;
    return true;
}

inline HostKind detect_host_kind() noexcept {
    wchar_t image_path[2048]{};
    const DWORD length = GetModuleFileNameW(nullptr, image_path,
        static_cast<DWORD>(sizeof(image_path) / sizeof(image_path[0])));
    if (!length || length >= sizeof(image_path) / sizeof(image_path[0])) return HostKind::Other;
    const wchar_t *base = image_path;
    for (const wchar_t *p = image_path; *p; ++p)
        if (*p == L'\\' || *p == L'/') base = p + 1;
    if (CompareStringOrdinal(base, -1, L"WindowsTerminal.exe", -1, TRUE) == CSTR_EQUAL)
        return HostKind::WindowsTerminal;
    if (CompareStringOrdinal(base, -1, L"conhost.exe", -1, TRUE) == CSTR_EQUAL)
        return HostKind::Conhost;
    if (CompareStringOrdinal(base, -1, L"Code.exe", -1, TRUE) == CSTR_EQUAL)
        return HostKind::Code;
    return HostKind::Other;
}

inline std::wstring event_file_path(const std::wstring &directory) {
    std::uint64_t created = 0;
    if (!process_creation_stamp(&created)) return {};
    wchar_t suffix[96]{};
    if (std::swprintf(suffix, sizeof(suffix) / sizeof(suffix[0]), L"\\tsf-diagnostics-%llu-%lu.jsonl",
        static_cast<unsigned long long>(created), static_cast<unsigned long>(GetCurrentProcessId())) < 0)
        return {};
    return directory + suffix;
}

inline bool write_line(const std::wstring &path, const char *line, DWORD size) noexcept {
    if (path.empty() || !line || size == 0) return false;
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(file, line, size, &written, nullptr);
    CloseHandle(file);
    return ok && written == size;
}

struct State {
    std::mutex mutex;
    std::wstring directory;
    std::wstring event_path;
    std::uint32_t written = 0;
    bool enabled = false;
    bool default_directory_pending = false;
};

inline std::shared_ptr<State> process_state() {
    static std::shared_ptr<State> state = std::make_shared<State>();
    return state;
}

inline std::wstring default_directory() {
    DWORD needed = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (!needed || needed > 32767) return {};
    std::wstring base(needed, L'\0');
    DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", base.data(), needed);
    if (!copied || copied >= needed) return {};
    base.resize(copied);
    return base + L"\\LiuKai";
}

inline bool valid_enum_values(const Record &r) noexcept {
    return (r.event >= Event::Created && r.event <= Event::ExtendedInterfaceQuery) &&
        (r.host_kind >= HostKind::Other && r.host_kind <= HostKind::Code) &&
        (r.keyboard_disabled >= -1 && r.keyboard_disabled <= 1) &&
        (r.empty_context >= -1 && r.empty_context <= 1) &&
        r.has_focus <= 1 && r.engine_loaded <= 1 && r.mode_english <= 1 &&
        r.composing <= 1 && r.eaten <= 1 && r.activation_entry_point <= 2;
}

inline bool encode(const Record &r, char *out, std::size_t capacity, DWORD *length) noexcept {
    if (!out || !length || capacity == 0 || !valid_enum_values(r)) return false;
    const int n = std::snprintf(out, capacity,
        "{\"event\":%u,\"pid\":%lu,\"tid\":%lu,\"hostKind\":%u,\"activationFlags\":%u,"
        "\"activationEntryPoint\":%u,\"activationArgumentFlags\":%u,"
        "\"contextDynamicFlags\":%u,\"contextStaticFlags\":%u,\"hasFocus\":%u,\"keyboardDisabled\":%d,"
        "\"emptyContext\":%d,\"resultHr\":%d,\"sessionHr\":%d,\"contextStatusHr\":%d,"
        "\"activeFlagsHr\":%d,\"engineLoaded\":%u,"
        "\"modeEnglish\":%u,\"composing\":%u,\"eaten\":%u}\n",
        static_cast<unsigned>(r.event), static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long>(GetCurrentThreadId()), static_cast<unsigned>(r.host_kind),
        r.activation_flags, r.activation_entry_point, r.activation_argument_flags,
        r.context_dynamic_flags, r.context_static_flags, r.has_focus, r.keyboard_disabled,
        r.empty_context, r.result_hr, r.session_hr, r.context_status_hr, r.active_flags_hr,
        r.engine_loaded, r.mode_english,
        r.composing, r.eaten);
    if (n < 0 || static_cast<std::size_t>(n) >= capacity) return false;
    *length = static_cast<DWORD>(n);
    return true;
}

} // namespace detail

class Journal final {
    std::shared_ptr<detail::State> state_;

    explicit Journal(std::shared_ptr<detail::State> state) noexcept : state_(std::move(state)) {}

    static void initialize(const std::shared_ptr<detail::State> &state,
        const std::wstring &directory, std::int64_t now) noexcept {
        try {
            state->directory = directory;
            state->default_directory_pending = directory.empty();
            state->enabled = detail::valid_marker(directory, now, nullptr);
            if (state->enabled) {
                state->event_path = detail::event_file_path(directory);
                if (state->event_path.empty()) state->enabled = false;
            }
        } catch (...) { state->enabled = false; }
    }

    bool marker_live() const noexcept {
        if (!state_) return false;
        try {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->default_directory_pending || state_->directory.empty()) {
                state_->directory = detail::default_directory();
                state_->default_directory_pending = false;
            }
            if (!detail::valid_marker(state_->directory, detail::utc_now_seconds(), nullptr)) {
                state_->enabled = false;
                return false;
            }
            if (state_->written >= kMaxEvents) return false;
            if (state_->event_path.empty()) state_->event_path = detail::event_file_path(state_->directory);
            state_->enabled = !state_->event_path.empty();
            return state_->enabled;
        } catch (...) { return false; }
    }

public:
    Journal() noexcept {
        try {
            state_ = detail::process_state();
            static std::once_flag initialized;
            std::call_once(initialized, [state = state_] {
                initialize(state, detail::default_directory(), detail::utc_now_seconds());
            });
        } catch (...) { state_.reset(); }
    }

    // Isolated-directory constructor for deterministic tests. Production code
    // should use the default constructor so the fixed LocalAppData marker is used.
    Journal(const std::wstring &isolated_directory, std::int64_t now_utc_seconds) noexcept {
        try {
            state_ = std::make_shared<detail::State>();
            initialize(state_, isolated_directory, now_utc_seconds);
        } catch (...) { state_.reset(); }
    }

    bool enabled() const noexcept { return marker_live(); }

    WriteResult write(const Record &record) noexcept {
        if (!state_) return WriteResult::Disabled;
        try {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->default_directory_pending || state_->directory.empty()) {
                state_->directory = detail::default_directory();
                state_->default_directory_pending = false;
            }
            if (!detail::valid_marker(state_->directory, detail::utc_now_seconds(), nullptr)) {
                state_->enabled = false;
                return WriteResult::Disabled;
            }
            if (state_->event_path.empty()) state_->event_path = detail::event_file_path(state_->directory);
            state_->enabled = !state_->event_path.empty();
            if (!state_->enabled) return WriteResult::IoError;
            if (state_->written >= kMaxEvents) return WriteResult::LimitReached;
            std::array<char, 768> line{};
            DWORD length = 0;
            if (!detail::encode(record, line.data(), line.size(), &length)) return WriteResult::IoError;
            if (!detail::write_line(state_->event_path, line.data(), length)) return WriteResult::IoError;
            ++state_->written;
            return WriteResult::Written;
        } catch (...) { return WriteResult::IoError; }
    }

    // Context inspection is deferred until the marker is still valid. This
    // prevents an expired opt-in from causing even read-only TSF queries.
    Record context(Record record, ITfThreadMgr *thread_manager, ITfContext *context) const noexcept;
};

inline std::int32_t tri_state(const tsf_activation::ContextFlag &flag) noexcept {
    return flag.known ? (flag.value ? 1 : 0) : -1;
}

// Read only existing host compartments. activation_diagnostics enumerates
// before GetCompartment, so absent keyboard/empty-context compartments are not
// created by this observer.
inline Record read_context_record(Record record, ITfThreadMgr *thread_manager,
    ITfContext *context) noexcept {
    if (!thread_manager || !context) {
        record.context_status_hr = static_cast<std::int32_t>(E_INVALIDARG);
        record.active_flags_hr = static_cast<std::int32_t>(E_INVALIDARG);
        return record;
    }
    const auto active = tsf_activation::read_active_flags(thread_manager);
    record.active_flags_hr = static_cast<std::int32_t>(FAILED(active.query_result) ?
        active.query_result : active.read_result);
    if (active.has_value) record.activation_flags = active.value;

    TS_STATUS status{};
    HRESULT hr = context->GetStatus(&status);
    record.context_status_hr = static_cast<std::int32_t>(hr);
    if (SUCCEEDED(hr)) {
        record.context_dynamic_flags = status.dwDynamicFlags;
        record.context_static_flags = status.dwStaticFlags;
    }
    const auto gates = tsf_activation::read_context_gates(context);
    record.keyboard_disabled = tri_state(gates.keyboard_disabled);
    record.empty_context = tri_state(gates.empty_context);
    return record;
}

inline Record Journal::context(Record record, ITfThreadMgr *thread_manager,
    ITfContext *context) const noexcept {
    if (!marker_live()) return record;
    return read_context_record(record, thread_manager, context);
}

using RuntimeDiagnostics = Journal;

} // namespace boshiamy::runtime_diagnostics

#endif
