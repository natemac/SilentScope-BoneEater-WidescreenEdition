#pragma once
#include "diagnostics/output_policy.h"
#include <cstdio>
#include <share.h>

namespace bone_eater::diagnostics {

// Returns only an output sink. A null result must never decide whether a
// functional hook/adapter is installed or marked failed. Callers may report
// one warning when optionalOutputEnabled() is true and this returns null.
template<typename WriteHeader>
FILE* openOptionalCsv(const wchar_t* path, WriteHeader writeHeader,
                      const wchar_t* mode = L"ab") noexcept {
    if (!optionalOutputEnabled()) return nullptr;
    FILE* file = nullptr;
    try {
        if (!CreateDirectoryW(L"desktop", nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return nullptr;
        file = _wfsopen(path, mode, _SH_DENYWR);
        if (!file) return nullptr;
        if (_fseeki64(file, 0, SEEK_END)) { std::fclose(file); return nullptr; }
        const auto length = _ftelli64(file);
        if (length < 0) { std::fclose(file); return nullptr; }
        if (!length) writeHeader(file);
        if (std::fflush(file) || std::ferror(file)) { std::fclose(file); return nullptr; }
        return file;
    } catch (...) {
        if (file) std::fclose(file);
        return nullptr;
    }
}

} // namespace bone_eater::diagnostics
