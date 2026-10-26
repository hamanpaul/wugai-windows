#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include "../installer/durable_transaction_snapshot.hpp"

#define CHECK(x) do { if(!(x)){std::cerr<<#x<<" line "<<__LINE__<<"\n";return false;} } while(0)
namespace fs=std::filesystem;
using namespace boshiamy::installer;
using namespace boshiamy::installer::durable_snapshot;
using boshiamy::installer::durable_journal::Identity;

static Identity make_identity(const fs::path& root) {
    Identity id;id.owner="LiuKai-TSF-v1";id.token="snapshot-01";
    id.install_root=root.wstring();id.package_version="test-2";
    id.package_manifest_sha256=std::string(64,'a');return id;
}
static ResourceStates states_for_test() {
    ResourceStates states{};
    for(size_t i=0;i<legacy_resource_count;++i) {
        auto& s=states[i];s.present=(i%2)==0;s.created_by_lifecycle=(i%3)==0;
        s.owner=L"owner-"+std::to_wstring(i);s.path=L"C:\\install\\resource-"+std::to_wstring(i);
        s.threading_model=i<2?L"Apartment":L"";
        s.private_data={static_cast<unsigned char>(i),0,0xff,static_cast<unsigned char>(255-i)};
    }
    states[static_cast<size_t>(Resource::profile)].path=std::wstring(L"icon")+wchar_t(0)+L".dll";
    return states;
}
static bool equal_states(const ResourceStates& a,const ResourceStates& b) {
    for(size_t i=0;i<a.size();++i)if(a[i]!=b[i])return false;
    return true;
}
static std::string read_bytes(const fs::path& path) {
    std::ifstream in(path,std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());
}
static bool write_bytes(const fs::path& path,const std::string& bytes) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    out.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));return out.good();
}
static bool independent_sha256(const std::string& input,std::array<uint8_t,32>* digest) {
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;DWORD object_size=0,got=0;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return false;
    std::vector<uint8_t> object;bool ok=BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_size),sizeof(object_size),&got,0)>=0;
    if(ok){object.resize(object_size);ok=BCryptCreateHash(algorithm,&hash,object.data(),object_size,nullptr,0,0)>=0;}
    if(ok&&!input.empty())ok=BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
        static_cast<ULONG>(input.size()),0)>=0;
    if(ok)ok=BCryptFinishHash(hash,digest->data(),static_cast<ULONG>(digest->size()),0)>=0;
    if(hash)BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm,0);
    return ok;
}
static bool read_u32(const std::string& data,size_t* at,uint32_t* value) {
    if(!at||!value||*at>data.size()||data.size()-*at<4)return false;
    *value=0;for(unsigned i=0;i<4;++i)*value|=static_cast<uint32_t>(static_cast<uint8_t>(data[(*at)++]))<<(i*8);
    return true;
}
static void put_u32_at(std::string* data,size_t at,uint32_t value) {
    for(unsigned i=0;i<4;++i)(*data)[at+i]=static_cast<char>((value>>(i*8))&0xff);
}
static bool resign(std::string* image) {
    if(!image||image->size()<32)return false;
    std::array<uint8_t,32> digest{};
    const size_t at=image->size()-digest.size();if(!independent_sha256(image->substr(0,at),&digest))return false;
    std::memcpy(image->data()+at,digest.data(),digest.size());return true;
}
static bool test_identity_bound_exact_snapshot_roundtrip(const fs::path& sandbox) {
    const auto root=sandbox/L"install-root";std::error_code ec;
    CHECK(fs::create_directory(root,ec)&&!ec);
    const auto id=make_identity(root);const auto path=root/L".prestate";
    durable_snapshot::Store store(path,id);const auto expected=states_for_test();
    CHECK(SUCCEEDED(store.create(expected)));
    ResourceStates reopened{};std::string fingerprint;
    CHECK(SUCCEEDED(store.read(id,&reopened,&fingerprint)));
    CHECK(equal_states(expected,reopened));
    CHECK(fingerprint.size()==64);
    Identity foreign=id;foreign.token="different-token";
    ResourceStates rejected{};CHECK(FAILED(store.read(foreign,&rejected)));
    return true;
}
static bool test_existing_snapshot_is_never_replaced(const fs::path& sandbox) {
    const auto root=sandbox/L"existing-root";std::error_code ec;CHECK(fs::create_directory(root,ec)&&!ec);
    const auto path=root/L".prestate";const std::string foreign="unrecognized existing snapshot";
    CHECK(write_bytes(path,foreign));durable_snapshot::Store store(path,make_identity(root));
    CHECK(FAILED(store.create(states_for_test())));CHECK(read_bytes(path)==foreign);return true;
}
static bool test_schema_checksum_order_and_tail_are_strict(const fs::path& sandbox) {
    const auto root=sandbox/L"corrupt-root";std::error_code ec;CHECK(fs::create_directory(root,ec)&&!ec);
    const auto path=root/L".prestate";const auto id=make_identity(root);const auto states=states_for_test();
    durable_snapshot::Store store(path,id);CHECK(SUCCEEDED(store.create(states)));const std::string original=read_bytes(path);
    CHECK(original.size()>64&&original.substr(0,8)=="LKTSNP01");
    std::string unknown=original;put_u32_at(&unknown,8,3);CHECK(resign(&unknown));CHECK(write_bytes(path,unknown));
    ResourceStates out{};std::string fingerprint="must-clear";CHECK(FAILED(store.read(id,&out,&fingerprint)));
    CHECK(fingerprint.empty()&&!out[0].present&&out[0].private_data.empty());
    // Schema 1 encoded five resources; reject it explicitly instead of
    // treating the sixth UIElement relation as absent after a crash.
    std::string old_schema=original;put_u32_at(&old_schema,8,1);CHECK(resign(&old_schema));CHECK(write_bytes(path,old_schema));
    CHECK(FAILED(store.read(id,&out)));CHECK(out[0].private_data.empty());
    std::string reordered=original;size_t at=12;uint32_t identity_size=0;
    CHECK(read_u32(reordered,&at,&identity_size));const size_t count_at=at+identity_size;at=count_at;
    uint32_t count=0;CHECK(read_u32(reordered,&at,&count)&&count==count_for(id));
    put_u32_at(&reordered,at,1);CHECK(resign(&reordered));CHECK(write_bytes(path,reordered));
    CHECK(FAILED(store.read(id,&out)));CHECK(out[0].private_data.empty());
    CHECK(write_bytes(path,original.substr(0,original.size()-1)));
    CHECK(FAILED(store.read(id,&out)));CHECK(out[0].private_data.empty());return true;
}
static bool test_prior_schema_ledger_does_not_block_fresh_transaction(const fs::path& sandbox) {
    std::error_code ec;
    const auto old_root=sandbox/L"r49-ledger";
    const auto next_root=sandbox/L"next-ledger";
    CHECK(fs::create_directory(old_root,ec)&&!ec);
    CHECK(fs::create_directory(next_root,ec)&&!ec);
    const auto old_id=make_identity(old_root);
    const auto old_path=old_root/L".prestate";
    durable_snapshot::Store old_store(old_path,old_id);
    CHECK(SUCCEEDED(old_store.create(states_for_test())));
    std::string old_image=read_bytes(old_path);
    put_u32_at(&old_image,8,1);CHECK(resign(&old_image));CHECK(write_bytes(old_path,old_image));
    ResourceStates old_states{};
    CHECK(FAILED(old_store.read(old_id,&old_states)));
    const auto next_id=make_identity(next_root);
    durable_snapshot::Store next_store(next_root/L".prestate",next_id);
    const auto expected=states_for_test();
    CHECK(SUCCEEDED(next_store.create(expected)));
    ResourceStates observed{};
    CHECK(SUCCEEDED(next_store.read(next_id,&observed))&&equal_states(expected,observed));
    CHECK(read_bytes(old_path)==old_image);
    return true;
}
static bool test_publish_failures_leave_no_snapshot_or_temp_files(const fs::path& sandbox) {
    const std::vector<FaultPoint> points={FaultPoint::after_partial_write,
        FaultPoint::before_flush,FaultPoint::after_flush_before_replace,FaultPoint::before_replace};
    unsigned number=0;
    for(const auto point:points) {
        const auto root=sandbox/(L"fault-root-"+std::to_wstring(number));std::error_code ec;
        CHECK(fs::create_directory(root,ec)&&!ec);const auto path=root/L".prestate";bool armed=true;
        FaultInjector fault;fault.fail=[&](FaultPoint current){if(armed&&current==point){armed=false;return true;}return false;};
        durable_snapshot::Store store(path,make_identity(root),&fault);CHECK(FAILED(store.create(states_for_test())));
        CHECK(!fs::exists(path));size_t entries=0;for(fs::directory_iterator it(root,ec),end;it!=end&&!ec;it.increment(ec))++entries;
        CHECK(!ec&&entries==0);++number;
    }
    return true;
}
static bool test_postpublish_readback_failure_retains_published_path(const fs::path& sandbox) {
    {const auto root=sandbox/L"readback-fail-root";std::error_code ec;CHECK(fs::create_directory(root,ec)&&!ec);
     const auto path=root/L".prestate";FaultInjector fault;
     fault.fail=[](FaultPoint point){return point==FaultPoint::before_readback;};
     durable_snapshot::Store store(path,make_identity(root),&fault);
     CHECK(FAILED(store.create(states_for_test())));CHECK(fs::is_regular_file(path));
     ResourceStates observed{};CHECK(SUCCEEDED(store.read(make_identity(root),&observed)));
     CHECK(equal_states(observed,states_for_test()));}
    {const auto root=sandbox/L"foreign-afterpublish-root";std::error_code ec;CHECK(fs::create_directory(root,ec)&&!ec);
     const auto path=root/L".prestate";FaultInjector fault;
     fault.fail=[&](FaultPoint point){if(point==FaultPoint::after_publish){CHECK(write_bytes(path,"external replacement"));return false;}return false;};
     durable_snapshot::Store store(path,make_identity(root),&fault);
     CHECK(FAILED(store.create(states_for_test())));CHECK(read_bytes(path)=="external replacement");}
    return true;
}
static bool test_invalid_utf16_and_oversized_state_refuse_create(const fs::path& sandbox) {
    const auto root=sandbox/L"invalid-root";std::error_code ec;CHECK(fs::create_directory(root,ec)&&!ec);
    const auto id=make_identity(root);const auto path=root/L".prestate";auto states=states_for_test();
    states[0].owner.assign(1,static_cast<wchar_t>(0xd800));durable_snapshot::Store store(path,id);
    CHECK(FAILED(store.create(states)));CHECK(!fs::exists(path));
    states=states_for_test();states[0].private_data.resize(max_private_data+1);
    CHECK(FAILED(store.create(states)));CHECK(!fs::exists(path));return true;
}

