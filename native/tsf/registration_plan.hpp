#ifndef LIUKAI_REGISTRATION_PLAN_HPP
#define LIUKAI_REGISTRATION_PLAN_HPP
#include <windows.h>
namespace boshiamy {
template<class CategoryStatus,class OwnershipStatus,class Remove>
inline HRESULT unregister_category_if_present(CategoryStatus&& category_status,OwnershipStatus&& ownership_status,Remove&& remove){
 HRESULT category=category_status();if(category!=S_OK)return category;
 HRESULT ownership=ownership_status();if(ownership!=S_OK)return FAILED(ownership)?ownership:E_PENDING;
 return remove();
}
// Each add operation returns S_OK only for newly owned state and S_FALSE for
// existing matching state. A failed operation must undo its own partial writes.
struct RegistrationBackend {
 virtual ~RegistrationBackend()=default;
 virtual HRESULT register_com()=0;
 virtual HRESULT register_profile()=0;
 virtual HRESULT register_keyboard_category()=0;
 virtual HRESULT register_ui_element_category(){return S_FALSE;}
 virtual HRESULT register_system_tray_category()=0;
 virtual HRESULT register_immersive_category(){return S_FALSE;}
 virtual HRESULT register_input_mode_category(){return S_FALSE;}
 virtual HRESULT unregister_com()=0;
 virtual HRESULT unregister_profile()=0;
 virtual HRESULT unregister_keyboard_category()=0;
 virtual HRESULT unregister_ui_element_category(){return S_FALSE;}
 virtual HRESULT unregister_system_tray_category()=0;
 virtual HRESULT unregister_immersive_category(){return S_FALSE;}
 virtual HRESULT unregister_input_mode_category(){return S_FALSE;}
};
inline HRESULT register_text_service(RegistrationBackend& backend,HRESULT* cleanup_status=nullptr){
 if(cleanup_status)*cleanup_status=S_OK;
 HRESULT com=backend.register_com();if(FAILED(com))return com;
 HRESULT profile=backend.register_profile();
 auto cleanup=[&](HRESULT status){if(FAILED(status)&&cleanup_status&&SUCCEEDED(*cleanup_status))*cleanup_status=status;};
 if(FAILED(profile)){if(com==S_OK)cleanup(backend.unregister_com());return profile;}
 HRESULT keyboard=backend.register_keyboard_category();
 if(FAILED(keyboard)){if(profile==S_OK){HRESULT undo=backend.unregister_profile();cleanup(undo);}if(com==S_OK)cleanup(backend.unregister_com());return keyboard;}
 const bool keyboard_owned=keyboard==S_OK;
 HRESULT ui_element=backend.register_ui_element_category();
 if(FAILED(ui_element)){
  if(keyboard_owned)cleanup(backend.unregister_keyboard_category());
  if(profile==S_OK)cleanup(backend.unregister_profile());
  if(com==S_OK)cleanup(backend.unregister_com());
  return ui_element;
 }
 HRESULT system_tray=backend.register_system_tray_category();
 if(FAILED(system_tray)){
  if(ui_element==S_OK)cleanup(backend.unregister_ui_element_category());
  if(keyboard_owned)cleanup(backend.unregister_keyboard_category());
  if(profile==S_OK)cleanup(backend.unregister_profile());
  if(com==S_OK)cleanup(backend.unregister_com());
  return system_tray;
 }
 HRESULT immersive=backend.register_immersive_category();
 if(FAILED(immersive)){
  if(system_tray==S_OK)cleanup(backend.unregister_system_tray_category());
  if(ui_element==S_OK)cleanup(backend.unregister_ui_element_category());
  if(keyboard_owned)cleanup(backend.unregister_keyboard_category());
  if(profile==S_OK)cleanup(backend.unregister_profile());
  if(com==S_OK)cleanup(backend.unregister_com());
  return immersive;
 }
 HRESULT input_mode=backend.register_input_mode_category();
 if(FAILED(input_mode)){
  if(immersive==S_OK)cleanup(backend.unregister_immersive_category());
  if(system_tray==S_OK)cleanup(backend.unregister_system_tray_category());
  if(ui_element==S_OK)cleanup(backend.unregister_ui_element_category());
  if(keyboard_owned)cleanup(backend.unregister_keyboard_category());
  if(profile==S_OK)cleanup(backend.unregister_profile());
  if(com==S_OK)cleanup(backend.unregister_com());
  return input_mode;
 }
 return com==S_FALSE&&profile==S_FALSE&&keyboard==S_FALSE&&ui_element==S_FALSE&&system_tray==S_FALSE&&immersive==S_FALSE&&input_mode==S_FALSE?S_FALSE:S_OK;
}
inline HRESULT unregister_text_service(RegistrationBackend& backend){
 bool changed=false;
 auto remove=[&](HRESULT result){if(FAILED(result))return result;if(result==S_OK)changed=true;return S_OK;};
 HRESULT result=remove(backend.unregister_input_mode_category());if(FAILED(result))return result;
 result=remove(backend.unregister_immersive_category());if(FAILED(result))return result;
 result=remove(backend.unregister_system_tray_category());if(FAILED(result))return result;
 result=remove(backend.unregister_ui_element_category());if(FAILED(result))return result;
 result=remove(backend.unregister_keyboard_category());if(FAILED(result))return result;
 result=remove(backend.unregister_profile());if(FAILED(result))return result;
 result=remove(backend.unregister_com());if(FAILED(result))return result;
 return changed?S_OK:S_FALSE;
}
}
#endif
