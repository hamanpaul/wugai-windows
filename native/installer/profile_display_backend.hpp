#pragma once
#include "dual_architecture_transaction.hpp"
#include "profile_display_plan.hpp"
#include "profile_display_payload.hpp"
namespace boshiamy::installer::profile_display_backend {
    using DisplaySnapshot=profile_display_state::DisplaySnapshot;
    class DisplayAccess {
    public:
        virtual ~DisplayAccess()=default;
        virtual HRESULT capture(DisplaySnapshot*)=0;
        // Authority includes this exact hive/view/key and an exact inverse; COM ownership alone is insufficient.
        virtual HRESULT authorize(const DisplaySnapshot& expected,const DisplaySnapshot& target)=0;
        virtual HRESULT exchange(const DisplaySnapshot& expected,const DisplaySnapshot& target)=0;
        virtual HRESULT verify_target(const DisplaySnapshot&)=0;
    };
    // Explicit policy persists across a transaction/recovery: never auto-wrap a legacy journal.
    enum class Mode {
        legacy_passthrough, existing_profile_metadata
    };
    namespace envelope {
        inline HRESULT pack(const std::vector<unsigned char>& base,const DisplaySnapshot& display,std::vector<unsigned char>* out) {
            profile_display_payload::Payload payload;
            payload.legacy=base;
            payload.has_display=true;
            payload.display=display;
            return profile_display_payload::encode(payload,out);
        }
        inline HRESULT unpack(const ResourceState& full,ResourceState* base,DisplaySnapshot* display) {
            if(!base||!display)return E_POINTER;
            *base={};
            *display={};
            profile_display_payload::Payload payload;
            HRESULT hr=profile_display_payload::decode(full.private_data,&payload);
            if(hr!=S_OK)return hr;
            if(!payload.has_display)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            *base=full;
            base->private_data=std::move(payload.legacy);
            *display=std::move(payload.display);
            return S_OK;
        }
    }
    class DisplayBackend final:public Backend {
        Backend& inner_;
        DisplayAccess& access_;
        Mode mode_;
        std::wstring stable_module_;
        ResourceState last_,applied_target_,applied_state_;
        bool valid_=false,applied_valid_=false,may_mutate_=false,target_prepared_=false;
        DisplaySnapshot health_target_;
        static HRESULT mismatch(){
            return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        }
        HRESULT capture_profile(ResourceState* out) {
            if(!out)return E_POINTER;
            *out={};
            ResourceState base;
            HRESULT hr=inner_.read(Resource::profile,&base);
            if(hr!=S_OK)return hr;
            if(!base.present){
                *out=std::move(base);
                return S_OK;
            }
            DisplaySnapshot display;
            hr=access_.capture(&display);
            if(hr!=S_OK)return hr;
            ResourceState check;
            hr=inner_.read(Resource::profile,&check);
            if(hr!=S_OK)return hr;
            if(check!=base)return mismatch();
            *out=base;
            hr=envelope::pack(base.private_data,display,&out->private_data);
            if(hr!=S_OK)*out={};
            return hr;
        }
        HRESULT metadata_change(const ResourceState& target,bool restoring) {
            applied_valid_=false;
            may_mutate_=false;
            ResourceState target_base;
            DisplaySnapshot wanted;
            HRESULT hr=envelope::unpack(target,&target_base,&wanted);
            if(hr!=S_OK)return hr;
            if(!target.present)return E_NOTIMPL;
            const ResourceState expected=last_;
            const bool had_expected=valid_;
            ResourceState current;
            hr=capture_profile(&current);
            if(hr!=S_OK){
                valid_=false;
                return hr;
            }
            if(had_expected&&current!=expected)return mismatch();
            ResourceState current_base;
            DisplaySnapshot before;
            hr=envelope::unpack(current,&current_base,&before);
            if(hr!=S_OK)return hr;
            // This slice never re-registers, removes, or adopts ownership of a shared profile.
            if(current_base!=target_base||!profile_display::detail::same_except_descriptions(before.profile,wanted.profile))return mismatch();
            DisplaySnapshot planned;
            if(restoring) {
                hr=profile_display_plan::make_target(wanted,stable_module_,&planned);
                if(hr!=S_OK)return hr;
                // A cold inverse accepts only attributable before/our-target partial phases.
                auto raw_allowed=[](const auto& value,const auto& original,const auto& ours){
                    return profile_display::resource_name::same_value(value,original)||profile_display::resource_name::same_value(value,ours);
                };
                const auto& bp=before.profile;
                const auto& wp=wanted.profile;
                const auto& pp=planned.profile;
                if((bp.api_description!=wp.api_description&&bp.api_description!=pp.api_description)||       (bp.registry.description!=wp.registry.description&&bp.registry.description!=pp.registry.description)||       !raw_allowed(before.descriptions.registry64,wanted.descriptions.registry64,planned.descriptions.registry64)||       !raw_allowed(before.descriptions.registry32,wanted.descriptions.registry32,planned.descriptions.registry32)||       !raw_allowed(before.display_binding.registry64,wanted.display_binding.registry64,planned.display_binding.registry64)||       !raw_allowed(before.display_binding.registry32,wanted.display_binding.registry32,planned.display_binding.registry32))return mismatch();
            }
            else {
                hr=profile_display_plan::make_target(before,stable_module_,&planned);
                if(hr!=S_OK)return hr;
                if(!profile_display_state::equal(planned,wanted))return mismatch();
            }
            hr=access_.authorize(before,wanted);
            if(hr!=S_OK)return hr;
            if(!profile_display_state::equal(before,wanted)){
                may_mutate_=true;
                hr=access_.exchange(before,wanted);
            }
            ResourceState after;
            const HRESULT observed=capture_profile(&after);
            valid_=observed==S_OK;
            if(valid_)last_=after;
            if(hr!=S_OK)return hr;
            if(observed!=S_OK)return observed;
            if(after!=target)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            if(!restoring){
                hr=access_.verify_target(wanted);
                if(hr!=S_OK)return hr;
                applied_target_=target;
                applied_state_=after;
                applied_valid_=true;
            }
            return S_OK;
        }
    public:
        // Existing metadata mode requires a module from validated durable package identity.
        DisplayBackend(Backend& inner,DisplayAccess& access,Mode mode,std::wstring stable_module = {})   :inner_(inner),access_(access),mode_(mode),stable_module_(std::move(stable_module)){}  HRESULT read(Resource r,ResourceState* out)override {
            if(r!=Resource::profile||mode_==Mode::legacy_passthrough)return inner_.read(r,out);
            HRESULT hr=capture_profile(out);
            valid_=hr==S_OK;
            if(valid_)last_=*out;
            else applied_valid_=false;
            return hr;
        }
        HRESULT apply(Resource r,const ResourceState& s)override{
            return r==Resource::profile&&mode_==Mode::existing_profile_metadata?metadata_change(s,false):inner_.apply(r,s);
        }
        HRESULT restore(Resource r,const ResourceState& s)override{
            return r==Resource::profile&&mode_==Mode::existing_profile_metadata?metadata_change(s,true):inner_.restore(r,s);
        }
        HRESULT validate_install(const InstallRequest& r)override{
            return inner_.validate_install(r);
        }
        HRESULT prepare_apply_targets(TransactionOperation operation,const InstallRequest& request,const ResourceStates& before,ResourceStates* desired)override {
            if(!desired)return E_POINTER;
            target_prepared_=false;
            HRESULT hr=inner_.prepare_apply_targets(operation,request,before,desired);
            if(hr!=S_OK||mode_==Mode::legacy_passthrough)return hr;
            const auto& old=before[static_cast<size_t>(Resource::profile)];
            auto& target=(*desired)[static_cast<size_t>(Resource::profile)];
            if(operation==TransactionOperation::uninstall)return old==target?S_OK:E_NOTIMPL;
            if(!old.present||!target.present)return E_NOTIMPL;
            ResourceState base;
            DisplaySnapshot captured;
            hr=envelope::unpack(old,&base,&captured);
            if(hr!=S_OK)return hr;
            ResourceState planned_base;
            DisplaySnapshot unused;
            hr=envelope::unpack(target,&planned_base,&unused);
            if(hr!=S_OK)return hr;
            if(planned_base!=base)return E_NOTIMPL;
            if(!profile_display::resource_name::exact_path_equal(request.icon_path,stable_module_))return E_INVALIDARG;
            DisplaySnapshot planned;
            hr=profile_display_plan::make_target(captured,stable_module_,&planned);
            if(hr!=S_OK)return hr;
            hr=access_.authorize(captured,planned);
            if(hr!=S_OK)return hr;
            hr=access_.authorize(planned,captured);
            if(hr!=S_OK)return hr;
            target=old;
            hr=envelope::pack(base.private_data,planned,&target.private_data);
            if(hr!=S_OK)return hr;
            health_target_=planned;
            target_prepared_=true;
            return S_OK;
        }
        HRESULT verify_applied(Resource r,const ResourceState& wanted,const ResourceState& observed)const override {
            if(r!=Resource::profile||mode_==Mode::legacy_passthrough)return inner_.verify_applied(r,wanted,observed);
            return valid_&&applied_valid_&&wanted==applied_target_&&observed==applied_state_&&observed==last_?S_OK:mismatch();
        }
        HRESULT project_apply_state(Resource r,const ResourceState& state,ResourceState* out)const override {
            if(r!=Resource::profile||mode_==Mode::legacy_passthrough)return inner_.project_apply_state(r,state,out);
            if(!out)return E_POINTER;
            *out={};
            ResourceState base;
            DisplaySnapshot display;
            HRESULT hr=envelope::unpack(state,&base,&display);
            if(hr!=S_OK)return hr;
            ResourceState projected;
            hr=inner_.project_apply_state(r,base,&projected);
            if(hr!=S_OK)return hr;
            *out=projected;
            hr=envelope::pack(projected.private_data,display,&out->private_data);
            if(hr!=S_OK)*out={};
            return hr;
        }
        bool failure_may_have_mutated(Resource r,HRESULT status)const override{
            return r==Resource::profile&&mode_==Mode::existing_profile_metadata?may_mutate_:inner_.failure_may_have_mutated(r,status);
        }
        HRESULT verify_install_health()override {
            HRESULT hr=inner_.verify_install_health();
            if(hr!=S_OK||mode_==Mode::legacy_passthrough||!target_prepared_)return hr;
            ResourceState current;
            hr=capture_profile(&current);
            if(hr!=S_OK)return hr;
            ResourceState base;
            DisplaySnapshot display;
            hr=envelope::unpack(current,&base,&display);
            if(hr!=S_OK)return hr;
            if(!profile_display_state::equal(display,health_target_))return mismatch();
            return access_.verify_target(display);
        }
    };
}