static bool test_machine_scope_exact_snapshot_and_legacy_isolation(const fs::path& sandbox) {
    static_assert(static_cast<size_t>(Resource::com_x64)==0);
    static_assert(static_cast<size_t>(Resource::ui_element_category)==5);
    static_assert(static_cast<size_t>(Resource::machine_com_x64)==6);
    static_assert(resource_count==7);
    const auto root=sandbox/L"scope-install";
    fs::create_directories(root);
    const auto old_id=make_identity(root);
    const auto old_path=root/L"legacy.snapshot";
    durable_snapshot::Store old_store(old_path,old_id);
    const auto legacy=states_for_test();
    CHECK(old_store.create(legacy)==S_OK);
    const auto old_bytes=read_bytes(old_path);
    size_t cursor=8;uint32_t schema=0;
    CHECK(read_u32(old_bytes,&cursor,&schema)&&schema==2);

    auto id=old_id;id.schema_version=3;
    const auto ledger=root.parent_path()/journal::detail::ledger_namespace(id)/
        fs::path(id.owner)/fs::path(id.token);
    fs::create_directories(ledger);
    id.storage_root=ledger.wstring();
    CHECK(journal::detail::valid_identity(id));
    auto wrong_namespace=id;
    wrong_namespace.storage_root=(root.parent_path()/L".liukai-installer-ledger"/
        fs::path(id.owner)/fs::path(id.token)).wstring();
    CHECK(!journal::detail::valid_identity(wrong_namespace));
    auto old_scope=id;old_scope.schema_version=2;
    CHECK(!journal::detail::valid_identity(old_scope));

    auto states=legacy;
    auto& machine=states[static_cast<size_t>(Resource::machine_com_x64)];
    machine.present=true;machine.created_by_lifecycle=true;machine.owner=L"machine-owner";
    machine.path=L"C:\\scope-install\\machine.dll";machine.threading_model=L"Apartment";
    machine.private_data={0,1,0xff,0,3,2};
    std::string rejected_image;
    CHECK(!durable_snapshot::detail::encode(old_id,states,&rejected_image));
    durable_snapshot::Store store(ledger/L"transaction.snapshot",id);
    CHECK(store.create(states)==S_OK);
    ResourceStates observed{};std::string present_hash;
    CHECK(store.read(id,&observed,&present_hash)==S_OK&&observed==states);
    CHECK(observed[6].private_data==machine.private_data);
    std::string absent_image,present_image,absent_hash;
    CHECK(durable_snapshot::detail::encode(id,states,&present_image));
    auto absent=states;absent[6]={};
    CHECK(durable_snapshot::detail::encode(id,absent,&absent_image));
    CHECK(durable_snapshot::detail::decode(absent_image,id,&observed,&absent_hash)==S_OK&&observed==absent);
    CHECK(absent_hash!=present_hash);
    // Old user-only bytes cannot authorize any machine snapshot or recovery.
    CHECK(durable_snapshot::detail::decode(old_bytes,id,&observed,nullptr)==HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    CHECK(observed==ResourceStates{});
    CHECK(durable_snapshot::detail::decode(present_image,old_id,&observed,nullptr)==HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    CHECK(store.create(absent)!=S_OK);
    CHECK(read_bytes(ledger/L"transaction.snapshot")==present_image);
    CHECK(read_bytes(old_path)==old_bytes);
    CHECK(old_store.read(old_id,&observed)==S_OK&&observed==legacy);
    return true;
}

int wmain(int argc,wchar_t** argv) {
    std::cout.setf(std::ios::unitbuf);
    if(argc!=2||!fs::path(argv[1]).is_absolute()) {
        std::wcerr<<L"usage: installer-durable-transaction-snapshot-tests.exe <absolute-new-sandbox>\n";return 2;
    }
    const fs::path sandbox=argv[1];std::error_code ec;
    if(fs::exists(sandbox,ec)||ec||!fs::create_directory(sandbox,ec)||ec)return 2;
    if(!test_machine_scope_exact_snapshot_and_legacy_isolation(sandbox)||
       !test_identity_bound_exact_snapshot_roundtrip(sandbox)||
       !test_existing_snapshot_is_never_replaced(sandbox)||
       !test_schema_checksum_order_and_tail_are_strict(sandbox)||
       !test_prior_schema_ledger_does_not_block_fresh_transaction(sandbox)||
       !test_publish_failures_leave_no_snapshot_or_temp_files(sandbox)||
       !test_postpublish_readback_failure_retains_published_path(sandbox)||
       !test_invalid_utf16_and_oversized_state_refuse_create(sandbox))return 1;
    std::cout<<"durable transaction snapshot tests passed\n";return 0;
}
