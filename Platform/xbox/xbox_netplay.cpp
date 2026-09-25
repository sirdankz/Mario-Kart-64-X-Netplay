// Copyright (c) 2026 sirdankz
// SPDX-License-Identifier: GPL-3.0-only
// See NETPLAY-LICENSE.md for license scope.
#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

/* RXDK's NetworkServer template uses winsockx.h this way alongside xtl.h. */
#ifndef _INC_WINDOWS
#define _INC_WINDOWS
#endif
#include <winsockx.h>

#include "netplay_protocol.h"
#include "xbox_netplay.h"
#include "kos.h"
#include "gfx_rendering_api.h"
#include "gl_fast_vert.h"

/* MK64_CROSSPLAY_COMPONENT_DIAG_R15_NET */
extern "C" void mk64_crossplay_component_diag(unsigned int *out, int cap);
static void mkdiag_write_component_snapshot(void);

extern "C" struct GfxRenderingAPI gfx_nv2a_api;
extern "C" dc_fast_t *pvr_reserve(int kind, size_t n);
extern "C" void gfx_pvr_set_blend(uint8_t kind);

extern "C" unsigned int xbox_netplay_state_hash(void);
extern "C" int xbox_crossplay_state_pack(unsigned char *out,int cap);
extern "C" void xbox_crossplay_state_apply(const unsigned char *in,int len);

namespace {

enum NetMode {
    NET_OFFLINE = 0,
    NET_HOST,
    NET_JOIN_LAN,
    NET_JOIN_DIRECT
};

struct NetConfig {
    NetMode mode;
    unsigned desired_players;
    unsigned local_players;
    unsigned timeout_seconds;
    char address[64];
};

struct PeerState {
    bool used;
    bool ready;
    bool acked;
    bool gameplay_seen;
    sockaddr_in addr;
    uint8_t nonce[16];
    unsigned slot;
    unsigned local_count;
    unsigned platform;
    DWORD last_received;
    DWORD last_offer;
    mknet::Latency latency;
};

static const unsigned kPort = 6464;
static const char *kConfigPath = "D:/mk64_netplay.cfg";
static const char *gLogPath = "D:/mk64-netplay.log";

static FILE *gLog = 0;
static bool gDiagnosticsEnabled = false; /* R41: logging/trace default OFF every boot. */
static bool gHosting = false;
static bool gLanJoin = false;
static bool gActive = false;
static bool gFailed = false;
static bool gHostSessionKnown = false;
static bool gStartSent = false;
static bool gJoinRejected = false;
static SOCKET gSocket = INVALID_SOCKET;
static uint8_t gSession[16];
static uint8_t gNonce[16];
static sockaddr_in gHostPeer;
static sockaddr_in gJoinTarget;
static PeerState gPeers[mknet::MAX_PLAYERS - 1];
static unsigned gAssignedSlot = 0;
static unsigned gPlayerCount = 1;
static unsigned gLocalSlot = 0;
static unsigned gLocalCount = 1;
static unsigned gDesiredPlayers = 2;
static unsigned gChosenDelay = 4;
static bool gSessionSplit = false;
static bool gCrossplay = false;
static unsigned gHostPlatform = mknet::PLATFORM_UNKNOWN;
static bool gHaveHostCommit = false;
static uint32_t gHostCommitFrame = 0;

struct CrossStateSlot {
    uint32_t frame;
    bool present;
    uint8_t data[mknet::CROSS_STATE_BYTES];
};
enum { CROSS_STATE_HISTORY = 32 };
static CrossStateSlot gHostStates[CROSS_STATE_HISTORY];
static unsigned gStateSyncRx = 0;
static unsigned gStateSyncApplied = 0;
static int gAppliedHostRaceState = -1;
static uint32_t gAppliedHostStateHash = 0;
static bool gAppliedHostStateHashValid = false;

/* R5 crossplay: Stream4 remains the proven Xbox 360 input transport, while
 * both architectures now publish the same canonical semantic state hash.
 * The hash serializes values in a fixed byte order and never hashes pointers,
 * structure padding, or native-endian memory. */
static mknet::BootBarrier gBoot;
static mknet::Stream4 gStream;
static bool gFirstGameplayInput = false;
static bool gMenuSync = true;
static const uint32_t kMenuLeadFrames = 2;
static unsigned gNetRxInput = 0;
static unsigned gNetRxInputReject = 0;
static unsigned gNetTxInput = 0;
static unsigned gNetTxInputFail = 0;

static bool gStackStarted = false;
static char gLocalAddressText[64] = "LOCAL IP: WAITING";


/* MK64_R42_OG_PUBLIC_IP_UI */
static bool gR42PublicIpVisible = true;
static char gR42PublicIpText[64] = "PUBLIC IP: UNAVAILABLE";


/* -------------------------------------------------------------------------
 * R2 PRE-GAME UI
 *
 * Draw through the port's existing NV2A backend instead of touching the D3D
 * device behind its back.  That means the menu uses the same frame begin/end
 * path as MK64 and leaves the renderer in a known state when the game starts.
 */
#define UI_KIND_OP 0

static void ui_put(dc_fast_t *v, float x, float y, uint32_t argb) {
    v->vert.x = x; v->vert.y = y; v->vert.z = 0.5f;
    v->rhw = 1.0f;
    v->color.packed = argb;
    v->pad0.vertindex = 0;
    v->texture.u = 0.0f; v->texture.v = 0.0f;
    v->flags = 0;
}

static void ui_quad(float x0, float y0, float x1, float y1, uint32_t argb) {
    dc_fast_t *v = pvr_reserve(UI_KIND_OP, 6);
    if (!v) return;
    ui_put(&v[0], x0, y0, argb); ui_put(&v[1], x1, y0, argb); ui_put(&v[2], x1, y1, argb);
    ui_put(&v[3], x0, y0, argb); ui_put(&v[4], x1, y1, argb); ui_put(&v[5], x0, y1, argb);
}

static const uint8_t *ui_glyph(char c) {
#define GLYPH(ch,a,b,c,d,e,f,g) case ch: { static const uint8_t r[7]={a,b,c,d,e,f,g}; return r; }
    switch (c) {
        GLYPH('A',0x0E,0x11,0x11,0x1F,0x11,0x11,0x11)
        GLYPH('B',0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E)
        GLYPH('C',0x0F,0x10,0x10,0x10,0x10,0x10,0x0F)
        GLYPH('D',0x1E,0x11,0x11,0x11,0x11,0x11,0x1E)
        GLYPH('E',0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F)
        GLYPH('F',0x1F,0x10,0x10,0x1E,0x10,0x10,0x10)
        GLYPH('G',0x0F,0x10,0x10,0x13,0x11,0x11,0x0F)
        GLYPH('H',0x11,0x11,0x11,0x1F,0x11,0x11,0x11)
        GLYPH('I',0x1F,0x04,0x04,0x04,0x04,0x04,0x1F)
        GLYPH('J',0x07,0x02,0x02,0x02,0x12,0x12,0x0C)
        GLYPH('K',0x11,0x12,0x14,0x18,0x14,0x12,0x11)
        GLYPH('L',0x10,0x10,0x10,0x10,0x10,0x10,0x1F)
        GLYPH('M',0x11,0x1B,0x15,0x15,0x11,0x11,0x11)
        GLYPH('N',0x11,0x19,0x15,0x13,0x11,0x11,0x11)
        GLYPH('O',0x0E,0x11,0x11,0x11,0x11,0x11,0x0E)
        GLYPH('P',0x1E,0x11,0x11,0x1E,0x10,0x10,0x10)
        GLYPH('Q',0x0E,0x11,0x11,0x11,0x15,0x12,0x0D)
        GLYPH('R',0x1E,0x11,0x11,0x1E,0x14,0x12,0x11)
        GLYPH('S',0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E)
        GLYPH('T',0x1F,0x04,0x04,0x04,0x04,0x04,0x04)
        GLYPH('U',0x11,0x11,0x11,0x11,0x11,0x11,0x0E)
        GLYPH('V',0x11,0x11,0x11,0x11,0x11,0x0A,0x04)
        GLYPH('W',0x11,0x11,0x11,0x15,0x15,0x15,0x0A)
        GLYPH('X',0x11,0x11,0x0A,0x04,0x0A,0x11,0x11)
        GLYPH('Y',0x11,0x11,0x0A,0x04,0x04,0x04,0x04)
        GLYPH('Z',0x1F,0x01,0x02,0x04,0x08,0x10,0x1F)
        GLYPH('0',0x0E,0x11,0x13,0x15,0x19,0x11,0x0E)
        GLYPH('1',0x04,0x0C,0x04,0x04,0x04,0x04,0x0E)
        GLYPH('2',0x0E,0x11,0x01,0x02,0x04,0x08,0x1F)
        GLYPH('3',0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E)
        GLYPH('4',0x02,0x06,0x0A,0x12,0x1F,0x02,0x02)
        GLYPH('5',0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E)
        GLYPH('6',0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E)
        GLYPH('7',0x1F,0x01,0x02,0x04,0x08,0x08,0x08)
        GLYPH('8',0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E)
        GLYPH('9',0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E)
        GLYPH('-',0x00,0x00,0x00,0x1F,0x00,0x00,0x00)
        GLYPH('.',0x00,0x00,0x00,0x00,0x00,0x0C,0x0C)
        GLYPH(':',0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00)
        GLYPH('/',0x01,0x02,0x02,0x04,0x08,0x08,0x10)
        GLYPH('>',0x10,0x08,0x04,0x02,0x04,0x08,0x10)
        GLYPH('^',0x04,0x0A,0x11,0x00,0x00,0x00,0x00)
        GLYPH('?',0x0E,0x11,0x01,0x02,0x04,0x00,0x04)
        GLYPH('(',0x02,0x04,0x08,0x08,0x08,0x04,0x02)
        GLYPH(')',0x08,0x04,0x02,0x02,0x02,0x04,0x08)
        default: { static const uint8_t blank[7]={0,0,0,0,0,0,0}; return blank; }
    }
#undef GLYPH
}

static void ui_text(float x, float y, const char *text, float scale, uint32_t argb) {
    if (!text) return;
    const float advance = 6.0f * scale;
    for (const char *p = text; *p; ++p, x += advance) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        if (c == ' ') continue;
        const uint8_t *rows = ui_glyph(c);
        for (int ry = 0; ry < 7; ++ry) {
            uint8_t bits = rows[ry];
            for (int rx = 0; rx < 5; ++rx) {
                if (bits & (1u << (4-rx))) {
                    float px = x + rx * scale, py = y + ry * scale;
                    ui_quad(px, py, px + scale, py + scale, argb);
                }
            }
        }
    }
}

