#include "diagnostics/output_policy.h"
#include <iostream>
#include <string>
int main(int argc,char** argv) {
    if(argc!=2) return 1;
    const std::string text=argv[1];
    const std::wstring wide(text.begin(),text.end());
    if(!SetEnvironmentVariableW(L"BONE_EATER_QUIET_DIAGNOSTICS",text=="absent" ? nullptr : wide.c_str())) return 2;
    const bool expected=text!="1";
    if(bone_eater::diagnostics::optionalOutputEnabled()!=expected) return 3;
    if(!SetEnvironmentVariableW(L"BONE_EATER_QUIET_DIAGNOSTICS",expected ? L"1" : L"0")) return 4;
    if(bone_eater::diagnostics::optionalOutputEnabled()!=expected) return 5;
    std::cout << "Exact opt-in and immutable policy passed: " << text << '\n';
    return 0;
}
