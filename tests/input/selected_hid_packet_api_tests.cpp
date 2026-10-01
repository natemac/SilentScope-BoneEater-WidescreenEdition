#include "input/selected_hid_packet.h"
#include "input/selected_hid_api.h"
#include "input/selected_hid_api_status.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace bone_eater::input;
namespace {
void check(bool condition, const char* expression, int line) {
    if (!condition) throw std::runtime_error(std::string(expression) + " at line " + std::to_string(line));
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

GunSourceOutput active(std::uint16_t x, std::uint16_t y, GunButtons buttons = {}) {
    GunSourceOutput value;
    value.mode = GunSourceMode::SelectedHid;
    value.state = GunSourceState::Active;
    value.aim = {true, x, y, GunSourceMode::SelectedHid, {7, 50, {}}};
    value.buttons = buttons;
    value.armed = value.sourceUsable = true;
    return value;
}
void packetChangesOnlyGunFields() {
    std::array<std::uint8_t, selectedNddPacketBytes + 4> bytes;
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(0xA0u + i);
    const auto before = bytes;
    const auto input = active(0x1234, 0xABCD, {true, true, false});
    CHECK(packSelectedNddGun(bytes.data(), selectedNddPacketBytes, input));
    CHECK(bytes[4] == ((before[4] & 0x1F) | 0xA0));
    CHECK(bytes[8] == 0xED && bytes[9] == 0xCB && bytes[10] == 0xAB && bytes[11] == 0xCD);
    for (std::size_t i = 0; i < bytes.size(); ++i)
        if (i != 4 && (i < 8 || i > 11)) CHECK(bytes[i] == before[i]);
    CHECK(input.aim.x == 0x1234 && input.aim.y == 0xABCD && input.buttons.trigger);
}
void endpointsAndIndependentBits() {
    for (unsigned bits = 0; bits < 8; ++bits) for (const auto x : {std::uint16_t(0), std::uint16_t(65535)}) {
        std::array<std::uint8_t, selectedNddPacketBytes> bytes;
        bytes.fill(0xFF);
        CHECK(packSelectedNddGun(bytes.data(), bytes.size(), active(x, static_cast<std::uint16_t>(65535u-x),
            {(bits&1)!=0, (bits&2)!=0, (bits&4)!=0})));
        CHECK((bytes[4]&0x1F) == 0x1F);
        CHECK((bytes[4]&0xE0) == ((bits&1?0x20:0)|(bits&2?0x80:0)|(bits&4?0x40:0)));
        CHECK(bytes[8] == (x ? 0 : 255) && bytes[9] == bytes[8]);
        CHECK(bytes[10] == bytes[8] && bytes[11] == bytes[8]);
    }
}
void nonusableSamplesHoldAimAndReleaseEvenMalformedHeldBits() {
    for (unsigned mutation = 0; mutation < 5; ++mutation) {
        auto value = active(0x1234, 0xABCD, {true,true,true});
        switch (mutation) {
            case 0: value.armed = false; break;
            case 1: value.sourceUsable = false; break;
            case 2: value.state = GunSourceState::Stale; break;
            case 3: value.aim.source = GunSourceMode::Legacy; break;
            default: value.aim.known = false; break;
        }
        std::array<std::uint8_t, selectedNddPacketBytes> bytes;
        bytes.fill(0xFF);
        CHECK(packSelectedNddGun(bytes.data(), bytes.size(), value));
        CHECK(bytes[4] == 0x1F);
        if (value.aim.known) CHECK(bytes[8] == 0xED && bytes[9] == 0xCB && bytes[10] == 0xAB && bytes[11] == 0xCD);
        else CHECK(bytes[8] == 0x80 && bytes[9] == 0 && bytes[10] == 0x7F && bytes[11] == 0xFF);
    }
}
void invalidPacketNeverPartiallyChangesMemory() {
    std::array<std::uint8_t, selectedNddPacketBytes> bytes;
    bytes.fill(0xA5);
    const auto original = bytes;
    CHECK(!packSelectedNddGun(nullptr, bytes.size(), active(1,2)));
    CHECK(!packSelectedNddGun(bytes.data(), bytes.size()-1, active(1,2)) && bytes == original);
    auto legacy = active(1,2); legacy.mode = GunSourceMode::Legacy;
    CHECK(!packSelectedNddGun(bytes.data(), bytes.size(), legacy) && bytes == original);
}
SelectedGunApiCheck checkJson(const char* input, SelectedGunApiFields fields) {
    rapidjson::Document document;
    document.Parse(input);
    CHECK(!document.HasParseError());
    return preflightSelectedGunWrite(document, fields);
}
void entireMixedRequestsAreRejectedBeforeServiceSideEffects() {
    for (const char* name : {"Gun Pressed", "Scope Right", "Scope Left"}) {
        const auto json = std::string("[[\"Service\",1],[\"") + name + "\",true],[\"Start\",0]]";
        CHECK(checkJson(json.c_str(), SelectedGunApiFields::Buttons) == SelectedGunApiCheck::GunOwned);
        // This represents the exact upcoming request gate: no native write loop
        // may start merely because an earlier service item was valid.
        unsigned writes = 0;
        if (checkJson(json.c_str(), SelectedGunApiFields::Buttons) == SelectedGunApiCheck::Allowed) ++writes;
        CHECK(writes == 0);
    }
    for (const char* name : {"Gun X", "Gun Y"}) {
        const auto json = std::string("[[\"Unrelated Analog\",0.5],[\"") + name + "\",0.3]]";
        CHECK(checkJson(json.c_str(), SelectedGunApiFields::Analogs) == SelectedGunApiCheck::GunOwned);
    }
}
void ordinaryValidControlsRemainAllowed() {
    CHECK(checkJson("[]", SelectedGunApiFields::Buttons) == SelectedGunApiCheck::Allowed);
    CHECK(checkJson("[[\"Service\",true],[\"Start\",0],[\"Coin Mech\",0.5]]", SelectedGunApiFields::Buttons) == SelectedGunApiCheck::Allowed);
    CHECK(checkJson("[[\"Other Axis\",0.25],[\"Other\",1]]", SelectedGunApiFields::Analogs) == SelectedGunApiCheck::Allowed);
    CHECK(!selectedGunApiField(SelectedGunApiFields::Buttons, "Gun X"));
    CHECK(!selectedGunApiField(SelectedGunApiFields::Analogs, "Gun Pressed"));
    CHECK(!selectedGunApiField(SelectedGunApiFields::Buttons, "Scope Right extra"));
}
void malformedOrNulNamesCannotBypassOwnership() {
    for (const auto fields : {SelectedGunApiFields::Buttons, SelectedGunApiFields::Analogs}) {
        for (const char* malformed : {"{}", "[1]", "[[]]", "[[\"Service\"]]", "[[1,0]]",
                "[[\"Service\",\"1\"]]", "[[\"Gun X\\u0000suffix\",0.5]]",
                "[[\"Scope Right\\u0000suffix\",1]]", "[[\"Service\",1],[null]]"})
            CHECK(checkJson(malformed, fields) == SelectedGunApiCheck::Malformed);
    }
    CHECK(checkJson("[[\"Other\",true]]", SelectedGunApiFields::Analogs) == SelectedGunApiCheck::Malformed);
    CHECK(checkJson("[]", static_cast<SelectedGunApiFields>(-1)) == SelectedGunApiCheck::Malformed);
}
void cachedReadbackPreservesProvenanceWithoutRefreshingButtons() {
    using namespace std::chrono_literals;
    rapidjson::Document document;
    SelectedHidBridgeStatus status;
    status.configured = status.selected = true;
    status.cabinetGeneration = 99;
    auto& aim = status.aim;
    aim.sourceMode = GunSourceMode::SelectedHid;
    aim.valid = aim.aimKnown = aim.armed = aim.triggerHeld = aim.scopeRightHeld = true;
    aim.sourceState = GunSourceState::Active;
    aim.rawX = 12345; aim.rawY = 54321;
    aim.sourceIdentity = {7, 12, GunSourceClock::time_point(1s)};
    aim.sampledAt = aim.sourceIdentity.receivedAt + 50ms;
    aim.sourceMaximumAge = 100ms;
    aim.sourceConsumptionGeneration = 77;
    const auto fresh = selectedInputApiStatus(status, aim.sourceIdentity.receivedAt + 100ms, document.GetAllocator());
    const auto expired = selectedInputApiStatus(status, aim.sourceIdentity.receivedAt + 101ms, document.GetAllocator());
    CHECK(!fresh["fresh_hid_poll"].GetBool());
    CHECK(fresh["cabinet_generation"].GetUint64() == 99);
    CHECK(fresh["runtime_lifecycle_code"].IsNull());
    CHECK(fresh["last_publication"]["receipt_eligible_at_query"].GetBool());
    CHECK(!expired["last_publication"]["receipt_eligible_at_query"].GetBool());
    CHECK(expired["last_publication"]["valid_at_publication"].GetBool());
    CHECK(expired["last_publication"]["trigger_at_publication"].GetBool());
    CHECK(expired["last_publication"]["source_generation"].GetUint64() == 12);
    CHECK(expired["last_publication"]["receipt_steady_ticks"].GetInt64() == aim.sourceIdentity.receivedAt.time_since_epoch().count());
    CHECK(expired["last_publication"]["x"].GetUint() == 12345);
    const auto future = selectedInputApiStatus(status, aim.sampledAt - 1ms, document.GetAllocator());
    CHECK(!future["last_publication"]["receipt_eligible_at_query"].GetBool());
    const auto legacy = selectedInputApiStatus({}, aim.sampledAt, document.GetAllocator());
    CHECK(!legacy["selected"].GetBool() && !legacy["last_publication"]["known"].GetBool());
}
}
int main() {
    struct Test { const char* name; void(*run)(); };
    const Test tests[] {
        {"only gun fields and one inversion", packetChangesOnlyGunFields},
        {"axis endpoints and independent bits", endpointsAndIndependentBits},
        {"nonusable hold/release", nonusableSamplesHoldAimAndReleaseEvenMalformedHeldBits},
        {"packet bounds atomic rejection", invalidPacketNeverPartiallyChangesMemory},
        {"mixed request ownership preflight", entireMixedRequestsAreRejectedBeforeServiceSideEffects},
        {"unrelated control acceptance", ordinaryValidControlsRemainAllowed},
        {"malformed and embedded NUL rejection", malformedOrNulNamesCannotBypassOwnership},
        {"cached readback aging and provenance", cachedReadbackPreservesProvenanceWithoutRefreshingButtons},
    };
    for (const auto& test : tests) try { test.run(); }
        catch (const std::exception& error) { std::cerr << "FAIL " << test.name << ": " << error.what() << '\n'; return 1; }
    std::cout << "Passed " << sizeof(tests)/sizeof(tests[0]) << " selected gun packet/API cases\n";
}
