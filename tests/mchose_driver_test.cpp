// Exercise the feature-report driver with status replies captured from real receivers.
#include "../src/drivers/drivers.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <map>

namespace {
// MCHOSE L7 Ultra+ on the shared 5253:1020 receiver: its status block came back on report 0x12, 81%.
const std::vector<BYTE> l7UltraPlus = {
    0x12, 0xF9, 0xC8, 0xC7, 0xE9, 0xBF, 0xDD, 0xFA, 0xFA, 0xFF, 0xF6, 0xAE, 0xFF, 0xA9
};
// MCHOSE A7 Ultra (RE) (issue 832) on the same receiver: report 0x12 only echoes zeros while the
// block arrives on the short report 0x11 - 58%, link 09 (receiver, connected), not charging.
// Captured from the device on 2026-11-24 (105 consecutive identical replies).
const std::vector<BYTE> a7UltraRe = {
    0x11, 0xF9, 0xC8, 0xC7, 0xEF, 0xBE, 0xE5, 0xFA, 0xFA, 0xFF, 0xF6, 0xC5, 0xFF
};
// A report id that answers but carries nothing: the request echoed with an all-zero payload.
std::vector<BYTE> EmptyEcho(BYTE rid) { return { rid, 0xF9 }; }
// MCHOSE A7 Ultra (RE) plugged in over the USB-C data cable: it enumerates as 3837:4110 and answers
// on 0x11 and 0x12 with this frame - link 00 (cable), level 57, charge status 1. Captured from the
// device on 2026-11-24 with the cable connected.
const std::vector<BYTE> a7UltraReCable = {
    0x11, 0xF9, 0xC8, 0xC7, 0xEF, 0xBE, 0xE5, 0xFA, 0xFA, 0xFF, 0xFF, 0xC6, 0xFE, 0xC6, 0xFF
};

std::map<BYTE, std::vector<BYTE>> replies;
std::vector<BYTE> asked;

HidColl Coll(USHORT vid, USHORT pid) {
    HidColl c;
    c.vid = vid;
    c.pid = pid;
    c.caps.UsagePage = 0xFF01;
    c.caps.Usage = 1;
    c.caps.FeatureReportByteLength = 65;
    return c;
}

PollContext Poll(std::vector<HidColl> colls, std::map<BYTE, std::vector<BYTE>> status) {
    replies = status;
    asked.clear();
    PollContext ctx;
    ctx.colls = colls;
    PollMchose(ctx);
    return ctx;
}

PollContext Poll(USHORT vid, USHORT pid, std::map<BYTE, std::vector<BYTE>> status) {
    return Poll({ Coll(vid, pid) }, status);
}
}  // namespace

bool HidDev::OpenFeatureOnly(const HidColl&) { return true; }
void HidDev::Close() {}
bool HidDev::SetFeature(const std::vector<BYTE>& data) {
    assert(data.size() == 65 && data[1] == 0xF9);  // status command 0x06, inverted
    assert(std::all_of(data.begin() + 2, data.end(), [](BYTE b) { return b == 0xFF; }));
    asked.push_back(data[0]);
    return true;
}
bool HidDev::GetFeature(BYTE reportId, std::vector<BYTE>& out) {
    auto it = replies.find(reportId);
    if (it == replies.end()) return false;  // that report id does not answer at all
    out = it->second;
    out.resize(65, 0xFF);
    return true;
}
std::wstring HidProduct(const HidColl&) { return L"MCHOSE A7 Ultra(RE)"; }
std::wstring Hex4(USHORT v) {
    wchar_t text[8];
    swprintf(text, 8, L"%04X", v);
    return text;
}
int Clamp100(int v) { return std::clamp(v, 0, 100); }

