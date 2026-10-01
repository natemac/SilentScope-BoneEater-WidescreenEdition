#pragma once
// Unit fixture only: hook installation is intentionally unavailable.
namespace detour {
template<class Function> bool trampoline_try(Function, Function, Function*) { return false; }
}
