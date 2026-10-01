#define main unused_standalone_entry
#include "../../src/standalone/main.cpp"
#undef main
#include <sstream>

int main_implementation(int,char**) { throw std::runtime_error("Unexpected native entry"); }
namespace bone_eater::input {
void initializeSelectedHidBridge(SelectedHidConfiguration) { throw std::runtime_error("Unexpected device initialization"); }
SelectedHidRuntimeCommand stopSelectedHidBridge(std::chrono::milliseconds) noexcept { return SelectedHidRuntimeCommand::Stopped; }
SelectedHidRuntimeDiagnostic SelectedHidRuntime::diagnose(const SelectedHidConfiguration& c) {
    // The real profile parser already validated schema; no device is opened.
    return c.mode==GunSourceMode::Legacy ? SelectedHidRuntimeDiagnostic::Legacy : SelectedHidRuntimeDiagnostic::Ready;
}
}
int main(int argc,char** argv) {
    const bool realProfile=argc>2 && std::string(argv[1])=="--native-precision-bypass";
    if(argc!=2 && !realProfile) return 1;
    // Isolate this fixture process from any user's unrelated experimental flags.
    LPWCH block=GetEnvironmentStringsW(); if(!block) return 1;
    std::vector<std::wstring> clearNames;
    for(const wchar_t* p=block; *p; p+=std::wcslen(p)+1) {
        std::wstring_view entry(p); const auto equals=entry.find(L'=');
        if(equals!=std::wstring_view::npos && entry.substr(0,11)==L"BONE_EATER_")
            clearNames.emplace_back(entry.substr(0,equals));
    }
    FreeEnvironmentStringsW(block);
    for(const auto& name:clearNames) if(!SetEnvironmentVariableW(name.c_str(),nullptr)) return 1;
    if(realProfile) {
        std::ostringstream captured; auto* previous=std::cerr.rdbuf(captured.rdbuf());
        const int result=unused_standalone_entry(argc,argv); std::cerr.rdbuf(previous);
        if(result!=1 || captured.str().find("currently supports legacy input only")==std::string::npos) return 1;
        std::cout<<"Precision CLI rejects a syntactically valid selected profile before game/device entry\n";
        return 0;
    }
    std::vector<std::string> args{"fixture","--diagnose","--native-input-ownership-observe"};
    const std::string mode=argv[1];
    const bool precision=mode.starts_with("precision-");
    const bool accepted=precision ? (mode=="precision-full" || mode=="precision-quiet" || mode=="precision-inherited") :
        (mode=="desktop" || mode=="quiet");
    if(precision) {
        args={"fixture","--diagnose"};
        if(mode=="precision-inherited") SetEnvironmentVariableW(L"BONE_EATER_NATIVE_PRECISION_BYPASS",L"1");
        else args.emplace_back("--native-precision-bypass");
        if(mode!="precision-missing") {
            args.insert(args.end(),{"-2Display","--native-wide","--native-hud-fit"});
            args.emplace_back(mode=="precision-wide"?"--native-input-wide":"--native-input-desktop");
        }
    } else if(mode!="missing") args.emplace_back(mode=="wide" ? "--native-input-wide" : "--native-input-desktop");
    if(mode=="quiet" || mode=="precision-quiet") args.emplace_back("--quiet-diagnostics");
    std::vector<char*> pointers; for(auto& a:args) pointers.push_back(a.data());
    // A validated flag reaches the inert fixture directory's deliberately
    // missing-file preflight (2). No native game or device entry is possible.
    const int result=unused_standalone_entry(static_cast<int>(pointers.size()),pointers.data());
    if(result!=(accepted ? 2 : 1)) return 1;
    if((mode=="quiet" || mode=="precision-quiet") && bone_eater::diagnostics::optionalOutputEnabled()) return 1;
    std::cout<<"Ownership CLI prerequisite/quiet case passed: "<<mode<<'\n';
}
