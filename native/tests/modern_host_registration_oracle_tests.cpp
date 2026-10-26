// New semantic oracle for the already implemented TSF candidate UIElement.
// The R49 registration-plan test is intentionally left frozen.
#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include "../tsf/registration_plan.hpp"

static int failures=0;
static void expect(bool condition,const char* label){
 if(!condition){std::cerr<<label<<"\n";++failures;}
}
struct ModernRegistration final:boshiamy::RegistrationBackend {
 std::vector<std::string> calls;
 int failure_at=0,existing_at=0,add_count=0;
 bool existing=false;
 HRESULT added(const char* label){
  calls.emplace_back(label);++add_count;
  if(add_count==failure_at)return E_ACCESSDENIED;
  return existing||add_count==existing_at?S_FALSE:S_OK;
 }
 HRESULT removed(const char* label){calls.emplace_back(label);return existing?S_FALSE:S_OK;}
 HRESULT register_com()override{return added("com+");}
 HRESULT register_profile()override{return added("profile+");}
 HRESULT register_keyboard_category()override{return added("keyboard+");}
 // This independently describes the supported capability. The R49 base does
 // not yet call it; that behavioral gap is the intended compiled RED.
 HRESULT register_ui_element_category(){return added("ui_element+");}
 HRESULT register_system_tray_category()override{return added("systray+");}
 HRESULT register_immersive_category(){return added("immersive+");}
 // Intentionally omits override so this oracle still compiles before the
 // registration plan exposes and calls the new seventh capability.
 HRESULT register_input_mode_category(){return added("input_mode+");}
 HRESULT unregister_com()override{return removed("com-");}
 HRESULT unregister_profile()override{return removed("profile-");}
 HRESULT unregister_keyboard_category()override{return removed("keyboard-");}
 HRESULT unregister_ui_element_category(){return removed("ui_element-");}
 HRESULT unregister_system_tray_category()override{return removed("systray-");}
 HRESULT unregister_immersive_category(){return removed("immersive-");}
 HRESULT unregister_input_mode_category(){return removed("input_mode-");}
};
int main(){
 ModernRegistration fresh;
 expect(boshiamy::register_text_service(fresh)==S_OK,"fresh registration result");
 expect(fresh.calls==std::vector<std::string>({"com+","profile+","keyboard+","ui_element+","systray+","immersive+","input_mode+"}),"fresh registration includes input mode compartment support");
 ModernRegistration existing;existing.existing=true;
 expect(boshiamy::register_text_service(existing)==S_FALSE,"idempotent registration result");
 expect(existing.calls==std::vector<std::string>({"com+","profile+","keyboard+","ui_element+","systray+","immersive+","input_mode+"}),"idempotent registration checks input mode compartment support");
 ModernRegistration ui_failure;ui_failure.failure_at=4;
 HRESULT cleanup=S_OK;
 expect(boshiamy::register_text_service(ui_failure,&cleanup)==E_ACCESSDENIED,"UIElement add failure result");
 expect(cleanup==S_OK,"UIElement add failure cleanup result");
 expect(ui_failure.calls==std::vector<std::string>({"com+","profile+","keyboard+","ui_element+","keyboard-","profile-","com-"}),"UIElement add failure rolls back owned prior resources");
 ModernRegistration tray_failure;tray_failure.failure_at=5;
 expect(boshiamy::register_text_service(tray_failure)==E_ACCESSDENIED,"tray add failure result");
 expect(tray_failure.calls==std::vector<std::string>({"com+","profile+","keyboard+","ui_element+","systray+","ui_element-","keyboard-","profile-","com-"}),"tray add failure rolls back UIElement too");
 ModernRegistration removal;
 expect(boshiamy::unregister_text_service(removal)==S_OK,"unregistration result");
 expect(removal.calls==std::vector<std::string>({"input_mode-","immersive-","systray-","ui_element-","keyboard-","profile-","com-"}),"unregistration removes input mode compartment before other capabilities");
 ModernRegistration immersive_failure;immersive_failure.failure_at=6;
 expect(boshiamy::register_text_service(immersive_failure)==E_ACCESSDENIED,"immersive registration failure is reported");
 expect(immersive_failure.calls==std::vector<std::string>({"com+","profile+","keyboard+","ui_element+","systray+","immersive+","systray-","ui_element-","keyboard-","profile-","com-"}),"immersive failure rolls back only newly owned previous registrations");
 ModernRegistration input_mode_failure;input_mode_failure.failure_at=7;
 cleanup=E_FAIL;
 expect(boshiamy::register_text_service(input_mode_failure,&cleanup)==E_ACCESSDENIED,"input mode registration failure is reported");
 expect(cleanup==S_OK,"input mode registration failure cleanup result");
 expect(input_mode_failure.calls==std::vector<std::string>({"com+","profile+","keyboard+","ui_element+","systray+","immersive+","input_mode+","immersive-","systray-","ui_element-","keyboard-","profile-","com-"}),"seventh-step failure rolls back newly created prior capabilities in reverse order");
 ModernRegistration input_mode_failure_with_existing;input_mode_failure_with_existing.failure_at=7;input_mode_failure_with_existing.existing_at=6;
 expect(boshiamy::register_text_service(input_mode_failure_with_existing)==E_ACCESSDENIED,"input mode failure with an existing matching capability is reported");
 expect(input_mode_failure_with_existing.calls==std::vector<std::string>({"com+","profile+","keyboard+","ui_element+","systray+","immersive+","input_mode+","systray-","ui_element-","keyboard-","profile-","com-"}),"failure cleanup preserves the pre-existing matching immersive capability");
 if(failures)std::cerr<<failures<<" modern-host registration oracle failures\n";
 return failures?1:0;
}