int main() {
    // L7 Ultra+ answers on the long report only; the short report stays silent.
    auto ctx = Poll(0x5253, 0x1020, { { 0x12, l7UltraPlus } });
    assert(ctx.IsClaimed(0x5253, 0x1020) && ctx.out.size() == 1);
    assert(ctx.out.front().percent == 81 && !ctx.out.front().charging && !ctx.out.front().asleep);
    assert(ctx.out.front().via == L"2.4 GHz receiver" && ctx.out.front().key == L"mchose:1020");

    // A7 Ultra (RE): 0x12 only echoes zeros, the real block is on 0x11. The driver asks 0x11 first,
    // so the long report never decides the outcome.
    ctx = Poll(0x5253, 0x1020, { { 0x11, a7UltraRe }, { 0x12, EmptyEcho(0x12) } });
    assert(ctx.IsClaimed(0x5253, 0x1020) && ctx.out.size() == 1);
    const auto& mouse = ctx.out.front();
    assert(mouse.percent == 58 && !mouse.charging && !mouse.asleep);
    assert(mouse.via == L"2.4 GHz receiver" && mouse.name == L"MCHOSE A7 Ultra(RE)");
    assert(asked.front() == 0x11);

    // Both channels asleep: claimed, reported as asleep, no level.
    ctx = Poll(0x5253, 0x1020, { { 0x11, EmptyEcho(0x11) }, { 0x12, EmptyEcho(0x12) } });
    assert(ctx.out.size() == 1 && ctx.out.front().asleep);

    // Neither channel answers: another driver may still handle the device.
    assert(Poll(0x5253, 0x1020, {}).out.empty());

    // A different model behind the shared receiver must not inherit the whitelisted identity.
    auto otherModel = a7UltraRe;
    otherModel[4] ^= 1;  // payload byte 2 = model id, low byte
    assert(Poll(0x5253, 0x1020, { { 0x11, otherModel } }).out.empty());
    assert(Poll(0x5253, 0x1021, { { 0x11, a7UltraRe } }).out.empty());

    // Wired: connectMode 0 in the link byte, charging set.
    auto cable = l7UltraPlus;
    cable[10] = 0xFF;  // link byte 0 -> cable
    cable[12] = 0xFE;  // charge status 1
    ctx = Poll(0x5253, 0x1020, { { 0x12, cable } });
    assert(ctx.out.size() == 1 && ctx.out.front().via == L"USB cable" && ctx.out.front().charging);

    // A7 Ultra (RE) on the USB-C data cable: enumerates as 3837:4110, no receiver id in sight. It
    // keeps the receiver key so the battery history of one mouse stays in one series.
    ctx = Poll(0x3837, 0x4110, { { 0x11, a7UltraReCable } });
    assert(ctx.IsClaimed(0x3837, 0x4110) && ctx.out.size() == 1);
    const auto& wired = ctx.out.front();
    assert(wired.percent == 57 && wired.charging && !wired.asleep);
    assert(wired.via == L"USB cable" && wired.key == L"mchose:1020");
    assert(wired.name == L"MCHOSE A7 Ultra(RE)");
    // Wired firmware also answers on the long report, so a 0x11-less reply is still a level.
    auto cableOnLongReport = a7UltraReCable;
    cableOnLongReport[0] = 0x12;
    ctx = Poll(0x3837, 0x4110, { { 0x12, cableOnLongReport } });
    assert(ctx.out.size() == 1 && ctx.out.front().percent == 57);
    // The 3837 dongle list must not swallow the cable id: a 3837:4110 dongle is not probed twice.
    assert(Poll(0x3837, 0x4110, {}).out.empty());

    // Cable in with the receiver still plugged in: one row, not two.
    ctx = Poll({ Coll(0x3837, 0x4110), Coll(0x5253, 0x1020) }, { { 0x11, a7UltraReCable } });
    assert(ctx.out.size() == 1 && ctx.out.front().via == L"USB cable");

    // Existing mice identify themselves with VID 5253; Realtek receivers can return 3837 or 41E4.
    auto pairedRealtek = l7UltraPlus;
    assert(Poll(0x3837, 0x1001, { { 0x12, pairedRealtek } }).out.size() == 1);
    pairedRealtek[2] = 0xE4 ^ 0xFF;
    pairedRealtek[3] = 0x41 ^ 0xFF;
    assert(Poll(0x3837, 0x100C, { { 0x12, pairedRealtek } }).out.size() == 1);
    pairedRealtek[2] = 0x53 ^ 0xFF;
    pairedRealtek[3] = 0x52 ^ 0xFF;
    assert(Poll(0x5253, 0x1020, { { 0x12, pairedRealtek } }).out.size() == 1);
    puts("MCHOSE feature-report driver tests passed");
}
