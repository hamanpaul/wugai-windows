#ifndef LIUKAI_REGISTRATION_DIAGNOSTICS_HPP
#define LIUKAI_REGISTRATION_DIAGNOSTICS_HPP
#include <windows.h>
#include <string>
#include <vector>
namespace boshiamy {
struct RegistrationEvent { std::string stage; HRESULT status; };
class RegistrationDiagnostics {
    std::vector<RegistrationEvent> events_;
    std::string failure_stage_;
    HRESULT failure_result_ = S_OK;
public:
    HRESULT record(const char* stage, HRESULT status) {
        events_.push_back({stage, status});
        if (FAILED(status) && failure_stage_.empty()) {
            failure_stage_ = stage;
            failure_result_ = status;
        }
        return status;
    }
    const std::vector<RegistrationEvent>& events() const { return events_; }
    const std::string& failure_stage() const { return failure_stage_; }
    HRESULT failure_result() const { return failure_result_; }
};
}
#endif