static void ui_screen(const char *title,
                      const char *a, const char *b, const char *c, const char *d,
                      const char *footer) {
    struct GfxRenderingAPI *r = &gfx_nv2a_api;
    r->start_frame();
    r->set_depth_test(0);
    r->set_depth_mask(0);
    r->select_texture(0, 0);
    gfx_pvr_set_blend(UI_KIND_OP);

    /* dark blue panel + gold separator, matching the 360 menu's feel */
    ui_quad(0, 0, 640, 480, 0xFF102030);
    ui_quad(36, 92, 604, 96, 0xFFFFD050);
    ui_text(40, 42, title ? title : "MARIO KART 64 - ONLINE", 3.0f, 0xFFFFD050);
    ui_text(50, 130, a ? a : "", 2.0f, 0xFFFFFFFF);
    ui_text(50, 190, b ? b : "", 2.0f, 0xFFFFFFFF);
    ui_text(50, 250, c ? c : "", 2.0f, 0xFF90D0FF);
    ui_text(50, 310, d ? d : "", 2.0f, 0xFF90D0FF);
    ui_text(50, 418, footer ? footer : "", 2.0f, 0xFFB8B8B8);

    r->end_frame();
    r->finish_render();
}

static uint32_t ui_buttons_now(void) {
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (!dev) return 0;
    cont_state_t *st = (cont_state_t *)maple_dev_status(dev);
    return st ? st->buttons : 0;
}

static const uint32_t kUiYButton = (1U << 9);
static uint32_t gUiPrevButtons = 0;
static uint32_t ui_pressed(void) {
    uint32_t now = ui_buttons_now();
    uint32_t edge = now & ~gUiPrevButtons;
    gUiPrevButtons = now;
    return edge;
}
static void ui_consume(void) { gUiPrevButtons = ui_buttons_now(); }

static bool ui_second_controller_connected(void) {
    return maple_enum_type(1, MAPLE_FUNC_CONTROLLER) != 0;
}

static void log_open(void) {
    if (!gDiagnosticsEnabled) return;
    if (gLog) return;
    gLogPath = "D:/mk64-netplay.log";
    gLog = fopen(gLogPath, "w");
    if (!gLog) {
        /* T: is already used by this port for writable title save data. */
        gLogPath = "T:/mk64-netplay.log";
        gLog = fopen(gLogPath, "w");
    }
}


/* MK64 R41 OG log privacy.
 * Disk/debug diagnostics may be manually enabled, but dotted IPv4 addresses
 * are scrubbed before printf/fputs so exported logs cannot contain endpoints.
 */
static int r41_og_digit(char c) {
    return c >= '0' && c <= '9';
}
static int r41_og_ipv4_at(const char *s, size_t *used) {
    const char *p = s;
    int part;
    if (!s || !r41_og_digit(*p)) return 0;
    for (part = 0; part < 4; ++part) {
        unsigned value = 0;
        int digits = 0;
        if (!r41_og_digit(*p)) return 0;
        while (r41_og_digit(*p)) {
            if (digits >= 3) return 0;
            value = value * 10u + (unsigned)(*p - '0');
            ++digits;
            ++p;
        }
        if (value > 255u) return 0;
        if (part != 3) {
            if (*p != '.') return 0;
            ++p;
        }
    }
    if (*p == '.' || r41_og_digit(*p)) return 0;
    if (used) *used = (size_t)(p - s);
    return 1;
}
static void r41_og_scrub_ipv4(const char *src, char *dst, size_t cap) {
    size_t i = 0, o = 0;
    static const char tag[] = "[IP]";
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = 0; return; }
    while (src[i] && o + 1 < cap) {
        size_t used = 0;
        int boundary = (i == 0) ||
            (!r41_og_digit(src[i - 1]) && src[i - 1] != '.');
        if (boundary && r41_og_digit(src[i]) &&
            r41_og_ipv4_at(src + i, &used)) {
            size_t k;
            for (k = 0; tag[k] && o + 1 < cap; ++k) dst[o++] = tag[k];
            i += used;
        } else {
            dst[o++] = src[i++];
        }
    }
    dst[o] = 0;
}

static void log_line(const char *fmt, ...) {
    if (!gDiagnosticsEnabled) return;
    char line[512];
    char safe[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    line[sizeof(line) - 1] = 0;
    r41_og_scrub_ipv4(line, safe, sizeof(safe));
    printf("%s", safe);
    if (gLog) {
        fputs(safe, gLog);
        fflush(gLog);
    }
}

extern "C" void xbox_netplay_trace(const char *fmt, ...) {
    char line[768];
    va_list ap;
    if (!gDiagnosticsEnabled || !gActive) return;
    log_open();
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    line[sizeof(line) - 1] = 0;
    log_line("%s", line);
}

static char *trim(char *s) {
    char *end;
    while (*s && isspace((unsigned char)*s)) ++s;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) --end;
    *end = 0;
    return s;
}

static bool ci_equal(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        ++a; ++b;
    }
    return *a == 0 && *b == 0;
}

static bool parse_uint(const char *s, unsigned &out) {
    unsigned v = 0;
    if (!s || !*s) return false;
    while (*s) {
        if (*s < '0' || *s > '9') return false;
        v = v * 10U + unsigned(*s - '0');
        if (v > 1000000U) return false;
        ++s;
    }
    out = v;
    return true;
}

static NetConfig load_config(void) {
    NetConfig c;
    memset(&c, 0, sizeof(c));
    c.mode = NET_OFFLINE;
    c.desired_players = 2;
    c.local_players = 1;
    c.timeout_seconds = 30;

    FILE *f = fopen(kConfigPath, "r");
    if (!f) {
        log_line("MK64XNET: no %s - offline mode\n", kConfigPath);
        return c;
    }

    char raw[192];
    while (fgets(raw, sizeof(raw), f)) {
        char *line = trim(raw);
        if (!*line || *line == '#' || *line == ';') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = 0;
        char *key = trim(line);
        char *value = trim(eq);

        if (ci_equal(key, "mode")) {
            if (ci_equal(value, "offline")) c.mode = NET_OFFLINE;
            else if (ci_equal(value, "host")) c.mode = NET_HOST;
            else if (ci_equal(value, "lan") || ci_equal(value, "lan_join")) c.mode = NET_JOIN_LAN;
            else if (ci_equal(value, "join") || ci_equal(value, "direct") || ci_equal(value, "direct_join")) c.mode = NET_JOIN_DIRECT;
        } else if (ci_equal(key, "players")) {
            unsigned v;
            if (parse_uint(value, v)) c.desired_players = v;
        } else if (ci_equal(key, "local_players")) {
            unsigned v;
            if (parse_uint(value, v)) c.local_players = v;
        } else if (ci_equal(key, "timeout_seconds")) {
            unsigned v;
            if (parse_uint(value, v)) c.timeout_seconds = v;
        } else if (ci_equal(key, "address")) {
            strncpy(c.address, value, sizeof(c.address) - 1);
            c.address[sizeof(c.address) - 1] = 0;
        }
    }
    fclose(f);

    if (c.local_players < 1 || c.local_players > 2) c.local_players = 1;
    if (c.desired_players < 2) c.desired_players = 2;
    if (c.desired_players > 4) c.desired_players = 4;
    if (c.mode == NET_HOST && c.desired_players < c.local_players + 1)
        c.desired_players = c.local_players + 1;
    if (c.mode == NET_HOST && c.timeout_seconds == 30)
        c.timeout_seconds = 0; /* Hosts wait indefinitely unless explicitly changed. */
    return c;
}

static void fill_token(uint8_t out[16], const XNADDR &xna, DWORD salt) {
    uint32_t seed = uint32_t(GetTickCount()) ^ uint32_t(xna.ina.s_addr) ^ uint32_t(salt) ^ 0x4D4B3634U;
    for (unsigned i = 0; i < 16; ++i) {
        seed = seed * 1664525U + 1013904223U + i * 97U;
        out[i] = uint8_t(seed >> 24);
    }
}

static void reset_peer(PeerState &p) {
    memset(&p, 0, sizeof(p));
}

static int peer_count(void) {
    int n = 0;
    while (n < int(mknet::MAX_PLAYERS - 1) && gPeers[n].used) ++n;
    return n;
}

static unsigned lobby_slots(void) {
    unsigned slots = gLocalCount;
    for (int i = 0; i < peer_count(); ++i) slots += gPeers[i].local_count;
    return slots;
}

static void assign_slots(void) {
    unsigned slot = gLocalCount;
    for (int i = 0; i < peer_count(); ++i) {
        PeerState &p = gPeers[i];
        if (p.slot != slot) {
            p.ready = false;
            p.acked = false;
            p.last_offer = 0;
        }
        p.slot = slot;
        slot += p.local_count;
    }
}

static int find_peer_address(const sockaddr_in &a) {
    for (int i = 0; i < peer_count(); ++i)
        if (gPeers[i].addr.sin_addr.s_addr == a.sin_addr.s_addr && gPeers[i].addr.sin_port == a.sin_port) return i;
    return -1;
}

static int find_peer_nonce(const uint8_t nonce[16]) {
    for (int i = 0; i < peer_count(); ++i)
        if (!memcmp(gPeers[i].nonce, nonce, 16)) return i;
    return -1;
}

static bool same_ip(const sockaddr_in &a, const sockaddr_in &b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr;
}

static void endpoint_text(char *dst, unsigned size, const sockaddr_in &a) {
    uint32_t ip = ntohl(a.sin_addr.s_addr);
    snprintf(dst, size - 1, "%u.%u.%u.%u:%u",
              unsigned(ip >> 24), unsigned((ip >> 16) & 255), unsigned((ip >> 8) & 255), unsigned(ip & 255),
              unsigned(ntohs(a.sin_port)));
    dst[size - 1] = 0;
}

static int send_packet_to(const sockaddr_in &to, mknet::Type type, const uint8_t sid[16], const uint8_t *payload, int payload_size) {
    if (gSocket == INVALID_SOCKET || payload_size < 0 || payload_size > int(mknet::MAX_PACKET - mknet::HEADER)) return SOCKET_ERROR;
    uint8_t packet[mknet::MAX_PACKET];
    int bytes = mknet::header(packet, type, sid, payload_size);
    if (payload_size && payload) memcpy(packet + mknet::HEADER, payload, payload_size);
    return sendto(gSocket, (const char *)packet, bytes, 0, (const sockaddr *)&to, sizeof(to));
}

static int send_peer_message(int index, mknet::Type type, const uint8_t *payload, int payload_size) {
    if (index < 0 || index >= peer_count()) return SOCKET_ERROR;
    return send_packet_to(gPeers[index].addr, type, gSession, payload, payload_size);
}

