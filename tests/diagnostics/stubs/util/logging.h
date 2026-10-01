#pragma once
template<class... Args> void log_info(const char*, const char*, Args&&...) { ++fixture::infoCalls; }
template<class... Args> void log_warning(const char*, const char*, Args&&...) {
    ++fixture::warningCalls;
    if (fixture::throwWarning) throw 1;
}
