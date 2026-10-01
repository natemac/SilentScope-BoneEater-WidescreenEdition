#include "standalone/input_profile.h"
#include <chrono>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;
using bone_eater::standalone::loadInputProfile;
constexpr const char* legacy = R"({"schema_version":1,"mode":"legacy"})";
#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)

struct Files {
    fs::path root;
    std::vector<fs::path> files;
    Files() {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::absolute("profile-fixture-" + std::to_string(suffix));
        if (!fs::create_directory(root)) throw std::runtime_error("Fixture directory already exists");
    }
    fs::path write(const fs::path& name, const std::string& bytes) {
        const auto path = root / name;
        if (fs::exists(path)) throw std::runtime_error("Fixture already exists");
        std::ofstream out(path, std::ios::binary);
        out.exceptions(std::ios::badbit | std::ios::failbit);
        files.push_back(path);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.close();
        return path;
    }
    ~Files() {
        std::error_code error;
        for (const auto& file : files) fs::remove(file, error);
        fs::remove(root, error); // Empty owned directory only; never recursive.
    }
};
template<class F> void rejects(F action) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    CHECK(rejected);
}
int main() {
    try {
        Files fixture;
        const auto unicode = fixture.write(fs::path(L"profile-\u65e5\u672c-\u03a9.json"), legacy);
        CHECK(loadInputProfile(fixture.root, unicode.filename()).mode == bone_eater::input::GunSourceMode::Legacy);
        CHECK(loadInputProfile(fs::path("unused-directory"), unicode).mode == bone_eater::input::GunSourceMode::Legacy);
        CHECK(!fs::exists(unicode.filename())); // Relative lookup must use supplied root, not cwd.
        std::string exact(legacy);
        exact.resize(bone_eater::input::selectedHidConfigMaximumBytes, ' ');
        const auto limit = fixture.write("limit.json", exact);
        CHECK(loadInputProfile(fixture.root, limit).mode == bone_eater::input::GunSourceMode::Legacy);
        fixture.write("oversize.json", exact + " ");
        rejects([&] { loadInputProfile(fixture.root, "oversize.json"); });
        fixture.write("empty.json", "");
        fixture.write("malformed.json", "{bad}");
        fixture.write("trailing.json", std::string(legacy) + std::string(1, '\0') + "ignored");
        for (const auto* name : {"empty.json", "malformed.json", "trailing.json", "missing.json"})
            rejects([&] { loadInputProfile(fixture.root, name); });
        rejects([&] { loadInputProfile(fixture.root, fixture.root); });
        rejects([&] { loadInputProfile(fixture.root, ""); });
#ifdef _WIN32
        // These resolve to an existing valid fixture with the old join rule,
        // so rejection cannot accidentally pass because the file is missing.
        const auto driveRelative = fixture.root.root_name() / unicode.filename();
        const auto driveImplicit = unicode.root_directory() / unicode.relative_path();
        CHECK(fs::exists(fixture.root / driveRelative));
        CHECK(fs::exists(fixture.root / driveImplicit));
        rejects([&] { loadInputProfile(fixture.root, driveRelative); });
        rejects([&] { loadInputProfile(fixture.root, driveImplicit); });
#endif
        std::cout << "Passed Unicode/rooted path, bounded file, and invalid-profile checks\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