static void send_offer(int index) {
    if (index < 0 || index >= peer_count()) return;
    PeerState &p = gPeers[index];
    uint8_t payload[24];
    memset(payload, 0, sizeof(payload));
    memcpy(payload, p.nonce, 16);
    DWORD stamp = GetTickCount();
    mknet::put32(payload + 16, stamp);
    payload[20] = uint8_t(p.slot);
    payload[21] = uint8_t(lobby_slots());
    payload[22] = uint8_t(p.local_count - 1);
    payload[23] = uint8_t(mknet::PLATFORM_OG_XBOX);
    send_peer_message(index, mknet::OFFER, payload, sizeof(payload));
    p.last_offer = stamp;
}

static void send_start(int index) {
    uint8_t payload[6];
    payload[0] = uint8_t(gChosenDelay);
    payload[1] = uint8_t(gPlayerCount);
    payload[2] = uint8_t(gPeers[index].slot);
    payload[3] = uint8_t(gPeers[index].local_count - 1);
    payload[4] = gSessionSplit ? 1 : 0;
    payload[5] = gCrossplay ? 1 : 0;
    send_peer_message(index, mknet::START, payload, sizeof(payload));
}

static bool all_ready(void) {
    int n = peer_count();
    if (n < 1) return false;
    for (int i = 0; i < n; ++i) if (!gPeers[i].ready) return false;
    return true;
}

static bool all_acked(void) {
    int n = peer_count();
    if (n < 1) return false;
    for (int i = 0; i < n; ++i) if (!gPeers[i].acked) return false;
    return true;
}

static bool network_stack_start(XNADDR &xna) {
    if (!gStackStarted) {
        XNetStartupParams params;
        memset(&params, 0, sizeof(params));
        params.cfgSizeOfStruct = sizeof(params);
        params.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;

        int xe = XNetStartup(&params);
        if (xe) {
            log_line("MK64XNET: XNetStartup failed err=%d\n", xe);
            return false;
        }

        WSADATA wsa;
        int we = WSAStartup(MAKEWORD(2, 2), &wsa);
        if (we) {
            log_line("MK64XNET: WSAStartup failed err=%d\n", we);
            return false;
        }
        gStackStarted = true;
    }

    DWORD flags = XNET_GET_XNADDR_PENDING;
    memset(&xna, 0, sizeof(xna));
    for (int i = 0; i < 300; ++i) {
        KeStallExecutionProcessor(100000);
        flags = XNetGetTitleXnAddr(&xna);
        if (flags & (XNET_GET_XNADDR_DHCP | XNET_GET_XNADDR_STATIC)) break;
    }
    if (!(flags & (XNET_GET_XNADDR_DHCP | XNET_GET_XNADDR_STATIC))) {
        log_line("MK64XNET: no DHCP/static IPv4 address (flags=0x%08lX)\n", (unsigned long)flags);
        strcpy(gLocalAddressText, "LOCAL IP: UNAVAILABLE");
        return false;
    }

    unsigned char *o = (unsigned char *)&xna.ina;
    snprintf(gLocalAddressText, sizeof(gLocalAddressText) - 1,
             "LOCAL IP: %u.%u.%u.%u", o[0], o[1], o[2], o[3]);
    gLocalAddressText[sizeof(gLocalAddressText) - 1] = 0;
    log_line("MK64XNET: local IPv4 %u.%u.%u.%u, UDP %u\n", o[0], o[1], o[2], o[3], kPort);
    return true;
}

