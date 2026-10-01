#pragma once
// Unit fixture only: no logging dependency or external side effect.
template<class... Args> void log_info(const char*, const char*, Args&&...) {}
template<class... Args> void log_warning(const char*, const char*, Args&&...) {}
