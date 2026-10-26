// Exercise the production CLI's report/exit boundary without opening COM,
// the registry, or running its entry point.
#define wmain installer_status_report_unused_entry
#include "../installer/lifecycle_main.cpp"
#undef wmain
#include <sstream>

using namespace boshiamy::installer;

struct ReportBackend final : Backend {
    HRESULT read_status = S_OK;
    ResourceState machine;
    unsigned writes = 0;
    HRESULT read(Resource resource, ResourceState* state) override {
        if (!state) return E_POINTER;
        *state = resource==Resource::machine_com_x64?machine:ResourceState{};
        return read_status;
    }
    HRESULT apply(Resource, const ResourceState&) override { ++writes; return E_UNEXPECTED; }
    HRESULT restore(Resource, const ResourceState&) override { ++writes; return E_UNEXPECTED; }
};

struct ReportApi final : TsfSharedApi {
    bool present = true;
    HRESULT profile_result = S_OK, caps_result = S_OK, category_result = S_OK, user_result = S_OK;
    unsigned writes = 0, user_reads = 0, caps_reads = 0;
    HRESULT profile_present(bool* out) override { *out = present; return profile_result; }
    HRESULT profile_capabilities(DWORD* out) override {
        ++caps_reads;
        if (caps_result == S_OK) *out = kInstallerProfileCapsSysTraySupport;
        return caps_result;
    }
    HRESULT category_present(Resource, bool* out) override {
        if (category_result == S_OK) *out = true;
        return category_result;
    }
    HRESULT profile_user_state(bool* enabled, bool* is_default) override {
        ++user_reads;
        if (user_result == S_OK) { *enabled = true; *is_default = false; }
        return user_result;
    }
    HRESULT register_profile(const std::wstring&, bool) override { ++writes; return E_UNEXPECTED; }
    HRESULT unregister_profile() override { ++writes; return E_UNEXPECTED; }
    HRESULT restore_profile_user_state(bool, bool) override { ++writes; return E_UNEXPECTED; }
    HRESULT register_category(Resource) override { ++writes; return E_UNEXPECTED; }
    HRESULT unregister_category(Resource) override { ++writes; return E_UNEXPECTED; }
};

struct CaptureReport {
    std::ostringstream output;
    std::streambuf* previous = std::cout.rdbuf(output.rdbuf());
    ~CaptureReport() { std::cout.rdbuf(previous); }
};

static bool check_case(const char* label, ReportBackend& backend, ReportApi& api,
                       HRESULT expected, bool expect_null_user = false) {
    HRESULT actual = E_UNEXPECTED;
    std::string json;
    {
        CaptureReport capture;
        actual = status_report(backend, api, S_OK, S_OK, nullptr);
        json = capture.output.str();
    }
    const bool known_user = json.find("\"user_enabled\":null") == std::string::npos;
    const bool passed = actual == expected &&
        lifecycle_exit_code(S_OK, S_OK, actual) == (expected == S_OK ? 0 : 1) &&
        backend.writes == 0 && api.writes == 0 && (!expect_null_user || !known_user);
    std::cout << label << " observed=" << static_cast<unsigned long>(actual)
              << " expected=" << static_cast<unsigned long>(expected)
              << " exit=" << lifecycle_exit_code(S_OK, S_OK, actual)
              << " user_known=" << known_user << " writes=" << backend.writes + api.writes
              << " passed=" << passed << '\n';
    return passed;
}

int main() {
    ReportBackend backend;
    ReportApi good;
    backend.machine={true,true,L"LiuKai-TSF-v1",L"C:\\LiuKai\\versions\\v60\\x64\\BoshiamyTsf.dll",L"Apartment",{}};
    {
        CaptureReport capture;
        if(status_report(backend,good,S_OK,S_OK,nullptr,true)!=S_OK)return 1;
        const auto json=capture.output.str();
        if(json.find("\"scope\":\"user-and-machine-com-x64\"")==std::string::npos||
           json.find("\"name\":\"com-machine-x64\"")==std::string::npos||
           json.find("\"hive\":\"HKLM\"")==std::string::npos||
           json.find("\"registry_view\":64")==std::string::npos||
           json.find("BoshiamyTsf.dll")==std::string::npos||backend.writes||good.writes)return 1;
    }
    backend.machine={};
    if (!check_case("known-profile", backend, good, S_OK)) return 1;
    ReportApi absent;
    absent.present = false;
    absent.caps_result = E_ACCESSDENIED;
    absent.user_result = E_ACCESSDENIED;
    if (!check_case("known-absence", backend, absent, S_OK) || absent.caps_reads || absent.user_reads) return 1;

    for (HRESULT status : {S_FALSE, E_ACCESSDENIED}) {
        ReportApi user;
        user.user_result = status;
        if (!check_case("unknown-user", backend, user, status, true)) return 1;
        ReportApi profile;
        profile.profile_result = status;
        if (!check_case("unknown-profile", backend, profile, status, true)) return 1;
        ReportApi caps;
        caps.caps_result = status;
        if (!check_case("unknown-caps", backend, caps, status)) return 1;
        ReportApi category;
        category.category_result = status;
        if (!check_case("unknown-category", backend, category, status)) return 1;
    }
    backend.read_status = S_FALSE;
    ReportApi later;
    later.user_result = E_ACCESSDENIED;
    if (!check_case("preserve-first-read-status", backend, later, S_FALSE, true)) return 1;
    std::cout << "production status report/exit boundary passed; fake dependencies, zero writes\n";
}