static bool socket_open(void) {
    gSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (gSocket == INVALID_SOCKET) {
        log_line("MK64XNET: socket() failed wsa=%d\n", WSAGetLastError());
        return false;
    }

    int yes = 1;
    setsockopt(gSocket, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
    setsockopt(gSocket, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof(yes));
    int netbuf = 64 * 1024;
    setsockopt(gSocket, SOL_SOCKET, SO_RCVBUF, (const char *)&netbuf, sizeof(netbuf));
    setsockopt(gSocket, SOL_SOCKET, SO_SNDBUF, (const char *)&netbuf, sizeof(netbuf));

    sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons((u_short)kPort);
    if (bind(gSocket, (sockaddr *)&bind_addr, sizeof(bind_addr)) == SOCKET_ERROR) {
        log_line("MK64XNET: bind UDP %u failed wsa=%d\n", kPort, WSAGetLastError());
        closesocket(gSocket);
        gSocket = INVALID_SOCKET;
        return false;
    }

    u_long nonblocking = 1;
    if (ioctlsocket(gSocket, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        log_line("MK64XNET: FIONBIO failed wsa=%d\n", WSAGetLastError());
        closesocket(gSocket);
        gSocket = INVALID_SOCKET;
        return false;
    }
    log_line("MK64XNET: UDP %u bind OK\n", kPort);
    return true;
}


/* R42 host-only WAN IPv4 discovery. */
static bool r42_stun_response(const uint8_t *p, int n,
                              const uint8_t tx[12], uint32_t &ip) {
    if (!p || n < 20) return false;
    if (p[0] != 0x01 || p[1] != 0x01) return false;
    if (mknet::get32(p + 4) != 0x2112A442U) return false;
    if (memcmp(p + 8, tx, 12) != 0) return false;

    unsigned message = (unsigned(p[2]) << 8) | unsigned(p[3]);
    if (message + 20U != unsigned(n) || (message & 3U)) return false;

    for (unsigned off = 20; off + 4 <= unsigned(n); ) {
        unsigned type = (unsigned(p[off]) << 8) | unsigned(p[off + 1]);
        unsigned len = (unsigned(p[off + 2]) << 8) | unsigned(p[off + 3]);
        off += 4;
        if (len > unsigned(n) - off) return false;

        if (type == 0x0020 && len >= 8 && p[off + 1] == 0x01) {
            ip = mknet::get32(p + off + 4) ^ 0x2112A442U;
            return ip != 0 && ip != 0xFFFFFFFFU;
        }
        if (type == 0x0001 && len >= 8 && p[off + 1] == 0x01) {
            ip = mknet::get32(p + off + 4);
            return ip != 0 && ip != 0xFFFFFFFFU;
        }
        off += (len + 3U) & ~3U;
    }
    return false;
}

static void r42_set_public_ip_text(uint32_t ip) {
    snprintf(gR42PublicIpText, sizeof(gR42PublicIpText) - 1,
             "PUBLIC IP: %u.%u.%u.%u",
             unsigned((ip >> 24) & 255U),
             unsigned((ip >> 16) & 255U),
             unsigned((ip >> 8) & 255U),
             unsigned(ip & 255U));
    gR42PublicIpText[sizeof(gR42PublicIpText) - 1] = 0;
}


/* MK64_R42_2_OG_PUBLIC_IP_FALLBACKS
 * Robust OG public-IP discovery with DNS and DNS-independent fallbacks.
 * The public address itself is never sent to log_line().
 */
static bool r42_try_stun_addr(const sockaddr_in &stun,
                              const char *label,
                              uint32_t &public_ip) {
    uint8_t request[20] = {
        0x00,0x01,0x00,0x00, 0x21,0x12,0xA4,0x42,
        0,0,0,0,0,0,0,0,0,0,0,0
    };
    if (XNetRandom(request + 8, 12) != 0) {
        log_line("R42.2: STUN transaction generation failed (%s)\n", label);
        return false;
    }

    DWORD begin = GetTickCount();
    DWORD last_send = 0;
    unsigned sends = 0;

    while (GetTickCount() - begin < 4500U) {
        DWORD now = GetTickCount();

        if (!last_send || now - last_send >= 500U) {
            int sr = sendto(gSocket, (const char *)request, sizeof(request), 0,
                            (const sockaddr *)&stun, sizeof(stun));
            ++sends;
            if (sr == SOCKET_ERROR && sends <= 3) {
                log_line("R42.2: STUN send failed server=%s try=%u wsa=%d\n",
                         label, sends, WSAGetLastError());
            }
            last_send = now;
        }

        uint8_t response[1024];
        sockaddr_in from;
        int from_len = sizeof(from);
        int n = recvfrom(gSocket, (char *)response, sizeof(response), 0,
                         (sockaddr *)&from, &from_len);

        if (n > 0 &&
            from.sin_addr.s_addr == stun.sin_addr.s_addr &&
            from.sin_port == stun.sin_port) {
            uint32_t ip = 0;
            if (r42_stun_response(response, n, request + 8, ip)) {
                public_ip = ip;
                log_line("R42.2: public IP discovery succeeded via %s\n", label);
                return true;
            }
        }

        ui_screen("HOST 2-4 PLAYER GAME",
                  "FINDING PUBLIC IP", gLocalAddressText,
                  label, "UDP STUN",
                  "PLEASE WAIT");
        Sleep(10);
    }

    log_line("R42.2: STUN timed out server=%s sends=%u\n", label, sends);
    return false;
}

static bool r42_try_stun_dns(const char *host, unsigned port,
                             const char *label, uint32_t &public_ip) {
    XNDNS *dns = 0;
    int rc = XNetDnsLookup(host, 0, &dns);
    if (rc != 0 || !dns) {
        log_line("R42.2: DNS start failed server=%s rc=%d\n", label, rc);
        return false;
    }

    DWORD begin = GetTickCount();
    while (dns->iStatus == WSAEINPROGRESS &&
           GetTickCount() - begin < 4500U) {
        ui_screen("HOST 2-4 PLAYER GAME",
                  "FINDING PUBLIC IP", gLocalAddressText,
                  label, "DNS LOOKUP",
                  "PLEASE WAIT");
        Sleep(10);
    }

    int status = dns->iStatus;
    int count = dns->cina;

    sockaddr_in stun;
    memset(&stun, 0, sizeof(stun));
    stun.sin_family = AF_INET;
    stun.sin_port = htons((u_short)port);

    bool found = status == 0 && count > 0;
    if (found) stun.sin_addr = dns->aina[0];
    XNetDnsRelease(dns);

    if (!found) {
        log_line("R42.2: DNS failed server=%s status=%d count=%d\n",
                 label, status, count);
        return false;
    }

    return r42_try_stun_addr(stun, label, public_ip);
}

static bool r42_try_google_direct(uint32_t &public_ip) {
    sockaddr_in stun;
    memset(&stun, 0, sizeof(stun));
    stun.sin_family = AF_INET;
    stun.sin_port = htons(19302);
    stun.sin_addr.s_addr = htonl(0x4A7DFA81U); /* 74.125.250.129 */
    return r42_try_stun_addr(stun, "GOOGLE DIRECT", public_ip);
}

static void r42_discover_public_ip(void) {
    strcpy(gR42PublicIpText, "PUBLIC IP: UNAVAILABLE");
    gR42PublicIpVisible = true;

    uint32_t ip = 0;

    /* MK64_R42_3_OG_DIRECT_STUN_FIRST
     * Hardware proved the DNS-independent Google endpoint works on OG Xbox,
     * while XNetDnsLookup remained WSAEINPROGRESS/10036 for every hostname.
     * Try the proven path first for a fast host-lobby result. */
    if (r42_try_google_direct(ip) ||
        r42_try_stun_dns("stun.cloudflare.com", 3478,
                         "CLOUDFLARE 3478", ip) ||
        r42_try_stun_dns("stun.cloudflare.com", 53,
                         "CLOUDFLARE 53", ip) ||
        r42_try_stun_dns("stun.l.google.com", 19302,
                         "GOOGLE 19302", ip)) {
        r42_set_public_ip_text(ip);
        return;
    }

    strcpy(gR42PublicIpText, "PUBLIC IP: UNAVAILABLE");
    log_line("R42.2: all public-IP discovery fallbacks failed\n");
}



static void host_receive(const uint8_t *packet, int bytes, const sockaddr_in &from) {
    const uint8_t *q = packet + mknet::HEADER;
    DWORD now = GetTickCount();

    if (packet[5] == mknet::HELLO && !gActive && !gStartSent) {
        unsigned requested = q[0];
        unsigned platform = q[1];
        if (platform == mknet::PLATFORM_XBOX360) {
            uint8_t reason = 2;
            send_packet_to(from, mknet::JOIN_REJECT, packet + 12, &reason, 1);
            log_line("MK64XNET: Xbox 360 crossplay join rejected - Xbox 360 must HOST crossplay\n");
            return;
        }
        int index = find_peer_nonce(packet + 12);
        if (index < 0) index = find_peer_address(from);
        unsigned previous = index < 0 ? 0U : gPeers[index].local_count;
        if (!mknet::reservation_fits(lobby_slots(), previous, requested, 4)) {
            uint8_t reason = 1;
            send_packet_to(from, mknet::JOIN_REJECT, packet + 12, &reason, 1);
            log_line("MK64XNET: HELLO rejected - not enough racer slots\n");
            return;
        }

        index = find_peer_nonce(packet + 12);
        if (index >= 0) {
            if (!same_ip(gPeers[index].addr, from)) return;
            gPeers[index].addr = from;
        } else {
            index = find_peer_address(from);
            if (index < 0) {
                int count = peer_count();
                if (count >= 3) return;
                index = count;
                reset_peer(gPeers[index]);
                gPeers[index].used = true;
                gPeers[index].addr = from;
            }
            memcpy(gPeers[index].nonce, packet + 12, 16);
            gPeers[index].ready = false;
            gPeers[index].acked = false;
            memset(&gPeers[index].latency, 0, sizeof(gPeers[index].latency));
        }

        if (gPeers[index].local_count != requested) {
            gPeers[index].ready = false;
            gPeers[index].last_offer = 0;
        }
        gPeers[index].local_count = requested;
        gPeers[index].platform = platform;
        assign_slots();
        gPeers[index].last_received = now;
        char who[80]; endpoint_text(who, sizeof(who), from);
        log_line("MK64XNET: HELLO %s -> slot P%u (%u local racer%s)\n",
                 who, gPeers[index].slot + 1, requested, requested == 1 ? "" : "s");
        send_offer(index);
        return;
    }

    if (memcmp(packet + 12, gSession, 16)) return;
    int index = find_peer_address(from);
    if (index < 0) return;
    PeerState &peer = gPeers[index];

    if (packet[5] == mknet::READY && !gActive && !gStartSent) {
        if (memcmp(q, peer.nonce, 16) || q[20] != peer.slot || q[22] + 1 != peer.local_count) return;
        DWORD stamp = mknet::get32(q + 16);
        DWORD rtt = now - stamp;
        if (stamp != peer.last_offer || rtt > 10000) return;
        peer.latency.add((unsigned)rtt);
        peer.ready = true;
        peer.last_received = now;
        log_line("MK64XNET: P%u READY, RTT=%lu ms\n", peer.slot + 1, (unsigned long)rtt);
        return;
    }

    if (gActive && packet[5] == mknet::BOOT_READY) {
        if (!gBoot.ready_span(peer.slot, peer.local_count)) return;
        peer.last_received = now;
        if (gBoot.complete) send_peer_message(index, mknet::BOOT_GO, 0, 0);
        return;
    }

    if (packet[5] == mknet::START_ACK && gStartSent) {
        if (q[0] != peer.slot) return;
        peer.acked = true;
        peer.last_received = now;
        log_line("MK64XNET: P%u START_ACK\n", peer.slot + 1);
        return;
    }

    if ((gActive || gStartSent) && packet[5] == mknet::CLIENT_INPUT) {
        ++gNetRxInput;
        bool accepted = gStream.receive_remote(peer.slot, packet, bytes, peer.local_count);
        if (accepted) {
            peer.last_received = now;
            peer.gameplay_seen = true;
            gFirstGameplayInput = true;
            if (gStartSent) peer.acked = true;

            /* Match the 360 low-latency path: relay a guest's input to every
             * other guest immediately. Authoritative FRAMESET packets remain
             * enabled as redundant recovery. */
            if (gPlayerCount > 2) {
                int pc = peer_count();
                for (int j = 0; j < pc; ++j) {
                    if (j == index) continue;
                    int sr = sendto(gSocket, (const char *)packet, bytes, 0,
                                    (const sockaddr *)&gPeers[j].addr, sizeof(gPeers[j].addr));
                    ++gNetTxInput;
                    if (sr == SOCKET_ERROR) ++gNetTxInputFail;
                }
            }
        } else {
            ++gNetRxInputReject;
        }
        return;
    }

    if (packet[5] == mknet::GOODBYE && gActive) {
        log_line("MK64XNET: P%u disconnected\n", peer.slot + 1);
        gFailed = true;
    }
}

static void client_receive(const uint8_t *packet, int bytes, const sockaddr_in &from) {
    const uint8_t *q = packet + mknet::HEADER;

    if (packet[5] == mknet::JOIN_REJECT && !gActive && !memcmp(packet + 12, gNonce, 16)) {
        if (!gLanJoin && !same_ip(gJoinTarget, from)) return;
        gJoinRejected = true;
        gFailed = true;
        if (q[0] == 2) log_line("MK64XNET: join rejected - Xbox 360 must HOST crossplay\n");
        else log_line("MK64XNET: host rejected join - lobby has insufficient slots\n");
        return;
    }

    if (packet[5] == mknet::OFFER && !gActive) {
        if (!gLanJoin && !same_ip(gJoinTarget, from)) return;
        if (memcmp(q, gNonce, 16)) return;
        unsigned slot = q[20];
        gHostPlatform = q[23];
        if (slot < 1 || slot + gLocalCount > 4 || q[22] + 1 != gLocalCount) return;

        gHostPeer = from;
        memcpy(gSession, packet + 12, 16);
        gHostSessionKnown = true;
        gAssignedSlot = slot;

        uint8_t ready[24];
        memcpy(ready, q, sizeof(ready));
        send_packet_to(gHostPeer, mknet::READY, gSession, ready, sizeof(ready));
        char who[80]; endpoint_text(who, sizeof(who), from);
        log_line("MK64XNET: OFFER accepted from %s; assigned P%u\n", who, gAssignedSlot + 1);
        return;
    }

    if (!gHostSessionKnown || memcmp(packet + 12, gSession, 16)) return;
    if (!same_ip(gHostPeer, from)) return;

    if (packet[5] == mknet::START) {
        unsigned delay = q[0], players = q[1], slot = q[2];
        if (slot != gAssignedSlot || q[3] + 1 != gLocalCount || slot + gLocalCount > players || players > 4) return;
        gHostPeer = from;
        if (!gActive) {
            gChosenDelay = delay;
            gPlayerCount = players;
            gLocalSlot = slot;
            gLocalCount = q[3] + 1;
            gSessionSplit = q[4] != 0;
            gCrossplay = q[5] != 0;
            if (gCrossplay && gHostPlatform != mknet::PLATFORM_XBOX360) { gFailed = true; return; }
            gStream.reset(gChosenDelay, gPlayerCount, gLocalSlot, gLocalCount);
            gBoot.reset(gPlayerCount);
            gActive = true;
            log_line("MK64XNET: START received - ACTIVE as P%u, players=%u, delay=%u crossplay=%u\n",
                     gLocalSlot + 1, gPlayerCount, gChosenDelay, gCrossplay ? 1U : 0U);
        }
        uint8_t ack = uint8_t(gLocalSlot);
        send_packet_to(gHostPeer, mknet::START_ACK, gSession, &ack, 1);
        return;
    }

    if (gActive && packet[5] == mknet::BOOT_GO) {
        gBoot.complete = true;
        return;
    }

    if (gActive && packet[5] == mknet::CLIENT_INPUT) {
        unsigned source = q[0];
        if (source >= gPlayerCount ||
            (source < gLocalSlot + gLocalCount && source + (q[3] + 1) > gLocalSlot)) {
            ++gNetRxInputReject;
            return;
        }
        ++gNetRxInput;
        if (gStream.receive_remote(source, packet, bytes, q[3] + 1)) {
            gFirstGameplayInput = true;
        } else {
            ++gNetRxInputReject;
        }
        return;
    }

    if (gActive && packet[5] == mknet::FRAMESET) {
        ++gNetRxInput;
        if (gStream.receive_frameset(packet, bytes)) {
            gFirstGameplayInput = true;
            if (gCrossplay && gMenuSync) {
                uint32_t first = mknet::get32(q + 4);
                uint32_t last = first + q[1] - 1;
                if (!gHaveHostCommit || last > gHostCommitFrame) { gHostCommitFrame = last; gHaveHostCommit = true; }
            }
        } else {
            ++gNetRxInputReject;
        }
        return;
    }

    if (gActive && packet[5] == mknet::STATE_SYNC) {
        if (!gCrossplay || gHostPlatform != mknet::PLATFORM_XBOX360) return;
        uint32_t sf = mknet::get32(q);
        if (sf < gStream.frame || sf > gStream.frame + CROSS_STATE_HISTORY - 1) return;
        CrossStateSlot &slot = gHostStates[sf % CROSS_STATE_HISTORY];
        slot.frame = sf;
        slot.present = true;
        memcpy(slot.data, q + 4, mknet::CROSS_STATE_BYTES);
        ++gStateSyncRx;
        return;
    }

    if (packet[5] == mknet::GOODBYE && gActive) {
        log_line("MK64XNET: host disconnected\n");
        gFailed = true;
    }
}

static void pump_packets(void) {
    if (gSocket == INVALID_SOCKET) return;
    for (int i = 0; i < 96; ++i) {
        uint8_t packet[mknet::MAX_PACKET + 1];
        sockaddr_in from;
        int from_len = sizeof(from);
        int bytes = recvfrom(gSocket, (char *)packet, mknet::MAX_PACKET, 0, (sockaddr *)&from, &from_len);
        if (bytes == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e != WSAEWOULDBLOCK && e != WSAEMSGSIZE) {
                log_line("MK64XNET: recvfrom failed wsa=%d\n", e);
                gFailed = true;
            }
            break;
        }
        if (!mknet::valid(packet, bytes)) continue;
        if (gHosting) host_receive(packet, bytes, from);
        else client_receive(packet, bytes, from);
    }
}

static bool make_direct_target(const char *address, sockaddr_in &out) {
    uint32_t ip;
    uint16_t port;
    if (!address || !*address || !mknet::parse_endpoint(address, ip, port)) {
        /* For convenience allow a bare IPv4 address; parse_endpoint already does. */
        return false;
    }
    if (port != kPort) {
        log_line("MK64XNET: direct join must use UDP 6464 for MK64 protocol compatibility\n");
        return false;
    }
    memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_addr.s_addr = htonl(ip);
    out.sin_port = htons((u_short)kPort);
    return true;
}

static void make_broadcast_target(sockaddr_in &out) {
    memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    out.sin_port = htons((u_short)kPort);
}

static bool handshake_host(unsigned timeout_seconds) {
    DWORD begin = GetTickCount();
    DWORD last_start = 0;
    log_line("MK64XNET: HOST waiting for %u total racer slots; LAN/direct-public use same UDP %u socket\n",
             gDesiredPlayers, kPort);

    while (!gActive && !gFailed) {
        pump_packets();
        DWORD now = GetTickCount();
        int n = peer_count();

        if (!gStartSent) {
            for (int i = 0; i < n; ++i)
                if (!gPeers[i].last_offer || now - gPeers[i].last_offer >= 1000) send_offer(i);

            if (all_ready() && lobby_slots() >= gDesiredPlayers) {
                unsigned worst = 0, second = 0;
                for (int i = 0; i < n; ++i) {
                    unsigned budget = gPeers[i].latency.budget();
                    if (budget >= worst) { second = worst; worst = budget; }
                    else if (budget > second) second = budget;
                }
                gPlayerCount = lobby_slots();
                gLocalSlot = 0;
                gSessionSplit = gLocalCount == 2;
                for (int i = 0; i < n; ++i) if (gPeers[i].local_count == 2) gSessionSplit = true;
                gChosenDelay = gPlayerCount == 2 ? mknet::input_delay_2p(worst)
                                                : mknet::input_delay_early_relay(worst, second);
                gStartSent = true;
                for (int i = 0; i < n; ++i) gPeers[i].acked = false;
                log_line("MK64XNET: all requested racers READY; START %uP delay=%u\n", gPlayerCount, gChosenDelay);
            }
        }

        if (gStartSent && (!last_start || now - last_start >= 100)) {
            for (int i = 0; i < n; ++i) if (!gPeers[i].acked) send_start(i);
            last_start = now;
        }
        if (gStartSent && all_acked()) {
            gActive = true;
            log_line("MK64XNET: HOST ACTIVE - all START_ACKs received\n");
            break;
        }

        if (timeout_seconds && now - begin >= timeout_seconds * 1000U) {
            log_line("MK64XNET: host handshake timeout after %u seconds\n", timeout_seconds);
            gFailed = true;
            break;
        }
        KeStallExecutionProcessor(10000); /* 10 ms without starving network DPCs. */
    }
    return gActive;
}

static bool handshake_client(unsigned timeout_seconds) {
    DWORD begin = GetTickCount();
    DWORD last_hello = 0;
    uint8_t hello[2] = { uint8_t(gLocalCount), uint8_t(mknet::PLATFORM_OG_XBOX) };
    char target[80]; endpoint_text(target, sizeof(target), gJoinTarget);
    log_line("MK64XNET: JOIN %s target=%s local_players=%u\n",
             gLanJoin ? "LAN-BROADCAST" : "DIRECT-IP", target, gLocalCount);

    while (!gActive && !gFailed) {
        DWORD now = GetTickCount();
        if (!gHostSessionKnown && (!last_hello || now - last_hello >= 250)) {
            int sent = send_packet_to(gJoinTarget, mknet::HELLO, gNonce, hello, sizeof(hello));
            if (sent == SOCKET_ERROR) log_line("MK64XNET: HELLO send failed wsa=%d\n", WSAGetLastError());
            last_hello = now;
        }
        pump_packets();
        if (gActive) break;

        if (timeout_seconds && now - begin >= timeout_seconds * 1000U) {
            log_line("MK64XNET: join timeout after %u seconds\n", timeout_seconds);
            gFailed = true;
            break;
        }
        KeStallExecutionProcessor(10000);
    }
    return gActive;
}

static void reset_state(void) {
    gHosting = false;
    gLanJoin = false;
    gActive = false;
    gFailed = false;
    gHostSessionKnown = false;
    gStartSent = false;
    gJoinRejected = false;
    gAssignedSlot = 0;
    gPlayerCount = 1;
    gLocalSlot = 0;
    gLocalCount = 1;
    gDesiredPlayers = 2;
    gChosenDelay = 4;
    gSessionSplit = false;
    gCrossplay = false;
    gHostPlatform = mknet::PLATFORM_UNKNOWN;
    gHaveHostCommit = false;
    gHostCommitFrame = 0;
    memset(gHostStates, 0, sizeof(gHostStates));
    gStateSyncRx = gStateSyncApplied = 0; gAppliedHostRaceState = -1;
    gAppliedHostStateHash = 0;
    gAppliedHostStateHashValid = false;
    memset(&gBoot, 0, sizeof(gBoot));
    memset(&gStream, 0, sizeof(gStream));
    gFirstGameplayInput = false;
    gNetRxInput = gNetRxInputReject = gNetTxInput = gNetTxInputFail = 0;
    memset(&gHostPeer, 0, sizeof(gHostPeer));
    memset(&gJoinTarget, 0, sizeof(gJoinTarget));
    memset(gSession, 0, sizeof(gSession));
    memset(gNonce, 0, sizeof(gNonce));
    for (unsigned i = 0; i < mknet::MAX_PLAYERS - 1; ++i) reset_peer(gPeers[i]);
    mknet::lobby_capacity() = 4;
}


static int ready_peer_count(void) {
    int n = peer_count(), r = 0;
    for (int i = 0; i < n; ++i) if (gPeers[i].ready) ++r;
    return r;
}

static int ack_peer_count(void) {
    int n = peer_count(), r = 0;
    for (int i = 0; i < n; ++i) if (gPeers[i].acked) ++r;
    return r;
}

static void begin_start_sequence(void) {
    int n = peer_count();
    unsigned worst = 0, second = 0;
    for (int i = 0; i < n; ++i) {
        unsigned budget = gPeers[i].latency.budget();
        if (budget >= worst) { second = worst; worst = budget; }
        else if (budget > second) second = budget;
    }
    gPlayerCount = lobby_slots();
    gLocalSlot = 0;
    gSessionSplit = gLocalCount == 2;
    for (int i = 0; i < n; ++i) if (gPeers[i].local_count == 2) gSessionSplit = true;
    gChosenDelay = gPlayerCount == 2 ? mknet::input_delay_2p(worst)
                                     : mknet::input_delay_early_relay(worst, second);
    gStream.reset(gChosenDelay, gPlayerCount, 0, gLocalCount);
    gBoot.reset(gPlayerCount);
    if (gLocalCount > 1) gBoot.ready_span(1, gLocalCount - 1);
    gStartSent = true;
    for (int i = 0; i < n; ++i) gPeers[i].acked = false;
    log_line("MK64XNET: HOST pressed A - starting %uP delay=%u\n", gPlayerCount, gChosenDelay);
}

static bool host_lobby_menu(void) {
    DWORD last_start = 0;
    ui_consume();
    for (;;) {
        pump_packets();
        DWORD now = GetTickCount();
        int n = peer_count();

        if (!gStartSent) {
            for (int i = 0; i < n; ++i)
                if (!gPeers[i].last_offer || now - gPeers[i].last_offer >= 1000) send_offer(i);
        }

        uint32_t p = ui_pressed();
        if (p & CONT_B) {
            log_line("MK64XNET: host cancelled lobby\n");
            return false;
        }
        if (p & kUiYButton) {
            gR42PublicIpVisible = !gR42PublicIpVisible;
        }

        bool can_start = !gStartSent && all_ready() && lobby_slots() >= 2 && lobby_slots() <= 4;
        if (can_start && (p & CONT_A)) begin_start_sequence();

        if (gStartSent && (!last_start || now - last_start >= 100)) {
            for (int i = 0; i < n; ++i) if (!gPeers[i].acked) send_start(i);
            last_start = now;
        }
        if (gStartSent && all_acked()) {
            gActive = true;
            log_line("MK64XNET: HOST ACTIVE - all START_ACKs received\n");
            return true;
        }
        if (gFailed) return false;

        char status[80], peers[80], footer[96];
        const char *public_line = gR42PublicIpVisible
            ? gR42PublicIpText
            : "PUBLIC IP: HIDDEN";
        if (gStartSent) {
            snprintf(status, sizeof(status)-1, "STARTING %uP - ACK %d/%d", gPlayerCount, ack_peer_count(), n);
        } else if (n == 0) {
            snprintf(status, sizeof(status)-1, "WAITING FOR PLAYERS (%u/4)", gLocalCount);
        } else if (can_start) {
            snprintf(status, sizeof(status)-1, "PLAYERS %u/4 READY - A START", lobby_slots());
        } else {
            snprintf(status, sizeof(status)-1, "PLAYERS %u/4 - READY %d/%d", lobby_slots(), ready_peer_count(), n);
        }
        status[sizeof(status)-1] = 0;
        snprintf(peers, sizeof(peers)-1, "LOCAL RACERS: %u   REMOTE CONSOLES: %d", gLocalCount, n);
        peers[sizeof(peers)-1] = 0;
        snprintf(footer, sizeof(footer)-1, "Y %s IP   A START   B CANCEL   UDP 6464",
                 gR42PublicIpVisible ? "HIDE" : "SHOW");
        footer[sizeof(footer)-1] = 0;
        ui_screen("HOST 2-4 PLAYER GAME",
                  public_line, gLocalAddressText, status, peers, footer);
        Sleep(10);
    }
}

static bool join_lobby_menu(void) {
    DWORD last_hello = 0;
    uint8_t hello[2] = { uint8_t(gLocalCount), uint8_t(mknet::PLATFORM_OG_XBOX) };
    char target[80];
    endpoint_text(target, sizeof(target), gJoinTarget);
    ui_consume();

    for (;;) {
        DWORD now = GetTickCount();
        if (!gHostSessionKnown && (!last_hello || now - last_hello >= 250)) {
            int sent = send_packet_to(gJoinTarget, mknet::HELLO, gNonce, hello, sizeof(hello));
            if (sent == SOCKET_ERROR) log_line("MK64XNET: HELLO send failed wsa=%d\n", WSAGetLastError());
            last_hello = now;
        }
        pump_packets();
        if (gActive) return true;
        if (gFailed) return false;

        uint32_t p = ui_pressed();
        if (p & CONT_B) {
            log_line("MK64XNET: join cancelled\n");
            return false;
        }

        char status[80], target_line[96], racers[80];
        if (gHostSessionKnown)
            snprintf(status, sizeof(status)-1, "ASSIGNED P%u - WAITING FOR HOST", gAssignedSlot + 1);
        else
            snprintf(status, sizeof(status)-1, "CONNECTING TO HOST");
        status[sizeof(status)-1] = 0;
        snprintf(target_line, sizeof(target_line)-1, "HOST: %s", target);
        target_line[sizeof(target_line)-1] = 0;
        snprintf(racers, sizeof(racers)-1, "LOCAL RACERS RESERVED: %u", gLocalCount);
        racers[sizeof(racers)-1] = 0;
        ui_screen("JOIN 2-4 PLAYER GAME", target_line, status, gLocalAddressText, racers, "B CANCEL    ALL GUESTS USE SAME HOST IP");
        Sleep(10);
    }
}

static bool select_local_players(bool host, unsigned &count) {
    ui_consume();
    for (;;) {
        bool second = ui_second_controller_connected();
        count = second ? 2U : 1U;
        const char *one_line = count == 1 ? "> 1 PLAYER - FULL SCREEN" : "  1 PLAYER - FULL SCREEN";
        const char *two = count == 2 ? "> 2 PLAYERS - SPLIT SCREEN" : "  2 PLAYERS - SPLIT SCREEN";
        const char *detect = second ? "EXTRA CONTROLLER DETECTED" : "NO EXTRA CONTROLLER DETECTED";
        const char *hint = second ? "AUTO ASSIGN: TWO RACER SLOTS" : "CONNECT CONTROLLER IN PORT 2 FOR LOCAL P2";
        ui_screen(host ? "HOST - PLAYERS ON THIS CONSOLE" : "JOIN - PLAYERS ON THIS CONSOLE",
                  one_line, two, detect, hint, "A CONTINUE    B BACK");
        uint32_t p = ui_pressed();
        if (p & CONT_B) { ui_consume(); return false; }
        if (p & CONT_A) { ui_consume(); return true; }
        Sleep(16);
    }
}

static bool edit_host_ip(char out[64]) {
    char digits[16] = "000.000.000.000";
    int cursor = 0;
    ui_consume();
    for (;;) {
        char marker[16];
        memset(marker, ' ', 15); marker[15] = 0; marker[cursor] = '^';
        ui_screen("JOIN 2-4 PLAYER GAME", digits, marker,
                  "ENTER HOST LAN OR PUBLIC IPV4", "UDP PORT 6464 IS AUTOMATIC",
                  "LEFT/RIGHT MOVE  UP/DOWN CHANGE  A CONNECT  B BACK");
        uint32_t p = ui_pressed();
        if (p & CONT_B) { ui_consume(); return false; }
        if (p & CONT_DPAD_LEFT)  { do { cursor = (cursor + 14) % 15; } while (digits[cursor] == '.'); }
        if (p & CONT_DPAD_RIGHT) { do { cursor = (cursor + 1) % 15; } while (digits[cursor] == '.'); }
        if (p & CONT_DPAD_UP)    digits[cursor] = digits[cursor] == '9' ? '0' : char(digits[cursor] + 1);
        if (p & CONT_DPAD_DOWN)  digits[cursor] = digits[cursor] == '0' ? '9' : char(digits[cursor] - 1);
        if (p & CONT_A) {
            char ep[24];
            snprintf(ep, sizeof(ep)-1, "%s:%u", digits, kPort); ep[sizeof(ep)-1] = 0;
            uint32_t ip; uint16_t port;
            if (mknet::parse_endpoint(ep, ip, port)) {
                strncpy(out, digits, 63); out[63] = 0;
                ui_consume();
                return true;
            }
        }
        Sleep(16);
    }
}

static void crossplay_release_gate(void);

static bool begin_online_session(bool host, unsigned local_players, const char *address) {
    log_open();
    reset_state();
    gHosting = host;
    gLanJoin = false;
    gLocalCount = local_players;
    gDesiredPlayers = 2;
    if (gLog) log_line("MK64XNET: log=%s\n", gLogPath);

    ui_screen("NETWORK INIT", "INITIALIZING XNET", "WAITING FOR IPV4", "UDP 6464", "", "PLEASE WAIT");
    XNADDR xna;
    if (!network_stack_start(xna) || !socket_open()) {
        log_line("MK64XNET: network initialization failed\n");
        xbox_netplay_shutdown();
        return false;
    }
    fill_token(gSession, xna, 0x13579BDFU);
    fill_token(gNonce, xna, 0x2468ACE0U);

    bool ok = false;
    if (host) {
        r42_discover_public_ip();
        log_line("MK64XNET: HOST lobby open local_count=%u UDP=%u\n", gLocalCount, kPort);
        ok = host_lobby_menu();
    } else {
        char endpoint[80];
        snprintf(endpoint, sizeof(endpoint)-1, "%s:%u", address ? address : "", kPort);
        endpoint[sizeof(endpoint)-1] = 0;
        if (!make_direct_target(endpoint, gJoinTarget)) {
            log_line("MK64XNET: invalid join address '%s'\n", address ? address : "");
            xbox_netplay_shutdown();
            return false;
        }
        gHostPeer = gJoinTarget;
        log_line("MK64XNET: JOIN target=%s local_count=%u\n", endpoint, gLocalCount);
        ok = join_lobby_menu();
    }

    if (!ok) {
        if (gJoinRejected) log_line("MK64XNET: join rejected - not enough racer slots\n");
        xbox_netplay_shutdown();
        return false;
    }

    log_line("MK64XNET: SESSION ESTABLISHED role=%s local_slot=P%u local_count=%u players=%u crossplay=%u\n",
             gHosting ? "HOST" : "JOIN", gLocalSlot + 1, gLocalCount, gPlayerCount, gCrossplay ? 1U : 0U);
    log_line("MK64XNET: R31-COLLISION-CANON + R30 diagnostics A/B - MK4P v10/BA100922\n");
    crossplay_release_gate();
    return true;
}

static void crossplay_release_gate(void) {
    if (!gCrossplay) { ui_consume(); return; }
    unsigned stable=0;
    while(stable<4) {
        ui_screen("CROSSPLAY SYNC","RELEASE ALL BUTTONS","XBOX 360 HOST CONTROLS P1","STARTING NORMAL MK64 MENUS","","PLEASE RELEASE CONTROLS");
        if (ui_buttons_now()==0) ++stable; else stable=0;
        pump_packets(); Sleep(16);
    }
    ui_consume();
}

static void wait_return_to_menu(const char *why) {
    ui_consume();
    for (;;) {
        ui_screen("CONNECTION NOT ESTABLISHED", why ? why : "NO GAME WAS STARTED",
                  "CHECK HOST IP / UDP 6464", "", "", "A OR B RETURN TO MENU");
        uint32_t p = ui_pressed();
        if (p & (CONT_A | CONT_B)) { ui_consume(); return; }
        Sleep(16);
    }
}

static bool host_gameplay_timeout(DWORD now) {
    if (!gHosting) return false;
    int n = peer_count();
    for (int i = 0; i < n; ++i) {
        DWORD timeout = gPeers[i].gameplay_seen ? 15000U : 60000U;
        if (now - gPeers[i].last_received > timeout) return true;
    }
    return false;
}

struct NetPadCompat {
    unsigned short button;
    signed char stick_x;
    signed char stick_y;
    unsigned char err_no;
};

static bool apply_host_state_for_current_frame(void) {
    if (!gCrossplay || !gMenuSync || gHosting) return true;
    DWORD begin = GetTickCount();
    for (;;) {
        CrossStateSlot &slot = gHostStates[gStream.frame % CROSS_STATE_HISTORY];
        if (slot.present && slot.frame == gStream.frame) {
            /* Hash the exact authoritative host snapshot before applying it.
             * R11 intentionally does NOT overwrite the guest's current
             * gGamestate; this host hash lets the transition frame still
             * compare equal while the OG runs update_gamestate() locally. */
            uint32_t h = 2166136261U;
            for (unsigned i = 0; i < mknet::CROSS_STATE_BYTES; ++i)
                h = (h ^ slot.data[i]) * 16777619U;
            gAppliedHostStateHash = h;
            gAppliedHostStateHashValid = true;
            gAppliedHostRaceState = (int)mknet::get32(slot.data + 96);
            xbox_crossplay_state_apply(slot.data, mknet::CROSS_STATE_BYTES);
            slot.present = false;
            ++gStateSyncApplied;
            if (gStateSyncApplied <= 6 || (gStateSyncApplied % 300U) == 0)
                log_line("MK64XNET10: applied host state frame=%lu rx=%u applied=%u\n",
                         (unsigned long)gStream.frame, gStateSyncRx, gStateSyncApplied);
            return true;
        }
        pump_packets();
        if (gFailed || gStream.fault) return false;
        if (GetTickCount() - begin > 15000U) {
            log_line("MK64XNET10: state sync timeout frame=%lu rx=%u\n",
                     (unsigned long)gStream.frame, gStateSyncRx);
            return false;
        }
        Sleep(1);
    }
}

static void sample_locals_crossplay_hash(const mknet::Pad *input) {
    uint32_t h = xbox_netplay_state_hash();
    if (gCrossplay && gMenuSync && !gHosting && gAppliedHostStateHashValid)
        h = gAppliedHostStateHash;
    gAppliedHostStateHashValid = false;
    gStream.sample_locals(input, h);
}

static bool menu_frame_window_ready(void) {
    if (!gMenuSync) return true;
    if (!gCrossplay) return true;
    /* Crossplay menus/countdown are host-committed. The OG guest can only
     * consume frames the Xbox 360 host has already included in a complete
     * authoritative FRAMESET. This preserves the proven input-delay pipeline
     * but prevents the guest from entering the next semantic screen early. */
    if (gHosting) return true;
    return gHaveHostCommit && gStream.frame <= gHostCommitFrame;
}




/* MK64_ASTRA_TRACE_R17: synchronized input/hash history dumped only on fault. */
extern "C" void mk64_astra_diag_dump(void);extern "C" void mk64_astra_rng_dump(void);
static void astra_r17_dump_net_history(void){uint32_t cur=gStream.frame,first=cur>24U?cur-24U:0U;log_line("ASTRA_NET_BEGIN SIDE=OG FIRST=%lu LAST=%lu\n",(unsigned long)first,(unsigned long)cur);for(uint32_t f=first;f<=cur;++f){const mknet::HashSlot &lh=gStream.hashes[f%mknet::HISTORY];for(unsigned s=0;s<gPlayerCount&&s<4;++s){const mknet::InputSlot &in=gStream.inputs[s][f%mknet::HISTORY];const mknet::HashSlot &ph=gStream.peer_hashes[s][f%mknet::HISTORY];if(in.present&&in.frame==f)log_line("ASTRA_NET SIDE=OG F=%lu P=%u B=%04X X=%d Y=%d LH=%08lX LHP=%u PH=%08lX PHP=%u\n",(unsigned long)f,s+1,in.pad.buttons,(int)in.pad.x,(int)in.pad.y,(unsigned long)((lh.present&&lh.frame==f)?lh.value:0),(lh.present&&lh.frame==f)?1U:0U,(unsigned long)((ph.present&&ph.frame==f)?ph.value:0),(ph.present&&ph.frame==f)?1U:0U);}}log_line("ASTRA_NET_END SIDE=OG\n");}

/* MK64_CROSSPLAY_R25_ASTRA_GOLD_TRACE */

extern "C" unsigned int xbox_crossplay_r25_count(void);
extern "C" int xbox_crossplay_r25_get(unsigned int index,unsigned int *out,int outCount);
extern "C" unsigned int mk64_r27_speed_probe(void);
extern "C" unsigned int mk64_crossplay_r27_count(void);
extern "C" int mk64_crossplay_r27_get(unsigned int index, unsigned int *out, int outCount);
static void r25_astra_dump(uint32_t mismatch){
    if(!gDiagnosticsEnabled)return;
    unsigned int n=xbox_crossplay_r25_count(),w[88];
    const char *path="D:/mk64-astra-r25.log";FILE *f=fopen(path,"w");
    if(!f){path="T:/mk64-astra-r25.log";f=fopen(path,"w");}
    log_line("R25_ASTRA_GOLD mismatch=%lu records=%u file=%s ok=%u\n",(unsigned long)mismatch,n,path,f?1U:0U);
    if(!f)return;
    fprintf(f,"R25_HEADER SIDE=OG mismatch=%lu records=%u words=88 demo_filtered=1 phases=1,10-21,30-32\n",(unsigned long)mismatch,n);
    for(unsigned int k=0;k<n;++k){
        if(!xbox_crossplay_r25_get(k,w,88))continue;
        fprintf(f,"R25_PHASE SIDE=OG I=%u F=%u PH=%u TK=%08X GT=%u RS=%u SEED=%04X CT=%08X VT=%08X DEMO=%u TICKS=%u PC=%u SM=%u MODE=%u KIN=%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X LOG=%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X\n",
          k,w[0],w[1],w[2],w[3],w[4],w[5]&0xFFFFU,w[6],w[7],w[8],w[9],w[10],(w[11]>>16)&0xFFFFU,w[11]&0xFFFFU,
          w[12],w[13],w[14],w[15],w[16],w[17],w[18],w[19],w[20],w[21],w[22],w[23],w[24],w[25],w[26],w[27]);
        for(int p=0;p<2;++p){unsigned int *d=&w[28+p*30];
          fprintf(f,"R25_P SIDE=OG I=%u F=%u PH=%u TK=%08X P=%d TYPE=%04X LAP=%d RANK=%d PATH=%d ITEM=%d EFF=%08X TRIG=%08X PROP=%04X POS=%08X,%08X,%08X VEL=%08X,%08X,%08X SPD=%08X CUR=%08X PREV=%08X OLD=%08X,%08X,%08X ROTY=%04X SLOPE=%04X U98=%08X U8C=%08X BBOX=%08X SURF2=%08X ORI=%08X,%08X,%08X U90=%08X RSV=%08X BTN=%04X X=%d Y=%d BP=%04X BD=%04X\n",
           k,w[0],w[1],w[2],p+1,d[0]&0xFFFFU,(int)(short)(d[1]>>16),(int)(short)(d[1]&0xFFFFU),(int)(short)(d[2]>>16),(int)(short)(d[2]&0xFFFFU),d[3],d[4],d[5]&0xFFFFU,
           d[6],d[7],d[8],d[9],d[10],d[11],d[12],d[13],d[14],d[15],d[16],d[17],(d[18]>>16)&0xFFFFU,d[18]&0xFFFFU,d[19],d[20],d[21],d[22],d[23],d[24],d[25],d[26],d[27],(d[28]>>16)&0xFFFFU,(int)(signed char)((d[28]>>8)&0xFFU),(int)(signed char)(d[28]&0xFFU),(d[29]>>16)&0xFFFFU,d[29]&0xFFFFU);
        }
    }
    fprintf(f,"R31_HEADER SIDE=OG build=R31-COLLISION-CANON words=56 anchor=1024 recent=2048 exact_gp_hash=1 speed6=%08X\n",mk64_r27_speed_probe());
    for (unsigned int k=0;k<mk64_crossplay_r27_count();++k) {
        unsigned int c[56]; if (!mk64_crossplay_r27_get(k,c,56)) continue;
        fprintf(f,"R27_CPU SIDE=OG I=%u F=%u TK=%u ST=%u P=%u W=",k,c[0],c[1],c[2],c[3]+1);
        for (unsigned int j=0;j<56;++j) fprintf(f,j?",%08X":"%08X",c[j]);
        fprintf(f,"\n");
    }
    fflush(f);fclose(f);
}

static void crossplay_fault_log(void) {
    if(!gDiagnosticsEnabled)return;
    static bool written=false;
    if(written)return;written=true;
    log_open();
    uint32_t mf=0xFFFFFFFFU,lh=0,ph=0;unsigned ms=0;
    for(unsigned back=0;back<16;++back){
        if(back>gStream.frame)break;
        uint32_t f=gStream.frame-back;
        const mknet::HashSlot &a=gStream.hashes[f%mknet::HISTORY];
        if(!a.present||a.frame!=f)continue;
        for(unsigned slot=0;slot<gPlayerCount;++slot){
            if(slot>=gLocalSlot&&slot<gLocalSlot+gLocalCount)continue;
            const mknet::HashSlot &b=gStream.peer_hashes[slot][f%mknet::HISTORY];
            if(b.present&&b.frame==f&&a.value!=b.value){mf=f;lh=a.value;ph=b.value;ms=slot;break;}
        }
        if(mf!=0xFFFFFFFFU)break;
    }
    r25_astra_dump(mf);
    log_line("CROSSPLAY_FAULT role=%s frame=%lu menuSync=%u localSlot=%u localCount=%u players=%u latestLocal=%lu latestComplete=%lu\n",
             gHosting?"HOST":"JOIN",(unsigned long)gStream.frame,gMenuSync?1U:0U,gLocalSlot,gLocalCount,gPlayerCount,
             (unsigned long)gStream.latest_local,(unsigned long)gStream.latest_complete);
    if(mf!=0xFFFFFFFFU)
        log_line("CROSSPLAY_HASH_MISMATCH frame=%lu local=%08lX peerP%u=%08lX\n",
                 (unsigned long)mf,(unsigned long)lh,ms+1,(unsigned long)ph);
    else
        log_line("CROSSPLAY_FAULT no recent hash mismatch found; possible conflicting input/history fault\n");
    for(unsigned slot=0;slot<gPlayerCount;++slot)
        log_line("CROSSPLAY_PEER_FRAME P%u=%lu\n",slot+1,(unsigned long)gStream.peer_frame[slot]);
    {
        unsigned char st[mknet::CROSS_STATE_BYTES];
        int n=xbox_crossplay_state_pack(st,sizeof(st));
        char hex[mknet::CROSS_STATE_BYTES*2+1];
        static const char d[]="0123456789ABCDEF";
        int o=0;
        for(int i=0;i<n && o+2<(int)sizeof(hex);++i){hex[o++]=d[st[i]>>4];hex[o++]=d[st[i]&15];}
        hex[o]=0;
        for (int offset = 0; offset < o; offset += 512)
            log_line("CROSSPLAY_STATE_HEX offset=%d %.512s\n", offset / 2, hex + offset);
    mkdiag_write_component_snapshot(); /* MK64_CROSSPLAY_COMPONENT_DIAG_R15_CALL */
    }

    astra_r17_dump_net_history();
    mk64_astra_diag_dump();
    mk64_astra_rng_dump();
}



} /* anonymous namespace */

