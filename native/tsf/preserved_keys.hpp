#ifndef LIUKAI_WINDOWS_TSF_PRESERVED_KEYS_HPP
#define LIUKAI_WINDOWS_TSF_PRESERVED_KEYS_HPP

#include <windows.h>
#include <msctf.h>
#include <olectl.h>

namespace boshiamy::preserved_keys {

// Service-owned command identifiers. They are passed back by TSF to
// ITfKeyEventSink::OnPreservedKey when the corresponding shortcut is pressed.
inline const GUID kLanguageToggleCommand =
    {0x3d5b8ec1,0xa710,0x4f26,{0x9a,0x32,0x51,0x6c,0xb3,0x6f,0x12,0x01}};
inline const GUID kShapeToggleCommand =
    {0x3d5b8ec1,0xa710,0x4f26,{0x9a,0x32,0x51,0x6c,0xb3,0x6f,0x12,0x02}};

enum class Key { language_toggle, shape_toggle };

struct OperationResult {
    HRESULT operation = S_OK;
    // Used by register_all when a later PreserveKey call fails and rollback
    // of an earlier key also fails. The failed key remains owned and retryable.
    HRESULT cleanup = S_OK;
};

namespace detail {
struct KeySpec {
    Key id;
    GUID command;
    TF_PRESERVEDKEY key;
    const wchar_t* description;
    ULONG description_length;
};

inline const KeySpec& spec(int index) {
    static const KeySpec entries[] = {
        {Key::language_toggle, kLanguageToggleCommand,
         {VK_SPACE, TF_MOD_CONTROL}, L"切換中英文", 5},
        {Key::shape_toggle, kShapeToggleCommand,
         {VK_SPACE, TF_MOD_SHIFT}, L"切換全半形", 5},
    };
    return entries[index];
}
}

class PreservedKeys final {
    bool owned_[2] = {false, false};

    bool& owned(Key key) { return owned_[key == Key::language_toggle ? 0 : 1]; }
    bool owned(Key key) const { return owned_[key == Key::language_toggle ? 0 : 1]; }

    template<class KeyApi>
    static HRESULT remove_owned(KeyApi& api, const detail::KeySpec& entry, bool& owns_key) {
        if (!owns_key) return S_FALSE;
        const HRESULT hr = api.UnpreserveKey(entry.command, &entry.key);
        if (hr == S_OK || hr == CONNECT_E_NOCONNECTION) {
            owns_key = false;
            return S_OK;
        }
        return FAILED(hr) ? hr : E_UNEXPECTED;
    }

public:
    template<class KeyApi>
    OperationResult register_all(KeyApi& api, TfClientId client) {
        OperationResult result{};
        bool added_here[2] = {false, false};
        for (int i = 0; i < 2; ++i) {
            const auto& entry = detail::spec(i);
            if (owned(entry.id)) continue;
            const HRESULT hr = api.PreserveKey(client, entry.command, &entry.key,
                                               entry.description, entry.description_length);
            if (hr != S_OK) {
                result.operation = FAILED(hr) ? hr : E_UNEXPECTED;
                for (int rollback = i - 1; rollback >= 0; --rollback) {
                    if (!added_here[rollback]) continue;
                    const auto& previous = detail::spec(rollback);
                    const HRESULT remove_hr = remove_owned(api, previous, owned(previous.id));
                    if (FAILED(remove_hr) && SUCCEEDED(result.cleanup)) result.cleanup = remove_hr;
                }
                return result;
            }
            owned(entry.id) = true;
            added_here[i] = true;
        }
        if (!added_here[0] && !added_here[1]) result.operation = S_FALSE;
        return result;
    }

    template<class KeyApi>
    OperationResult clear(KeyApi& api) {
        OperationResult result{};
        bool attempted = false;
        for (int i = 1; i >= 0; --i) {
            const auto& entry = detail::spec(i);
            if (!owned(entry.id)) continue;
            attempted = true;
            const HRESULT hr = remove_owned(api, entry, owned(entry.id));
            if (FAILED(hr) && SUCCEEDED(result.operation)) result.operation = hr;
        }
        if (!attempted) result.operation = S_FALSE;
        return result;
    }

    bool owns(Key key) const { return owned(key); }
};

} // namespace boshiamy::preserved_keys

#endif
