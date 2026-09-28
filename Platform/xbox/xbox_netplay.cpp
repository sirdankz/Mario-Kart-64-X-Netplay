/* MK64_R62_COMPLETE_RESULTS_SOCIAL_PLAYERS_IN_GAME */
/* MK64_R61_SOCIAL_HUB_PROFILE_MAIL_FONT_FIX */
/* MK64_R60_BRIDGED_NAT_HARDENING */
/* MK64_R58_5_OG_UI_GPU_SAFETY */
// Copyright (c) 2026 sirdankz
// SPDX-License-Identifier: MPL-2.0
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
/* MK64_R54_1_OG_UI_UX_DELETE_REPAIR */
/* MK64_R55_LOW_LATENCY_EXACT_CURRENT */

/* MK64_R53_OG_UDATA_ACCOUNT_SAVE
 * Public Match account/settings persistence always uses the Xbox UDATA drive.
 * Physical title folder for this XBE Title ID: E:/UDATA/FFFF0401/.
 * Works identically from a loose XBE or an XISO.
 */
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
/* R57 directory presence is shown only while Public Match is connected. */
static int gR57OnlineCount=-1;
static int r62_game_total=-1;
static bool r62_modal_lobby=false;
static bool gR57PresenceActive=false;
/* R59 world chat is a control-plane feature.  Keep its render/cache state
 * independent from room chat so RS can switch views without losing either
 * history.  It is active only after a Public Match account is signed in. */
static bool gR59WorldUiActive=false;
static bool gR59WorldOverlaySuppress=false;
static bool gR59WorldLobbyView=false;
static bool gR59WorldKeyboard=false;
static unsigned gR59WorldSeq=0;
static DWORD gR59WorldLastPoll=0,gR59WorldResume=0;
static bool gR59WorldPendingReady=false;
static char gR59WorldPending[56]={0};
static char gR59WorldLines[6][88]={{0}};
static const char *kConfigPath = "D:/mk64_netplay.cfg";
static const char *gLogPath = "D:/mk64-netplay.log";

static FILE *gLog = 0;
/* R58.4 restores the three OG diagnostic outputs as independent options. */
/* MK64_R59_1_LOBBY_PRESENCE_SMOOTHNESS */
static bool gNetplayLoggingEnabled = false;
static bool gAstraLoggingEnabled = false;
static bool gComponentLoggingEnabled = false;
static bool gDiagnosticsEnabled = false; /* derived master gate; all loggers default OFF */
static unsigned gR584PunchTraceCount = 0;
static unsigned gR584JoinLoopTraceCount = 0;
static bool mkdiag_component_written = false;
static bool gHosting = false;
static bool gLanJoin = false;
static bool gActive = false;
static bool gFailed = false;
static bool gHostSessionKnown = false;
static bool gStartSent = false;
static bool gJoinRejected = false;
/* MK64_R45_CONTROLS_PREMENU: GOODBYE is a clean return-to-menu event. */
static bool gReturnToPremenu = false;
static SOCKET gSocket = INVALID_SOCKET;
/* R58.2: Public Match NAT/gameplay uses a dedicated ephemeral UDP socket. */
static SOCKET gR58Socket = INVALID_SOCKET;
static uint8_t gSession[16];
static uint8_t gNonce[16];
static sockaddr_in gHostPeer;
static sockaddr_in gJoinTarget;
static PeerState gPeers[mknet::MAX_PLAYERS - 1];

/* MK64_R56A_DIRECT_MESH_FALLBACK
 * 3-4 player optimization only. Guests send their CLIENT_INPUT directly to
 * other guests while the host keeps relaying the same packet as a fallback.
 * Duplicate identical frame input is already safe in Stream4. */
struct MeshPeerState {
    bool used;
    bool direct_seen;
    sockaddr_in addr;
    unsigned slot;
    unsigned local_count;
    DWORD last_probe;
    mknet::Latency latency;
};
struct MeshReportState { unsigned budget,samples; bool seen; };
static MeshPeerState gMeshPeers[mknet::MAX_PLAYERS - 1];
static MeshReportState gMeshReports[mknet::MAX_PLAYERS][mknet::MAX_PLAYERS];
static unsigned gMeshDirectTx = 0, gMeshDirectRx = 0, gMeshProbeTx = 0, gMeshProbeRx = 0;
static DWORD gMeshLastAnnounce = 0;

static unsigned gAssignedSlot = 0;
static unsigned gPlayerCount = 1;
static unsigned gLocalSlot = 0;
static unsigned gLocalCount = 1;
static unsigned gDesiredPlayers = 2;
static unsigned gChosenDelay = 4;
static bool gSessionSplit = false;
static bool gCrossplay = false;
/* MK64_R66_OG_60HZ_NETPLAY: menu opt-in, never auto-upgrade a legacy or Xbox 360 session. */
static bool gR66Requested60 = false;
static bool gR66VersionMismatch = false;
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

/* R58.5 OG UI GPU safety.
 * The pre-game bitmap font used to emit one six-vertex quad for every lit
 * 5x7 font pixel.  The richer Public Room screen can exceed the NV2A
 * deferred UI vertex/push-buffer budget in a single frame.  Keep a hard
 * menu-only ceiling as a final guard even if future screens add more text. */
static unsigned gUiQuadCount = 0;
static const unsigned kUiQuadBudget = 2200; /* <= 13,200 UI vertices/frame */
static bool gUiQuadClipped = false;
static void ui_budget_reset(void) { gUiQuadCount = 0; gUiQuadClipped = false; }

static void ui_quad(float x0, float y0, float x1, float y1, uint32_t argb) {
    if (gUiQuadCount >= kUiQuadBudget) { gUiQuadClipped = true; return; }
    dc_fast_t *v = pvr_reserve(UI_KIND_OP, 6);
    if (!v) { gUiQuadClipped = true; return; }
    ++gUiQuadCount;
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
        GLYPH('M',0x11,0x1B,0x1F,0x15,0x11,0x11,0x11) /* R62: distinct, stronger M at TV font scales */
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
        GLYPH('!',0x04,0x04,0x04,0x04,0x04,0x00,0x04)
        GLYPH(',',0x00,0x00,0x00,0x00,0x06,0x06,0x04)
        GLYPH('+',0x00,0x04,0x04,0x1F,0x04,0x04,0x00)
        GLYPH('_',0x00,0x00,0x00,0x00,0x00,0x00,0x1F)
        GLYPH('=',0x00,0x1F,0x00,0x1F,0x00,0x00,0x00)
        GLYPH('@',0x0E,0x11,0x17,0x15,0x17,0x10,0x0E)
        GLYPH('#',0x0A,0x1F,0x0A,0x0A,0x1F,0x0A,0x00)
        GLYPH('[',0x0E,0x08,0x08,0x08,0x08,0x08,0x0E)
        GLYPH(']',0x0E,0x02,0x02,0x02,0x02,0x02,0x0E)
        GLYPH('*',0x00,0x0A,0x04,0x1F,0x04,0x0A,0x00)
        GLYPH('%',0x19,0x19,0x02,0x04,0x08,0x13,0x13)
        GLYPH('\'',0x04,0x04,0x08,0x00,0x00,0x00,0x00)
        default: { static const uint8_t blank[7]={0,0,0,0,0,0,0}; return blank; }
    }
#undef GLYPH
}

/* R61: count the exact vertical-merged run rectangles for one glyph.
 * Reserve the entire glyph's budget before touching the NV2A deferred buffer,
 * so a near-full UI frame cannot cut a letter halfway through. */
static unsigned r61_glyph_quads(const uint8_t *rows) {
    bool live[5][5]={{false}}; unsigned total=0;
    for (int ry=0; ry<=7; ++ry) {
        bool present[5][5]={{false}};
        if (ry<7) { const uint8_t bits=rows[ry];int rx=0;
            while(rx<5){if(!(bits&(1u<<(4-rx)))){++rx;continue;}
                int first=rx;while(rx+1<5&&(bits&(1u<<(4-(rx+1)))))++rx;
                present[first][rx]=true;++rx;
            }
        }
        for(int x0=0;x0<5;++x0)for(int x1=x0;x1<5;++x1){
            if(live[x0][x1]&&!present[x0][x1]){++total;live[x0][x1]=false;}
            if(present[x0][x1])live[x0][x1]=true;
        }
    }
    return total;
}
static unsigned r61_line_quads(const char *line) {
    unsigned n=0;
    if(!line)return 0;
    for(;*line;++line){char c=*line;
        if(c>='a'&&c<='z')c=(char)(c-'a'+'A');
        if(c!=' ')n+=r61_glyph_quads(ui_glyph(c));
    }
    return n;
}
static bool r61_line_fits(const char *line,unsigned reserve) {
    return gUiQuadCount+r61_line_quads(line)+reserve<=kUiQuadBudget;
}

static int gR73ControlScrollRow=-1;
static void ui_text(float x, float y, const char *text, float scale, uint32_t argb) {
    if (!text) return;
    const size_t chars=strlen(text);
    const float safeRight=gR73ControlScrollRow>=0?565.0f:592.0f;
    if(chars&&x<safeRight){const float maxScale=(safeRight-x)/((float)chars*6.0f);if(scale>maxScale)scale=maxScale;}
    if(x>=safeRight)return;
    const float advance = 6.0f * scale;
    for (const char *p = text; *p; ++p, x += advance) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        if (c == ' ') continue;
        const uint8_t *rows = ui_glyph(c);
        unsigned need=r61_glyph_quads(rows);
        if (gUiQuadCount+need > kUiQuadBudget) {gUiQuadClipped=true;break;}

        /* R58.5: merge identical horizontal runs vertically.  A typical
         * glyph now needs ~2-9 rectangles instead of one rectangle for every
         * lit pixel.  This cuts the Public Room frame from ~17K vertices to
         * roughly 5-6K for the normal one-player host case. */
        int active[5][5];
        for (int a = 0; a < 5; ++a) for (int b = 0; b < 5; ++b) active[a][b] = -1;
        for (int ry = 0; ry <= 7; ++ry) {
            bool present[5][5] = {{false}};
            if (ry < 7) {
                uint8_t bits = rows[ry];
                int rx = 0;
                while (rx < 5) {
                    if (!(bits & (1u << (4-rx)))) { ++rx; continue; }
                    int x0 = rx;
                    while (rx + 1 < 5 && (bits & (1u << (4-(rx+1))))) ++rx;
                    int x1 = rx;
                    present[x0][x1] = true;
                    ++rx;
                }
            }
            for (int x0 = 0; x0 < 5; ++x0) {
                for (int x1 = x0; x1 < 5; ++x1) {
                    if (active[x0][x1] >= 0 && !present[x0][x1]) {
                        float qx0 = (float)(int)(x + x0 * scale + 0.5f);
                        float qx1 = (float)(int)(x + (x1 + 1) * scale + 0.5f);
                        float qy0 = (float)(int)(y + active[x0][x1] * scale + 0.5f);
                        float qy1 = (float)(int)(y + ry * scale + 0.5f);
                        if(qx1<=qx0)qx1=qx0+1;
                        if(qy1<=qy0)qy1=qy0+1;
                        ui_quad(qx0, qy0, qx1, qy1, argb);
                        active[x0][x1] = -1;
                    }
                    if (present[x0][x1] && active[x0][x1] < 0) active[x0][x1] = ry;
                }
            }
        }
    }
}

/* R73 OG music: UDATA is writable for both XBE and XISO; D: is fallback. */
static bool gR73MusicEnabled=true,gR73MusicLoaded=false;
extern "C" void xbox_music_notify_changed(void);
static void r73_music_load(void){
    if(gR73MusicLoaded)return;gR73MusicLoaded=true;
    const char *paths[2]={"U:/mk64-music.cfg","D:/mk64-music.cfg"};
    FILE *f=0;for(int i=0;i<2&&!f;++i)f=fopen(paths[i],"rb");
    if(!f)return;char b[16]={0};size_t n=fread(b,1,sizeof(b)-1,f);fclose(f);
    if(n>=10&&!strncmp(b,"MKMUSIC1|",9)&&(b[9]=='0'||b[9]=='1'))
        gR73MusicEnabled=(b[9]=='1');
}
extern "C" int xbox_music_enabled(void){r73_music_load();return gR73MusicEnabled?1:0;}
static bool r73_music_set_enabled(bool enabled){
    gR73MusicLoaded=true;gR73MusicEnabled=enabled;xbox_music_notify_changed();
    const char *paths[2]={"U:/mk64-music.cfg","D:/mk64-music.cfg"};
    const char *b=enabled?"MKMUSIC1|1\n":"MKMUSIC1|0\n";
    for(int i=0;i<2;++i){FILE *f=fopen(paths[i],"wb");if(!f)continue;
        size_t n=fwrite(b,1,11,f);int closed=fclose(f);
        if(n==11&&closed==0)return true;
    }
    return false;
}
/* MK64_R54_1_OG_UI_UX_DELETE_REPAIR: CRT-safe layout; colors unchanged. */
static float r54_og_fit(const char *t,float pref,float minv,float maxw){if(!t||!*t)return pref;float v=maxw/((float)strlen(t)*6.0f);return v>=pref?pref:(v<minv?minv:v);}
static void r54_og_footer(const char *t){if(!t||!*t)return;int n=(int)strlen(t);if(n<=48){ui_text(48,414,t,r54_og_fit(t,1.75f,1.30f,542.0f),0xFFB8B8B8);return;}int cut=48;while(cut>25&&t[cut]!=' ')--cut;if(cut<=25)cut=48;char a[72],b[96];int q=cut<71?cut:71;memcpy(a,t,q);a[q]=0;const char *r=t+cut;while(*r==' ')++r;strncpy(b,r,95);b[95]=0;ui_text(48,397,a,r54_og_fit(a,1.50f,1.20f,542.0f),0xFFB8B8B8);ui_text(48,425,b,r54_og_fit(b,1.50f,1.20f,542.0f),0xFFB8B8B8);}
static void ui_screen(const char *title,const char *a,const char *b,const char *c,const char *d,const char *footer){
    struct GfxRenderingAPI *r=&gfx_nv2a_api;r->start_frame();ui_budget_reset();r->set_depth_test(0);r->set_depth_mask(0);r->select_texture(0,0);gfx_pvr_set_blend(UI_KIND_OP);
    ui_quad(0,0,640,480,0xFF102030);ui_quad(42,94,590,98,0xFFFFD050);
    const char *tt=title?title:"MARIO KART 64 - ONLINE";ui_text(46,44,tt,r54_og_fit(tt,2.8f,2.1f,548.0f),0xFFFFD050);
    if(gR59WorldUiActive){const char *hub="(C) 2026 SIRDANKZ  ORIGINAL CODE: MPL-2.0";ui_text(46,75,hub,1.0f,0xFF90D0FF);ui_text(46,86,"NINTENDO / TEAM RESURGENT / UPSTREAM RIGHTS UNCHANGED",.80f,0xFF90D0FF);}
    if(gR57PresenceActive){char online[32],inGame[40];snprintf(online,sizeof(online)-1,"PLAYERS ONLINE: %s",gR57OnlineCount>=0?"":"--");if(gR57OnlineCount>=0)snprintf(online,sizeof(online)-1,"PLAYERS ONLINE: %d",gR57OnlineCount);online[sizeof(online)-1]=0;ui_text(449,70,online,1.02f,0xFF90D0FF);snprintf(inGame,sizeof(inGame)-1,"PLAYERS IN GAME: %s",r62_game_total>=0?"":"--");if(r62_game_total>=0)snprintf(inGame,sizeof(inGame)-1,"PLAYERS IN GAME: %d",r62_game_total);inGame[sizeof(inGame)-1]=0;ui_text(449,82,inGame,1.02f,0xFF90D0FF);}
    const char *l0=a?a:"",*l1=b?b:"",*l2=c?c:"",*l3=d?d:"";
    ui_text(52,132,l0,r54_og_fit(l0,1.9f,1.4f,510.0f),0xFFFFFFFF);ui_text(52,188,l1,r54_og_fit(l1,1.9f,1.4f,510.0f),0xFFFFFFFF);ui_text(52,246,l2,r54_og_fit(l2,1.9f,1.4f,510.0f),0xFF90D0FF);ui_text(52,304,l3,r54_og_fit(l3,1.9f,1.4f,510.0f),0xFF90D0FF);
    /* The footer is mandatory navigation information, so reserve it before
     * any optional World Chat lines consume the remaining GPU-safe budget. */
    r54_og_footer(footer?footer:"");
    /* R59.2 adaptive world-chat layout.  Use empty menu space instead of
     * always squeezing chat into the bottom strip.  The busiest screens keep
     * a compact two-message ticker; sparse screens show up to all six cached
     * messages at a larger, easier-to-read size. */
    if(gR59WorldUiActive&&!gR59WorldOverlaySuppress){
        int first=5,count=1;float titleY=345.0f,msgY=368.0f,step=20.0f,pref=1.0f,minv=.90f;
        if(!l1[0]&&!l2[0]&&!l3[0]){first=0;count=6;titleY=166.0f;msgY=191.0f;step=29.0f;pref=.96f;minv=.76f;}
        else if(!l2[0]&&!l3[0]){first=1;count=5;titleY=222.0f;msgY=247.0f;step=28.0f;pref=.95f;minv=.75f;}
        else if(!l3[0]){first=3;count=3;titleY=288.0f;msgY=313.0f;step=26.0f;pref=.94f;minv=.74f;}
        float panelBottom=msgY+(count-1)*step+15.0f;if(panelBottom>398.0f)panelBottom=398.0f;
        if(r61_line_fits("WORLD CHAT   RS OPEN",18)){
            ui_quad(44,titleY-8.0f,590,panelBottom,0xFF16293B);ui_text(52,titleY,"WORLD CHAT   RS OPEN",1.0f,0xFF90D0FF);
            bool any=false;int row=0;for(int i=first;i<6&&row<count;++i){if(!gR59WorldLines[i][0])continue;const char *w=gR59WorldLines[i];char brief[56];strncpy(brief,w,sizeof(brief)-1);brief[sizeof(brief)-1]=0;
                if(!r61_line_fits(brief,2))break;
                ui_text(52,msgY+row*step,brief,r54_og_fit(brief,pref,minv,508.0f),0xFFFFFFFF);any=true;++row;}
            if(!any&&r61_line_fits("NO WORLD MESSAGES YET",0))ui_text(52,msgY,"NO WORLD MESSAGES YET",pref,0xFFFFFFFF);
        }
    }
    /* R73 controller scrollbar: 18 rows, four visible per page. */
    if(gR73ControlScrollRow>=0&&tt&&strstr(tt,"CONTROLLER ")==tt){
        int top=(gR73ControlScrollRow/4)*4;int thumb=132+(top/4)*179/4;
        ui_quad(583,127,591,363,0xFF27415A);
        ui_quad(583,(float)thumb,591,(float)(thumb+47),0xFFFFD050);
        char scroll[32];snprintf(scroll,sizeof(scroll)-1,"%02d-%02d / 18",top+1,top+4>18?18:top+4);
        scroll[sizeof(scroll)-1]=0;ui_text(467,367,scroll,1.03f,0xFF90D0FF);
    }
    r->end_frame();r->finish_render();
}

/* MK64_R62_3_RIGHT_TRIGGER_STATS */
/* Only premenu sees the physical Right Trigger via CONT_C. */
extern "C" int xbox_ui_right_trigger_down(void);
static uint32_t ui_buttons_now(void) {
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (!dev) return 0;
    cont_state_t *st = (cont_state_t *)maple_dev_status(dev);
    return st ? (st->buttons | (xbox_ui_right_trigger_down() ? CONT_C : 0U)) : 0;
}

static const uint32_t kUiYButton = (1U << 9);
/* kos_xbox.c maps the physical right-thumb click to the otherwise-unused
 * Dreamcast CONT_D bit so the OG pre-game UI can use the same RS hotkey as
 * Xbox 360 without affecting MK64 gameplay input. */
static const uint32_t kUiRightStickButton = (1U << 11);
static uint32_t gUiPrevButtons = 0;
static uint32_t ui_pressed(void) {
    uint32_t now = ui_buttons_now();
    uint32_t edge = now & ~gUiPrevButtons;
    gUiPrevButtons = now;
    return edge;
}
static void ui_consume(void) { gUiPrevButtons = ui_buttons_now(); }
static DWORD r54_og_delete_begin=0; static bool r54_og_delete_fired=false;
static bool r54_og_delete_hold(void){uint32_t b=ui_buttons_now();bool down=(b&CONT_X)&&(b&CONT_Y);DWORD now=GetTickCount();if(!down){r54_og_delete_begin=0;r54_og_delete_fired=false;return false;}if(!r54_og_delete_begin)r54_og_delete_begin=now;if(!r54_og_delete_fired&&now-r54_og_delete_begin>=3000U){r54_og_delete_fired=true;return true;}return false;}


static bool ui_second_controller_connected(void) {
    return maple_enum_type(1, MAPLE_FUNC_CONTROLLER) != 0;
}