extern "C" int xbox_netplay_diagnostics_enabled(void) { return gDiagnosticsEnabled ? 1 : 0; }

extern "C" void xbox_netplay_controllers(void *pads_, int count) {
    if (!gActive || count < 2 || !pads_) return;
    NetPadCompat *pads = (NetPadCompat *)pads_;

    /* Match Xbox 360's boot barrier: no input frame advances until all
     * participating consoles have reached their first in-game controller read. */
    if (!gBoot.complete) {
        DWORD begin = GetTickCount(), sent = 0;
        while (!gBoot.complete && !gFailed && GetTickCount() - begin < 60000U) {
            DWORD now = GetTickCount();
            if (!gHosting && (!sent || now - sent >= 100U)) {
                send_packet_to(gHostPeer, mknet::BOOT_READY, gSession, 0, 0);
                sent = now;
            }
            pump_packets();
            if (gHosting && gBoot.all_ready()) {
                gBoot.complete = true;
                for (int i = 0; i < peer_count(); ++i) {
                    gPeers[i].last_received = now;
                    send_peer_message(i, mknet::BOOT_GO, 0, 0);
                }
            }
            if (!gBoot.complete) Sleep(1);
        }
        if (!gBoot.complete) gFailed = true;
    }

    if (!apply_host_state_for_current_frame()) {
        gFailed = true;
    }

    mknet::Pad input[2];
    memset(input, 0, sizeof(input));
    for (unsigned i = 0; i < gLocalCount && i < 2; ++i) {
        input[i].buttons = pads[i].err_no ? 0 : pads[i].button;
        input[i].x = pads[i].err_no ? 0 : pads[i].stick_x;
        input[i].y = pads[i].err_no ? 0 : pads[i].stick_y;
    }
    sample_locals_crossplay_hash(input);

    DWORD begin = GetTickCount(), sent = 0;
    DWORD last_rx = begin;
    uint32_t sent_complete = 0xFFFFFFFFU;
    mknet::Pad frame_pads[mknet::MAX_PLAYERS];

    while (!gFailed && !gStream.fault) {
        unsigned before_rx = gNetRxInput;
        pump_packets();
        DWORD now = GetTickCount();
        if (gNetRxInput != before_rx) last_rx = now;

        if (!sent || now - sent >= 15U || (gHosting && sent_complete != gStream.latest_complete)) {
            uint8_t packet[mknet::MAX_PACKET];
            int n = 0;
            if (gHosting) {
                int pc = peer_count();

                /* Same 2P/3P/4P early-input path as Xbox 360. */
                for (int i = 0; i < pc; ++i) {
                    n = gStream.client_packet(packet, gSession, gPeers[i].slot);
                    if (!n) break;
                    int sr = sendto(gSocket, (const char *)packet, n, 0,
                                    (const sockaddr *)&gPeers[i].addr, sizeof(gPeers[i].addr));
                    ++gNetTxInput;
                    if (sr == SOCKET_ERROR) ++gNetTxInputFail;
                }

                /* Authoritative redundant complete frame sets. */
                for (int i = 0; i < pc; ++i) {
                    n = gStream.frameset_packet(packet, gSession, gPeers[i].slot);
                    if (!n) break;
                    int sr = sendto(gSocket, (const char *)packet, n, 0,
                                    (const sockaddr *)&gPeers[i].addr, sizeof(gPeers[i].addr));
                    ++gNetTxInput;
                    if (sr == SOCKET_ERROR) ++gNetTxInputFail;
                }
            } else {
                n = gStream.client_packet(packet, gSession);
                if (!n) break;
                int sr = sendto(gSocket, (const char *)packet, n, 0,
                                (const sockaddr *)&gHostPeer, sizeof(gHostPeer));
                ++gNetTxInput;
                if (sr == SOCKET_ERROR) ++gNetTxInputFail;
            }
            sent = now;
            sent_complete = gStream.latest_complete;
        }

        if (gHosting) {
            if (host_gameplay_timeout(now)) break;
        } else if (now - last_rx > (gFirstGameplayInput ? 15000U : 60000U)) {
            break;
        }

        if (menu_frame_window_ready() && gStream.consume(frame_pads)) {
            memset(pads, 0, sizeof(*pads) * count);
            for (unsigned i = 0; i < gPlayerCount && i < (unsigned)count; ++i) {
                pads[i].button = frame_pads[i].buttons;
                pads[i].stick_x = frame_pads[i].x;
                pads[i].stick_y = frame_pads[i].y;
                pads[i].err_no = 0;
            }
            for (int i = (int)gPlayerCount; i < count; ++i) pads[i].err_no = 1;
            return;
        }
        Sleep(1);
    }

    if(gStream.fault) crossplay_fault_log();
    log_line("MK64XNET: gameplay lockstep stopped frame=%lu fault=%u failed=%u RX=%u REJ=%u TX=%u TF=%u\n",
             (unsigned long)gStream.frame, gStream.fault ? 1U : 0U, gFailed ? 1U : 0U,
             gNetRxInput, gNetRxInputReject, gNetTxInput, gNetTxInputFail);
    gFailed = true;
    const bool desync = gStream.fault;
    char detail[96];
    snprintf(detail, sizeof(detail), "FRAME %lu  RX %u  REJECTED %u",
             (unsigned long)gStream.frame, gNetRxInput, gNetRxInputReject);
    xbox_netplay_shutdown();
    ui_consume();
    for (;;) {
        ui_screen(desync ? "GAME STATE MISMATCH" : "CONNECTION LOST",
                  "SESSION STOPPED", detail,
                  gDiagnosticsEnabled ? "NETPLAY LOG SAVED" : "DIAGNOSTICS WERE DISABLED",
                  "", "B EXIT TO DASHBOARD");
        if (ui_pressed() & CONT_B) XLaunchNewImage(NULL, NULL);
        Sleep(16);
    }
}


