#pragma once
// Fixture only: inspect installation reachability; never patch an executable.
namespace detour {
template<class Function> bool trampoline_try(Function target, Function replacement, Function* original) {
    ++fixture::hookCalls;
    fixture::validHookSeed = target && replacement && original && *original == target;
    return fixture::validHookSeed;
}
}
