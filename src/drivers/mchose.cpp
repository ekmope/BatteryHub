// MCHOSE wireless mice on the 2.4 GHz receiver or USB cable (VID 5253): L7 / L7 Pro / L7 Ultra,
// M7 / M7 Ultra, A7 / A7 Ultra, K7 (the receiver PID is 1020 for several of them, so the model is
// not told apart by PID; the product string is used for the name). The same protocol also runs on
// the Realtek ODM dongles with VID 3837 (A5 V2 Ultra receiver 3837:100C verified on real hardware;
// the A7 V2 Ultra+ receiver 3837:100B is requested in issue 227, and 1001 / 100A / 1012 appear as
// MCHOSE products in the protocol projects). Other 3837 products (the G3 V2, keyboards) use other
// protocols and are deliberately not probed here.
//   Status frame, identical layout on the short report 0x11 and on the long report 0x12:
//     request: [rid][06][00 ...], with EVERY payload byte inverted (^ 0xFF), padding included
//     reply:   [rid][F9 = 06 ^ FF][payload, inverted]
//     payload = paired vid:u16, model:u16, firmware:u32, link:u8, level:u8, charging:u8
//     link    = connectMode in bits 0-2 (0 = USB cable, 1 = receiver), connectStatus in bit 3
// Report 0x11 is asked first, then 0x12. Firmware that keeps the status block on the short report
// answers 0x11 with the real block and only echoes 0x12 with an all-zero payload (MCHOSE A7 Ultra
// (RE), 5253:1020, issue 832: 0x11 returned level 58 with link 09 while 0x12 returned zeros), and
// the L7 Ultra+ answers on 0x12. A non-zero reply whose paired id is this device is a status block;
// all-zero means the mouse itself is asleep, and then the last value is kept.
// The request must be sent again for every read attempt; reading again without asking returns
// the request itself.
// Protocol: github.com/alexfrih/mchose-linux (PROTOCOL.md, recovered from MCHOSE's M HUB driver),
// confirmed on a real MCHOSE L7 Ultra (5253:1020, receiver), MCHOSE A7 Ultra (RE) (5253:1020,
// receiver, 0x11 channel) and MCHOSE A5 V2 Ultra (3837:100C, receiver).
#include "drivers.h"

namespace {

const USHORT kVid = 0x5253;
const USHORT kVidRealtek = 0x3837;
// 3837 dongle PIDs seen in the wild; kept explicit so keyboards and the G3 V2 on the same vendor
// id are never probed with this protocol.
const USHORT kRealtekPids[] = { 0x100C, 0x100B, 0x100A, 0x1001, 0x1012 };
// Status is asked on the short report first, then on the long one (see the header comment).
const BYTE kReports[] = { 0x11, 0x12 };
const BYTE kCmdStatus = 0x06;

// Returns 1 for a status reply, 0 for a valid echo with an empty payload (mouse asleep), -1 otherwise.
int ParseStatus(const std::vector<BYTE>& r, BYTE rid, USHORT vid, USHORT pid, int& level, int& charging, int& link) {
    if (r.size() < 13 || r[0] != rid || r[1] != (kCmdStatus ^ 0xFF)) return -1;
    BYTE p[11];
    for (int i = 0; i < 11; i++) p[i] = r[2 + i] ^ 0xFF;
    bool allZero = true;
    for (BYTE b : p)
        if (b) allZero = false;
    if (allZero) return 0;
    const USHORT pairedVid = (USHORT)(p[0] | (p[1] << 8));
    const USHORT model = (USHORT)(p[2] | (p[3] << 8));
    // The shared 5253:1020 receiver reports the Realtek chipset id as the paired vid, never 5253:
    // 3837:4016 on the L7 Ultra+ and 3837:4110 on the A7 Ultra (RE). Models on that receiver are
    // whitelisted so a different device behind the same receiver PID is not guessed at.
    const bool sharedReceiver = vid == kVid && pid == 0x1020 && pairedVid == kVidRealtek && (model == 0x4016 || model == 0x4110);
    if (pairedVid != vid && !(vid == kVidRealtek && pairedVid == 0x41E4) && !sharedReceiver) return -1;
    link = p[8];
    level = p[9];
    charging = p[10];
    return level <= 100 ? 1 : -1;
}

int Query(HidDev& d, const HidColl& c, BYTE rid, USHORT vid, USHORT pid, int& level, int& charging, int& link) {
    std::vector<BYTE> req(c.featLen(), 0xFF);  // inverted zero padding
    req[0] = rid;
    req[1] = kCmdStatus ^ 0xFF;
    int state = -1;
    for (int attempt = 0; attempt < 5 && state != 1; attempt++) {
        if (attempt) Sleep(100);
        std::vector<BYTE> r;
        if (!d.SetFeature(req)) break;
        Sleep(120);
        if (!d.GetFeature(rid, r)) continue;
        int st = ParseStatus(r, rid, vid, pid, level, charging, link);
        if (st > state) state = st;
    }
    return state;
}

void PollProduct(PollContext& ctx, USHORT vid, USHORT pid) {
    for (const HidColl* c : ctx.Find(vid, pid, [](const HidColl& c) { return c.page() == 0xFF01 && c.usage() == 0x0001 && c.featLen() >= 65; })) {
        HidDev d;
        if (!d.OpenFeatureOnly(*c)) continue;
        int state = -1, level = 0, charging = 0, link = 0;
        for (BYTE rid : kReports) {
            int st = Query(d, *c, rid, vid, pid, level, charging, link);
            if (st == 1) {
                state = 1;
                break;
            }
            if (st == 0 && state < 0) state = 0;  // echoed, but empty: asleep
        }
        if (state < 0) continue;  // not answering this protocol
        DeviceStatus s;
        s.key = L"mchose:" + Hex4(pid);
        s.name = HidProduct(*c);
        if (s.name.empty()) s.name = L"MCHOSE mouse";
        s.kind = DeviceKind::Mouse;
        if (state == 1) {
            s.percent = Clamp100(level);
            s.charging = charging != 0;
            // link byte: connectMode in bits 0-2, connectStatus in bit 3 (verified: 0x09 = receiver,
            // connected). Only a clear 0 means the cable; anything else stays "receiver".
            s.via = (link & 0x07) == 0 ? L"USB cable" : L"2.4 GHz receiver";
        } else {
            s.asleep = true;
            s.via = L"2.4 GHz receiver";
        }
        ctx.Add(s, vid, pid);
        break;
    }
}

}  // namespace

void PollMchose(PollContext& ctx) {
    for (auto [vid, pid] : ctx.Products(kVid)) {
        PollProduct(ctx, vid, pid);
    }
    for (USHORT pid : kRealtekPids) {
        if (ctx.IsClaimed(kVidRealtek, pid)) continue;
        // The vid field differs between 3837 dongles: 41E4 on the A5 V2 Ultra (100C), 3837 on the
        // K7 Ultra (1001).
        PollProduct(ctx, kVidRealtek, pid);
    }
}