extern "C" void xbox_netplay_set_menu_sync(int enabled) { gMenuSync = enabled != 0; }
extern "C" int xbox_netplay_host_race_state(void) { return gAppliedHostRaceState; }

extern "C" int xbox_netplay_boot_menu(void) {
    /* Absolutely no filesystem or network work before the player chooses an
     * online mode. This is the critical difference from R1. */
    int selection = 0;
    ui_consume();

    for (;;) {
        char a[64], b[64], c[64], footer[80];
        snprintf(a, sizeof(a)-1, "%c OFFLINE", selection == 0 ? '>' : ' '); a[sizeof(a)-1] = 0;
        snprintf(b, sizeof(b)-1, "%c HOST 2-4 PLAYER GAME (OG ONLY)", selection == 1 ? '>' : ' '); b[sizeof(b)-1] = 0;
        snprintf(c, sizeof(c)-1, "%c JOIN 2-4 PLAYER GAME (X360 HOST)", selection == 2 ? '>' : ' '); c[sizeof(c)-1] = 0;
        snprintf(footer, sizeof(footer)-1, "A SELECT  UP/DOWN CHOOSE  Y DIAGNOSTICS %s", gDiagnosticsEnabled ? "ON" : "OFF");
        footer[sizeof(footer)-1] = 0;
        ui_screen("MARIO KART 64 - ONLINE", a, b, c,
                  "CROSSPLAY: XBOX 360 MUST HOST",
                  footer);

        uint32_t p = ui_pressed();
        if (p & kUiYButton) {
            gDiagnosticsEnabled = !gDiagnosticsEnabled;
            if (!gDiagnosticsEnabled && gLog) { fflush(gLog); fclose(gLog); gLog = 0; }
            ui_consume();
            continue;
        }
        if (p & CONT_DPAD_DOWN) selection = (selection + 1) % 3;
        if (p & CONT_DPAD_UP) selection = (selection + 2) % 3;

        if (p & CONT_A) {
            if (selection == 0) {
                ui_consume();
                return 0;
            }

            unsigned local_players = 1;
            bool host = selection == 1;
            if (!select_local_players(host, local_players)) continue;

            char host_ip[64] = {0};
            if (!host && !edit_host_ip(host_ip)) continue;

            bool ok = begin_online_session(host, local_players, host_ip);
            if (ok) {
                char role[64], slots[64], delay[64];
                snprintf(role, sizeof(role)-1, "%s - LOCAL SLOT P%u", host ? "HOST" : "JOIN", gLocalSlot + 1);
                role[sizeof(role)-1] = 0;
                snprintf(slots, sizeof(slots)-1, "PLAYERS: %u   LOCAL RACERS: %u", gPlayerCount, gLocalCount);
                slots[sizeof(slots)-1] = 0;
                snprintf(delay, sizeof(delay)-1, "NEGOTIATED INPUT DELAY: %u FRAMES", gChosenDelay);
                delay[sizeof(delay)-1] = 0;
                ui_screen("SESSION ESTABLISHED", role, slots, delay,
                          "STARTING MARIO KART 64", "MK4P V7 / UDP 6464");
                Sleep(700);
                ui_consume();
                return 1;
            }

            wait_return_to_menu(gJoinRejected ? "HOST LOBBY HAS NO FREE RACER SLOTS" : "NO GAME WAS STARTED");
        }
        Sleep(16);
    }
}