static void log_open(void) {
    if (!gNetplayLoggingEnabled) return;
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
    if (!gNetplayLoggingEnabled) return;
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
    if (!gNetplayLoggingEnabled || !gActive) return;
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

/* R59.1: mirror the 360 pre-start peer cleanup so cancelled/disconnected
 * guests cannot remain as ghost racer slots on the host screen. */
static void remove_peer(int idx) {
    int n = peer_count();
    if (idx < 0 || idx >= n) return;
    log_line("R59.1_LOBBY: removing P%u before start\n", gPeers[idx].slot + 1);
    for (int i = idx; i < n - 1; ++i) {
        gPeers[i] = gPeers[i + 1];
        gPeers[i].ready = false;
        gPeers[i].acked = false;
        gPeers[i].last_offer = 0;
    }
    reset_peer(gPeers[n - 1]);
    assign_slots();
}

static void prune_prestart_timeouts(DWORD now) {
    if (gStartSent) return;
    for (int i = peer_count() - 1; i >= 0; --i)
        if (now - gPeers[i].last_received > 8000U) remove_peer(i);
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

static bool same_address(const sockaddr_in &a, const sockaddr_in &b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

static void endpoint_text(char *dst, unsigned size, const sockaddr_in &a) {
    uint32_t ip = ntohl(a.sin_addr.s_addr);
    snprintf(dst, size - 1, "%u.%u.%u.%u:%u",
              unsigned(ip >> 24), unsigned((ip >> 16) & 255), unsigned((ip >> 8) & 255), unsigned(ip & 255),
              unsigned(ntohs(a.sin_port)));
    dst[size - 1] = 0;
}


/* R58.2 relay envelope is 4 bytes; current largest MK4P packet remains within a 1472-byte UDP payload. */
/* MK64_R58_AUTO_NAT_RELAY
 * Public Match uses a dedicated ephemeral UDP socket to rendezvous
 * through the directory server on UDP 6465.  Peers try direct UDP hole
 * punching first.  If the direct path is unavailable, MK4P datagrams are
 * transparently wrapped and relayed through the directory server.
 * Direct/manual play is unchanged. */
struct R58NatRoute {
    bool used;
    bool direct_seen;
    uint8_t cookie[8];
    uint8_t relay_id;
    sockaddr_in addr;
    DWORD last_direct_rx;
    DWORD last_punch;
    unsigned relay_tx;
    unsigned relay_rx;
    unsigned direct_valid_rx; /* R60: only real MK4P packets validate direct NAT. */
};
static bool gR58NatEnabled=false, gR58NatHost=false;
static bool gR58NatHaveSelf=false;
static uint8_t gR58NatSelf[8];
static uint8_t gR58NatSelfId=0;
static char gR58NatRoom[20]={0}, gR58NatAuth1[40]={0}, gR58NatAuth2[40]={0};
static DWORD gR58NatLastControl=0;
static R58NatRoute gR58NatRoutes[mknet::MAX_PLAYERS];
static const char *kR58NatServerIp="172.233.145.244";
static const unsigned kR58NatServerPort=6465;

static bool r58_same_address(const sockaddr_in &a,const sockaddr_in &b){return a.sin_addr.s_addr==b.sin_addr.s_addr&&a.sin_port==b.sin_port;}
static sockaddr_in r58_server_addr(){sockaddr_in a;memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_addr.s_addr=inet_addr(kR58NatServerIp);a.sin_port=htons((u_short)kR58NatServerPort);return a;}
static bool r58_from_server(const sockaddr_in &a){sockaddr_in s=r58_server_addr();return r58_same_address(a,s);}
static int r58_hexval(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
static bool r58_parse_cookie(const char *s,uint8_t out[8]){if(!s||strlen(s)<16)return false;for(int i=0;i<8;++i){int a=r58_hexval(s[i*2]),b=r58_hexval(s[i*2+1]);if(a<0||b<0)return false;out[i]=(uint8_t)((a<<4)|b);}return true;}
static void r58_cookie_text(const uint8_t c[8],char out[17]){static const char h[]="0123456789abcdef";for(int i=0;i<8;++i){out[i*2]=h[c[i]>>4];out[i*2+1]=h[c[i]&15];}out[16]=0;}
static bool r58_cookie_equal(const uint8_t a[8],const uint8_t b[8]){return memcmp(a,b,8)==0;}
static int r58_route_cookie(const uint8_t c[8]){for(unsigned i=0;i<mknet::MAX_PLAYERS;++i)if(gR58NatRoutes[i].used&&r58_cookie_equal(gR58NatRoutes[i].cookie,c))return (int)i;return -1;}
static int r58_route_addr(const sockaddr_in &a){for(unsigned i=0;i<mknet::MAX_PLAYERS;++i)if(gR58NatRoutes[i].used&&r58_same_address(gR58NatRoutes[i].addr,a))return (int)i;return -1;}
static void r58_rebind_known(const sockaddr_in &oldAddr,const sockaddr_in &newAddr){
    if(r58_same_address(gHostPeer,oldAddr))gHostPeer=newAddr;
    if(r58_same_address(gJoinTarget,oldAddr))gJoinTarget=newAddr;
    for(int i=0;i<peer_count();++i)if(r58_same_address(gPeers[i].addr,oldAddr))gPeers[i].addr=newAddr;
    for(unsigned i=0;i<mknet::MAX_PLAYERS-1;++i)if(gMeshPeers[i].used&&r58_same_address(gMeshPeers[i].addr,oldAddr))gMeshPeers[i].addr=newAddr;
}
static int r58_route_install(const uint8_t c[8],const sockaddr_in &a){
    int i=r58_route_cookie(c);if(i<0){for(unsigned j=0;j<mknet::MAX_PLAYERS;++j)if(!gR58NatRoutes[j].used){i=(int)j;memset(&gR58NatRoutes[j],0,sizeof(gR58NatRoutes[j]));gR58NatRoutes[j].used=true;memcpy(gR58NatRoutes[j].cookie,c,8);break;}}
    if(i<0)return -1;R58NatRoute &r=gR58NatRoutes[i];if(r.addr.sin_family&& !r58_same_address(r.addr,a)){r58_rebind_known(r.addr,a);r.direct_seen=false;r.direct_valid_rx=0;r.last_direct_rx=0;}r.addr=a;return i;
}
static void r58_transport_close(){if(gR58Socket!=INVALID_SOCKET){closesocket(gR58Socket);gR58Socket=INVALID_SOCKET;}}
static bool r58_transport_open(){
    if(gR58Socket!=INVALID_SOCKET)return true;
    gR58Socket=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(gR58Socket==INVALID_SOCKET){log_line("R58.2_NAT: dedicated socket() failed wsa=%d\n",WSAGetLastError());return false;}
    int netbuf=128*1024;setsockopt(gR58Socket,SOL_SOCKET,SO_RCVBUF,(const char*)&netbuf,sizeof(netbuf));setsockopt(gR58Socket,SOL_SOCKET,SO_SNDBUF,(const char*)&netbuf,sizeof(netbuf));
    sockaddr_in a;memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_ANY);a.sin_port=htons(0);
    if(bind(gR58Socket,(sockaddr*)&a,sizeof(a))==SOCKET_ERROR){log_line("R58.2_NAT: dedicated bind failed wsa=%d\n",WSAGetLastError());r58_transport_close();return false;}
    u_long nb=1;if(ioctlsocket(gR58Socket,FIONBIO,&nb)==SOCKET_ERROR){log_line("R58.2_NAT: dedicated FIONBIO failed wsa=%d\n",WSAGetLastError());r58_transport_close();return false;}
    log_line("R58.2_NAT: dedicated ephemeral Public Match socket ready\n");return true;
}
static SOCKET r58_io_socket(){return (gR58NatEnabled&&gR58Socket!=INVALID_SOCKET)?gR58Socket:gSocket;}
static void r58_nat_reset(){r58_transport_close();gR584PunchTraceCount=0;gR584JoinLoopTraceCount=0;gR58NatEnabled=gR58NatHost=gR58NatHaveSelf=false;memset(gR58NatSelf,0,sizeof(gR58NatSelf));gR58NatSelfId=0;gR58NatRoom[0]=gR58NatAuth1[0]=gR58NatAuth2[0]=0;gR58NatLastControl=0;memset(gR58NatRoutes,0,sizeof(gR58NatRoutes));}
static bool r58_nat_host(const char *room,const char *token){r58_nat_reset();if(!r58_transport_open())return false;gR58NatEnabled=gR58NatHost=true;strncpy(gR58NatRoom,room?room:"",sizeof(gR58NatRoom)-1);strncpy(gR58NatAuth1,token?token:"",sizeof(gR58NatAuth1)-1);log_line("R58.2_NAT: public host dedicated NAT transport enabled\n");return true;}
static bool r58_nat_join(const char *room,const char *pid,const char *secret){r58_nat_reset();if(!r58_transport_open())return false;gR58NatEnabled=true;gR58NatHost=false;strncpy(gR58NatRoom,room?room:"",sizeof(gR58NatRoom)-1);strncpy(gR58NatAuth1,pid?pid:"",sizeof(gR58NatAuth1)-1);strncpy(gR58NatAuth2,secret?secret:"",sizeof(gR58NatAuth2)-1);log_line("R58.2_NAT: public join dedicated NAT transport enabled\n");return true;}
static int r58_raw_send(const sockaddr_in &to,const void *p,int n){if(gR58Socket==INVALID_SOCKET)return SOCKET_ERROR;return sendto(gR58Socket,(const char*)p,n,0,(const sockaddr*)&to,sizeof(to));}
static void r58_send_control(DWORD now){
    if(!gR58NatEnabled||gR58Socket==INVALID_SOCKET||!gR58NatRoom[0])return;
    DWORD interval=gR58NatHaveSelf?500U:300U;if(gR58NatLastControl&&now-gR58NatLastControl<interval)return;
    char m[180];if(gR58NatHaveSelf){char c[17];r58_cookie_text(gR58NatSelf,c);snprintf(m,sizeof(m)-1,"MKNAT1|KEEP|%s",c);}else if(gR58NatHost)snprintf(m,sizeof(m)-1,"MKNAT1|HOST|%s|%s",gR58NatRoom,gR58NatAuth1);else snprintf(m,sizeof(m)-1,"MKNAT1|JOIN|%s|%s|%s",gR58NatRoom,gR58NatAuth1,gR58NatAuth2);m[sizeof(m)-1]=0;sockaddr_in s=r58_server_addr();r58_raw_send(s,m,(int)strlen(m));gR58NatLastControl=now;
}
static void r58_send_punches(DWORD now){
    if(!gR58NatEnabled||!gR58NatHaveSelf)return;char self[17];r58_cookie_text(gR58NatSelf,self);char m[48];snprintf(m,sizeof(m)-1,"MKNAT1|PUNCH|%s",self);m[sizeof(m)-1]=0;
    for(unsigned i=0;i<mknet::MAX_PLAYERS;++i){R58NatRoute &r=gR58NatRoutes[i];if(!r.used)continue;if(r.direct_seen&&r.direct_valid_rx>=3U&&now-r.last_direct_rx<1500U)continue;if(r.last_punch&&now-r.last_punch<120U)continue;
        if(gNetplayLoggingEnabled&&gR584PunchTraceCount<24U)log_line("R58.4_TRACE: direct punch BEGIN route=%u relay_id=%u port=%u\n",i,(unsigned)r.relay_id,(unsigned)ntohs(r.addr.sin_port));
        int prc=r58_raw_send(r.addr,m,(int)strlen(m));
        if(gNetplayLoggingEnabled&&gR584PunchTraceCount<24U){int pe=(prc==SOCKET_ERROR)?WSAGetLastError():0;log_line("R58.4_TRACE: direct punch END route=%u rc=%d wsa=%d\n",i,prc,pe);++gR584PunchTraceCount;}
        r.last_punch=now;}
}
static void r58_nat_tick(DWORD now){r58_send_control(now);r58_send_punches(now);}
static void r58_mark_direct(const sockaddr_in &from){int i=r58_route_addr(from);if(i>=0){R58NatRoute &r=gR58NatRoutes[i];if(!r.direct_seen)log_line("R60_NAT: validated direct MK4P path\n");r.direct_seen=true;if(r.direct_valid_rx<0xFFFFFFFFU)++r.direct_valid_rx;r.last_direct_rx=GetTickCount();}}
static bool r58_handle_control(const uint8_t *p,int n,const sockaddr_in &from){
    if(n<7||memcmp(p,"MKNAT1|",7)!=0)return false;char b[220];int c=n<(int)sizeof(b)-1?n:(int)sizeof(b)-1;memcpy(b,p,c);b[c]=0;
    if(!strncmp(b,"MKNAT1|SELF|",12)&&r58_from_server(from)){char hex[17]={0};unsigned rid=0;if(sscanf(b,"MKNAT1|SELF|%16[^|]|%u",hex,&rid)==2&&rid>0&&rid<256){uint8_t ck[8];if(r58_parse_cookie(hex,ck)){memcpy(gR58NatSelf,ck,8);gR58NatSelfId=(uint8_t)rid;if(!gR58NatHaveSelf)log_line("R58_NAT: rendezvous registered relay_id=%u\n",rid);gR58NatHaveSelf=true;}}return true;}
    if(!strcmp(b,"MKNAT1|RESET")&&r58_from_server(from)){gR58NatHaveSelf=false;gR58NatSelfId=0;memset(gR58NatSelf,0,sizeof(gR58NatSelf));memset(gR58NatRoutes,0,sizeof(gR58NatRoutes));gR58NatLastControl=0;log_line("R58.2_NAT: server route reset - re-registering\n");return true;}
    if(!strncmp(b,"MKNAT1|INTRO|",13)&&r58_from_server(from)){char hex[17]={0},ip[32]={0},role[4]={0};unsigned rid=0,port=0;if(sscanf(b,"MKNAT1|INTRO|%16[^|]|%u|%31[^|]|%u|%3s",hex,&rid,ip,&port,role)==5&&rid>0&&rid<256){uint8_t ck[8];sockaddr_in a;memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_addr.s_addr=inet_addr(ip);a.sin_port=htons((u_short)port);if(a.sin_addr.s_addr!=INADDR_NONE&&r58_parse_cookie(hex,ck)){log_line("R58.4_TRACE: INTRO parsed role=%s relay_id=%u port=%u\n",role,rid,port);int ri=r58_route_install(ck,a);log_line("R58.4_TRACE: INTRO route_install=%d\n",ri);if(ri>=0)gR58NatRoutes[ri].relay_id=(uint8_t)rid;if(!gR58NatHost&&role[0]=='H'){log_line("R58.4_TRACE: guest host-endpoint assign BEGIN\n");gJoinTarget=a;gHostPeer=a;log_line("R58.4_TRACE: guest host-endpoint assign END\n");}log_line("R58_NAT: peer introduced role=%s relay_id=%u port=%u\n",role,rid,port);}}return true;}
    if(!strncmp(b,"MKNAT1|PUNCH|",13)&&!r58_from_server(from)){uint8_t ck[8];if(r58_parse_cookie(b+13,ck)){int i=r58_route_cookie(ck);if(i>=0){R58NatRoute &r=gR58NatRoutes[i];if(!r58_same_address(r.addr,from)){sockaddr_in old=r.addr;r.addr=from;r58_rebind_known(old,from);r.direct_seen=false;r.direct_valid_rx=0;r.last_direct_rx=0;}/* R60: a punch proves reachability only; keep relay alive until valid MK4P arrives. */if(gNetplayLoggingEnabled)log_line("R60_NAT: direct punch hint received; relay remains armed\n");}}return true;}
    return true;
}
static int r58_route_relay_id(unsigned rid){for(unsigned i=0;i<mknet::MAX_PLAYERS;++i)if(gR58NatRoutes[i].used&&gR58NatRoutes[i].relay_id==rid)return (int)i;return -1;}
static bool r58_relay_unwrap(uint8_t *p,int &n,sockaddr_in &from){
    if(n<4||p[0]!='M'||p[1]!='R'||!r58_from_server(from)||!gR58NatHaveSelf)return false;unsigned src=p[2],dst=p[3];if(dst!=gR58NatSelfId)return true;int i=r58_route_relay_id(src);if(i<0)return true;unsigned len=(unsigned)n-4U;if(len>mknet::MAX_PACKET)return true;R58NatRoute &r=gR58NatRoutes[i];memmove(p,p+4,len);n=(int)len;from=r.addr;if(!r.relay_rx)log_line("R58.2_NAT: relay fallback receiving gameplay\n");++r.relay_rx;return true;
}
static int r58_relay_send(R58NatRoute &r,const uint8_t *p,int n){
    if(!gR58NatEnabled||!gR58NatHaveSelf||!gR58NatSelfId||!r.relay_id||n<=0||n>(int)mknet::MAX_PACKET)return SOCKET_ERROR;uint8_t b[mknet::MAX_PACKET+4];b[0]='M';b[1]='R';b[2]=gR58NatSelfId;b[3]=r.relay_id;memcpy(b+4,p,n);sockaddr_in s=r58_server_addr();int rc=r58_raw_send(s,b,n+4);if(rc!=SOCKET_ERROR){if(!r.relay_tx)log_line("R58.2_NAT: relay fallback sending gameplay\n");++r.relay_tx;}return rc;
}
static bool r60_force_relay_type(mknet::Type type){
    return type==mknet::HELLO||type==mknet::OFFER||type==mknet::READY||type==mknet::START||
           type==mknet::START_ACK||type==mknet::JOIN_REJECT||type==mknet::BOOT_READY||
           type==mknet::BOOT_GO||type==mknet::GOODBYE;
}
static int r58_send_game_packet(const sockaddr_in &to,const uint8_t *p,int n,bool allow_relay,bool force_relay=false){
    SOCKET io=r58_io_socket();if(io==INVALID_SOCKET)return SOCKET_ERROR;int direct=sendto(io,(const char*)p,n,0,(const sockaddr*)&to,sizeof(to));int relay=SOCKET_ERROR;int ri=r58_route_addr(to);if(allow_relay&&ri>=0){R58NatRoute &r=gR58NatRoutes[ri];DWORD now=GetTickCount();bool direct_stable=r.direct_seen&&r.direct_valid_rx>=3U&&now-r.last_direct_rx<=1500U;if(force_relay||!direct_stable)relay=r58_relay_send(r,p,n);}return direct!=SOCKET_ERROR?direct:relay;
}

static int send_packet_to(const sockaddr_in &to, mknet::Type type, const uint8_t sid[16], const uint8_t *payload, int payload_size) {
    if (gSocket == INVALID_SOCKET || payload_size < 0 || payload_size > int(mknet::MAX_PACKET - mknet::HEADER)) return SOCKET_ERROR;
    uint8_t packet[mknet::MAX_PACKET];
    int bytes = mknet::header(packet, type, sid, payload_size);
    if (payload_size && payload) memcpy(packet + mknet::HEADER, payload, payload_size);
    bool force_relay=gR58NatEnabled&&(!gActive||r60_force_relay_type(type));
    return r58_send_game_packet(to,packet,bytes,type!=mknet::PEER_PROBE,force_relay);
}

static int send_peer_message(int index, mknet::Type type, const uint8_t *payload, int payload_size) {
    if (index < 0 || index >= peer_count()) return SOCKET_ERROR;
    return send_packet_to(gPeers[index].addr, type, gSession, payload, payload_size);
}

static void mesh_reset(void) {
    memset(gMeshPeers, 0, sizeof(gMeshPeers));
    memset(gMeshReports, 0, sizeof(gMeshReports));
    gMeshDirectTx = gMeshDirectRx = gMeshProbeTx = gMeshProbeRx = 0;
    gMeshLastAnnounce = 0;
}

static int mesh_find_slot(unsigned slot, unsigned local_count) {
    for (unsigned i = 0; i < mknet::MAX_PLAYERS - 1; ++i)
        if (gMeshPeers[i].used && gMeshPeers[i].slot == slot && gMeshPeers[i].local_count == local_count) return int(i);
    return -1;
}

static int mesh_alloc_slot(unsigned slot, unsigned local_count) {
    int existing = mesh_find_slot(slot, local_count);
    if (existing >= 0) return existing;
    for (unsigned i = 0; i < mknet::MAX_PLAYERS - 1; ++i) {
        if (!gMeshPeers[i].used) {
            memset(&gMeshPeers[i], 0, sizeof(gMeshPeers[i]));
            gMeshPeers[i].used = true;
            gMeshPeers[i].slot = slot;
            gMeshPeers[i].local_count = local_count;
            return int(i);
        }
    }
    return -1;
}

static void mesh_install_info(const uint8_t *q) {
    unsigned slot = q[0], locals = unsigned(q[1]) + 1U;
    unsigned mySlot = gActive ? gLocalSlot : gAssignedSlot;
    if (slot == mySlot || (slot < mySlot + gLocalCount && slot + locals > mySlot)) return;
    int mi = mesh_alloc_slot(slot, locals);
    if (mi < 0) return;
    MeshPeerState &m = gMeshPeers[mi];
    memset(&m.addr, 0, sizeof(m.addr));
    m.addr.sin_family = AF_INET;
    m.addr.sin_addr.s_addr = htonl(mknet::get32(q + 2));
    m.addr.sin_port = htons(uint16_t(uint16_t(q[6]) << 8 | q[7]));
    m.last_probe = 0;
    log_line("R56_MESH: peer P%u locals=%u announced\n", slot + 1, locals);
}

static void send_mesh_info_to(int target) {
    if (lobby_slots() <= 2 || peer_count() < 2 || target < 0 || target >= peer_count()) return;
    for (int j = 0; j < peer_count(); ++j) {
        if (j == target) continue;
        const PeerState &p = gPeers[j];
        uint8_t q[8];
        q[0] = uint8_t(p.slot); q[1] = uint8_t(p.local_count - 1);
        mknet::put32(q + 2, ntohl(p.addr.sin_addr.s_addr));
        unsigned port = ntohs(p.addr.sin_port); q[6] = uint8_t(port >> 8); q[7] = uint8_t(port);
        send_peer_message(target, mknet::PEER_INFO, q, sizeof(q));
    }
}

static void mesh_send_report(const MeshPeerState &m) {
    if (gHosting || !gHostSessionKnown || m.latency.samples==0) return;
    uint8_t q[8]; memset(q,0,sizeof(q));
    q[0]=uint8_t(gActive?gLocalSlot:gAssignedSlot); q[1]=uint8_t(m.slot);
    q[2]=uint8_t(m.latency.samples>255U?255U:m.latency.samples); q[3]=m.direct_seen?1:0;
    mknet::put32(q+4,m.latency.budget());
    send_packet_to(gHostPeer,mknet::MESH_REPORT,gSession,q,sizeof(q));
}

static void mesh_send_probes(DWORD now) {
    if (gHosting || !gHostSessionKnown) return;
    unsigned mySlot=gActive?gLocalSlot:gAssignedSlot;
    if (mySlot==0) return;
    for (unsigned i = 0; i < mknet::MAX_PLAYERS - 1; ++i) {
        MeshPeerState &m = gMeshPeers[i];
        if (!m.used || (m.last_probe && now - m.last_probe < 100U)) continue;
        uint8_t q[8]; memset(q,0,sizeof(q));
        q[0]=uint8_t(mySlot);q[1]=uint8_t(gLocalCount-1);q[2]=0;q[3]=0;mknet::put32(q+4,now);
        if (send_packet_to(m.addr, mknet::PEER_PROBE, gSession, q, sizeof(q)) != SOCKET_ERROR) ++gMeshProbeTx;
        m.last_probe = now;
    }
}

static bool r57_mesh_delay(unsigned &outDelay,unsigned relayDelay) {
    int n=peer_count();
    if(n<2){outDelay=relayDelay;return false;}
    unsigned worst=0;
    for(int i=0;i<n;++i){unsigned b=gPeers[i].latency.budget();if(b>worst)worst=b;}
    for(int i=0;i<n;++i)for(int j=i+1;j<n;++j){
        unsigned a=gPeers[i].slot,b=gPeers[j].slot;
        const MeshReportState &ab=gMeshReports[a][b],&ba=gMeshReports[b][a];
        if(!ab.seen||!ba.seen||ab.samples<3U||ba.samples<3U){outDelay=relayDelay;return false;}
        unsigned pair=ab.budget>ba.budget?ab.budget:ba.budget;if(pair>worst)worst=pair;
    }
    unsigned meshDelay=mknet::input_delay_2p(worst);
    outDelay=meshDelay<relayDelay?meshDelay:relayDelay;
    return true;
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
    send_mesh_info_to(index);
}

static bool all_ready(void) {
    int n = peer_count();
    if (n < 1) return false;
    for (int i = 0; i < n; ++i) if (!gPeers[i].ready) return false;
    return true;
}

static bool r55_latency_ready(void) {
    int n = peer_count();
    if (n < 1) return false;
    for (int i = 0; i < n; ++i) if (gPeers[i].latency.samples < 3U) return false;
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
    /* R55: match the proven Xbox 360 gameplay socket buffering. */
    int netbuf = 128 * 1024;
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

/* MK64_R47_PUBLIC_MATCH
 * Unranked directory only. Gameplay remains direct peer-to-peer UDP 6464.
 * Directory service: 172.233.145.244:6465. */
static const char *kR47DirectoryIp = "172.233.145.244";
static const unsigned kR47DirectoryPort = 6465;
static bool gR47PublicHost = false;
static bool gR47RoomRegistered = false;
static char gR47RoomId[20] = {0};
static char gR47RoomToken[40] = {0};
static DWORD gR47LastHeartbeat = 0;

struct R47Room {
    char id[20];
    char name[40];
    char ip[32];
    char platform[16];
    unsigned port, players, max_players;
};

static bool r47_directory_address(sockaddr_in &a) {
    memset(&a,0,sizeof(a));
    a.sin_family=AF_INET;
    a.sin_port=htons((u_short)kR47DirectoryPort);
    a.sin_addr.s_addr=inet_addr(kR47DirectoryIp);
    return a.sin_addr.s_addr!=INADDR_NONE;
}

static int r47_send(SOCKET s,const char *msg) {
    sockaddr_in a;
    if(s==INVALID_SOCKET || !msg || !r47_directory_address(a)) return SOCKET_ERROR;
    return sendto(s,msg,(int)strlen(msg),0,(const sockaddr*)&a,sizeof(a));
}

static bool r47_register_room(void) {
    char msg[192],buf[256];
    snprintf(msg,sizeof(msg)-1,"MKDIR1|REGISTER|OG XBOX ROOM|6464|%u|4|OG|0|10|BE100927",gLocalCount);
    msg[sizeof(msg)-1]=0;
    DWORD begin=GetTickCount(),last=0;
    while(GetTickCount()-begin<1800U) {
        DWORD now=GetTickCount();
        if(!last || now-last>=300U){r47_send(gSocket,msg);last=now;}
        sockaddr_in from;int flen=sizeof(from);
        int n=recvfrom(gSocket,buf,sizeof(buf)-1,0,(sockaddr*)&from,&flen);
        if(n>0){
            buf[n]=0;
            char id[20]={0},token[40]={0};
            if(sscanf(buf,"MKDIR1|REGOK|%19[^|]|%39s",id,token)==2){
                strncpy(gR47RoomId,id,sizeof(gR47RoomId)-1);
                strncpy(gR47RoomToken,token,sizeof(gR47RoomToken)-1);
                gR47RoomRegistered=true;gR47LastHeartbeat=0;
                return true;
            }
        }
        ui_screen("MARIO KART HUB","CONNECTING TO HUB","MADE BY SIRDANKZ - SIRDANKZ CODE: MPL-2.0","UNRANKED 2-4 PLAYERS","AUTO NAT + DIRECT P2P + HUB RELAY","B CANCEL");
        if(ui_pressed()&CONT_B) return false;
        Sleep(10);
    }
    return false;
}

static void r47_heartbeat(void) {
    if(!gR47RoomRegistered || gSocket==INVALID_SOCKET) return;
    DWORD now=GetTickCount();
    if(gR47LastHeartbeat && now-gR47LastHeartbeat<4000U) return;
    char msg[160];
    snprintf(msg,sizeof(msg)-1,"MKDIR1|HEARTBEAT|%s|%s|%u",gR47RoomId,gR47RoomToken,lobby_slots());
    msg[sizeof(msg)-1]=0;r47_send(gSocket,msg);gR47LastHeartbeat=now;
}

static void r47_unregister_room(void) {
    if(gR47RoomRegistered && gSocket!=INVALID_SOCKET){
        char msg[160];
        snprintf(msg,sizeof(msg)-1,"MKDIR1|UNREGISTER|%s|%s",gR47RoomId,gR47RoomToken);
        msg[sizeof(msg)-1]=0;r47_send(gSocket,msg);
    }
    gR47RoomRegistered=false;gR47RoomId[0]=0;gR47RoomToken[0]=0;gR47LastHeartbeat=0;
}

static int r47_fetch_rooms(R47Room rooms[8]) {
    XNADDR xna;
    if(!network_stack_start(xna)) return -1;
    SOCKET s=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(s==INVALID_SOCKET) return -1;
    u_long nb=1;ioctlsocket(s,FIONBIO,&nb);
    const char *query="MKDIR1|LIST|10|BE100927|0";
    int count=0;DWORD begin=GetTickCount(),last=0;bool ended=false;
    while(GetTickCount()-begin<1800U && !ended){
        DWORD now=GetTickCount();if(!last||now-last>=400U){r47_send(s,query);last=now;}
        for(;;){
            char buf[320];sockaddr_in from;int flen=sizeof(from);
            int n=recvfrom(s,buf,sizeof(buf)-1,0,(sockaddr*)&from,&flen);
            if(n<=0)break;buf[n]=0;
            if(!strncmp(buf,"MKDIR1|LISTEND",14)){ended=true;break;}
            if(!strncmp(buf,"MKDIR1|ROOM|",12) && count<8){
                R47Room r;memset(&r,0,sizeof(r));unsigned ranked=0;
                if(sscanf(buf,"MKDIR1|ROOM|%19[^|]|%39[^|]|%31[^|]|%u|%u|%u|%15[^|]|%u",
                    r.id,r.name,r.ip,&r.port,&r.players,&r.max_players,r.platform,&ranked)==8 && !ranked){
                    rooms[count++]=r;
                }
            }
        }
        Sleep(10);
    }
    closesocket(s);return count;
}

static bool r47_browse_public_rooms(char out[64]) {
    int selected=0;
    for(;;){
        R47Room rooms[8];memset(rooms,0,sizeof(rooms));
        int count=r47_fetch_rooms(rooms);
        if(count<0){
            ui_screen("MARIO KART HUB","HUB UNAVAILABLE","MADE BY SIRDANKZ","CHECK NETWORK / HUB STATUS","","A RETRY    B BACK");
            for(;;){uint32_t p=ui_pressed();if(p&CONT_B)return false;if(p&CONT_A)break;Sleep(16);}continue;
        }
        if(count==0){
            ui_screen("PUBLIC ROOMS","NO UNRANKED ROOMS FOUND","","HOST CAN CREATE A PUBLIC ROOM","","A REFRESH    B BACK");
            for(;;){uint32_t p=ui_pressed();if(p&CONT_B)return false;if(p&CONT_A)break;Sleep(16);}continue;
        }
        if(selected>=count)selected=count-1;
        for(;;){
            char l0[96]="",l1[96]="",l2[96]="",detail[96];
            int first=selected-1;if(first<0)first=0;if(first>count-3)first=count-3;if(first<0)first=0;
            char *lines[3]={l0,l1,l2};
            for(int j=0;j<3;++j){int i=first+j;if(i>=count)continue;
                snprintf(lines[j],95,"%c %s  %u/%u  %s",i==selected?'>':' ',rooms[i].name,rooms[i].players,rooms[i].max_players,rooms[i].platform);lines[j][95]=0;}
            snprintf(detail,sizeof(detail)-1,"ROOM %d/%d  UNRANKED",selected+1,count);detail[sizeof(detail)-1]=0;
            ui_screen("PUBLIC ROOMS",l0,l1,l2,detail,"A JOIN  UP/DOWN  Y REFRESH  B BACK");
            uint32_t p=ui_pressed();
            if(p&CONT_B)return false;
            if(p&kUiYButton)break;
            if(p&CONT_DPAD_UP)selected=(selected+count-1)%count;
            if(p&CONT_DPAD_DOWN)selected=(selected+1)%count;
            if(p&CONT_A){
                snprintf(out,63,"%s:%u",rooms[selected].ip,rooms[selected].port);out[63]=0;ui_consume();return true;
            }
            Sleep(16);
        }
    }
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
/* MK64_R50_1_PUBLIC_MATCH_IP_PRIVACY */
static bool gR501HideAddressUi = false;

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

        if (gR501HideAddressUi)
            ui_screen("PUBLIC MATCH", "PREPARING PUBLIC CONNECTION",
                      "FINDING INTERNET ROUTE", label, "UDP 6464", "PLEASE WAIT");
        else
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
        if (gR501HideAddressUi)
            ui_screen("PUBLIC MATCH", "PREPARING PUBLIC CONNECTION",
                      "RESOLVING INTERNET SERVICE", label, "UDP 6464", "PLEASE WAIT");
        else
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

static void r42_discover_public_ip(bool hide_addresses) {
    gR501HideAddressUi = hide_addresses;
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
            if (!same_ip(gPeers[index].addr, from) && !gR58NatEnabled) return;
            if(!same_address(gPeers[index].addr,from)){gPeers[index].addr=from;gPeers[index].ready=false;gPeers[index].acked=false;log_line("R58.2_NAT: P%u endpoint rebound during HELLO\n",gPeers[index].slot+1);}
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

    /* R59.1: accept a clean leave even before START. If the guest has not
     * received OFFER yet it identifies itself with its HELLO nonce; afterward
     * it can use the real session ID. */
    if (packet[5] == mknet::GOODBYE && !gActive && !gStartSent) {
        int leave_index = find_peer_nonce(packet + 12);
        if (leave_index < 0 && !memcmp(packet + 12, gSession, 16)) leave_index = find_peer_address(from);
        if (leave_index >= 0) remove_peer(leave_index);
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
        log_line("R55_LATENCY: P%u sample=%u rtt=%lu mean=%u var=%u budget=%u\n",
                 peer.slot + 1, peer.latency.samples, (unsigned long)rtt,
                 peer.latency.mean, peer.latency.variation, peer.latency.budget());
        return;
    }

    if (packet[5] == mknet::MESH_REPORT && !gActive && !gStartSent) {
        unsigned reporter=q[0],target=q[1],samples=q[2],direct=q[3];
        if(reporter!=peer.slot||target==reporter||target>=mknet::lobby_capacity()) return;
        int ti=-1;for(int k=0;k<peer_count();++k)if(gPeers[k].slot==target){ti=k;break;}
        if(ti<0||!direct) return;
        MeshReportState &mr=gMeshReports[reporter][target];mr.budget=mknet::get32(q+4);mr.samples=samples;mr.seen=true;
        peer.last_received=now;
        if(samples<=3U)log_line("R57_MESH: report P%u->P%u samples=%u budget=%u\n",reporter+1,target+1,samples,mr.budget);
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
                    int sr = r58_send_game_packet(gPeers[j].addr,packet,bytes,true);
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
        log_line("MK64XNET: P%u requested return to premenu\n", peer.slot + 1);
        gReturnToPremenu = true;
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

    /* R56A direct mesh control/data is accepted only for announced guest slots.
     * The original host path below remains authoritative and always active. */
    if (packet[5] == mknet::PEER_INFO) {
        if (!same_ip(gHostPeer, from)) return;
        mesh_install_info(q);
        return;
    }
    if (packet[5] == mknet::PEER_PROBE) {
        unsigned source=q[0],locals=unsigned(q[1])+1U,reply=q[2];
        int mi=mesh_find_slot(source,locals);
        if(mi<0||!same_ip(gMeshPeers[mi].addr,from))return;
        MeshPeerState &m=gMeshPeers[mi];bool first=!m.direct_seen;m.addr=from;m.direct_seen=true;++gMeshProbeRx;
        if(first)log_line("R57_MESH: direct path P%u established\n",source+1);
        if(!reply){uint8_t r[8];memset(r,0,sizeof(r));r[0]=uint8_t(gActive?gLocalSlot:gAssignedSlot);r[1]=uint8_t(gLocalCount-1);r[2]=1;mknet::put32(r+4,mknet::get32(q+4));send_packet_to(m.addr,mknet::PEER_PROBE,gSession,r,sizeof(r));}
        else{DWORD stamp=mknet::get32(q+4),now=GetTickCount();if(now-stamp<=10000U){m.latency.add(unsigned(now-stamp));mesh_send_report(m);if(m.latency.samples<=3U)log_line("R57_MESH: P%u direct sample=%u rtt=%u budget=%u\n",source+1,m.latency.samples,unsigned(now-stamp),m.latency.budget());}}
        return;
    }
    if (gActive && packet[5] == mknet::CLIENT_INPUT) {
        unsigned source = q[0], locals = unsigned(q[3]) + 1U;
        int mi = mesh_find_slot(source, locals);
        if (mi >= 0 && same_address(gMeshPeers[mi].addr, from)) {
            ++gNetRxInput;
            if (gStream.receive_remote(source, packet, bytes, locals)) {
                ++gMeshDirectRx; gFirstGameplayInput = true;
                if (gMeshDirectRx <= 8U) log_line("R56_MESH: DIRECT_INPUT P%u accepted frame=%lu\n", source + 1, (unsigned long)gStream.frame);
            } else ++gNetRxInputReject;
            return;
        }
    }

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
        log_line("MK64XNET: host requested return to premenu\n");
        gReturnToPremenu = true;
        gFailed = true;
    }
}

static void pump_packets(void) {
    SOCKET io=r58_io_socket();
    if (io == INVALID_SOCKET) return;
    r58_nat_tick(GetTickCount());
    /* R59.1: bound one active-race receive burst on the slower OG CPU. The
     * lockstep wait loop calls pump_packets() repeatedly, so queued UDP data is
     * still drained without letting one burst monopolize a rendered frame. */
    const int rx_budget = gActive ? 64 : 96;
    for (int i = 0; i < rx_budget; ++i) {
        uint8_t packet[mknet::MAX_PACKET + 23];
        sockaddr_in from;
        int from_len = sizeof(from);
        int bytes = recvfrom(io, (char *)packet, sizeof(packet)-1, 0, (sockaddr *)&from, &from_len);
        if (bytes == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e != WSAEWOULDBLOCK && e != WSAEMSGSIZE) {
                if(e==10051||e==10052||e==10054||e==10065){log_line("R58.2_NAT: transient UDP recv error wsa=%d ignored\n",e);break;}
                log_line("MK64XNET: recvfrom failed wsa=%d\n", e);
                gFailed = true;
            }
            break;
        }
        if (r58_handle_control(packet,bytes,from)) continue;
        bool relayed=false;if(bytes>=2&&packet[0]=='M'&&packet[1]=='R'){if(!r58_relay_unwrap(packet,bytes,from))continue;relayed=true;}
        if (!mknet::valid(packet, bytes)) {
            if (!gR66VersionMismatch && bytes >= mknet::HEADER &&
                !memcmp(packet, mknet::wire_magic(), 4) &&
                (packet[4] == mknet::VERSION || packet[4] == mknet::VERSION_60)) {
                gR66VersionMismatch = true;
                log_line("R66_RATE_MISMATCH: remote wire version %u build %08lX; local version %u build %08lX; packet rejected\n",
                         (unsigned)packet[4], (unsigned long)mknet::get32(packet + 8),
                         mknet::wire_version(), (unsigned long)mknet::wire_build());
            }
            continue;
        }
        if(!relayed)r58_mark_direct(from);
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

/* MK64_R48_3_EARLY_DECL_FIX
 * The R47 public-host handshake calls these before the full R48 helper block. */
extern bool gR48PublicMode;
static void r48_heartbeat();
static void r48_poll_chat();
/* R57.1: handshake_host() services directory presence before the full
 * R57 public-match helper block is defined later in this translation unit. */
static bool r61_social_packet(const char *b);
static void r57_presence_tick(bool drain);

static bool handshake_host(unsigned timeout_seconds) {
    DWORD begin = GetTickCount();
    DWORD last_start = 0;
    log_line("MK64XNET: HOST waiting for %u total racer slots; LAN/direct-public use same UDP %u socket\n",
             gDesiredPlayers, kPort);

    while (!gActive && !gFailed) {
        pump_packets();
        DWORD now = GetTickCount();
        prune_prestart_timeouts(now);
        int n = peer_count();
        if (gR48PublicMode) { r48_heartbeat(); r57_presence_tick(false); r48_poll_chat(); }

        if (!gStartSent) {
            for (int i = 0; i < n; ++i)
                if (!gPeers[i].last_offer || now - gPeers[i].last_offer >= 250) send_offer(i);

            if (all_ready() && r55_latency_ready() && lobby_slots() >= gDesiredPlayers) {
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
                log_line("R55_LATENCY: LEGACY CHOSEN delay=%u players=%u worst_budget=%u second_budget=%u\n",
                         gChosenDelay, gPlayerCount, worst, second);
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
    gR66VersionMismatch = false;
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
    mesh_reset();
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
    for (int i = 0; i < n; ++i) {
        log_line("R55_LATENCY: START P%u samples=%u mean=%u var=%u budget=%u\n",
                 gPeers[i].slot + 1, gPeers[i].latency.samples, gPeers[i].latency.mean,
                 gPeers[i].latency.variation, gPeers[i].latency.budget());
    }
    unsigned relayDelay = gPlayerCount == 2 ? mknet::input_delay_2p(worst)
                                            : mknet::input_delay_early_relay(worst, second);
    bool meshReady=false;gChosenDelay=relayDelay;
    if(gPlayerCount>2)meshReady=r57_mesh_delay(gChosenDelay,relayDelay);
    log_line("R57_LATENCY: CHOSEN delay=%u players=%u worst_budget=%u second_budget=%u relay=%u mesh=%s\n",
             gChosenDelay,gPlayerCount,worst,second,relayDelay,meshReady?"READY":"FALLBACK");
    gStream.reset(gChosenDelay, gPlayerCount, 0, gLocalCount);
    gBoot.reset(gPlayerCount);
    if (gLocalCount > 1) gBoot.ready_span(1, gLocalCount - 1);
    gStartSent = true;
    for (int i = 0; i < n; ++i) gPeers[i].acked = false;
    log_line("MK64XNET: HOST pressed A - starting %uP delay=%u\n", gPlayerCount, gChosenDelay);
}

/* MK64_R48_2_FORWARD_DECL_FIX
 * R48.1's full helper definitions live later in this translation unit.
 * host_lobby_menu()/join_lobby_menu() use this subset before those definitions,
 * so C++ needs declarations here. */
extern bool gR48PublicMode;
extern char gR48Chat1[80];
extern char gR48Chat2[80];
static void r48_heartbeat();
static void r48_poll_chat();
static void r48_chat_compose();
static void r59_world_chat_view();
static void r59_world_compose(bool lobbyPump);
static void r61_mailbox_view();
static void r62_lobby_show_stats(); /* later, after account type and social implementation */
static void r70_room_view();
static void r50_public_lobby_screen(const char *title,const char *status,bool host);

static bool host_lobby_menu(void) {
    DWORD last_start = 0;
    ui_consume();
    for (;;) {
        pump_packets();
        DWORD now = GetTickCount();
        int n = peer_count();

        /* R56A.1: keep the public directory room alive while the host waits
         * for players. This call was lost when the OG host lobby was split
         * into host_lobby_menu(), causing the 20s directory TTL to expire. */
        if (gR48PublicMode) { r48_heartbeat(); r57_presence_tick(false); r48_poll_chat(); }

        if (!gStartSent) {
            for (int i = 0; i < n; ++i)
                if (!gPeers[i].last_offer || now - gPeers[i].last_offer >= 250) send_offer(i);
            if(n>=2&&all_ready()&&(!gMeshLastAnnounce||now-gMeshLastAnnounce>=500U)){for(int i=0;i<n;++i)send_mesh_info_to(i);gMeshLastAnnounce=now;}
        }

        uint32_t p = ui_pressed();
        if (p & CONT_B) {
            log_line("MK64XNET: host cancelled lobby\n");
            return false;
        }
        if (gR48PublicMode && (p & CONT_C)) {r62_lobby_show_stats();continue;}
        if (gR48PublicMode && (p & CONT_DPAD_RIGHT)) {r62_modal_lobby=true;r70_room_view();r62_modal_lobby=false;ui_consume();continue;}
        if (p & kUiYButton) { if(gR48PublicMode){r62_modal_lobby=true;r61_mailbox_view();r62_modal_lobby=false;ui_consume();continue;}else gR42PublicIpVisible = !gR42PublicIpVisible; }
        if (gR48PublicMode && (p & kUiRightStickButton)) { gR59WorldLobbyView=!gR59WorldLobbyView; ui_consume(); Sleep(32); continue; }
        if (gR48PublicMode && (p & CONT_X)) { if(gR59WorldLobbyView)r59_world_compose(true);else r48_chat_compose(); ui_consume(); Sleep(32); continue; }

        bool can_start = !gStartSent && all_ready() && r55_latency_ready() && lobby_slots() >= 2 && lobby_slots() <= 4;
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
        } else if (all_ready() && !r55_latency_ready()) {
            unsigned min_samples = 0xFFFFFFFFU;
            for (int i = 0; i < n; ++i) if (gPeers[i].latency.samples < min_samples) min_samples = gPeers[i].latency.samples;
            if (min_samples > 3U) min_samples = 3U;
            snprintf(status, sizeof(status)-1, "MEASURING NETWORK %u/3", min_samples);
        } else {
            snprintf(status, sizeof(status)-1, "PLAYERS %u/4 - READY %d/%d", lobby_slots(), ready_peer_count(), n);
        }
        status[sizeof(status)-1] = 0;
        snprintf(peers, sizeof(peers)-1, "LOCAL RACERS: %u   REMOTE CONSOLES: %d", gLocalCount, n);
        peers[sizeof(peers)-1] = 0;
        snprintf(footer, sizeof(footer)-1, "Y %s IP   A START   B CANCEL   UDP 6464",
                 gR42PublicIpVisible ? "HIDE" : "SHOW");
        footer[sizeof(footer)-1] = 0;
                if (gR48PublicMode) r50_public_lobby_screen("PUBLIC ROOM - HOST",status,true);
        else ui_screen("HOST 2-4 PLAYER GAME", public_line, gLocalAddressText, status, peers, footer);
        Sleep(16); /* R59.1: do not spin the OG lobby UI at ~100 Hz. */
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
            if(gNetplayLoggingEnabled&&gR584JoinLoopTraceCount<32U)log_line("R58.4_TRACE: guest HELLO BEGIN known=%u\n",gHostSessionKnown?1U:0U);
            int sent = send_packet_to(gJoinTarget, mknet::HELLO, gNonce, hello, sizeof(hello));
            if (sent == SOCKET_ERROR) log_line("MK64XNET: HELLO send failed wsa=%d\n", WSAGetLastError());
            if(gNetplayLoggingEnabled&&gR584JoinLoopTraceCount<32U)log_line("R58.4_TRACE: guest HELLO END rc=%d\n",sent);
            last_hello = now;
        }
        if(gNetplayLoggingEnabled&&gR584JoinLoopTraceCount<32U)log_line("R58.4_TRACE: guest pump BEGIN\n");
        pump_packets();
        if(gNetplayLoggingEnabled&&gR584JoinLoopTraceCount<32U)log_line("R58.4_TRACE: guest pump END failed=%u active=%u known=%u\n",gFailed?1U:0U,gActive?1U:0U,gHostSessionKnown?1U:0U);
        mesh_send_probes(now);
        if (gR48PublicMode) { r57_presence_tick(false); r48_poll_chat(); }
        if (gActive) return true;
        if (gFailed) return false;

        uint32_t p = ui_pressed();
        if (p & CONT_B) { log_line("MK64XNET: join cancelled\n"); return false; }
        if (gR48PublicMode && (p & CONT_C)) {r62_lobby_show_stats();continue;}
        if (gR48PublicMode && (p & CONT_DPAD_RIGHT)) {r62_modal_lobby=true;r70_room_view();r62_modal_lobby=false;ui_consume();continue;}
        if (gR48PublicMode && (p & kUiYButton)) {r62_modal_lobby=true;r61_mailbox_view();r62_modal_lobby=false;ui_consume();continue;}
        if (gR48PublicMode && (p & kUiRightStickButton)) { gR59WorldLobbyView=!gR59WorldLobbyView; ui_consume(); Sleep(32); continue; }
        if (gR48PublicMode && (p & CONT_X)) { if(gR59WorldLobbyView)r59_world_compose(true);else r48_chat_compose(); ui_consume(); Sleep(32); continue; }
        /* R57 keeps the separate UDP-6465 control socket alive during the
         * pre-game handshake so chat/member presence cannot expire. */

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
        if(gNetplayLoggingEnabled&&gR584JoinLoopTraceCount<32U)log_line("R58.4_TRACE: guest lobby render BEGIN public=%u\n",gR48PublicMode?1U:0U);
        if (gR48PublicMode)
            r50_public_lobby_screen("PUBLIC ROOM - JOIN", status, false);
        else
            ui_screen("JOIN 2-4 PLAYER GAME", target_line, status, gLocalAddressText, racers, "B CANCEL    ALL GUESTS USE SAME HOST IP");
        if(gNetplayLoggingEnabled&&gR584JoinLoopTraceCount<32U){log_line("R58.4_TRACE: guest lobby render END\n");++gR584JoinLoopTraceCount;}
        Sleep(16); /* R59.1: keep pre-game rendering lightweight on OG. */
    }
}

static bool select_local_players(bool host, unsigned &count) {
    ui_consume();
    for (;;) {
        if(gR59WorldUiActive)r57_presence_tick(true);
        bool second = ui_second_controller_connected();
        count = second ? 2U : 1U;
        const char *one_line = count == 1 ? "> 1 PLAYER - FULL SCREEN" : "  1 PLAYER - FULL SCREEN";
        const char *two = count == 2 ? "> 2 PLAYERS - SPLIT SCREEN" : "  2 PLAYERS - SPLIT SCREEN";
        const char *detect = second ? "EXTRA CONTROLLER DETECTED" : "NO EXTRA CONTROLLER DETECTED";
        const char *hint = second ? "AUTO ASSIGN: TWO RACER SLOTS" : "CONNECT CONTROLLER IN PORT 2 FOR LOCAL P2";
        ui_screen(host ? "HOST - PLAYERS ON THIS CONSOLE" : "JOIN - PLAYERS ON THIS CONSOLE",
                  one_line, two, detect, hint, "A CONTINUE    B BACK");
        uint32_t p = ui_pressed();
        if(gR59WorldUiActive&&(p&kUiRightStickButton)){r59_world_chat_view();ui_consume();continue;}
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

/* MK64_R48_ACCOUNTS_LOBBY_CHAT
 * Directory/control plane only. Gameplay remains direct UDP 6464 P2P.
 * Adds server accounts, custom/password rooms, and PRE-GAME lobby text chat.
 * In-game text entry is intentionally not hooked into deterministic gameplay. */
bool gR48PublicMode=false;
static bool gR48PublicHost=false;
static SOCKET gR48DirSocket=INVALID_SOCKET;
static bool gR48RoomRegistered=false;
static char gR48RoomId[20]={0},gR48RoomToken[40]={0};
static char gR48RoomName[32]={0},gR48RoomPassword[20]={0};
static DWORD gR48LastHeartbeat=0,gR48LastPoll=0;
/* MK64_R50_CHAT_ACCOUNT_UX */
static unsigned gR48ChatSeq=0;
static bool gR57SignedIn=false;
static DWORD gR57LastPresence=0;
static char gR57Members[4][40];static unsigned gR57MemberCount=0;
char gR48Chat1[80]="MARIO KART HUB: Public lobby ready";
char gR48Chat2[80]="X: type lobby chat";
static char gR48Chat3[80]={0},gR48Chat4[80]={0},gR48Chat5[80]={0},gR48Chat6[80]={0};
/* R58.1: chat send is queued until the keyboard has fully returned to the lobby frame. */
static char gR581ChatPending[56]={0};static bool gR581ChatPendingReady=false;static DWORD gR582ChatResume=0;
/* MK64_R49_ACCOUNT_PASSWORD_LOGIN */
struct R48AccountOG{char id[20],secret[40],name[20],password[20];};
static R48AccountOG gR48Accounts[4];static int gR48AccountCount=0,gR48AccountIndex=0;static bool gR48AccountsLoaded=false;

/* MK64_R48_4_VISIBLE_SAVED_PASSWORD
 * Lobby password is intentionally saved/displayed in readable form by user request. */
static char gR484SavedUser[20]={0},gR484SavedPassword[20]={0},gR484SavedRoom[32]={0},gR484Region[12]="UNSET";static bool gR484PrefsLoaded=false;
static void r484_load_prefs(){if(gR484PrefsLoaded)return;gR484PrefsLoaded=true;const char *paths[2]={"U:/mk64-public-r48.cfg","U:/mk64-public-r48.cfg"};FILE *f=0;for(int i=0;i<2&&!f;++i)f=fopen(paths[i],"r");if(!f)return;char line[128];while(fgets(line,sizeof(line),f)){char *e=strpbrk(line,"\r\n");if(e)*e=0;if(!strncmp(line,"USERNAME=",9)){strncpy(gR484SavedUser,line+9,sizeof(gR484SavedUser)-1);gR484SavedUser[sizeof(gR484SavedUser)-1]=0;}else if(!strncmp(line,"PASSWORD=",9)){strncpy(gR484SavedPassword,line+9,sizeof(gR484SavedPassword)-1);gR484SavedPassword[sizeof(gR484SavedPassword)-1]=0;}else if(!strncmp(line,"ROOM=",5)){strncpy(gR484SavedRoom,line+5,sizeof(gR484SavedRoom)-1);gR484SavedRoom[sizeof(gR484SavedRoom)-1]=0;}else if(!strncmp(line,"REGION=",7)){strncpy(gR484Region,line+7,sizeof(gR484Region)-1);gR484Region[sizeof(gR484Region)-1]=0;}}fclose(f);}
static void r484_save_prefs(){const char *paths[2]={"U:/mk64-public-r48.cfg","U:/mk64-public-r48.cfg"};FILE *f=0;for(int i=0;i<2&&!f;++i)f=fopen(paths[i],"w");if(!f)return;fprintf(f,"USERNAME=%s\nPASSWORD=%s\nROOM=%s\nREGION=%s\n",gR484SavedUser,gR484SavedPassword,gR484SavedRoom,gR484Region);fclose(f);}
static void r484_remember_user(const char *name){r484_load_prefs();if(name){strncpy(gR484SavedUser,name,sizeof(gR484SavedUser)-1);gR484SavedUser[sizeof(gR484SavedUser)-1]=0;}r484_save_prefs();}

struct R48RoomOG{char id[20],name[32],platform[16],owner[20],region[12];unsigned players,max_players,locked,rate60;};

static void r48_set_chat(const char *who,const char *msg){
    strncpy(gR48Chat1,gR48Chat2,sizeof(gR48Chat1)-1);gR48Chat1[sizeof(gR48Chat1)-1]=0;
    strncpy(gR48Chat2,gR48Chat3,sizeof(gR48Chat2)-1);gR48Chat2[sizeof(gR48Chat2)-1]=0;
    strncpy(gR48Chat3,gR48Chat4,sizeof(gR48Chat3)-1);gR48Chat3[sizeof(gR48Chat3)-1]=0;
    strncpy(gR48Chat4,gR48Chat5,sizeof(gR48Chat4)-1);gR48Chat4[sizeof(gR48Chat4)-1]=0;
    strncpy(gR48Chat5,gR48Chat6,sizeof(gR48Chat5)-1);gR48Chat5[sizeof(gR48Chat5)-1]=0;
    snprintf(gR48Chat6,sizeof(gR48Chat6)-1,"%s: %s",who?who:"HUB",msg?msg:"");gR48Chat6[sizeof(gR48Chat6)-1]=0;
}
static void r50_chat_reset(const char *room,const char *name){gR59WorldLobbyView=false;gR581ChatPending[0]=0;gR581ChatPendingReady=false;gR582ChatResume=0;gR48Chat1[0]=gR48Chat2[0]=gR48Chat3[0]=gR48Chat4[0]=0;snprintf(gR48Chat5,sizeof(gR48Chat5)-1,"ROOM: %s",room?room:"");gR48Chat5[sizeof(gR48Chat5)-1]=0;snprintf(gR48Chat6,sizeof(gR48Chat6)-1,"%s: waiting for players",name?name:"PLAYER");gR48Chat6[sizeof(gR48Chat6)-1]=0;}
static void r50_public_lobby_screen(const char *title,const char *status,bool host){
    struct GfxRenderingAPI *r=&gfx_nv2a_api;r->start_frame();ui_budget_reset();r->set_depth_test(0);r->set_depth_mask(0);r->select_texture(0,0);gfx_pvr_set_blend(UI_KIND_OP);ui_quad(0,0,640,480,0xFF102030);
    ui_text(46,30,title?title:"PUBLIC ROOM",2.28f,0xFFFFD050);char on[32];snprintf(on,sizeof(on)-1,"PLAYERS ONLINE: %s",gR57OnlineCount>=0?"":"--");if(gR57OnlineCount>=0)snprintf(on,sizeof(on)-1,"PLAYERS ONLINE: %d",gR57OnlineCount);on[sizeof(on)-1]=0;ui_text(449,26,on,.98f,0xFF90D0FF);
    char inGame[40];snprintf(inGame,sizeof(inGame)-1,"PLAYERS IN GAME: %s",r62_game_total>=0?"":"--");if(r62_game_total>=0)snprintf(inGame,sizeof(inGame)-1,"PLAYERS IN GAME: %d",r62_game_total);inGame[sizeof(inGame)-1]=0;ui_text(449,46,inGame,.97f,0xFF90D0FF);
    ui_text(48,76,"PLAYERS",1.22f,0xFF90D0FF);for(unsigned i=0;i<gR57MemberCount&&i<4;++i)ui_text(48,103+i*27,gR57Members[i],1.12f,0xFFFFFFFF);
    ui_quad(286,68,288,378,0xFF385068);const char *panel=gR59WorldLobbyView?"WORLD CHAT":"LOBBY CHAT";ui_text(304,72,panel,1.22f,0xFF90D0FF);const char *chat[6];if(gR59WorldLobbyView){for(int i=0;i<6;++i)chat[i]=gR59WorldLines[i];}else{chat[0]=gR48Chat1;chat[1]=gR48Chat2;chat[2]=gR48Chat3;chat[3]=gR48Chat4;chat[4]=gR48Chat5;chat[5]=gR48Chat6;}for(int i=0;i<6;++i){const char *line=chat[i]?chat[i]:"";if(!*line)continue;ui_text(304,101+i*34,line,r54_og_fit(line,.98f,.74f,284.0f),0xFFFFFFFF);}
    ui_text(48,222,status?status:"",1.26f,0xFFFFD050);char reg[48];snprintf(reg,sizeof(reg)-1,"YOUR REGION: %s",gR484Region);reg[sizeof(reg)-1]=0;ui_text(48,257,reg,1.08f,0xFFFFFFFF);ui_text(48,296,host?"A START  RT STATS  Y MAIL  B CANCEL":"RT STATS  Y MAIL  B CANCEL",1.06f,0xFFB8B8B8);
    ui_text(48,319,"RIGHT PLAYERS  A PROFILE / FRIEND",.96f,0xFF90D0FF);ui_text(304,321,gR59WorldLobbyView?"X SEND WORLD CHAT":"X SEND LOBBY CHAT",1.05f,0xFF90D0FF);ui_text(304,350,"RS SWITCH CHAT VIEW",1.02f,0xFF90D0FF);ui_text(48,432,host?"AUTO NAT: DIRECT FIRST - HUB RELAY FALLBACK":"LOBBY + WORLD CHAT STAY LIVE UNTIL START",.95f,0xFFB8B8B8);r->end_frame();r->finish_render();
}
static bool r48_dir_addr(sockaddr_in &a){memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_port=htons((u_short)6465);a.sin_addr.s_addr=inet_addr("172.233.145.244");return a.sin_addr.s_addr!=INADDR_NONE;}
static bool r48_dir_open(){if(gR48DirSocket!=INVALID_SOCKET)return true;gR48DirSocket=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(gR48DirSocket==INVALID_SOCKET)return false;u_long nb=1;if(ioctlsocket(gR48DirSocket,FIONBIO,&nb)==SOCKET_ERROR){closesocket(gR48DirSocket);gR48DirSocket=INVALID_SOCKET;return false;}return true;}
static void r48_dir_close(){if(gR48DirSocket!=INVALID_SOCKET){closesocket(gR48DirSocket);gR48DirSocket=INVALID_SOCKET;}}
static int r48_send(const char *msg){sockaddr_in a;if(gR48DirSocket==INVALID_SOCKET||!msg||!r48_dir_addr(a))return SOCKET_ERROR;return sendto(gR48DirSocket,msg,(int)strlen(msg),0,(const sockaddr*)&a,sizeof(a));}
static void r57_parse_status(const char *b){unsigned n=0,g=0;if(!b)return;int f=sscanf(b,"MKDIR2|STATUS|%u|%u",&n,&g);if(f>=1)gR57OnlineCount=(int)n;if(f>=2)r62_game_total=(int)g;}
static void r57_parse_state(const char *b){unsigned count=0;char list[220]={0};if(!b||sscanf(b,"MKDIR2|STATE|%u|%219[^\r\n]",&count,list)<1)return;gR57MemberCount=0;char *tok=strtok(list,",");while(tok&&gR57MemberCount<4){char name[20]={0},region[12]={0};if(sscanf(tok,"%19[^~]~%11s",name,region)==2)snprintf(gR57Members[gR57MemberCount],sizeof(gR57Members[0])-1,"%s [%s]",name,region);else snprintf(gR57Members[gR57MemberCount],sizeof(gR57Members[0])-1,"%s",tok);gR57Members[gR57MemberCount][sizeof(gR57Members[0])-1]=0;++gR57MemberCount;tok=strtok(NULL,",");}}

/* R59 shared world chat.  This is deliberately separate from room chat: room
 * history remains untouched while the lobby panel temporarily shows WORLD. */
static void r59_world_reset(){gR59WorldSeq=0;gR59WorldLastPoll=0;gR59WorldResume=0;gR59WorldPendingReady=false;gR59WorldPending[0]=0;gR59WorldLobbyView=false;for(int i=0;i<6;++i)gR59WorldLines[i][0]=0;}
static void r59_world_push(const char *who,const char *region,const char *platform,const char *msg){for(int i=0;i<5;++i){strncpy(gR59WorldLines[i],gR59WorldLines[i+1],sizeof(gR59WorldLines[i])-1);gR59WorldLines[i][sizeof(gR59WorldLines[i])-1]=0;}snprintf(gR59WorldLines[5],sizeof(gR59WorldLines[5])-1,"%s [%s %s]: %s",who?who:"PLAYER",platform?platform:"?",region?region:"UNSET",msg?msg:"");gR59WorldLines[5][sizeof(gR59WorldLines[5])-1]=0;}
static bool r59_world_packet(const char *b){if(!b)return false;unsigned seq=0;char who[20]={0},region[12]={0},platform[16]={0},msg[80]={0};if(sscanf(b,"MKDIR2|WORLD|%u|%19[^|]|%11[^|]|%15[^|]|%79[^\r\n]",&seq,who,region,platform,msg)==5){if(seq>gR59WorldSeq){gR59WorldSeq=seq;r59_world_push(who,region,platform,msg);}return true;}if(!strncmp(b,"MKDIR2|WORLDOK|",15)||!strncmp(b,"MKDIR2|WORLDEND|",16))return true;return false;}
static void r59_world_send_tick(DWORD now){if(!gR59WorldUiActive||gR48DirSocket==INVALID_SOCKET)return;R48AccountOG *me=(gR48AccountIndex>=0&&gR48AccountIndex<gR48AccountCount)?&gR48Accounts[gR48AccountIndex]:0;if(!me)return;if(gR59WorldPendingReady&&gR59WorldPending[0]&&(!gR59WorldResume||now>=gR59WorldResume)){char m[360];snprintf(m,sizeof(m)-1,"MKDIR2|WORLD_SEND|%s|%s|%s|OG|%s",me->id,me->secret,gR484Region,gR59WorldPending);m[sizeof(m)-1]=0;if(r48_send(m)!=SOCKET_ERROR){gR59WorldPendingReady=false;gR59WorldPending[0]=0;gR59WorldLastPoll=0;gR59WorldResume=now+120U;}return;}if(!gR59WorldLastPoll||now-gR59WorldLastPoll>=900U){char m[240];snprintf(m,sizeof(m)-1,"MKDIR2|WORLD_POLL|%s|%s|%u|%s|OG",me->id,me->secret,gR59WorldSeq,gR484Region);m[sizeof(m)-1]=0;r48_send(m);gR59WorldLastPoll=now;}}
static void r71_refresh_device();
static void r57_presence_tick(bool drain){
    r71_refresh_device();
    if(!gR57PresenceActive||gR48DirSocket==INVALID_SOCKET)return;R48AccountOG *me=(gR48AccountIndex>=0&&gR48AccountIndex<gR48AccountCount)?&gR48Accounts[gR48AccountIndex]:0;if(!me)return;DWORD now=GetTickCount();
    if(!gR57LastPresence||now-gR57LastPresence>=5000U){char m[220];snprintf(m,sizeof(m)-1,"MKDIR2|PRESENCE|%s|%s|%s|OG",me->id,me->secret,gR484Region);m[sizeof(m)-1]=0;r48_send(m);gR57LastPresence=now;}
    r59_world_send_tick(now);
    if(drain){for(;;){char b[512];sockaddr_in from;int flen=sizeof(from);int n=recvfrom(gR48DirSocket,b,sizeof(b)-1,0,(sockaddr*)&from,&flen);if(n<=0)break;b[n]=0;if(r59_world_packet(b))continue;if(r61_social_packet(b))continue;r57_parse_status(b);}}
}

static void r48_drain(){if(gR48DirSocket==INVALID_SOCKET)return;char b[512];for(;;){sockaddr_in from;int flen=sizeof(from);int n=recvfrom(gR48DirSocket,b,sizeof(b)-1,0,(sockaddr*)&from,&flen);if(n<=0)break;b[n]=0;if(r59_world_packet(b))continue;if(r61_social_packet(b))continue;if(!strncmp(b,"MKDIR2|STATUS|",14))r57_parse_status(b);}}
static bool r48_wait_prefix(const char *msg,const char *prefix,char *out,int cap,DWORD timeout=2200U){r48_drain();DWORD begin=GetTickCount(),last=0;while(GetTickCount()-begin<timeout){DWORD now=GetTickCount();if(!last||now-last>=350U){r48_send(msg);last=now;}char b[512];sockaddr_in from;int flen=sizeof(from);int n=recvfrom(gR48DirSocket,b,sizeof(b)-1,0,(sockaddr*)&from,&flen);if(n>0){b[n]=0;if(r59_world_packet(b))continue;if(r61_social_packet(b))continue;if(!strncmp(b,"MKDIR2|STATUS|",14)){r57_parse_status(b);continue;}if(!strncmp(b,prefix,strlen(prefix))){if(out&&cap>0){strncpy(out,b,cap-1);out[cap-1]=0;}return true;}if(!strncmp(b,"MKDIR2|ERR|",11)||!strncmp(b,"MKDIR2|ACCOUNT_LIMIT|",21)){if(out&&cap>0){strncpy(out,b,cap-1);out[cap-1]=0;}return false;}}Sleep(10);}return false;}
static void r48_load_accounts(){if(gR48AccountsLoaded)return;gR48AccountsLoaded=true;gR48AccountCount=0;const char *paths[2]={"U:/mk64-accounts-r2.cfg","U:/mk64-accounts-r2.cfg"};FILE *f=0;for(int i=0;i<2&&!f;++i)f=fopen(paths[i],"r");if(!f)return;char line[200];while(gR48AccountCount<4&&fgets(line,sizeof(line),f)){R48AccountOG a;memset(&a,0,sizeof(a));int n=sscanf(line,"%19[^|]|%39[^|]|%19[^|]|%19[^\r\n]",a.id,a.secret,a.name,a.password);if(n>=3)gR48Accounts[gR48AccountCount++]=a;}fclose(f);}
static void r48_save_accounts(){const char *paths[2]={"U:/mk64-accounts-r2.cfg","U:/mk64-accounts-r2.cfg"};FILE *f=0;for(int i=0;i<2&&!f;++i)f=fopen(paths[i],"w");if(!f)return;for(int i=0;i<gR48AccountCount;++i)fprintf(f,"%s|%s|%s|%s\n",gR48Accounts[i].id,gR48Accounts[i].secret,gR48Accounts[i].name,gR48Accounts[i].password);fclose(f);}
/* MK64_R48_3_SIMPLE_KEYBOARD
 * Fixed 4x9 A-Z + 0-9 keyboard. No caps/symbol layers. */
static char r48_key(int row,int col){
    static const char *rows[4]={"ABCDEFGHI","JKLMNOPQR","STUVWXYZ0","123456789"};
    if(row<0||row>3||col<0||col>8)return '?';
    return rows[row][col];
}
static void r48_keyboard_draw(const char *prompt,const char *shown,int row,int col,bool lobbyPump){
    struct GfxRenderingAPI *r=&gfx_nv2a_api;r->start_frame();ui_budget_reset();r->set_depth_test(0);r->set_depth_mask(0);r->select_texture(0,0);gfx_pvr_set_blend(UI_KIND_OP);ui_quad(0,0,640,480,0xFF102030);
    bool live=lobbyPump||gR59WorldKeyboard;float py=live?104.0f:72.0f;if(live){ui_text(42,24,gR59WorldKeyboard?"WORLD CHAT":"LOBBY CHAT",1.15f,0xFF90D0FF);const char *c0=gR59WorldKeyboard?gR59WorldLines[3]:gR48Chat4;const char *c1=gR59WorldKeyboard?gR59WorldLines[4]:gR48Chat5;const char *c2=gR59WorldKeyboard?gR59WorldLines[5]:gR48Chat6;ui_text(42,40,c0,r54_og_fit(c0,.95f,.72f,546.0f),0xFFFFFFFF);ui_text(42,62,c1,r54_og_fit(c1,.95f,.72f,546.0f),0xFFFFFFFF);ui_text(42,84,c2,r54_og_fit(c2,.95f,.72f,546.0f),0xFFFFFFFF);}
    const float px=155.0f;ui_text(px,py,prompt?prompt:"KEYBOARD",1.8f,0xFFFFD050);char typed[72];snprintf(typed,sizeof(typed)-1,"TEXT: %s",shown?shown:"");typed[sizeof(typed)-1]=0;ui_text(px,py+32,typed,1.35f,0xFFFFFFFF);
    const float gx=168.0f,gy=py+72.0f,dx=34.0f,dy=32.0f;for(int rr=0;rr<4;++rr){for(int cc=0;cc<9;++cc){float x=gx+cc*dx,y=gy+rr*dy;char k[2]={r48_key(rr,cc),0};if(rr==row&&cc==col){ui_quad(x-5,y-5,x+23,y+22,0xFFFFD050);ui_text(x,y,k,1.55f,0xFF102030);}else ui_text(x,y,k,1.55f,0xFFFFFFFF);}}
    char selected[48];snprintf(selected,sizeof(selected)-1,"SELECTED: %c",r48_key(row,col));selected[sizeof(selected)-1]=0;ui_text(px,gy+140,selected,1.25f,0xFFFFD050);ui_text(105,gy+172,"A TYPE  X DEL  Y SPACE  START DONE  B CANCEL",1.05f,0xFFB8B8B8);r->end_frame();r->finish_render();
}
static bool r48_keyboard(const char *prompt,char *out,int cap,bool secret,bool lobbyPump){
    (void)secret;int row=0,col=0,len=(int)strlen(out);ui_consume();for(;;){if(lobbyPump){pump_packets();if(gActive){ui_consume();return false;}if(gR48PublicHost&&gR48RoomRegistered)r48_heartbeat();r57_presence_tick(false);r48_poll_chat();}else if(gR59WorldKeyboard){r57_presence_tick(true);}
        char shown[56];strncpy(shown,out,sizeof(shown)-1);shown[sizeof(shown)-1]=0;r48_keyboard_draw(prompt,shown,row,col,lobbyPump);uint32_t p=ui_pressed();if(p&CONT_B){ui_consume();return false;}if(p&CONT_DPAD_LEFT)col=(col+8)%9;if(p&CONT_DPAD_RIGHT)col=(col+1)%9;if(p&CONT_DPAD_UP)row=(row+3)%4;if(p&CONT_DPAD_DOWN)row=(row+1)%4;if((p&CONT_X)&&len>0)out[--len]=0;if((p&CONT_Y)&&len<cap-1){out[len++]=' ';out[len]=0;}if((p&CONT_A)&&len<cap-1){out[len++]=r48_key(row,col);out[len]=0;}if(p&CONT_START){while(len>0&&out[len-1]==' ')out[--len]=0;if(len>0){ui_consume();return true;}}Sleep(16);}
}
/* MK64_R48_5_OG_ACCOUNT_CHAT - retry-safe account create for directory R2.2. */
static unsigned gR485CreateCounter=0;static char gR485CreateName[20]={0},gR485CreateNonce[24]={0};
static const char *r485_create_nonce_for(const char *name){if(!name)name="";if(strcmp(gR485CreateName,name)||!gR485CreateNonce[0]){strncpy(gR485CreateName,name,sizeof(gR485CreateName)-1);gR485CreateName[sizeof(gR485CreateName)-1]=0;++gR485CreateCounter;snprintf(gR485CreateNonce,sizeof(gR485CreateNonce)-1,"O%08lX%04X",(unsigned long)GetTickCount(),gR485CreateCounter&0xFFFFU);gR485CreateNonce[sizeof(gR485CreateNonce)-1]=0;}return gR485CreateNonce;}
static void r485_create_nonce_done(){gR485CreateName[0]=0;gR485CreateNonce[0]=0;}
/* MK64_R49_ACCOUNT_PASSWORD_LOGIN */
static unsigned gR49CreateCounter=0;static char gR49CreateKey[48]={0},gR49CreateNonce[24]={0};
static const char *r49_nonce_for(const char *name,const char *password){char key[48];snprintf(key,sizeof(key)-1,"%s:%s",name?name:"",password?password:"");key[sizeof(key)-1]=0;if(strcmp(key,gR49CreateKey)||!gR49CreateNonce[0]){strncpy(gR49CreateKey,key,sizeof(gR49CreateKey)-1);gR49CreateKey[sizeof(gR49CreateKey)-1]=0;++gR49CreateCounter;snprintf(gR49CreateNonce,sizeof(gR49CreateNonce)-1,"P%08lX%04X",(unsigned long)GetTickCount(),gR49CreateCounter&0xFFFFU);gR49CreateNonce[sizeof(gR49CreateNonce)-1]=0;}return gR49CreateNonce;}
static void r49_nonce_done(){gR49CreateKey[0]=0;gR49CreateNonce[0]=0;}
static int r49_find_local(const char *name){for(int i=0;i<gR48AccountCount;++i)if(!strcmp(gR48Accounts[i].name,name))return i;return -1;}
static bool r49_store_ok(const char *resp,const char *password){R48AccountOG a;memset(&a,0,sizeof(a));if(sscanf(resp,"MKDIR2|ACCOUNT_OK|%19[^|]|%39[^|]|%19[^\r\n]",a.id,a.secret,a.name)!=3)return false;strncpy(a.password,password?password:"",sizeof(a.password)-1);a.password[sizeof(a.password)-1]=0;int i=r49_find_local(a.name);if(i<0){if(gR48AccountCount>=4)return false;i=gR48AccountCount++;}gR48Accounts[i]=a;gR48AccountIndex=i;r48_save_accounts();r484_remember_user(a.name);return true;}
static void r49_wait_a_or_b(){while(!(ui_pressed()&(CONT_A|CONT_B)))Sleep(16);ui_consume();}
/* MK64_R51_SECURE_ACCOUNT_CHALLENGE */
/* MK64_R51_SECURE_ACCOUNT_AUTH - self-contained SHA256/HMAC/PBKDF2 */
struct R51ShaCtx{unsigned int h[8];unsigned long long bits;unsigned char b[64];unsigned int used;};
static unsigned int r51_rr(unsigned int x,unsigned n){return (x>>n)|(x<<(32-n));}
static void r51_sha_init(R51ShaCtx *c){static const unsigned int iv[8]={0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U};memcpy(c->h,iv,32);c->bits=0;c->used=0;}
static void r51_sha_block(R51ShaCtx *c,const unsigned char *p){static const unsigned int k[64]={0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U};unsigned int w[64];for(int i=0;i<16;++i)w[i]=((unsigned)p[i*4]<<24)|((unsigned)p[i*4+1]<<16)|((unsigned)p[i*4+2]<<8)|p[i*4+3];for(int i=16;i<64;++i){unsigned int s0=r51_rr(w[i-15],7)^r51_rr(w[i-15],18)^(w[i-15]>>3),s1=r51_rr(w[i-2],17)^r51_rr(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+s0+w[i-7]+s1;}unsigned int a=c->h[0],b=c->h[1],cc=c->h[2],d=c->h[3],e=c->h[4],f=c->h[5],g=c->h[6],h=c->h[7];for(int i=0;i<64;++i){unsigned int S1=r51_rr(e,6)^r51_rr(e,11)^r51_rr(e,25),ch=(e&f)^((~e)&g),t1=h+S1+ch+k[i]+w[i],S0=r51_rr(a,2)^r51_rr(a,13)^r51_rr(a,22),maj=(a&b)^(a&cc)^(b&cc),t2=S0+maj;h=g;g=f;f=e;e=d+t1;d=cc;cc=b;b=a;a=t1+t2;}c->h[0]+=a;c->h[1]+=b;c->h[2]+=cc;c->h[3]+=d;c->h[4]+=e;c->h[5]+=f;c->h[6]+=g;c->h[7]+=h;}
static void r51_sha_update(R51ShaCtx *c,const unsigned char *p,unsigned int n){c->bits+=(unsigned long long)n*8ULL;while(n){unsigned int take=64-c->used;if(take>n)take=n;memcpy(c->b+c->used,p,take);c->used+=take;p+=take;n-=take;if(c->used==64){r51_sha_block(c,c->b);c->used=0;}}}
static void r51_sha_final(R51ShaCtx *c,unsigned char out[32]){c->b[c->used++]=0x80;if(c->used>56){while(c->used<64)c->b[c->used++]=0;r51_sha_block(c,c->b);c->used=0;}while(c->used<56)c->b[c->used++]=0;for(int i=7;i>=0;--i)c->b[c->used++]=(unsigned char)(c->bits>>(i*8));r51_sha_block(c,c->b);for(int i=0;i<8;++i){out[i*4]=(unsigned char)(c->h[i]>>24);out[i*4+1]=(unsigned char)(c->h[i]>>16);out[i*4+2]=(unsigned char)(c->h[i]>>8);out[i*4+3]=(unsigned char)c->h[i];}}
static void r51_hmac(const unsigned char *key,unsigned int kn,const unsigned char *msg,unsigned int mn,unsigned char out[32]){unsigned char k0[64],ip[64],op[64],inner[32];memset(k0,0,64);if(kn>64){R51ShaCtx c;r51_sha_init(&c);r51_sha_update(&c,key,kn);r51_sha_final(&c,k0);}else memcpy(k0,key,kn);for(int i=0;i<64;++i){ip[i]=(unsigned char)(k0[i]^0x36);op[i]=(unsigned char)(k0[i]^0x5c);}R51ShaCtx c;r51_sha_init(&c);r51_sha_update(&c,ip,64);r51_sha_update(&c,msg,mn);r51_sha_final(&c,inner);r51_sha_init(&c);r51_sha_update(&c,op,64);r51_sha_update(&c,inner,32);r51_sha_final(&c,out);}
static int r51_hex_nib(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
static bool r51_hex32(const char *s,unsigned char out[16]){if(!s||strlen(s)!=32)return false;for(int i=0;i<16;++i){int a=r51_hex_nib(s[i*2]),b=r51_hex_nib(s[i*2+1]);if(a<0||b<0)return false;out[i]=(unsigned char)((a<<4)|b);}return true;}
static void r51_tohex(const unsigned char *p,unsigned int n,char *out){static const char h[]="0123456789abcdef";for(unsigned int i=0;i<n;++i){out[i*2]=h[p[i]>>4];out[i*2+1]=h[p[i]&15];}out[n*2]=0;}
static bool r51_pbkdf2(const char *password,const char *salt_hex,unsigned int rounds,unsigned char out[32]){if(!password||!password[0]||rounds<1||rounds>250000U)return false;unsigned char salt[16],m[20],u[32],t[32];if(!r51_hex32(salt_hex,salt))return false;memcpy(m,salt,16);m[16]=0;m[17]=0;m[18]=0;m[19]=1;r51_hmac((const unsigned char*)password,(unsigned int)strlen(password),m,20,u);memcpy(t,u,32);for(unsigned int i=1;i<rounds;++i){r51_hmac((const unsigned char*)password,(unsigned int)strlen(password),u,32,u);for(int j=0;j<32;++j)t[j]^=u[j];}memcpy(out,t,32);return true;}
static bool r51_verifier_hex(const char *password,const char *salt_hex,unsigned int rounds,char out[65]){unsigned char v[32];if(!r51_pbkdf2(password,salt_hex,rounds,v))return false;r51_tohex(v,32,out);return true;}
static bool r51_proof_hex(const char *password,const char *salt_hex,unsigned int rounds,const char *purpose,const char *pid,const char *nonce,char out[65]){unsigned char v[32],p[32];char msg[128];if(!r51_pbkdf2(password,salt_hex,rounds,v))return false;snprintf(msg,sizeof(msg)-1,"MK64|%s|%s|%s",purpose,pid,nonce);msg[sizeof(msg)-1]=0;r51_hmac(v,32,(const unsigned char*)msg,(unsigned int)strlen(msg),p);r51_tohex(p,32,out);return true;}

/* R71 console pseudonym. The raw MAC is never transmitted or written to the Hub.
 * If firmware does not supply a usable MAC (e.g. some emulators), use a
 * cryptographically random persistent per-install ID. Neither is attested. */
static char r71_console_key[65]={0};
static bool r71_local_fallback(unsigned char material[16]){
    FILE *f=fopen("U:/mk64-device-v1.bin","rb");
    if(f){size_t n=fread(material,1,16,f);fclose(f);if(n==16)return true;}
    if(XNetRandom(material,16)!=0)return false;
    f=fopen("U:/mk64-device-v1.bin","wb");if(!f)return false;
    size_t n=fwrite(material,1,16,f);fclose(f);return n==16;
}
static bool r71_console_id(char out[65]){
    if(r71_console_key[0]){memcpy(out,r71_console_key,65);return true;}
    XNADDR a;memset(&a,0,sizeof(a));XNetGetTitleXnAddr(&a);
    unsigned char material[16]={0};unsigned int len=6;bool found=false, allff=true;
    for(unsigned i=0;i<6;++i){material[i]=a.abEnet[i];if(material[i])found=true;if(material[i]!=255)allff=false;}
    bool hw=found&&!allff;
    if(!hw){len=16;if(!r71_local_fallback(material))return false;}
    static const char domain[]="MK64-HUB-R71-CONSOLE-V1";
    static const char hardware[]="HARDWARE";static const char local[]="INSTALL";
    R51ShaCtx ctx;unsigned char digest[32];r51_sha_init(&ctx);
    r51_sha_update(&ctx,(const unsigned char*)domain,(unsigned int)(sizeof(domain)-1));
    const char *kind=hw?hardware:local;
    r51_sha_update(&ctx,(const unsigned char*)kind,(unsigned int)strlen(kind));
    r51_sha_update(&ctx,material,len);r51_sha_final(&ctx,digest);
    r51_tohex(digest,32,r71_console_key);memcpy(out,r71_console_key,65);return true;
}
static void r71_refresh_device(){
    static DWORD last=0;DWORD now=GetTickCount();if(last&&now-last<30000U)return;
    char id[65],pkt[110];if(!r71_console_id(id))return;
    snprintf(pkt,sizeof(pkt)-1,"MKDIR2|DEVICE_HELLO|%s",id);pkt[sizeof(pkt)-1]=0;
    if(r48_send(pkt)!=SOCKET_ERROR)last=now;
}
static bool r71_register_console(){
    char id[65],req[110],resp[120]={0};
    if(!r71_console_id(id)){ui_screen("CONSOLE ID ERROR","COULD NOT SAVE CONSOLE ID","CHECK GAME STORAGE / NETWORK","","","A/B BACK");r49_wait_a_or_b();return false;}
    snprintf(req,sizeof(req)-1,"MKDIR2|DEVICE_HELLO|%s",id);req[sizeof(req)-1]=0;
    if(!r48_wait_prefix(req,"MKDIR2|DEVICE_ACK|",resp,sizeof(resp),4000U) ||
       strcmp(resp,"MKDIR2|DEVICE_ACK|OK")){
        if(strstr(resp,"BANNED"))ui_screen("CONSOLE BANNED","ACCESS DENIED","CONTACT SERVER ADMIN","","","A/B BACK");
        else ui_screen("HUB CONNECTION ERROR","CONSOLE ID NOT ACCEPTED","CHECK SERVER R2.17 / NETWORK","","","A/B BACK");
        r49_wait_a_or_b();return false;
    }
    return true;
}

static bool r49_login_name_password(const char *name,const char *password){
    char begin[128],challenge[320]={0};
    if(!r71_register_console())return false;
    snprintf(begin,sizeof(begin)-1,"MKDIR2|AUTH_BEGIN|LOGIN|%s",name);begin[sizeof(begin)-1]=0;
    ui_screen("SECURE LOGIN",name,"REQUESTING ONE-TIME CHALLENGE","PASSWORD WILL NOT BE SENT","","PLEASE WAIT");
    if(!r48_wait_prefix(begin,"MKDIR2|AUTH_CHALLENGE|",challenge,sizeof(challenge),5000U)){
        if(strstr(challenge,"ACCOUNT_NOT_FOUND"))ui_screen("LOGIN FAILED","ACCOUNT NOT FOUND",name,"","","A/B BACK");
        else if(strstr(challenge,"LEGACY_ACCOUNT_ADMIN"))ui_screen("LOGIN FAILED","LEGACY ACCOUNT NEEDS MIGRATION",name,"","","A/B BACK");
        else ui_screen("LOGIN FAILED","MARIO KART HUB DID NOT START LOGIN","CHECK MARIO KART HUB / NETWORK","","","A/B BACK");
        r49_wait_a_or_b();return false;
    }
    char purpose[12]={0},pid[20]={0},server_name[20]={0},salt[40]={0},nonce[40]={0};unsigned rounds=0;
    if(sscanf(challenge,"MKDIR2|AUTH_CHALLENGE|%11[^|]|%19[^|]|%19[^|]|%39[^|]|%u|%39s",purpose,pid,server_name,salt,&rounds,nonce)!=6||strcmp(purpose,"LOGIN")){ui_screen("LOGIN ERROR","BAD CHALLENGE RESPONSE","","","","A/B BACK");;r49_wait_a_or_b();return false;}
    ui_screen("SECURE LOGIN",name,"DERIVING PASSWORD PROOF","PASSWORD STAYS ON THIS CONSOLE","","PLEASE WAIT");
    char proof[65];if(!r51_proof_hex(password,salt,rounds,"LOGIN",pid,nonce,proof)){ui_screen("SECURE AUTH ERROR","COULD NOT DERIVE PASSWORD PROOF","","","","A/B BACK");;r49_wait_a_or_b();return false;}
    char finish[256],resp[256]={0};snprintf(finish,sizeof(finish)-1,"MKDIR2|AUTH_FINISH|LOGIN|%s|%s|%s",pid,nonce,proof);finish[sizeof(finish)-1]=0;
    if(!r48_wait_prefix(finish,"MKDIR2|ACCOUNT_OK|",resp,sizeof(resp),5000U)){if(strstr(resp,"BAD_ACCOUNT_PASSWORD"))ui_screen("LOGIN FAILED","WRONG PASSWORD",name,"","","A/B BACK"); else ui_screen("LOGIN FAILED","MARIO KART HUB DID NOT ACCEPT LOGIN","CHECK MARIO KART HUB / NETWORK","","","A/B BACK"); r49_wait_a_or_b();return false;}
    if(!r49_store_ok(resp,password)){ui_screen("LOGIN ERROR","HUB LOGIN SUCCEEDED","BUT CREDENTIALS COULD NOT BE SAVED","","","A/B BACK");;r49_wait_a_or_b();return false;}
    ui_consume();return true;
}
static bool r49_login_other(){char name[20]="",pass[20]="";if(!r48_keyboard("LOGIN USERNAME",name,18,false,false))return false;if(!r48_keyboard("LOGIN PASSWORD",pass,19,false,false))return false;return r49_login_name_password(name,pass);}
static bool r48_create_account(){
    if(gR48AccountCount>=4){ui_screen("ACCOUNTS","THIS CONSOLE ALREADY SAVED 4 ACCOUNTS","HUB LIMITS 4 ACCOUNTS PER CONSOLE","","","A/B BACK");;r49_wait_a_or_b();return false;}
    char name[20]="",pass[20]="",confirm[20]="";if(!r48_keyboard("CREATE USERNAME",name,18,false,false))return false;
    for(;;){pass[0]=confirm[0]=0;if(!r48_keyboard("CREATE ACCOUNT PASSWORD",pass,19,false,false))return false;if(!r48_keyboard("CONFIRM ACCOUNT PASSWORD",confirm,19,false,false))return false;if(!strcmp(pass,confirm))break;ui_screen("PASSWORDS DO NOT MATCH","RE-ENTER THE ACCOUNT PASSWORD","","","","A/B RETRY");;r49_wait_a_or_b();}
    if(!r71_register_console())return false;
    const char *request_nonce=r49_nonce_for(name,pass);char begin[160],challenge[320]={0};snprintf(begin,sizeof(begin)-1,"MKDIR2|ACCOUNT_CREATE_BEGIN|%s|%s",name,request_nonce);begin[sizeof(begin)-1]=0;ui_screen("CREATING ACCOUNT",name,"REQUESTING SECURE ACCOUNT SALT","PASSWORD WILL NOT BE SENT","","PLEASE WAIT");
    if(!r48_wait_prefix(begin,"MKDIR2|ACCOUNT_CREATE_CHALLENGE|",challenge,sizeof(challenge),5000U)){if(!strncmp(challenge,"MKDIR2|ACCOUNT_LIMIT|",21))ui_screen("ACCOUNT LIMIT","You reached the account limit for your console","ASK HUB ADMIN FOR HELP","","","A/B BACK"); else if(strstr(challenge,"NAME_TAKEN")){ui_screen("USERNAME ALREADY EXISTS",name,"USE LOGIN OR CHOOSE ANOTHER NAME","","","A/B BACK");;r49_nonce_done();}else ui_screen("ACCOUNT CREATE FAILED","HUB DID NOT CONFIRM SECURE CREATE","CHECK MARIO KART HUB / NETWORK","","","A/B BACK"); r49_wait_a_or_b();return false;}
    char server_name[20]={0},salt[40]={0},nonce[40]={0},echo[32]={0};unsigned rounds=0;
    if(sscanf(challenge,"MKDIR2|ACCOUNT_CREATE_CHALLENGE|%19[^|]|%39[^|]|%u|%39[^|]|%31s",server_name,salt,&rounds,nonce,echo)!=5){ui_screen("ACCOUNT CREATE ERROR","BAD HUB CHALLENGE","","","","A/B BACK");;r49_wait_a_or_b();return false;}
    ui_screen("CREATING ACCOUNT",name,"DERIVING SALTED PASSWORD VERIFIER","PASSWORD STAYS ON THIS CONSOLE","","PLEASE WAIT");
    char verifier[65];if(!r51_verifier_hex(pass,salt,rounds,verifier)){ui_screen("SECURE AUTH ERROR","COULD NOT DERIVE PASSWORD PROOF","","","","A/B BACK");;r49_wait_a_or_b();return false;}
    char finish[360],resp[256]={0};snprintf(finish,sizeof(finish)-1,"MKDIR2|ACCOUNT_CREATE_FINISH|%s|%s|%u|%s|%s|%s",server_name,salt,rounds,nonce,verifier,request_nonce);finish[sizeof(finish)-1]=0;
    if(!r48_wait_prefix(finish,"MKDIR2|ACCOUNT_OK|",resp,sizeof(resp),5000U)){if(!strncmp(resp,"MKDIR2|ACCOUNT_LIMIT|",21))ui_screen("ACCOUNT LIMIT","You reached the account limit for your console","ASK HUB ADMIN FOR HELP","","","A/B BACK"); else if(strstr(resp,"NAME_TAKEN"))ui_screen("USERNAME ALREADY EXISTS",name,"USE LOGIN OR CHOOSE ANOTHER NAME","","","A/B BACK"); else ui_screen("ACCOUNT CREATE FAILED","HUB DID NOT CONFIRM SECURE CREATE","CHECK MARIO KART HUB / NETWORK","","","A/B BACK"); r49_wait_a_or_b();return false;}
    if(!r49_store_ok(resp,pass)){ui_screen("ACCOUNT RESPONSE ERROR","ACCOUNT WAS CREATED BUT COULD NOT BE SAVED","ASK HUB ADMIN BEFORE RETRYING","","","A/B BACK");;r49_wait_a_or_b();return false;}r49_nonce_done();ui_screen("ACCOUNT CREATED",name,"SAVED TO THIS CONSOLE","PASSWORD ITSELF WAS NOT SENT","","A CONTINUE");;r49_wait_a_or_b();return true;
}
static void r50_remove_local_account(int idx){if(idx<0||idx>=gR48AccountCount)return;char deleted[20];strncpy(deleted,gR48Accounts[idx].name,sizeof(deleted)-1);deleted[sizeof(deleted)-1]=0;for(int i=idx;i+1<gR48AccountCount;++i)gR48Accounts[i]=gR48Accounts[i+1];if(gR48AccountCount>0)--gR48AccountCount;if(gR48AccountIndex>=gR48AccountCount)gR48AccountIndex=0;r48_save_accounts();r484_load_prefs();if(!strcmp(gR484SavedUser,deleted)){gR484SavedUser[0]=0;r484_save_prefs();}}
static bool r50_delete_current_account(){
    if(gR48AccountIndex<0||gR48AccountIndex>=gR48AccountCount)return false;R48AccountOG a=gR48Accounts[gR48AccountIndex];
    for(;;){ui_screen("DELETE ACCOUNT?",a.name,"THIS DELETES HUB + LOCAL ACCOUNT","A CONTINUE  B CANCEL","","PASSWORD REQUIRED TO CONFIRM"); uint32_t q=ui_pressed(); if(q&CONT_B){ui_consume(); return false;}if(q&CONT_A)break;Sleep(16);}ui_consume();
    char pass[20]="";if(!r48_keyboard("ENTER ACCOUNT PASSWORD TO DELETE",pass,sizeof(pass),false,false))return false;
    char begin[180],challenge[320]={0};snprintf(begin,sizeof(begin)-1,"MKDIR2|AUTH_BEGIN|DELETE|%s|%s",a.id,a.secret);begin[sizeof(begin)-1]=0;ui_screen("DELETING ACCOUNT",a.name,"REQUESTING ONE-TIME CHALLENGE","PASSWORD WILL NOT BE SENT","","PLEASE WAIT");
    if(!r48_wait_prefix(begin,"MKDIR2|AUTH_CHALLENGE|",challenge,sizeof(challenge),5000U)){ui_screen("DELETE FAILED","HUB DID NOT CONFIRM SECURE DELETE","LOCAL ACCOUNT WAS KEPT","","","A/B BACK");;r49_wait_a_or_b();return false;}
    char purpose[12]={0},pid[20]={0},server_name[20]={0},salt[40]={0},nonce[40]={0};unsigned rounds=0;
    if(sscanf(challenge,"MKDIR2|AUTH_CHALLENGE|%11[^|]|%19[^|]|%19[^|]|%39[^|]|%u|%39s",purpose,pid,server_name,salt,&rounds,nonce)!=6||strcmp(purpose,"DELETE")){ui_screen("DELETE FAILED","HUB DID NOT CONFIRM SECURE DELETE","LOCAL ACCOUNT WAS KEPT","","","A/B BACK");;r49_wait_a_or_b();return false;}
    char proof[65];if(!r51_proof_hex(pass,salt,rounds,"DELETE",pid,nonce,proof)){ui_screen("SECURE AUTH ERROR","COULD NOT DERIVE PASSWORD PROOF","","","","A/B BACK");;r49_wait_a_or_b();return false;}
    char finish[256],resp[256]={0};snprintf(finish,sizeof(finish)-1,"MKDIR2|AUTH_FINISH|DELETE|%s|%s|%s",pid,nonce,proof);finish[sizeof(finish)-1]=0;
    if(!r48_wait_prefix(finish,"MKDIR2|ACCOUNT_DELETED|",resp,sizeof(resp),5000U)){if(strstr(resp,"BAD_ACCOUNT_PASSWORD"))ui_screen("DELETE FAILED","WRONG ACCOUNT PASSWORD",a.name,"","","A/B BACK"); else ui_screen("DELETE FAILED","HUB DID NOT CONFIRM SECURE DELETE","LOCAL ACCOUNT WAS KEPT","","","A/B BACK"); r49_wait_a_or_b();return false;}
    int idx=gR48AccountIndex;r50_remove_local_account(idx);ui_screen("ACCOUNT DELETED",a.name,"REMOVED FROM MARIO KART HUB","REMOVED FROM THIS CONSOLE","","A CONTINUE");;r49_wait_a_or_b();return true;
}
static bool r50_account_actions(){R48AccountOG *me=(gR48AccountIndex>=0&&gR48AccountIndex<gR48AccountCount)?&gR48Accounts[gR48AccountIndex]:0;if(!me)return false;int row=0;ui_consume();for(;;){char a[72],b[72],c[72],d[72];snprintf(a,71,"SIGNED IN: %s",me->name);snprintf(b,71,"%c CONTINUE TO PUBLIC MATCH",row==0?'>':' ');snprintf(c,71,"%c BACK TO ACCOUNT LIST",row==1?'>':' ');d[0]=0;ui_screen("ACCOUNT",a,b,c,d,"A SELECT  B BACK - DELETE: HOLD X+Y ON ACCOUNT LIST");uint32_t p=ui_pressed();if(p&CONT_B)return false;if(p&CONT_DPAD_DOWN)row=(row+1)%2;if(p&CONT_DPAD_UP)row=(row+1)%2;if(p&CONT_A){if(row==0){ui_consume();return true;}return false;}Sleep(16);}}
static bool r48_account_login(int idx){if(idx<0||idx>=gR48AccountCount)return false;gR48AccountIndex=idx;if(!gR48Accounts[idx].password[0]){ui_screen("SAVED PASSWORD MISSING",gR48Accounts[idx].name,"USE LOGIN TO ANOTHER ACCOUNT","TO SAVE ITS PASSWORD AGAIN","","A/B BACK");r49_wait_a_or_b();return false;}/* R50 privacy: saved password is used internally and never displayed. */if(!r49_login_name_password(gR48Accounts[idx].name,gR48Accounts[idx].password))return false;ui_consume();return true;}
static bool r48_choose_account(){r48_load_accounts();r484_load_prefs();int row=0;for(int i=0;i<gR48AccountCount;++i)if(gR484SavedUser[0]&&!strcmp(gR48Accounts[i].name,gR484SavedUser)){row=i;break;}ui_consume();for(;;){
        if(row<gR48AccountCount && r54_og_delete_hold()){gR48AccountIndex=row;ui_consume();r50_delete_current_account();ui_consume();continue;}int items=gR48AccountCount+3;if(row>=items)row=0;char l[4][80];for(int i=0;i<4;++i)l[i][0]=0;int first=row-1;if(first<0)first=0;if(first>items-4)first=items-4;if(first<0)first=0;for(int j=0;j<4;++j){int i=first+j;if(i>=items)continue;if(i<gR48AccountCount)snprintf(l[j],79,"%c %s",i==row?'>':' ',gR48Accounts[i].name);else if(i==gR48AccountCount)snprintf(l[j],79,"%c LOGIN TO ANOTHER ACCOUNT",i==row?'>':' ');else if(i==gR48AccountCount+1)snprintf(l[j],79,"%c + %s",i==row?'>':' ',gR48AccountCount?"CREATE ANOTHER ACCOUNT":"CREATE ACCOUNT");else snprintf(l[j],79,"%c BACK",i==row?'>':' ');l[j][79]=0;}ui_screen("PUBLIC MATCH ACCOUNT",l[0],l[1],l[2],l[3],"A SELECT  B BACK  HOLD X+Y 3 SEC: DELETE");uint32_t p=ui_pressed();if(p&CONT_B)return false;if(p&CONT_DPAD_DOWN)row=(row+1)%items;if(p&CONT_DPAD_UP)row=(row+items-1)%items;if(p&CONT_A){if(row<gR48AccountCount){if(r48_account_login(row)){ui_consume();return true;}ui_consume();}else if(row==gR48AccountCount){if(r49_login_other()){ui_consume();return true;}ui_consume();}else if(row==gR48AccountCount+1){int before=gR48AccountCount;if(r48_create_account()){row=(gR48AccountCount>before)?gR48AccountCount-1:gR48AccountCount;continue;}ui_consume();}else return false;}Sleep(16);}}
static R48AccountOG *r48_me(){return (gR48AccountIndex>=0&&gR48AccountIndex<gR48AccountCount)?&gR48Accounts[gR48AccountIndex]:0;}
#define R61_OG
#define R61_WORLD_SUPPRESS gR59WorldOverlaySuppress
static bool r70_social_packet(const char *b);
#include "r61_social_shared.inl"
static void r62_lobby_show_stats(){R48AccountOG *me=r48_me();if(!me)return;r62_modal_lobby=true;r61_profile_view(me->id,true);r62_modal_lobby=false;ui_consume();}
static void r62_lobby_service();
static void r61_modal_service(){if(r62_modal_lobby)r62_lobby_service();}
#include "r62_game_stats.inl"
#define R70_ROOM_ID gR48RoomId
#define R70_LOBBY_RUNNING (!gActive && !gFailed)
#include "r70_room_social.inl"
#include "r73_solo_online.inl"
#undef R70_ROOM_ID
#undef R70_LOBBY_RUNNING
/* R61 two-column Public Match hub. UI is independent of game simulation. */
static void r61_menu_draw(int left_row,bool focus_right,int right_row){
    struct GfxRenderingAPI *r=&gfx_nv2a_api;r->start_frame();ui_budget_reset();
    r->set_depth_test(0);r->set_depth_mask(0);r->select_texture(0,0);gfx_pvr_set_blend(UI_KIND_OP);
    ui_quad(0,0,640,480,0xFF101C2A);
    ui_quad(40,98,592,100,0xFFFFD050);
    ui_text(46,32,"MARIO KART HUB / ONLINE",2.15f,0xFFFFD050);
    
    ui_text(46,84,"(C) 2026 SIRDANKZ  MPL-2.0 / UPSTREAM RIGHTS UNCHANGED",1.00f,0xFF8AA7C0);
    char count[32];snprintf(count,sizeof(count)-1,"PLAYERS ONLINE: %d",gR57OnlineCount>=0?gR57OnlineCount:0);count[sizeof(count)-1]=0;
    ui_text(449,70,count,.99f,0xFF90D0FF);
    char gcount[34];snprintf(gcount,sizeof(gcount)-1,"PLAYERS IN GAME: %d",r62_game_total>=0?r62_game_total:0);gcount[sizeof(gcount)-1]=0;ui_text(449,87,gcount,.99f,0xFF90D0FF);
    ui_quad(40,119,358,391,0xFF182B3F);ui_quad(367,119,592,391,0xFF182B3F);
    ui_quad(352,128,354,380,0xFF34506B);
    ui_text(51,130,"PLAY",1.35f,0xFFFFD050);
    ui_text(378,130,"ONLINE PLAYERS",1.22f,0xFFFFD050);
    const char *items[6]={"CREATE PUBLIC ROOM","BROWSE PUBLIC ROOMS","SOLO ONLINE","REGION","LEADERBOARDS","SIGN OUT / BACK"};
    for(int i=0;i<6;++i){
        char item[60];if(i==3)snprintf(item,sizeof(item)-1,"REGION: %s",gR484Region);else snprintf(item,sizeof(item)-1,"%s",items[i]);item[sizeof(item)-1]=0;
        if(i==left_row&&!focus_right)ui_quad(47,159+i*35,346,191+i*35,0xFF334B64);
        char line[72];snprintf(line,sizeof(line)-1,"%c %s",i==left_row&&!focus_right?'>':' ',item);line[sizeof(line)-1]=0;
        ui_text(54,171+i*35,line,1.19f,i==left_row&&!focus_right?0xFFFFD050:0xFFE4F1FA);
    }
    /* Four comfortably spaced rows avoid saturating the OG Xbox's safe NV2A
     * UI vertex budget when five long player names are online. All five Hub
     * records remain selectable: the fifth slides the four-row view down. */
    int first_player=(focus_right&&right_row>=4)?1:0;
    int visible=r61_count_people()-first_player;if(visible<0)visible=0;if(visible>4)visible=4;
    char page[48];snprintf(page,sizeof(page)-1,"%d-%d OF %d  UP/DOWN",r61_total? r61_offset+first_player+1:0,r61_offset+first_player+visible,r61_total);page[sizeof(page)-1]=0;
    ui_text(380,368,page,.97f,0xFF9EB4C9);
    ui_text(49,404,focus_right?"A PROFILE  LEFT MENU":"A SELECT  SOLO SAVES TT BESTS",1.23f,0xFFFFD050);
    ui_text(49,435,"RT STATS  Y PROFILE  X MAIL  RS CHAT  B BACK",1.06f,0xFFB8CAD9);
    ui_text(379,406,"HOST: PORT FORWARD UDP 6464",.94f,0xFFFF6868);
    ui_text(379,423,"FOR BEST CONNECTION",.90f,0xFFFF6868);
    if(r61_total<=0){ui_text(378,194,"NO OTHER PLAYERS",1.08f,0xFFFFFFFF);ui_text(378,214,"ONLINE RIGHT NOW",.98f,0xFF9EB4C9);}
    else for(int j=0;j<4;++j){
        int i=j+first_player;
        if(!r61_people[i].found)continue;
        const R61Person &u=r61_people[i];
        float yy=164.0f+j*47.0f;
        if(focus_right&&i==right_row)ui_quad(373,yy-7,587,yy+25,0xFF334B64);
        char line[64];snprintf(line,sizeof(line)-1,"%c %.16s",focus_right&&i==right_row?'>':' ',u.name);line[sizeof(line)-1]=0;
        char extra[45];snprintf(extra,sizeof(extra)-1,"%s / %s%s",u.platform,u.region,u.playing?" IN GAME":"");extra[sizeof(extra)-1]=0;
        if(!r61_line_fits(line,r61_line_quads(extra)+3))break;
        ui_text(380,yy,line,r54_og_fit(line,1.13f,1.0f,205.0f),focus_right&&i==right_row?0xFFFFD050:0xFFFFFFFF);
        ui_text(381,yy+15,extra,1.0f,0xFF90D0FF);
    }
    r->end_frame();r->finish_render();
}
static void r57_region_menu(){static const char *regions[]={"UNSET","US-W","US-C","US-E","CANADA","LATAM","EUROPE","ASIA","OCEANIA","OTHER"};int row=0;for(int i=0;i<10;++i)if(!strcmp(gR484Region,regions[i]))row=i;ui_consume();for(;;){r57_presence_tick(true);char a[64],b[64],c[64],d[64];int first=(row/4)*4;const char *l[4]={"","","",""};for(int j=0;j<4;++j)if(first+j<10)l[j]=regions[first+j];snprintf(a,63,"%c %s",row==first?'>':' ',l[0]);snprintf(b,63,"%c %s",row==first+1?'>':' ',l[1]);snprintf(c,63,"%c %s",row==first+2?'>':' ',l[2]);snprintf(d,63,"%c %s",row==first+3?'>':' ',l[3]);ui_screen("SELECT REGION",a,b,c,d,"A SAVE  UP/DOWN  B BACK");uint32_t q=ui_pressed();if(q&kUiRightStickButton){r59_world_chat_view();continue;}if(q&CONT_B)return;if(q&CONT_DPAD_DOWN)row=(row+1)%10;if(q&CONT_DPAD_UP)row=(row+9)%10;if(q&CONT_A){strncpy(gR484Region,regions[row],sizeof(gR484Region)-1);gR484Region[sizeof(gR484Region)-1]=0;r484_save_prefs();gR57LastPresence=0;gR59WorldLastPoll=0;ui_consume();return;}Sleep(16);}}

static bool r48_room_setup(){R48AccountOG *me=r48_me();if(!me)return false;r484_load_prefs();r484_remember_user(me->name);if(gR484SavedRoom[0]){strncpy(gR48RoomName,gR484SavedRoom,sizeof(gR48RoomName)-1);gR48RoomName[sizeof(gR48RoomName)-1]=0;}else{snprintf(gR48RoomName,sizeof(gR48RoomName)-1,"%s ROOM",me->name);gR48RoomName[sizeof(gR48RoomName)-1]=0;}strncpy(gR48RoomPassword,gR484SavedPassword,sizeof(gR48RoomPassword)-1);gR48RoomPassword[sizeof(gR48RoomPassword)-1]=0;int row=0;ui_consume();for(;;){r57_presence_tick(true);char a[72],b[72],c[72],d[72];snprintf(a,71,"%c NAME: %s",row==0?'>':' ',gR48RoomName);if(gR48RoomPassword[0])snprintf(b,71,"%c ROOM PASSWORD: %s",row==1?'>':' ',gR48RoomPassword);else snprintf(b,71,"%c ROOM PASSWORD: NONE (OPTIONAL)",row==1?'>':' ');snprintf(c,71,"%c START HOSTING",row==2?'>':' ');snprintf(d,71,"%c BACK",row==3?'>':' ');ui_screen("PUBLIC ROOM SETUP",a,b,c,d,gR66Requested60?
        "Y 60FPS ON | MENU DELAY HIGHER; RACE TARGET 60": "Y 60FPS OFF | A SELECT");uint32_t p=ui_pressed();if(p&kUiRightStickButton){r59_world_chat_view();continue;}
        if(p&kUiYButton){gR66Requested60=!gR66Requested60;ui_consume();continue;}
        if(p&CONT_B)return false;if(p&CONT_DPAD_DOWN)row=(row+1)%4;if(p&CONT_DPAD_UP)row=(row+3)%4;if(row==1&&(p&CONT_X)){gR48RoomPassword[0]=gR484SavedPassword[0]=0;r484_save_prefs();}if(p&CONT_A){if(row==0){char tmp[32];strncpy(tmp,gR48RoomName,sizeof(tmp)-1);tmp[sizeof(tmp)-1]=0;if(r48_keyboard("ROOM NAME",tmp,sizeof(tmp),false,false)&&tmp[0]){strncpy(gR48RoomName,tmp,sizeof(gR48RoomName)-1);gR48RoomName[sizeof(gR48RoomName)-1]=0;strncpy(gR484SavedRoom,gR48RoomName,sizeof(gR484SavedRoom)-1);gR484SavedRoom[sizeof(gR484SavedRoom)-1]=0;r484_save_prefs();}}else if(row==1){char tmp[20];strncpy(tmp,gR48RoomPassword,sizeof(tmp)-1);tmp[sizeof(tmp)-1]=0;if(r48_keyboard("OPTIONAL ROOM PASSWORD",tmp,sizeof(tmp),false,false)){strncpy(gR48RoomPassword,tmp,sizeof(gR48RoomPassword)-1);gR48RoomPassword[sizeof(gR48RoomPassword)-1]=0;strncpy(gR484SavedPassword,gR48RoomPassword,sizeof(gR484SavedPassword)-1);gR484SavedPassword[sizeof(gR484SavedPassword)-1]=0;r484_save_prefs();}}else if(row==2){strncpy(gR484SavedRoom,gR48RoomName,sizeof(gR484SavedRoom)-1);strncpy(gR484SavedPassword,gR48RoomPassword,sizeof(gR484SavedPassword)-1);gR484SavedRoom[sizeof(gR484SavedRoom)-1]=0;gR484SavedPassword[sizeof(gR484SavedPassword)-1]=0;r484_save_prefs();
            if(gR66Requested60){char tagged[32];snprintf(tagged,sizeof(tagged),"[60] %.18s",gR48RoomName);strcpy(gR48RoomName,tagged);}
            return true;}else return false;}Sleep(16);}}
static bool r48_register_room(){R48AccountOG *me=r48_me();if(!me)return false;char msg[360],resp[256];snprintf(msg,sizeof(msg)-1,"MKDIR2|REGISTER|%s|%s|%s|%s|6464|%u|4|OG|%s|%s|%s",me->id,me->secret,gR48RoomName,gR48RoomPassword,gLocalCount,gR66Requested60?"11":"10",gR66Requested60?"BE610928":"BE100927",gR484Region);msg[sizeof(msg)-1]=0;if(!r48_wait_prefix(msg,"MKDIR2|REGOK|",resp,sizeof(resp)))return false;if(sscanf(resp,"MKDIR2|REGOK|%19[^|]|%39s",gR48RoomId,gR48RoomToken)!=2)return false;gR48RoomRegistered=true;gR48LastHeartbeat=gR48LastPoll=0;gR48ChatSeq=0;r50_chat_reset(gR48RoomName,me->name);return true;}
static void r48_heartbeat(){if(!gR48RoomRegistered)return;DWORD now=GetTickCount();if(gR48LastHeartbeat&&now-gR48LastHeartbeat<4000U)return;char m[180];snprintf(m,sizeof(m)-1,"MKDIR2|HEARTBEAT|%s|%s|%u",gR48RoomId,gR48RoomToken,lobby_slots());m[sizeof(m)-1]=0;bool first=(gR48LastHeartbeat==0);int sr=r48_send(m);if(sr==SOCKET_ERROR)log_line("R56_ROOM: directory heartbeat send failed wsa=%d\n",WSAGetLastError());else if(first)log_line("R56_ROOM: public lobby heartbeat active\n");gR48LastHeartbeat=now;}
static void r48_unregister_room(){if(gR48RoomRegistered){char m[180];snprintf(m,sizeof(m)-1,"MKDIR2|UNREGISTER|%s|%s",gR48RoomId,gR48RoomToken);m[sizeof(m)-1]=0;r48_send(m);}gR48RoomRegistered=false;gR48RoomId[0]=gR48RoomToken[0]=0;}
static void r48_leave_room(){R48AccountOG *me=r48_me();if(!me||!gR48RoomId[0])return;char m[220];snprintf(m,sizeof(m)-1,"MKDIR2|ROOM_LEAVE|%s|%s|%s",gR48RoomId,me->id,me->secret);m[sizeof(m)-1]=0;for(int i=0;i<3;++i)r48_send(m);}
static void r48_poll_chat(){R48AccountOG *me=r48_me();if(!me||!gR48RoomId[0]||gR48DirSocket==INVALID_SOCKET)return;DWORD now=GetTickCount();if(gR582ChatResume&&now<gR582ChatResume)return;if(gR581ChatPendingReady&&gR581ChatPending[0]){char cm[360];snprintf(cm,sizeof(cm)-1,"MKDIR2|CHAT_SEND|%s|%s|%s|%s",gR48RoomId,me->id,me->secret,gR581ChatPending);cm[sizeof(cm)-1]=0;if(r48_send(cm)!=SOCKET_ERROR){gR581ChatPendingReady=false;gR581ChatPending[0]=0;gR48LastPoll=0;gR582ChatResume=now+100U;log_line("R58.2_CHAT: queued message sent on isolated lobby frame\n");}return;}if(!gR48LastPoll||now-gR48LastPoll>=450U){char m[240];snprintf(m,sizeof(m)-1,"MKDIR2|ROOM_POLL|%s|%s|%s|%u",gR48RoomId,me->id,me->secret,gR48ChatSeq);m[sizeof(m)-1]=0;r48_send(m);gR48LastPoll=now;}for(;;){char b[512];sockaddr_in from;int flen=sizeof(from);int n=recvfrom(gR48DirSocket,b,sizeof(b)-1,0,(sockaddr*)&from,&flen);if(n<=0)break;b[n]=0;unsigned seq=0,sys=0;char who[20]={0},msg[80]={0};if(r59_world_packet(b)){}else if(r61_social_packet(b)){}else if(!strncmp(b,"MKDIR2|STATUS|",14)){r57_parse_status(b);}else if(!strncmp(b,"MKDIR2|STATE|",13)){r57_parse_state(b);}else if(sscanf(b,"MKDIR2|CHAT|%u|%19[^|]|%79[^|]|%u",&seq,who,msg,&sys)==4){if(seq>gR48ChatSeq){gR48ChatSeq=seq;r48_set_chat(who,msg);}}else{unsigned end=0;if(sscanf(b,"MKDIR2|POLLEND|%u",&end)==1&&end>gR48ChatSeq)gR48ChatSeq=end;}}}
static void r62_lobby_service(){if(!r62_modal_lobby)return;pump_packets();if(gR48PublicMode){if(gHosting)r48_heartbeat();r57_presence_tick(false);r48_poll_chat();}}
static void r48_chat_compose(){R48AccountOG *me=r48_me();if(!me||!gR48RoomId[0])return;char msg[56]="";if(!r48_keyboard("LOBBY CHAT",msg,53,false,true)||!msg[0])return;strncpy(gR581ChatPending,msg,sizeof(gR581ChatPending)-1);gR581ChatPending[sizeof(gR581ChatPending)-1]=0;gR581ChatPendingReady=true;gR582ChatResume=GetTickCount()+150U;ui_consume();}
static void r59_world_compose(bool lobbyPump){char msg[56]="";gR59WorldKeyboard=true;bool ok=r48_keyboard("WORLD CHAT",msg,53,false,lobbyPump);gR59WorldKeyboard=false;if(!ok||!msg[0]){ui_consume();return;}strncpy(gR59WorldPending,msg,sizeof(gR59WorldPending)-1);gR59WorldPending[sizeof(gR59WorldPending)-1]=0;gR59WorldPendingReady=true;gR59WorldResume=GetTickCount()+150U;ui_consume();}
static void r59_world_chat_view(){bool old=gR59WorldOverlaySuppress;gR59WorldOverlaySuppress=true;ui_consume();for(;;){r57_presence_tick(true);struct GfxRenderingAPI *r=&gfx_nv2a_api;r->start_frame();ui_budget_reset();r->set_depth_test(0);r->set_depth_mask(0);r->select_texture(0,0);gfx_pvr_set_blend(UI_KIND_OP);ui_quad(0,0,640,480,0xFF102030);ui_text(42,26,"WORLD CHAT",2.55f,0xFFFFD050);ui_text(42,63,"MARIO KART HUB - MADE BY SIRDANKZ",1.25f,0xFF90D0FF);ui_quad(40,92,592,96,0xFFFFD050);bool any=false;int row=0;for(int i=0;i<6;++i){if(!gR59WorldLines[i][0])continue;const char *w=gR59WorldLines[i];ui_text(50,122+row*43,w,r54_og_fit(w,1.06f,.80f,540.0f),0xFFFFFFFF);any=true;++row;}if(!any)ui_text(50,122,"NO WORLD MESSAGES YET",1.06f,0xFFFFFFFF);ui_text(48,405,"X SEND MESSAGE",1.15f,0xFF90D0FF);ui_text(48,435,"RS / B CLOSE",1.05f,0xFFB8B8B8);r->end_frame();r->finish_render();uint32_t q=ui_pressed();if(q&CONT_X){r59_world_compose(false);ui_consume();continue;}if((q&CONT_B)||(q&kUiRightStickButton))break;Sleep(16);}gR59WorldOverlaySuppress=old;ui_consume();}
static int r48_fetch_rooms(R48RoomOG rooms[8]){
    /* MK64_R66_OG_60HZ_NETPLAY: the Hub filters by BOTH wire version and build. */
    const char *queries[2]={"MKDIR2|LIST|10|BE100927","MKDIR2|LIST|11|BE610928"};
    int count=0;
    for(int rate=0;rate<2 && count<8;++rate){
        /* Distinct ephemeral sockets prevent delayed/reordered 30Hz replies
         * from being mistaken for 60Hz rooms (or vice versa). */
        SOCKET listing=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        if(listing==INVALID_SOCKET)continue;
        u_long nonblocking=1;
        sockaddr_in directory;
        if(ioctlsocket(listing,FIONBIO,&nonblocking)==SOCKET_ERROR ||
           !r48_dir_addr(directory)){closesocket(listing);continue;}
        DWORD begin=GetTickCount(),last=0;bool ended=false;
        while(GetTickCount()-begin<1800U&&!ended){
            DWORD now=GetTickCount();
            if(!last||now-last>=400U){
                sendto(listing,queries[rate],(int)strlen(queries[rate]),0,
                       (const sockaddr*)&directory,sizeof(directory));
                last=now;
            }
            for(;;){
                char b[400];sockaddr_in from;int flen=sizeof(from);
                int n=recvfrom(listing,b,sizeof(b)-1,0,(sockaddr*)&from,&flen);
                if(n<=0)break;
                b[n]=0;
                if(r59_world_packet(b))continue;
                if(r61_social_packet(b))continue;
                if(!strncmp(b,"MKDIR2|STATUS|",14)){r57_parse_status(b);continue;}
                if(!strncmp(b,"MKDIR2|LISTEND",14)){ended=true;break;}
                if(!strncmp(b,"MKDIR2|ROOM|",12)&&count<(rate?8:4)){
                    R48RoomOG r;memset(&r,0,sizeof(r));
                    if(sscanf(b,"MKDIR2|ROOM|%19[^|]|%31[^|]|%u|%u|%15[^|]|%u|%19[^|]|%11[^\r\n]",
                              r.id,r.name,&r.players,&r.max_players,r.platform,&r.locked,r.owner,r.region)==8){
                        r.rate60=(unsigned)rate;
                        rooms[count++]=r;
                    }
                }
            }
            Sleep(10);
        }
        closesocket(listing);
    }
    return count;
}
static bool r48_join_room(const R48RoomOG &r,char out[64]){R48AccountOG *me=r48_me();if(!me)return false;r484_load_prefs();char pass[20];strncpy(pass,gR484SavedPassword,sizeof(pass)-1);pass[sizeof(pass)-1]=0;if(r.locked&&!r48_keyboard("ROOM PASSWORD",pass,sizeof(pass),false,false))return false;char msg[320],resp[320];snprintf(msg,sizeof(msg)-1,"MKDIR2|JOIN_ROOM|%s|%s|%s|%s|%u|%s",r.id,me->id,me->secret,pass,gLocalCount,gR484Region);msg[sizeof(msg)-1]=0;if(!r48_wait_prefix(msg,"MKDIR2|JOIN|",resp,sizeof(resp))){if(strstr(resp,"BAD_PASSWORD")){for(;;){ui_screen("WRONG PASSWORD","ROOM PASSWORD WAS NOT ACCEPTED","","","","B BACK");if(ui_pressed()&CONT_B)break;Sleep(16);}}else if(strstr(resp,"ROOM_FULL")){for(;;){ui_screen("ROOM FULL","NOT ENOUGH RACER SLOTS FOR THIS CONSOLE","","","","B BACK");if(ui_pressed()&CONT_B)break;Sleep(16);}}return false;}char lid[20],ip[32];unsigned port=0;if(sscanf(resp,"MKDIR2|JOIN|%19[^|]|%31[^|]|%u",lid,ip,&port)!=3)return false;strncpy(gR48RoomId,lid,sizeof(gR48RoomId)-1);gR48RoomId[sizeof(gR48RoomId)-1]=0;snprintf(out,63,"%s:%u",ip,port);out[63]=0;gR48ChatSeq=0;gR48LastPoll=0;r50_chat_reset(r.name,me->name);
    /* Version was verified by the Hub's LIST query, not by the room name. */
    gR66Requested60=r.rate60!=0;
    mknet::set_rate60(gR66Requested60);if(pass[0]){strncpy(gR484SavedPassword,pass,sizeof(gR484SavedPassword)-1);gR484SavedPassword[sizeof(gR484SavedPassword)-1]=0;r484_save_prefs();}return true;}
static bool r48_browse_rooms(char out[64]){int selected=0;for(;;){r57_presence_tick(true);R48RoomOG rooms[8];memset(rooms,0,sizeof(rooms));int count=r48_fetch_rooms(rooms);if(count==0){for(;;){r57_presence_tick(true);ui_screen("PUBLIC ROOMS","NO UNRANKED ROOMS FOUND","","","","A REFRESH  B BACK");uint32_t p=ui_pressed();if(p&kUiRightStickButton){r59_world_chat_view();continue;}if(p&CONT_B)return false;if(p&CONT_A)break;Sleep(16);}continue;}if(selected>=count)selected=count-1;for(;;){r57_presence_tick(true);char a[96]="",b[96]="",c[96]="",d[96];int first=selected-1;if(first<0)first=0;if(first>count-3)first=count-3;if(first<0)first=0;char *line[3]={a,b,c};for(int j=0;j<3;++j){int i=first+j;if(i>=count)continue;snprintf(line[j],95,"%c %s [%s] %s %u/%u %s",i==selected?'>':' ',rooms[i].locked?"[LOCK]":"",rooms[i].region,rooms[i].name,rooms[i].players,rooms[i].max_players,rooms[i].platform);line[j][95]=0;}snprintf(d,sizeof(d)-1,"HOST: %s  REGION: %s",rooms[selected].owner,rooms[selected].region);ui_screen("PUBLIC ROOMS",a,b,c,d,"A JOIN  UP/DOWN  Y REFRESH  B BACK");uint32_t p=ui_pressed();if(p&kUiRightStickButton){r59_world_chat_view();continue;}if(p&CONT_B)return false;if(p&kUiYButton)break;if(p&CONT_DPAD_UP)selected=(selected+count-1)%count;if(p&CONT_DPAD_DOWN)selected=(selected+1)%count;if(p&CONT_A){if(r48_join_room(rooms[selected],out)){ui_consume();return true;}}Sleep(16);}}}
static bool begin_online_session(bool host, unsigned local_players, const char *address) {
    log_open();
    reset_state();
    /* Direct Play selects the rate before HELLO. Public rooms select after
     * the host setup or the version-filtered room browse. */
    mknet::set_rate60(gR66Requested60);
    log_line("R66_SESSION initial rate=%u Hz version=%u build=%08lX history=%u maxdelay=%u\n",
             mknet::nominal_fps(),mknet::wire_version(),
             (unsigned long)mknet::wire_build(),mknet::redundancy(),mknet::max_delay());
    r58_nat_reset();
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
    if (gR48PublicMode) {
        if (!r48_dir_open() || (!gR57SignedIn && !r48_choose_account())) { r48_dir_close(); xbox_netplay_shutdown(); return false; }
        gR57SignedIn=true;gR57PresenceActive=true;r484_load_prefs();r57_presence_tick(true);
        if (host && !r48_room_setup()) { r48_dir_close(); xbox_netplay_shutdown(); return false; }
        if (host) {
            mknet::set_rate60(gR66Requested60);
            log_line("R66_PUBLIC_HOST selected_rate=%u version=%u build=%08lX\n",
                     mknet::nominal_fps(),mknet::wire_version(),
                     (unsigned long)mknet::wire_build());
        }
    }

    bool ok = false;
    if (host) {
        if(gR48PublicMode){strcpy(gR42PublicIpText,"PUBLIC IP: AUTO NAT / RELAY");gR42PublicIpVisible=true;}else r42_discover_public_ip(false);
        log_line("MK64XNET: HOST lobby open local_count=%u UDP=%u\n", gLocalCount, kPort);
        if (gR48PublicMode && !r48_register_room()) { log_line("R48: public room registration failed\n"); r48_dir_close(); xbox_netplay_shutdown(); return false; }
        if(gR48PublicMode&&!r58_nat_host(gR48RoomId,gR48RoomToken)){log_line("R58.2_NAT: host transport init failed\n");r48_unregister_room();r48_dir_close();xbox_netplay_shutdown();return false;}
            ok = host_lobby_menu();
            if (gR48PublicMode) {
                if(ok && gActive)r62_begin_game();
                r48_unregister_room();
                if(!ok)r48_dir_close();
            }
    } else {
        char endpoint[80]; char public_ep[64]={0}; const char *join_addr=address;
        if (gR48PublicMode) {
            if(!r48_browse_rooms(public_ep)){r48_dir_close();xbox_netplay_shutdown();return false;}
            mknet::set_rate60(gR66Requested60);
            log_line("R66_PUBLIC_JOIN selected_rate=%u version=%u build=%08lX\n",
                     mknet::nominal_fps(),mknet::wire_version(),
                     (unsigned long)mknet::wire_build());
            R48AccountOG *natme=r48_me();if(!natme||!r58_nat_join(gR48RoomId,natme->id,natme->secret)){r48_leave_room();r48_dir_close();xbox_netplay_shutdown();return false;}
            join_addr=public_ep;
            /* R57: keep the independent UDP-6465 control socket alive until
             * START so lobby chat, member presence and regions remain live.
             * Gameplay itself still uses only direct UDP 6464. */
            log_line("MK64XNET: R57 public control socket retained through pre-game lobby\n");
        }
        if (join_addr && strchr(join_addr, ':')) snprintf(endpoint, sizeof(endpoint)-1, "%s", join_addr);
        else snprintf(endpoint, sizeof(endpoint)-1, "%s:%u", join_addr ? join_addr : "", kPort);
        endpoint[sizeof(endpoint)-1] = 0;
        if (!make_direct_target(endpoint, gJoinTarget)) {
            log_line("MK64XNET: invalid join address '%s'\n", address ? address : "");
            xbox_netplay_shutdown();
            return false;
        }
        gHostPeer = gJoinTarget;
        log_line("MK64XNET: JOIN target=%s local_count=%u\n", endpoint, gLocalCount);
            ok = join_lobby_menu();
            if (gR48PublicMode) {
                if(ok && gActive)r62_begin_game();
                else {r48_leave_room();r48_dir_close();}
            }
    }

    if (!ok) {
        if (gJoinRejected) log_line("MK64XNET: join rejected - not enough racer slots\n");
        xbox_netplay_shutdown();
        return false;
    }

    log_line("MK64XNET: SESSION ESTABLISHED role=%s local_slot=P%u local_count=%u players=%u crossplay=%u\n",
             gHosting ? "HOST" : "JOIN", gLocalSlot + 1, gLocalCount, gPlayerCount, gCrossplay ? 1U : 0U);
    log_line("MK64XNET: R31-COLLISION-CANON + R30 diagnostics A/B - MK4P v10/BE100927\n");
    crossplay_release_gate();
    return true;
}

static bool r57_public_account_gate(){XNADDR xna;if(!network_stack_start(xna))return false;if(!r48_dir_open())return false;r484_load_prefs();if(!gR57SignedIn){if(!r48_choose_account()){r48_dir_close();return false;}gR57SignedIn=true;r59_world_reset();}gR57PresenceActive=true;gR59WorldUiActive=true;gR57LastPresence=0;gR59WorldLastPoll=0;r57_presence_tick(true);return true;}

static int r57_public_match_menu(){
    if(!r57_public_account_gate())return 0;
    int row=0,person=0;bool right=false;r61_offset=0;r61_last_list=0;ui_consume();
    for(;;){
        if(!r48_dir_open()){gR57SignedIn=false;gR57PresenceActive=false;gR59WorldUiActive=false;return 0;}
        r57_presence_tick(true);r61_online_tick(false);r61_menu_draw(row,right,person);
        uint32_t q=ui_pressed();
        if(q&CONT_C){r61_profile_view(r48_me()->id,true);r61_last_list=0;continue;}
        if(q&kUiYButton){r61_profile_view(r48_me()->id);r61_last_list=0;continue;}
        if(q&CONT_X){r61_mailbox_view();r61_last_list=0;continue;}
        if(q&kUiRightStickButton){r59_world_chat_view();r61_last_list=0;continue;}
        if((q&CONT_DPAD_RIGHT)&&r61_count_people()){right=true;person=0;ui_consume();continue;}
        if((q&CONT_DPAD_LEFT)&&right){right=false;ui_consume();continue;}
        if(q&CONT_B){if(right){right=false;ui_consume();continue;}row=5;}
        if(right){
            int n=r61_count_people();
            if((q&CONT_DPAD_DOWN)&&n){if(person+1<n)++person;else if(r61_offset+5<r61_total){r61_offset+=5;person=0;r61_online_tick(true);}else {r61_offset=0;person=0;r61_online_tick(true);}}
            if((q&CONT_DPAD_UP)&&n){if(person>0)--person;else if(r61_offset>0){r61_offset-=5;person=4;r61_online_tick(true);}else person=n-1;}
            if((q&CONT_A)&&person<n&&r61_people[person].found){char id[20];r61_copy(id,sizeof(id),r61_people[person].id);r61_profile_view(id);r61_online_tick(true);ui_consume();}
        }else{
            if(q&CONT_DPAD_DOWN)row=(row+1)%6;
            if(q&CONT_DPAD_UP)row=(row+5)%6;
            if(q&(CONT_A|CONT_B)){
                if(row==5){gR57SignedIn=false;gR57PresenceActive=false;gR59WorldUiActive=false;gR57OnlineCount=-1;r48_dir_close();ui_consume();return 0;}
                if(row==2){if(r73_solo_begin())return 2;ui_consume();continue;}
                if(row==3){r57_region_menu();ui_consume();continue;}
                if(row==4){r70_top_view();ui_consume();continue;}
                bool host=row==0;unsigned local_players=1;if(!select_local_players(host,local_players)){ui_consume();continue;}
                gR48PublicMode=true;gR48PublicHost=host;bool ok=begin_online_session(host,local_players,"");gR48PublicMode=false;gR48PublicHost=false;
                if(ok)return 1;
                r48_dir_open();gR57PresenceActive=true;gR59WorldUiActive=true;gR57LastPresence=0;gR59WorldLastPoll=0;r61_last_list=0;ui_consume();
            }
        }
        Sleep(16);
    }
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
    if(!gAstraLoggingEnabled)return;
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
    if(!(gNetplayLoggingEnabled||gAstraLoggingEnabled||gComponentLoggingEnabled))return;
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
    if(gAstraLoggingEnabled) r25_astra_dump(mf);
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
    if(gComponentLoggingEnabled) mkdiag_write_component_snapshot(); /* MK64_CROSSPLAY_COMPONENT_DIAG_R15_CALL */
    }

    if(gAstraLoggingEnabled){ astra_r17_dump_net_history(); mk64_astra_diag_dump(); mk64_astra_rng_dump(); }
}



} /* anonymous namespace */

extern "C" int xbox_netplay_diagnostics_enabled(void) { return (gNetplayLoggingEnabled||gAstraLoggingEnabled||gComponentLoggingEnabled) ? 1 : 0; }

extern "C" void xbox_netplay_controllers(void *pads_, int count) {
    if (!gActive || count < 2 || !pads_) return;
    NetPadCompat *pads = (NetPadCompat *)pads_;
    r71_refresh_device();r62_game_tick();

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
            mesh_send_probes(now);
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
    static unsigned r55_wait_samples = 0, r55_wait_nonzero = 0, r55_wait_sum = 0, r55_wait_max = 0;

    while (!gFailed && !gStream.fault) {
        unsigned before_rx = gNetRxInput;
        pump_packets();
        DWORD now = GetTickCount();
        mesh_send_probes(now);
        if (gNetRxInput != before_rx) last_rx = now;

        if (!sent || now - sent >= (mknet::rate60() ? 12U : 15U) ||
            (gHosting && sent_complete != gStream.latest_complete)) {
            uint8_t packet[mknet::MAX_PACKET];
            int n = 0;
            if (gHosting) {
                int pc = peer_count();

                /* Same 2P/3P/4P early-input path as Xbox 360. */
                for (int i = 0; i < pc; ++i) {
                    n = gStream.client_packet(packet, gSession, gPeers[i].slot);
                    if (!n) break;
                    int sr = r58_send_game_packet(gPeers[i].addr,packet,n,true);
                    ++gNetTxInput;
                    if (sr == SOCKET_ERROR) ++gNetTxInputFail;
                }

                /* Authoritative redundant complete frame sets. */
                for (int i = 0; i < pc; ++i) {
                    n = gStream.frameset_packet(packet, gSession, gPeers[i].slot);
                    if (!n) break;
                    int sr = r58_send_game_packet(gPeers[i].addr,packet,n,true);
                    ++gNetTxInput;
                    if (sr == SOCKET_ERROR) ++gNetTxInputFail;
                }
            } else {
                n = gStream.client_packet(packet, gSession);
                if (!n) break;
                int sr = r58_send_game_packet(gHostPeer,packet,n,true);
                ++gNetTxInput;
                if (sr == SOCKET_ERROR) ++gNetTxInputFail;

                /* R56A: duplicate the guest's newest redundant input history
                 * directly to every announced guest. Host relay above remains
                 * enabled, so this is an optimization rather than a dependency. */
                if (gPlayerCount > 2) {
                    for (unsigned mi = 0; mi < mknet::MAX_PLAYERS - 1; ++mi) {
                        MeshPeerState &m = gMeshPeers[mi];
                        if (!m.used) continue;
                        n = gStream.client_packet(packet, gSession, m.slot);
                        if (!n) break;
                        sr = r58_send_game_packet(m.addr,packet,n,false);
                        ++gMeshDirectTx; ++gNetTxInput;
                        if (sr == SOCKET_ERROR) ++gNetTxInputFail;
                    }
                }
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
            if (mknet::rate60() && gNetplayLoggingEnabled) {
                static DWORD lastTick=0;
                static uint32_t lastFrame=0;
                DWORD stamp=GetTickCount();
                if (!lastTick){lastTick=stamp;lastFrame=gStream.frame;}
                else if (gStream.frame-lastFrame>=120U){
                    DWORD elapsed=stamp-lastTick;
                    log_line("R66_SIMRATE frame=%lu ticks=%lu elapsed_ms=%lu rate_x100=%lu\n",
                             (unsigned long)gStream.frame,
                             (unsigned long)(gStream.frame-lastFrame),
                             (unsigned long)elapsed,
                             (unsigned long)(elapsed?((gStream.frame-lastFrame)*100000UL/elapsed):0UL));
                    lastTick=stamp;lastFrame=gStream.frame;
                }
            }
            if (gNetplayLoggingEnabled) {
                unsigned wait_ms = (unsigned)(GetTickCount() - begin);
                ++r55_wait_samples; r55_wait_sum += wait_ms; if (wait_ms) ++r55_wait_nonzero;
                if (wait_ms > r55_wait_max) r55_wait_max = wait_ms;
                if (r55_wait_samples >= 120U) {
                    log_line("R56_WAIT: role=%s frame=%lu delay=%u samples=%u blocked=%u avg_ms=%u max_ms=%u rx=%u rej=%u txf=%u meshTx=%u meshRx=%u probeTx=%u probeRx=%u\n",
                             gHosting ? "HOST" : "JOIN", (unsigned long)gStream.frame, gChosenDelay,
                             r55_wait_samples, r55_wait_nonzero, r55_wait_sum / r55_wait_samples, r55_wait_max,
                             gNetRxInput, gNetRxInputReject, gNetTxInputFail, gMeshDirectTx, gMeshDirectRx, gMeshProbeTx, gMeshProbeRx);
                    r55_wait_samples = r55_wait_nonzero = r55_wait_sum = r55_wait_max = 0;
                }
            }
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

    if (gReturnToPremenu) {
        memset(pads, 0, sizeof(*pads) * count);
        return;
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
                  (gNetplayLoggingEnabled||gAstraLoggingEnabled||gComponentLoggingEnabled) ? "DIAGNOSTIC LOGGING ENABLED" : "DIAGNOSTICS WERE DISABLED",
                  "", "B EXIT TO DASHBOARD");
        if (ui_pressed() & CONT_B) XLaunchNewImage(NULL, NULL);
        Sleep(16);
    }
}


extern "C" void xbox_netplay_set_menu_sync(int enabled) { gMenuSync = enabled != 0; }
extern "C" int xbox_netplay_host_race_state(void) { return gAppliedHostRaceState; }

/* R45 controller settings UI. Menu navigation always uses physical defaults. */
static unsigned int r45_controls_prev = 0;
static unsigned int r45_controls_pressed(void) {
    unsigned int now = xbox_controls_down();
    unsigned int edge = now & ~r45_controls_prev;
    r45_controls_prev = now;
    return edge;
}
static void r45_controls_consume(void) { r45_controls_prev = xbox_controls_down(); }

static void r45_controls_menu(void) {
    int player = 0, row = 0;
    bool dirty = false, save_failed = false;
    xbox_controls_load();
    r45_controls_consume();

    for (;;) {
        char title[64], line[4][96], footer[96];
        int page = (row / 4) * 4;
        gR73ControlScrollRow=row;
        snprintf(title, sizeof(title)-1, "CONTROLLER %d - SETTINGS", player + 1);
        title[sizeof(title)-1] = 0;

        for (int i = 0; i < 4; ++i) {
            int item = page + i;
            if (item < 14)
                snprintf(line[i], sizeof(line[i])-1, "%c %s: %s",
                         item == row ? '>' : ' ', xbox_control_action(item),
                         xbox_control_binding(player, item));
            else if (item == 14)
                snprintf(line[i], sizeof(line[i])-1, "%c STEERING: %s STICK",
                         item == row ? '>' : ' ', xbox_control_stick(player, 0) ? "RIGHT" : "LEFT");
            else if (item == 15)
                snprintf(line[i], sizeof(line[i])-1, "%c DEAD ZONE: %d PERCENT",
                         item == row ? '>' : ' ', xbox_control_deadzone(player, 0));
            else if (item == 16)
                snprintf(line[i], sizeof(line[i])-1, "%c SENSITIVITY: %d PERCENT",
                         item == row ? '>' : ' ', xbox_control_sensitivity(player, 0));
            else if (item == 17)
                snprintf(line[i],sizeof(line[i])-1,"%c IN-GAME MUSIC: %s",
                         item == row ? '>' : ' ',xbox_music_enabled()?"ON":"OFF");
            else line[i][0] = 0;
            line[i][sizeof(line[i])-1] = 0;
        }

        snprintf(footer, sizeof(footer)-1, "%s | B BACK A SELECT X CLEAR Y DEFAULT  UP/DOWN SCROLL",
                 save_failed ? "SAVE FAILED" : "WHITE/BLACK PAD");
        footer[sizeof(footer)-1] = 0;
        ui_screen(title, line[0], line[1], line[2], line[3], footer);

        unsigned int p = r45_controls_pressed();
        if (p & (1U << XBOX_CTRL_SRC_B)) {
            if (dirty && !xbox_controls_save()) save_failed = true;
            if (!save_failed || !dirty) { gR73ControlScrollRow=-1;ui_consume(); return; }
        }
        if (p & (1U << XBOX_CTRL_SRC_DOWN)) row = (row + 1) % 18;
        if (p & (1U << XBOX_CTRL_SRC_UP)) row = (row + 17) % 18;
        if (p & (1U << XBOX_CTRL_SRC_WHITE)) player = (player + 3) % 4;
        if (p & (1U << XBOX_CTRL_SRC_BLACK)) player = (player + 1) % 4;

        if (p & (1U << XBOX_CTRL_SRC_Y)) {
            xbox_control_defaults(player); dirty = true; save_failed = false;
        }
        if (row < 14 && (p & (1U << XBOX_CTRL_SRC_X))) {
            xbox_control_bind(player, row, XBOX_CTRL_UNBOUND); dirty = true; save_failed = false;
        }
        if (row == 14 &&
            (p & ((1U << XBOX_CTRL_SRC_A) | (1U << XBOX_CTRL_SRC_LEFT) | (1U << XBOX_CTRL_SRC_RIGHT)))) {
            xbox_control_stick(player, 1); dirty = true; save_failed = false;
        }
        if (row == 15) {
            if (p & (1U << XBOX_CTRL_SRC_LEFT)) { xbox_control_deadzone(player, -1); dirty = true; save_failed = false; }
            if (p & (1U << XBOX_CTRL_SRC_RIGHT)) { xbox_control_deadzone(player, 1); dirty = true; save_failed = false; }
        }
        if (row == 16) {
            if (p & (1U << XBOX_CTRL_SRC_LEFT)) { xbox_control_sensitivity(player, -5); dirty = true; save_failed = false; }
            if (p & (1U << XBOX_CTRL_SRC_RIGHT)) { xbox_control_sensitivity(player, 5); dirty = true; save_failed = false; }
        }

        if (row==17 && (p & ((1U<<XBOX_CTRL_SRC_A)|(1U<<XBOX_CTRL_SRC_LEFT)|(1U<<XBOX_CTRL_SRC_RIGHT))))
            save_failed=!r73_music_set_enabled(!xbox_music_enabled());
        if (row < 14 && (p & (1U << XBOX_CTRL_SRC_A))) {
            bool released = false;
            for (;;) {
                ui_screen("PRESS A CONTROL", xbox_control_action(row),
                          "BUTTON / TRIGGER / STICK DIRECTION",
                          "BACK CANCELS", "DPAD STEERING: 8 DIRECTIONS",
                          "RELEASE CURRENT CONTROL FIRST");
                unsigned int down = xbox_controls_down();
                if (!down) released = true;
                if (released && down) {
                    if (!(down & (1U << XBOX_CTRL_SRC_BACK))) {
                        for (int b = 0; b < XBOX_CTRL_SOURCES; ++b) {
                            if (down & (1U << b)) {
                                xbox_control_bind(player, row, b);
                                dirty = true; save_failed = false;
                                break;
                            }
                        }
                    }
                    break;
                }
                Sleep(16);
            }
            r45_controls_consume();
        }
        Sleep(16);
    }
}

/* R75 PUBLIC BUILD: no diagnostic menu or logging controller shortcuts. */
static void r584_options_menu(void){
    int row=0;ui_consume();
    for(;;){
        char a[72],b[72],c[72],d[72];
        snprintf(a,71,"%c CONTROLS",row==0?'>':' ');
        snprintf(b,71,"%c IN-GAME MUSIC: %s",row==1?'>':' ',xbox_music_enabled()?"ON":"OFF");
        snprintf(c,71,"%c BACK",row==2?'>':' '); d[0]=0;
        ui_screen("OPTIONS / CONTROLS",a,b,c,d,"A SELECT/LEFT/RIGHT  B BACK");
        uint32_t p=ui_pressed();
        if(p&CONT_B){ui_consume();return;}
        if(p&CONT_DPAD_DOWN)row=(row+1)%3;
        if(p&CONT_DPAD_UP)row=(row+2)%3;
        if(p&(CONT_A|CONT_DPAD_LEFT|CONT_DPAD_RIGHT)){
            if(row==0){if(p&CONT_A){r45_controls_menu();ui_consume();}}
            else if(row==1){
                if(!r73_music_set_enabled(!xbox_music_enabled()))
                    ui_screen("MUSIC SAVE FAILED","SETTING ACTIVE FOR THIS SESSION",
                              "CHECK D: DRIVE WRITABILITY","","","B BACK");
                ui_consume();
            }else if(p&CONT_A){ui_consume();return;}
        }
        Sleep(16);
    }
}

extern "C" int xbox_netplay_boot_menu(void) {
    gReturnToPremenu=false;gR48PublicMode=false;gR48PublicHost=false;
    gR66Requested60=false;mknet::set_rate60(false);xbox_controls_load();r73_music_load();int selection=0;ui_consume();
    for(;;){char a[64],b[64],c[64],d[64],footer[96];snprintf(a,63,"%c OFFLINE",selection==0?'>':' ');snprintf(b,63,"%c DIRECT PLAY",selection==1?'>':' ');snprintf(c,63,"%c PUBLIC MATCH - UNRANKED",selection==2?'>':' ');snprintf(d,63,"%c OPTIONS / CONTROLS",selection==3?'>':' ');snprintf(footer,95,"A SELECT  UP/DOWN");ui_screen("MARIO KART 64 - ONLINE",a,b,c,d,footer);uint32_t p=ui_pressed();if(p&CONT_DPAD_DOWN)selection=(selection+1)%4;if(p&CONT_DPAD_UP)selection=(selection+3)%4;if(!(p&CONT_A)){Sleep(16);continue;}
        if(selection==0){ui_consume();return 0;}if(selection==3){r584_options_menu();ui_consume();continue;}
        if(selection==2){int public_result=r57_public_match_menu();if(public_result==2){ui_consume();return 0;}if(public_result==1){char role[64],slots[64],delay[64];snprintf(role,63,"PUBLIC %s - LOCAL SLOT P%u",gHosting?"HOST":"JOIN",gLocalSlot+1);snprintf(slots,63,"PLAYERS: %u   LOCAL RACERS: %u",gPlayerCount,gLocalCount);snprintf(delay,63,"NEGOTIATED INPUT DELAY: %u FRAMES",gChosenDelay);ui_screen("SESSION ESTABLISHED",role,slots,delay,"STARTING MARIO KART 64",gR66Requested60?"60FPS V11 / UDP 6464":"MK4P V10 / UDP 6464");Sleep(700);ui_consume();return 1;}continue;}
        bool host=false;int row=0;ui_consume();for(;;){char p0[72],p1[72],p2[72];snprintf(p0,71,"%c HOST 2-4 PLAYER GAME (OG ONLY)",row==0?'>':' ');snprintf(p1,71,"%c JOIN 2-4 PLAYER GAME",row==1?'>':' ');snprintf(p2,71,"%c BACK",row==2?'>':' ');ui_screen("DIRECT PLAY",p0,p1,p2,"MANUAL HOST / IP JOIN",
            gR66Requested60?"Y 60FPS ON | MENUS: MORE DELAY; RACE TARGET 60":"Y 60FPS OFF  A SELECT");uint32_t q=ui_pressed();
            if(q&kUiYButton){gR66Requested60=!gR66Requested60;ui_consume();continue;}
            if(q&CONT_B)row=2;if(q&CONT_DPAD_DOWN)row=(row+1)%3;if(q&CONT_DPAD_UP)row=(row+2)%3;if((q&CONT_A)||(q&CONT_B)){if(row==2){ui_consume();break;}host=(row==0);unsigned local_players=1;char host_ip[64]={0};if(!select_local_players(host,local_players)){ui_consume();break;}if(!host&&!edit_host_ip(host_ip)){ui_consume();break;}gR48PublicMode=false;gR48PublicHost=false;bool ok=begin_online_session(host,local_players,host_ip);if(ok){char role[64],slots[64],delay[64];snprintf(role,63,"%s - LOCAL SLOT P%u",host?"HOST":"JOIN",gLocalSlot+1);snprintf(slots,63,"PLAYERS: %u   LOCAL RACERS: %u",gPlayerCount,gLocalCount);snprintf(delay,63,"NEGOTIATED INPUT DELAY: %u FRAMES",gChosenDelay);ui_screen("SESSION ESTABLISHED",role,slots,delay,"STARTING MARIO KART 64",gR66Requested60?"60FPS V11 / UDP 6464":"MK4P V10 / UDP 6464");Sleep(700);ui_consume();return 1;}wait_return_to_menu(gR66VersionMismatch?"30/60 FPS BUILD MISMATCH":
                (gJoinRejected?"HOST LOBBY HAS NO FREE RACER SLOTS":"NO GAME WAS STARTED"));break;}Sleep(16);}
    }
}

extern "C" void xbox_netplay_r62_observe(int state,int mode,int course,int cup,int winner){r62_observe_result(state,mode,course,cup,winner);}
extern "C" void xbox_netplay_pump(void) {
    r73_solo_tick();
    if (!gActive || gSocket == INVALID_SOCKET) return;
    pump_packets();
}

extern "C" int xbox_netplay_return_requested(void) { return gReturnToPremenu ? 1 : 0; }
extern "C" void xbox_netplay_clear_return_request(void) { gReturnToPremenu = false; }

extern "C" void xbox_netplay_full_restart(void) {
    xbox_netplay_shutdown();
    /* The RXDK deploy tree stages the title as D:\\default.xbe. */
    XLaunchNewImage("D:\\default.xbe", NULL);
    /* If a dashboard/loader refuses the explicit path, fall back to a clean launch exit. */
    XLaunchNewImage(NULL, NULL);
}

extern "C" void xbox_netplay_shutdown(void) {
    r73_solo_exit();
    r62_end_game();
    if (gSocket != INVALID_SOCKET) {
        /* R59.1: GOODBYE is valid before START too. Three best-effort copies
         * make a menu exit survive an ordinary UDP loss without adding delay. */
        if (gHosting) {
            for (int i = 0; i < peer_count(); ++i)
                for (int retry = 0; retry < 3; ++retry) send_peer_message(i, mknet::GOODBYE, 0, 0);
        } else if (gHostSessionKnown) {
            for (int retry = 0; retry < 3; ++retry) send_packet_to(gHostPeer, mknet::GOODBYE, gSession, 0, 0);
        } else if (gJoinTarget.sin_family == AF_INET && gJoinTarget.sin_port != 0) {
            for (int retry = 0; retry < 3; ++retry) send_packet_to(gJoinTarget, mknet::GOODBYE, gNonce, 0, 0);
        }
        closesocket(gSocket);
        gSocket = INVALID_SOCKET;
    }
    r58_nat_reset();
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
extern "C" int xbox_netplay_60fps_session(void) {
    return (gActive && mknet::rate60()) ? 1 : 0;
}
extern "C" int xbox_netplay_player_count(void) { return gActive ? int(gPlayerCount) : 1; }
extern "C" int xbox_netplay_local_slot(void) { return gActive ? int(gLocalSlot) : 0; }
extern "C" int xbox_netplay_local_count(void) { return gActive ? int(gLocalCount) : 1; }
extern "C" unsigned int xbox_netplay_frame(void) { return gActive ? gStream.frame : 0U; }


/* MK64_CROSSPLAY_COMPONENT_DIAG_R15_NET implementation.
 * Writes one snapshot at the first crossplay fault. No packet/wire changes.
 */

static void mkdiag_component_append(const char *text) {
    if (!gComponentLoggingEnabled) return;
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
    if (!gComponentLoggingEnabled) return;
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