extern "C" void xbox_netplay_pump(void) {
    if (!gActive || gSocket == INVALID_SOCKET) return;
    pump_packets();
}

extern "C" void xbox_netplay_shutdown(void) {
    if (gSocket != INVALID_SOCKET) {
        if (gActive) {
            if (gHosting) {
                for (int i = 0; i < peer_count(); ++i) send_peer_message(i, mknet::GOODBYE, 0, 0);
            } else if (gHostSessionKnown) {
                send_packet_to(gHostPeer, mknet::GOODBYE, gSession, 0, 0);
            }
        }
        closesocket(gSocket);
        gSocket = INVALID_SOCKET;
    }
    gActive = false;
    if (gLog) {
        fflush(gLog);
        fclose(gLog);
        gLog = 0;
    }
}

extern "C" int xbox_netplay_active(void) { return gActive ? 1 : 0; }
extern "C" int xbox_netplay_hosting(void) { return gHosting ? 1 : 0; }
extern "C" int xbox_netplay_crossplay(void) { return (gActive && gCrossplay) ? 1 : 0; }
extern "C" int xbox_netplay_player_count(void) { return gActive ? int(gPlayerCount) : 1; }
extern "C" int xbox_netplay_local_slot(void) { return gActive ? int(gLocalSlot) : 0; }
extern "C" int xbox_netplay_local_count(void) { return gActive ? int(gLocalCount) : 1; }
extern "C" unsigned int xbox_netplay_frame(void) { return gActive ? gStream.frame : 0U; }


/* MK64_CROSSPLAY_COMPONENT_DIAG_R15_NET implementation.
 * Writes one snapshot at the first crossplay fault. No packet/wire changes.
 */
static bool mkdiag_component_written = false;

static void mkdiag_component_append(const char *text) {
    if (!gDiagnosticsEnabled) return;
    static const char *paths[] = {
        "game:\\mk64-crossplay-components.log",
        "D:\\mk64-crossplay-components.log",
        "T:\\mk64-crossplay-components.log",
        "mk64-crossplay-components.log"
    };
    unsigned int i;
    if (!text || !text[0]) return;
    for (i = 0; i < sizeof(paths)/sizeof(paths[0]); ++i) {
        HANDLE f = CreateFileA(paths[i], GENERIC_WRITE, FILE_SHARE_READ,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD wrote = 0;
            SetFilePointer(f, 0, NULL, FILE_END);
            WriteFile(f, text, (DWORD)strlen(text), &wrote, NULL);
            CloseHandle(f);
            return;
        }
    }
}

static void mkdiag_write_component_snapshot(void) {
    if (!gDiagnosticsEnabled) return;
    unsigned int d[40];
    char line[1024];
    if (mkdiag_component_written) return;
    mkdiag_component_written = true;
    memset(d, 0, sizeof(d));
    mk64_crossplay_component_diag(d, 40);

    _snprintf(line, sizeof(line)-1,
        "MKDIAG V1 MAGIC=%08X TIMER=%08X GS=%08X MODE=%08X SEED=%08X CORE=%08X FULL=%08X\r\n",
        d[0],d[1],d[2],d[3],d[4],d[5],d[28]);
    line[sizeof(line)-1]=0; mkdiag_component_append(line);

    _snprintf(line, sizeof(line)-1,
        "MKDIAG P1 META=%08X POSRAW=%08X VELRAW=%08X POSQ=%08X VELQ=%08X TYPE=%08X LAP=%08X EFFECTS=%08X\r\n",
        d[6],d[7],d[8],d[9],d[10],d[30],d[31],d[32]);
    line[sizeof(line)-1]=0; mkdiag_component_append(line);

    _snprintf(line, sizeof(line)-1,
        "MKDIAG P1BITS PX=%08X PY=%08X PZ=%08X VX=%08X VY=%08X VZ=%08X\r\n",
        d[16],d[17],d[18],d[19],d[20],d[21]);
    line[sizeof(line)-1]=0; mkdiag_component_append(line);

    _snprintf(line, sizeof(line)-1,
        "MKDIAG P2 META=%08X POSRAW=%08X VELRAW=%08X POSQ=%08X VELQ=%08X TYPE=%08X LAP=%08X EFFECTS=%08X\r\n",
        d[11],d[12],d[13],d[14],d[15],d[33],d[34],d[35]);
    line[sizeof(line)-1]=0; mkdiag_component_append(line);

    _snprintf(line, sizeof(line)-1,
        "MKDIAG P2BITS PX=%08X PY=%08X PZ=%08X VX=%08X VY=%08X VZ=%08X\r\n",
        d[22],d[23],d[24],d[25],d[26],d[27]);
    line[sizeof(line)-1]=0; mkdiag_component_append(line);
}
