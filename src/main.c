/* This decomp treats uintptr_t as a pointer type throughout, for N64 segmented
 * addressing. gcc (the Dreamcast toolchain) warns about the resulting implicit
 * conversions; clang 15+ makes them errors. RXDK's C build does not accept
 * extra compiler flags, so the diagnostic is restored to gcc's behaviour here
 * rather than by editing hundreds of call sites -- this code is known-good as
 * written on Dreamcast, and hand-inserting casts would risk changing it. */
#pragma clang diagnostic ignored "-Wint-conversion"
#pragma clang diagnostic ignored "-Wincompatible-pointer-types"

#ifndef GCC
#define D_800DC510_AS_U16
#endif
#include <kos.h>
#include "kos_undef.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ultra64.h>
#include <PR/os.h>
#include <PR/ucode.h>
#include <macros.h>
#include <decode.h>
#include <mk64.h>
#include <course.h>

#include "profiler.h"
#include "main.h"
#if defined(TARGET_XBOX)
#include "xbox_netplay.h"
#include "xbox_debug.h"
#else
#define MK64X_DEBUG_TOOLS 0
#endif
#include "racing/memory.h"
#include "menus.h"
#include <segments.h>
#include <common_structs.h>
#include <defines.h>
#include "buffers.h"
#include "camera.h"
#include "profiler.h"
#include "race_logic.h"
#include "skybox_and_splitscreen.h"
#include "render_objects.h"
#include "effects.h"
#include "code_80281780.h"
#include "audio/external.h"
#include "code_800029B0.h"
#include "code_80280000.h"
#include "podium_ceremony_actors.h"
#include "menu_items.h"
#include "code_80057C60.h"
#include "profiler.h"
#include "player_controller.h"
#include "render_player.h"
#include "render_courses.h"
#include "actors.h"
#include "objects.h"
#include "actor_types.h"
#include "bomb_kart.h"
#include "staff_ghosts.h"
#include <debug.h>
#include "crash_screen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *fnpre;
const void *__kos_romdisk;

/* R14: stay in strict host-authoritative staging through the exact frame where
 * the 360 announces 3; normal race Stream4 begins next frame. */
static int sXplayGuestRaceReleaseSeen = 0;

void func_80091B78(void);
void audio_init(void);
void create_debug_thread(void);
void start_debug_thread(void);
struct SPTask* create_next_audio_frame_task(void);

struct VblankHandler* gVblankHandler1 = NULL;
struct VblankHandler* gVblankHandler2 = NULL;

struct SPTask* gActiveSPTask = NULL;
struct SPTask* sCurrentAudioSPTask = NULL;
struct SPTask* sCurrentDisplaySPTask = NULL;
struct SPTask* sNextAudioSPTask = NULL;
struct SPTask* sNextDisplaySPTask = NULL;

struct Controller gControllers[NUM_PLAYERS];
struct Controller* gControllerOne = &gControllers[0];
struct Controller* gControllerTwo = &gControllers[1];
struct Controller* gControllerThree = &gControllers[2];
struct Controller* gControllerFour = &gControllers[3];
struct Controller* gControllerFive = &gControllers[4]; // All physical controllers combined.`
struct Controller* gControllerSix = &gControllers[5];
struct Controller* gControllerSeven = &gControllers[6];
struct Controller* gControllerEight = &gControllers[7];

Player gPlayers[NUM_PLAYERS];
Player* gPlayerOne = &gPlayers[0];
Player* gPlayerTwo = &gPlayers[1];
Player* gPlayerThree = &gPlayers[2];
Player* gPlayerFour = &gPlayers[3];
Player* gPlayerFive = &gPlayers[4];
Player* gPlayerSix = &gPlayers[5];
Player* gPlayerSeven = &gPlayers[6];
Player* gPlayerEight = &gPlayers[7];

#if defined(TARGET_XBOX)
/* MK64_CROSSPLAY_R22_DETERMINISM_PARITY
 * Crossplay is x86 vs PPC.  Keep local presentation from mutating the
 * deterministic simulation state, and make the 30 Hz network clock use an
 * explicitly-defined binary32 reciprocal instead of compiler-dependent /30.
 */
static Player sR22CrossplayPlayers[NUM_PLAYERS];
static Object sR22CrossplayObjects[OBJECT_LIST_SIZE];
static struct Actor sR22CrossplayActors[ACTOR_LIST_SIZE];
static BombKart sR22CrossplayBombs[NUM_BOMB_KARTS_MAX];
static u16 sR22CrossplayRandomSeed;
static s32 sR22CrossplayPresentationSaved;

static f32 r22_crossplay_frame_time(unsigned int frame) {
    union { u32 bits; f32 value; } inv30;
    if (!xbox_netplay_crossplay()) {
        return (f32) frame / 30.0f;
    }
    /* Exact IEEE-754 binary32 bits for the reciprocal used by the PPC build. */
    inv30.bits = 0x3D088889U;
    return (f32) frame * inv30.value;
}

static void r22_crossplay_present_begin(void) {
    if (!xbox_netplay_crossplay() || sR22CrossplayPresentationSaved) return;
    memcpy(sR22CrossplayPlayers, gPlayers, sizeof(sR22CrossplayPlayers));
    memcpy(sR22CrossplayObjects, gObjectList, sizeof(sR22CrossplayObjects));
    memcpy(sR22CrossplayActors, gActorList, sizeof(sR22CrossplayActors));
    memcpy(sR22CrossplayBombs, gBombKarts, sizeof(sR22CrossplayBombs));
    sR22CrossplayRandomSeed = gRandomSeed16;
    sR22CrossplayPresentationSaved = 1;
}

static void r22_crossplay_present_end(void) {
    if (!sR22CrossplayPresentationSaved) return;
    memcpy(gPlayers, sR22CrossplayPlayers, sizeof(sR22CrossplayPlayers));
    memcpy(gObjectList, sR22CrossplayObjects, sizeof(sR22CrossplayObjects));
    memcpy(gActorList, sR22CrossplayActors, sizeof(sR22CrossplayActors));
    memcpy(gBombKarts, sR22CrossplayBombs, sizeof(sR22CrossplayBombs));
    gRandomSeed16 = sR22CrossplayRandomSeed;
    sR22CrossplayPresentationSaved = 0;
}
#endif

Player* gPlayerOneCopy = &gPlayers[0];
Player* gPlayerTwoCopy = &gPlayers[1];
//UNUSED Player* gPlayerThreeCopy = &gPlayers[2];
//UNUSED Player* gPlayerFourCopy = &gPlayers[3];

//UNUSED s32 D_800FD850[3];
struct GfxPool* gGfxPool;

//UNUSED s32 gfxPool_padding; // is this necessary?
struct VblankHandler gGameVblankHandler;
struct VblankHandler sSoundVblankHandler;
OSMesgQueue gDmaMesgQueue, gGameVblankQueue, gGfxVblankQueue, unused_gMsgQueue, gIntrMesgQueue, gSPTaskMesgQueue;
OSMesgQueue sSoundMesgQueue;
OSMesg sSoundMesgBuf[1];
OSMesg gDmaMesgBuf[1], gGameMesgBuf;
OSMesg gGfxMesgBuf[1];
//UNUSED OSMesg D_8014F010, D_8014F014;
OSMesg gIntrMesgBuf[16], gSPTaskMesgBuf[16];
OSMesg gMainReceivedMesg;
OSIoMesg gDmaIoMesg;
OSMesgQueue gSIEventMesgQueue;
OSMesg gSIEventMesgBuf[3];

OSContStatus gControllerStatuses[4];
OSContPad gControllerPads[4];
u8 gControllerBits;
u8 gKeyboardBit;
// Contains a 32x32 grid of indices into gCollisionIndices containing indices into gCollisionMesh
CollisionGrid gCollisionGrid[1024];
u16 gNumActors;
u16 gMatrixObjectCount;
s32 gTickSpeed = 2;
f32 D_80150118;

u16 wasSoftReset;
u16 D_8015011E;

s32 D_80150120;
s32 gGotoMode;
UNUSED s32 D_80150128;
UNUSED s32 D_8015012C;
f32 gCameraZoom[4]; // look like to be the fov of each character
UNUSED s32 D_80150140;
UNUSED s32 D_80150144;
f32 gScreenAspect;
f32 gCourseFarPersp;
f32 gCourseNearPersp;
UNUSED f32 D_80150154;

struct D_80150158 gD_80150158[16];
uintptr_t gSegmentTable[16];
Gfx* gDisplayListHead;

struct SPTask* gGfxSPTask;
s32 D_801502A0;
s32 D_801502A4;
u16* gPhysicalFramebuffers[3];
uintptr_t gPhysicalZBuffer = &gZBuffer;
UNUSED u32 D_801502B8;
UNUSED u32 D_801502BC;
Mat4 D_801502C0;

s32 padding[2048];

u16 D_80152300[4];
u16 D_80152308;

OSMesg gPIMesgBuf[32];
OSMesgQueue gPIMesgQueue;

s32 gGamestate = 0xFFFF;
// D_800DC510 is externed as an s32 in other files. D_800DC514 is only used in main.c, likely a developer mistake.
u16 D_800DC510 = 0;
u16 D_800DC514 = 0;
u16 creditsRenderMode = 0; // Renders the whole track. Displays red if used in normal race mode.
u16 gDemoMode = DEMO_MODE_INACTIVE;
u16 gEnableDebugMode = ENABLE_DEBUG_MODE;
s32 gGamestateNext = 7; // = COURSE_DATA_MENU?;
UNUSED s32 D_800DC528 = 1;
s32 gActiveScreenMode = SCREEN_MODE_1P;
s32 gScreenModeSelection = SCREEN_MODE_1P;
UNUSED s32 D_800DC534 = 0;
s32 gPlayerCountSelection1 = 2;

s32 gModeSelection = GRAND_PRIX;
s32 D_800DC540 = 0;
s32 D_800DC544 = 0;
s32 gCCSelection = CC_50;
s32 gGlobalTimer = 0;
UNUSED s32 D_800DC550 = 0;
UNUSED s32 D_800DC554 = 0;
UNUSED s32 D_800DC558 = 0;
// Framebuffer rendering values (max 3)
u16 sRenderedFramebuffer = 0;
u16 sRenderingFramebuffer = 0;
UNUSED u16 D_800DC564 = 0;
s32 D_800DC568 = 0;
s32 D_800DC56C[8] = { 0 };
s16 sNumVBlanks = 0;
UNUSED s16 D_800DC590 = 0;
f32 gVBlankTimer = 0.0f;
f32 gCourseTimer = 0.0f;
int inited = 0;

#include "gfx/gfx_pc.h"
#if defined(TARGET_XBOX)
// The Xbox backends live in Platform/xbox; gfx_dc.h is the Dreamcast one.
extern struct GfxWindowManagerAPI gfx_xbox;
extern struct GfxRenderingAPI gfx_nv2a_api;
#else
#include "gfx/gfx_dc.h"
#endif

int ever_loaded_save_yet = 0;

extern struct GfxWindowManagerAPI gfx_glx;
#if defined(TARGET_XBOX)
static struct GfxWindowManagerAPI *wm_api = &gfx_xbox;
static struct GfxRenderingAPI *rendering_api = &gfx_nv2a_api;
#else
extern struct GfxRenderingAPI gfx_pvr_api;
static struct GfxWindowManagerAPI *wm_api = &gfx_dc;
static struct GfxRenderingAPI *rendering_api = &gfx_pvr_api;   // make GFX_BACKEND=pvr
#endif

extern void gfx_run(Gfx *commands);

extern void thread5_game_loop(void *arg);

#include "dcaudio/audio_api.h"
#include "dcaudio/audio_dc.h"
extern void create_next_audio_buffer(s16* samples, u32 num_samples);

#include "buffer_sizes.h"
extern s16 audio_buffer[AUDIOBUF_SIZE] __attribute__((aligned(64)));
static struct AudioAPI *audio_api = NULL;

static int frameno = 0;
static int even_frame;
// 30Hz gate for once-per-rendered-frame game code that assumes the N64's 30fps (HUD flash
// counters, course critters, kart particles, staff-ghost replay). Vblank ACCUMULATOR, not
// frame parity: true 30Hz at any frame rate (60fps -> every other frame; a 45fps dip still
// ~30Hz). At a capped 30fps every frame spans >=2 vblanks so this is 1 every frame = stock.
// Consumers: race_logic_loop (below) + code_80057C60.c (func_8005C728 / update_object).
extern volatile uint64_t vblticker;   // 60Hz hardware vblank count (vblfunc, below)
s16 gRun30hz = 1;

void game_loop_one_iteration(void) {
    even_frame = !((frameno++) & 1);
    {
        static uint64_t last_30hz_vbl = 0;
#if defined(TARGET_XBOX)
        /* R4 crossplay timing parity: the Xbox 360 netplay game loop is one
         * deterministic 30 Hz simulation step per consumed network frame.
         * Never let OG Xbox's independent 60 Hz hardware vblank decide whether
         * a simulation update runs while online. */
        if (xbox_netplay_active()) {
            gRun30hz = 1;
            last_30hz_vbl = vblticker;
        } else
#endif
        {
            gRun30hz = (vblticker - last_30hz_vbl) >= 2;
            if (gRun30hz) {
                last_30hz_vbl = vblticker;
            }
        }
    }

    gfx_start_frame();

#if defined(TARGET_XBOX)
    /* Match the current Xbox 360 online phase ordering. Offline keeps the
     * original ordering. Online advances this game-owned audio/state phase
     * only AFTER read_controllers() has consumed the common lockstep frame. */
    if (!xbox_netplay_active()) {
        func_800CB2C4();
    }
#else
    func_800CB2C4();
#endif

#if MK64X_CEREMONY_JUMP
    /* BOTH TRIGGERS + BACK jumps to the award ceremony from anywhere, so its
     * rendering can be checked without winning a four-race cup each time. The
     * ceremony reads the GP results for the podium, so seed plausible ones --
     * the player wins, everyone else in default order -- rather than whatever
     * happens to be in memory. gGamestate is forced to 255 (the game's own
     * "re-enter this state" idiom, see the CREDITS transition) so the jump
     * works even while the ceremony is already on screen: press again to
     * restart it. */
    {
        extern int xbox_jump_ceremony;
        extern s8 gCharacterIdByGPOverallRank[];
        extern u8 defaultCharacterIds[];
        if (xbox_jump_ceremony) {
            xbox_jump_ceremony = 0;
            gModeSelection = GRAND_PRIX;
            for (s32 i = 0; i < 8; i++) {
                gCharacterIdByGPOverallRank[i] = (s8) defaultCharacterIds[i];
            }
            gCharacterIdByGPOverallRank[0] = (s8) gCharacterSelections[0];
            gGamestateNext = ENDING;
            gGamestate = 255;
        }
    }
#endif

    /*
     * R10 crossplay scheduling.  Same-platform/offline keeps the historical
     * ordering.  OG<->360 crossplay deliberately commits the synchronized
     * controller frame BEFORE applying a pending MK64 state transition.
     * This prevents a faster platform from entering the next title/menu/fade
     * state while the peer is still executing the previous one.
     */
#if defined(TARGET_XBOX)
    if (!xbox_netplay_crossplay())
#endif
    {
        if (gGamestateNext != gGamestate) {
#if MK64X_DEBUG_TOOLS
            printf("TRANS gamestate %d -> %d\n", (int) gGamestate, (int) gGamestateNext);
#endif
            gGamestate = gGamestateNext;
            update_gamestate();
#if MK64X_DEBUG_TOOLS
            printf("TRANS gamestate %d ready\n", (int) gGamestate);
#endif
        }
    }

    config_gfx_pool();

#if defined(TARGET_XBOX)
    if (xbox_netplay_active()) {
        if (xbox_netplay_crossplay() && gGamestate == RACING) {
            int hrs = xbox_netplay_host_race_state();
            int localLifecycle = (D_800DC510 != 3 ||
                                  gIsGamePaused != 0 ||
                                  gIsInQuitToMenuTransition != 0 ||
                                  gGamestateNext != gGamestate);
            /* R32: R31 proved the guest could stay in low-latency race mode
             * after the host had entered RACE_HUMAN_FINISHED/results.  That
             * left HOST menuSync=1 and OG menuSync=0 and caused a false hash
             * fault even while gameplay state still matched.  Local lifecycle
             * state is deterministic, so use it to re-enter strict sync. */
            if (localLifecycle || (hrs >= 0 && hrs != 3)) {
                sXplayGuestRaceReleaseSeen = 0;
                xbox_netplay_set_menu_sync(1);
            } else {
                xbox_netplay_set_menu_sync(sXplayGuestRaceReleaseSeen ? 0 : 1);
            }
        } else {
            sXplayGuestRaceReleaseSeen = 0;
            xbox_netplay_set_menu_sync((gGamestate != RACING ||
                                       D_800DC510 != 3 ||
                                       gIsGamePaused != 0 ||
                                       gIsInQuitToMenuTransition != 0 ||
                                       gGamestateNext != gGamestate) ? 1 : 0);
        }
    }
#endif
    read_controllers();

#if defined(TARGET_XBOX)
    /*
     * MK64_R35_PAUSE_RELEASE_SYNC
     *
     * Arm release from strict lifecycle synchronization ONLY after this OG
     * frame has consumed an authoritative host snapshot that is genuinely back
     * in active race state 3 and all local lifecycle flags have cleared.
     *
     * R32 used hostRS >= 3. During pause the host race state is also >= 3, so
     * the guest repeatedly armed sXplayGuestRaceReleaseSeen while still paused.
     * On the first unpause frame that stale "seen" bit made OG disable strict
     * sync one controller frame earlier than the 360 host.
     */
    if (xbox_netplay_crossplay() && gGamestate == RACING &&
        xbox_netplay_host_race_state() == 3 &&
        D_800DC510 == 3 &&
        gIsGamePaused == 0 &&
        gIsInQuitToMenuTransition == 0 &&
        gGamestateNext == gGamestate &&
        !sXplayGuestRaceReleaseSeen) {
        sXplayGuestRaceReleaseSeen = 1;
        xbox_netplay_trace("R35_HOST_GO_RELEASE_ARM frame=%u localRS=%u hostRS=%d pause=%u quit=%u\n",
                           xbox_netplay_frame(), (unsigned)D_800DC510,
                           xbox_netplay_host_race_state(),
                           (unsigned)gIsGamePaused,
                           (unsigned)gIsInQuitToMenuTransition);
    }
#endif

#if defined(TARGET_XBOX)
    if (xbox_netplay_crossplay()) {
        /* Make the MK64-visible clocks canonical at the host-committed frame.
         * Rendering/vblank remains local presentation timing. */
        gGlobalTimer = (s32)xbox_netplay_frame();
        gVBlankTimer = r22_crossplay_frame_time(xbox_netplay_frame());
        sNumVBlanks = 2;
        gRun30hz = 1;

        if (gGamestateNext != gGamestate) {
            xbox_netplay_trace("R12_TRANS PRE frame=%u gs=%d next=%d rs=%u course=%d activeSM=%d selSM=%d pc=%d\n",
                               xbox_netplay_frame(), (int)gGamestate, (int)gGamestateNext, (unsigned)D_800DC510,
                               (int)gCurrentCourseId, (int)gActiveScreenMode, (int)gScreenModeSelection,
                               (int)gPlayerCountSelection1);
#if MK64X_DEBUG_TOOLS
            printf("XPLAY TRANS gamestate %d -> %d frame=%u\n",
                   (int)gGamestate, (int)gGamestateNext, xbox_netplay_frame());
#endif
            gGamestate = gGamestateNext;
            update_gamestate();
            xbox_netplay_trace("R12_TRANS POST frame=%u gs=%d next=%d rs=%u course=%d activeSM=%d p1=(%.2f,%.2f,%.2f)\n",
                               xbox_netplay_frame(), (int)gGamestate, (int)gGamestateNext, (unsigned)D_800DC510,
                               (int)gCurrentCourseId, (int)gActiveScreenMode,
                               gPlayers[0].pos[0], gPlayers[0].pos[1], gPlayers[0].pos[2]);
        }
    }

    /* Xbox 360 does this immediately after its netplay controller barrier.
     * This prevents the faster console from advancing game-owned audio/state
     * while the slower peer is still waiting on the previous network frame. */
    if (xbox_netplay_active()) {
        func_800CB2C4();
    }
#endif

#if defined(TARGET_XBOX)
    {
        static int r12LifecycleFrames = 0;
        int r12TraceLifecycle = xbox_netplay_active() && gGamestate == RACING && r12LifecycleFrames < 24;
        if (r12TraceLifecycle)
            xbox_netplay_trace("R12_LOOP %d PRE_HANDLER net=%u rs=%u activeSM=%d tick=%d pause=%u quit=%u CT=%.3f VT=%.3f\n",
                               r12LifecycleFrames, xbox_netplay_frame(), (unsigned)D_800DC510, (int)gActiveScreenMode,
                               (int)gTickSpeed, (unsigned)gIsGamePaused, (unsigned)gIsInQuitToMenuTransition,
                               gCourseTimer, gVBlankTimer);
        game_state_handler();
        if (r12TraceLifecycle) xbox_netplay_trace("R12_LOOP %d POST_HANDLER\n", r12LifecycleFrames);
        if (r12TraceLifecycle) xbox_netplay_trace("R12_LOOP %d PRE_END_DL\n", r12LifecycleFrames);
        end_master_display_list();
        if (r12TraceLifecycle) xbox_netplay_trace("R12_LOOP %d POST_END_DL PRE_VSYNC\n", r12LifecycleFrames);
        display_and_vsync();
        if (r12TraceLifecycle) xbox_netplay_trace("R12_LOOP %d POST_VSYNC PRE_GFX_END\n", r12LifecycleFrames);
        gfx_end_frame();
        if (r12TraceLifecycle) {
            xbox_netplay_trace("R12_LOOP %d POST_GFX_END\n", r12LifecycleFrames);
            r12LifecycleFrames++;
        }
    }
#else
    game_state_handler();
    end_master_display_list();
    display_and_vsync();
    gfx_end_frame();
#endif
}


#if defined(TARGET_XBOX)

#define XPLAY_STATE_BYTES 1440
#define XPLAY_MENU_ITEM_BASE 160
#define XPLAY_MENU_ITEM_STRIDE 40
static void xplay_state_put32(u8 *p,u32 v){p[0]=(u8)(v>>24);p[1]=(u8)(v>>16);p[2]=(u8)(v>>8);p[3]=(u8)v;}
static u32 xplay_state_get32(const u8 *p){return ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|p[3];}
static u32 xplay_state_f32_bits(f32 v){u32 b=0;memcpy(&b,&v,sizeof(b));return b;}
static f32 xplay_state_bits_f32(u32 b){f32 v=0.0f;memcpy(&v,&b,sizeof(v));return v;}

int xbox_crossplay_state_pack(unsigned char *out,int cap){
    extern u16 gRandomSeed16;
    int i,j,k=0;
    if(!out||cap<XPLAY_STATE_BYTES)return 0;
    memset(out,0,XPLAY_STATE_BYTES);
#define XP32(off,v) xplay_state_put32(out+(off),(u32)(v))
    XP32(0,gGlobalTimer);
    XP32(4,gGamestate);
    XP32(8,gGamestateNext);
    XP32(12,gMenuSelection);
    XP32(16,gFadeModeSelection);
    XP32(20,gMenuFadeType);
    XP32(24,gMenuTimingCounter);
    XP32(28,gMenuDelayTimer);
    XP32(32,gPlayerCountSelection1);
    XP32(36,gScreenModeSelection);
    XP32(40,gModeSelection);
    XP32(44,gCCSelection);
    XP32(48,gCurrentCourseId);
    XP32(52,gCupSelection);
    XP32(56,gCourseIndexInCup);
    XP32(60,gMainMenuSelection);
    XP32(64,gPlayerSelectMenuSelection);
    XP32(68,gSubMenuSelection);
    XP32(72,gPlayerCount);
    XP32(76,gScreenModeListIndex);
    XP32(80,gDemoMode);
    XP32(84,gDemoUseController);
    XP32(88,unref_8018EE0C);
    XP32(92,gDebugMenuSelection);
    XP32(96,D_800DC510);
    XP32(100,gRandomSeed16);
    XP32(104,xplay_state_f32_bits(gVBlankTimer));
    XP32(108,xplay_state_f32_bits(gCourseTimer));
#undef XP32
    for(i=0;i<4;++i)out[112+i]=(u8)gCharacterSelections[i];
    for(i=0;i<4;++i)out[116+i]=(u8)gCharacterGridSelections[i];
    for(i=0;i<4;++i)out[120+i]=(u8)gCharacterGridIsSelected[i];
    for(i=0;i<5;++i)out[124+i]=(u8)D_8018E7AC[i];
    for(i=0;i<4;++i)out[129+i]=(u8)gGameModeMenuColumn[i];
    for(i=0;i<4;++i)for(j=0;j<3;++j)out[133+k++]=(u8)gGameModeSubMenuColumn[i][j];
    out[145]=gControllerBits;
    /* R32 lifecycle extension. Bytes 146..159 were unused/reserved in the
     * existing 1440-byte wire image, so protocol size and MenuItem layout stay
     * unchanged. Keep the result/cup/quit transition itself host-authoritative,
     * not only the menu drawn around it. */
    {
        extern s32 gDemoTimer;
        u16 demo=(u16)(s16)gDemoTimer;
        out[146]=(u8)(demo>>8);
        out[147]=(u8)demo;
    }
    out[148]=(u8)gGotoMode;
    out[149]=(u8)gIsGamePaused;
    out[150]=(u8)gIsInQuitToMenuTransition;
    out[151]=(u8)D_80150120;
    xplay_state_put32(out+152,(u32)D_800DC544);
    /* 156..159 remain reserved for a future lifecycle field without moving
     * XPLAY_MENU_ITEM_BASE. */
    /* The stock menu engine stores most transition timers/selection animation
     * state in gMenuItems[].  Both ports use the same logical MenuItem fields.
     * Serialize them explicitly so the 360 host is authoritative without
     * transmitting native structs, pointers or CPU-endian memory. */
    for(i=0;i<MENU_ITEMS_MAX;++i){
        const MenuItem *m=&gMenuItems[i];
        int o=XPLAY_MENU_ITEM_BASE+i*XPLAY_MENU_ITEM_STRIDE;
        xplay_state_put32(out+o+0,(u32)m->type);
        xplay_state_put32(out+o+4,(u32)m->state);
        xplay_state_put32(out+o+8,(u32)m->subState);
        xplay_state_put32(out+o+12,(u32)m->column);
        xplay_state_put32(out+o+16,(u32)m->row);
        out[o+20]=(u8)m->priority;
        out[o+21]=(u8)m->visible;
        out[o+22]=(u8)(((u16)m->unused)>>8);
        out[o+23]=(u8)((u16)m->unused);
        xplay_state_put32(out+o+24,(u32)m->D_8018DEE0_index);
        xplay_state_put32(out+o+28,(u32)m->param1);
        xplay_state_put32(out+o+32,(u32)m->param2);
        xplay_state_put32(out+o+36,xplay_state_f32_bits(m->paramf));
    }
    return XPLAY_STATE_BYTES;
}

void xbox_crossplay_state_apply(const unsigned char *in,int len){
    extern u16 gRandomSeed16;
    int i,j,k=0;
    if(!in||len!=XPLAY_STATE_BYTES)return;
#define XG32(off) ((s32)xplay_state_get32(in+(off)))
    gGlobalTimer=XG32(0);
    /* R11: gGamestate is a LOCAL lifecycle state, not a replicated value.
     * The 360 host is authoritative for the TARGET state (gGamestateNext),
     * but the OG must execute update_gamestate() locally when crossing a
     * state boundary.  Overwriting gGamestate here used to make the later
     * (gGamestateNext != gGamestate) check false, skipping setup_race() and
     * softlocking on the tiny-box -> race transition. */
    gGamestateNext=XG32(8);
    gMenuSelection=XG32(12);
    gFadeModeSelection=XG32(16);
    gMenuFadeType=XG32(20);
    gMenuTimingCounter=XG32(24);
    gMenuDelayTimer=XG32(28);
    gPlayerCountSelection1=XG32(32);
    gScreenModeSelection=XG32(36);
    gModeSelection=XG32(40);
    gCCSelection=XG32(44);
    gCurrentCourseId=(s16)XG32(48);
    gCupSelection=(s8)XG32(52);
    gCourseIndexInCup=(s8)XG32(56);
    gMainMenuSelection=(s8)XG32(60);
    gPlayerSelectMenuSelection=(s8)XG32(64);
    gSubMenuSelection=(s8)XG32(68);
    gPlayerCount=(s8)XG32(72);
    gScreenModeListIndex=(s8)XG32(76);
    gDemoMode=(u16)XG32(80);
    gDemoUseController=(s8)XG32(84);
    unref_8018EE0C=(s8)XG32(88);
    gDebugMenuSelection=(s8)XG32(92);
    {
        /* R13: once the OG has locally entered RACING, D_800DC510 is its
         * race lifecycle state.  Do not overwrite it from the 360 menu-state
         * snapshot.  R12 proved func_8028FCBC advanced 0 -> 1, then the next
         * STATE_SYNC forced it back to 0 forever, restarting the tiny-box
         * start transition every frame.  Before RACING, the host value is
         * still accepted so menu/transition setup remains authoritative. */
        u16 hostRaceState=(u16)XG32(96);
        if(gGamestate!=RACING){
            D_800DC510=hostRaceState;
        }else{
            static unsigned r13RaceStateKeepLogs=0;
            if((D_800DC510!=hostRaceState) &&
               (r13RaceStateKeepLogs<12 || (r13RaceStateKeepLogs%120U)==0)){
                xbox_netplay_trace("R13_KEEP_LOCAL_RS frame=%u local=%u host=%u\n",
                                   xbox_netplay_frame(), (unsigned)D_800DC510,
                                   (unsigned)hostRaceState);
            }
            ++r13RaceStateKeepLogs;
        }
    }
    gRandomSeed16=(u16)XG32(100);
#undef XG32
    gVBlankTimer=xplay_state_bits_f32(xplay_state_get32(in+104));
    gCourseTimer=xplay_state_bits_f32(xplay_state_get32(in+108));
    for(i=0;i<4;++i)gCharacterSelections[i]=(s8)in[112+i];
    for(i=0;i<4;++i)gCharacterGridSelections[i]=(s8)in[116+i];
    for(i=0;i<4;++i)gCharacterGridIsSelected[i]=(s8)in[120+i];
    for(i=0;i<5;++i)D_8018E7AC[i]=(s8)in[124+i];
    for(i=0;i<4;++i)gGameModeMenuColumn[i]=(s8)in[129+i];
    for(i=0;i<4;++i)for(j=0;j<3;++j)gGameModeSubMenuColumn[i][j]=(s8)in[133+k++];
    gControllerBits=in[145];
    {
        extern s32 gDemoTimer;
        gDemoTimer=(s32)(s16)(((u16)in[146]<<8)|in[147]);
    }
    gGotoMode=(s32)(u8)in[148];
    gIsGamePaused=(s32)(u8)in[149];
    gIsInQuitToMenuTransition=(s32)(u8)in[150];
    D_80150120=(s32)(u8)in[151];
    D_800DC544=(s32)xplay_state_get32(in+152);
    for(i=0;i<MENU_ITEMS_MAX;++i){
        MenuItem *m=&gMenuItems[i];
        int o=XPLAY_MENU_ITEM_BASE+i*XPLAY_MENU_ITEM_STRIDE;
        m->type=(s32)xplay_state_get32(in+o+0);
        m->state=(s32)xplay_state_get32(in+o+4);
        m->subState=(s32)xplay_state_get32(in+o+8);
        m->column=(s32)xplay_state_get32(in+o+12);
        m->row=(s32)xplay_state_get32(in+o+16);
        m->priority=(s8)in[o+20];
        m->visible=(bool8)in[o+21];
        m->unused=(s16)(((u16)in[o+22]<<8)|in[o+23]);
        m->D_8018DEE0_index=(s32)xplay_state_get32(in+o+24);
        m->param1=(s32)xplay_state_get32(in+o+28);
        m->param2=(s32)xplay_state_get32(in+o+32);
        m->paramf=xplay_state_bits_f32(xplay_state_get32(in+o+36));
    }
}

static u32 cross_hash_u8(u32 h,u8 v){return (h^v)*16777619U;}
static u32 cross_hash_u16(u32 h,u16 v){h=cross_hash_u8(h,(u8)(v>>8));return cross_hash_u8(h,(u8)v);}
static u32 cross_hash_u32(u32 h,u32 v){h=cross_hash_u8(h,(u8)(v>>24));h=cross_hash_u8(h,(u8)(v>>16));h=cross_hash_u8(h,(u8)(v>>8));return cross_hash_u8(h,(u8)v);}
static u32 cross_hash_f32(u32 h,f32 v){u32 bits=0;memcpy(&bits,&v,sizeof(bits));return cross_hash_u32(h,bits);}
static u32 cross_phase_hash(u32 h){
    int i;
    h=cross_hash_u32(h,(u32)gGamestate);
    h=cross_hash_u32(h,(u32)gGamestateNext);
    if(gGamestate==RACING){
        h=cross_hash_u32(h,(u32)gModeSelection);
        h=cross_hash_u16(h,(u16)D_800DC510);
        h=cross_hash_u32(h,(u32)gPlayerCountSelection1);
        h=cross_hash_u32(h,(u32)gScreenModeSelection);
    }else{
        h=cross_hash_u32(h,(u32)gMenuSelection);
        h=cross_hash_u8(h,(u8)gMainMenuSelection);
        h=cross_hash_u8(h,(u8)gPlayerSelectMenuSelection);
        h=cross_hash_u8(h,(u8)gSubMenuSelection);
        h=cross_hash_u8(h,(u8)gPlayerCount);
        h=cross_hash_u32(h,(u32)gPlayerCountSelection1);
        h=cross_hash_u32(h,(u32)gScreenModeSelection);
        h=cross_hash_u32(h,(u32)gModeSelection);
    }
    h=cross_hash_u8(h,(u8)gCupSelection);
    h=cross_hash_u8(h,(u8)gCourseIndexInCup);
    /* R32: hash the state that selects/times the next lifecycle boundary. */
    {
        extern s32 gDemoTimer;
        h=cross_hash_u16(h,(u16)(s16)gDemoTimer);
    }
    h=cross_hash_u8(h,(u8)gGotoMode);
    h=cross_hash_u8(h,(u8)gIsGamePaused);
    h=cross_hash_u8(h,(u8)gIsInQuitToMenuTransition);
    h=cross_hash_u8(h,(u8)D_80150120);
    h=cross_hash_u32(h,(u32)D_800DC544);
    for(i=0;i<4;++i)h=cross_hash_u8(h,(u8)gCharacterSelections[i]);
    return h;
}


/* R18 cross-platform race hash.
 * PPC 360 and x86 OG can differ by a few IEEE-754 LSBs even when the actual
 * kart state is equivalent. 360<->360 keeps the original exact hash.
 * Cross-platform racing uses a canonical quantized hash; raw RNG/timers/floats
 * stay available in the R17 diagnostics. */
static s32 cross_r18_qpos(f32 v){return v>=0.0f?(s32)(v*8.0f+0.5f):(s32)(v*8.0f-0.5f);}
static s32 cross_r18_qvel(f32 v){return v>=0.0f?(s32)(v*64.0f+0.5f):(s32)(v*64.0f-0.5f);}
static u32 cross_r18_race_hash(u32 h,int players){
    int i,j;
    h=cross_hash_u32(h,(u32)gGlobalTimer);
    h=cross_phase_hash(h);
    for(i=0;i<players;++i){
        h=cross_hash_u16(h,gPlayers[i].type);
        h=cross_hash_u16(h,(u16)gPlayers[i].lapCount);
        h=cross_hash_u32(h,gPlayers[i].effects);
        for(j=0;j<3;++j)h=cross_hash_u32(h,(u32)cross_r18_qpos(gPlayers[i].pos[j]));
        for(j=0;j<3;++j)h=cross_hash_u32(h,(u32)cross_r18_qvel(gPlayers[i].velocity[j]));
    }
    return h;
}

/* MK64_ASTRA_TRACE_R17: RAM-only rolling deterministic state trace. */
#if defined(TARGET_XBOX)
#define ASTRA_FRAME_HISTORY 512U
extern unsigned int mk64_astra_rng_call_count(void);
typedef struct {u32 h0,h1,h2,pos[3],oldPos[3],vel[3],speed,currentSpeed,size,previousSpeed,effects,triggers;u16 type,rank,lap,surface,path,character,kartProps,alpha;} AstraPlayerFrameR17;
typedef struct {u32 valid,frame,globalTimer,gamestate,raceState,mode,screenMode,course,players,seed,rngCalls,courseTimerBits,vblankTimerBits,tickSpeed;AstraPlayerFrameR17 p[4];} AstraFrameR17;
static AstraFrameR17 sAstraFrameR17[ASTRA_FRAME_HISTORY];
static u32 astra_r17_u32(const void *p){u32 v;memcpy(&v,p,4);return v;} static u16 astra_r17_u16(const void *p){u16 v;memcpy(&v,p,2);return v;}
static u32 astra_r17_hash(const unsigned char *p,unsigned n){u32 h=2166136261U;while(n--){h^=*p++;h*=16777619U;}return h;}
static void astra_r17_player(AstraPlayerFrameR17 *o,const void *vp){const unsigned char *p=(const unsigned char*)vp;memset(o,0,sizeof(*o));o->h0=astra_r17_hash(p,0x100);o->h1=astra_r17_hash(p+0x100,0x100);o->h2=astra_r17_hash(p+0x200,0x58);o->type=astra_r17_u16(p);o->rank=astra_r17_u16(p+4);o->lap=astra_r17_u16(p+8);o->triggers=astra_r17_u32(p+0xC);o->pos[0]=astra_r17_u32(p+0x14);o->pos[1]=astra_r17_u32(p+0x18);o->pos[2]=astra_r17_u32(p+0x1C);o->oldPos[0]=astra_r17_u32(p+0x20);o->oldPos[1]=astra_r17_u32(p+0x24);o->oldPos[2]=astra_r17_u32(p+0x28);o->vel[0]=astra_r17_u32(p+0x34);o->vel[1]=astra_r17_u32(p+0x38);o->vel[2]=astra_r17_u32(p+0x3C);o->kartProps=astra_r17_u16(p+0x44);o->speed=astra_r17_u32(p+0x94);o->currentSpeed=astra_r17_u32(p+0x9C);o->effects=astra_r17_u32(p+0xBC);o->alpha=astra_r17_u16(p+0xC6);o->surface=astra_r17_u16(p+0xF8);o->path=astra_r17_u16(p+0x220);o->size=astra_r17_u32(p+0x224);o->previousSpeed=astra_r17_u32(p+0x22C);o->character=astra_r17_u16(p+0x254);}
static void mk64_astra_diag_capture(void){extern u16 gRandomSeed16;extern int xbox_netplay_diagnostics_enabled(void);u32 f;AstraFrameR17 *r;int i;if(!xbox_netplay_active()||!xbox_netplay_diagnostics_enabled())return;f=xbox_netplay_frame();r=&sAstraFrameR17[f&(ASTRA_FRAME_HISTORY-1U)];memset(r,0,sizeof(*r));r->valid=1;r->frame=f;r->globalTimer=(u32)gGlobalTimer;r->gamestate=(u32)gGamestate;r->raceState=(u32)(u16)D_800DC510;r->mode=(u32)gModeSelection;r->screenMode=(u32)gScreenModeSelection;r->course=(u32)gCurrentCourseId;r->players=(u32)xbox_netplay_player_count();r->seed=(u32)gRandomSeed16;r->rngCalls=mk64_astra_rng_call_count();r->courseTimerBits=astra_r17_u32(&gCourseTimer);r->vblankTimerBits=astra_r17_u32(&gVBlankTimer);r->tickSpeed=(u32)gTickSpeed;for(i=0;i<4;i++)astra_r17_player(&r->p[i],&gPlayers[i]);}
void mk64_astra_diag_dump(void){u32 cur,first,f;int i;if(!xbox_netplay_active())return;cur=xbox_netplay_frame();first=cur>32U?cur-32U:0;xbox_netplay_trace("ASTRA_FRAME_BEGIN SIDE=OG FIRST=%lu LAST=%lu\n",(unsigned long)first,(unsigned long)cur);for(f=first;f<=cur;f++){AstraFrameR17 *r=&sAstraFrameR17[f&(ASTRA_FRAME_HISTORY-1U)];if(!r->valid||r->frame!=f)continue;xbox_netplay_trace("ASTRA_FRAME SIDE=OG F=%lu GT=%lu GS=%lu RS=%lu MODE=%lu SM=%lu COURSE=%lu PC=%lu SEED=%04lX RNG=%lu CT=%08lX VT=%08lX TICK=%lu\n",(unsigned long)r->frame,(unsigned long)r->globalTimer,(unsigned long)r->gamestate,(unsigned long)r->raceState,(unsigned long)r->mode,(unsigned long)r->screenMode,(unsigned long)r->course,(unsigned long)r->players,(unsigned long)r->seed,(unsigned long)r->rngCalls,(unsigned long)r->courseTimerBits,(unsigned long)r->vblankTimerBits,(unsigned long)r->tickSpeed);for(i=0;i<2;i++){AstraPlayerFrameR17 *p=&r->p[i];xbox_netplay_trace("ASTRA_PLAYER SIDE=OG F=%lu P=%d H0=%08lX H1=%08lX H2=%08lX TYPE=%04X RANK=%04X LAP=%04X EFF=%08lX TRIG=%08lX POS=%08lX,%08lX,%08lX OLD=%08lX,%08lX,%08lX VEL=%08lX,%08lX,%08lX SPD=%08lX CUR=%08lX PREV=%08lX SIZE=%08lX SURF=%04X PATH=%04X CHAR=%04X KPROP=%04X ALPHA=%04X\n",(unsigned long)r->frame,i+1,(unsigned long)p->h0,(unsigned long)p->h1,(unsigned long)p->h2,p->type,p->rank,p->lap,(unsigned long)p->effects,(unsigned long)p->triggers,(unsigned long)p->pos[0],(unsigned long)p->pos[1],(unsigned long)p->pos[2],(unsigned long)p->oldPos[0],(unsigned long)p->oldPos[1],(unsigned long)p->oldPos[2],(unsigned long)p->vel[0],(unsigned long)p->vel[1],(unsigned long)p->vel[2],(unsigned long)p->speed,(unsigned long)p->currentSpeed,(unsigned long)p->previousSpeed,(unsigned long)p->size,p->surface,p->path,p->character,p->kartProps,p->alpha);}}xbox_netplay_trace("ASTRA_FRAME_END SIDE=OG\n");}
#endif

/* MK64_CROSSPLAY_R25_ASTRA_GOLD_TRACE
 * ASTRA handoff trace. Diagnostic only: no RNG calls, no file I/O during
 * gameplay, and no writes to simulation state. Title/demo races are excluded.
 */
#define R25_RING 2048U
#define R25_WORDS 88
typedef struct { u32 w[R25_WORDS]; } R25Rec;
static R25Rec sR25Ring[R25_RING];
static u32 sR25Total;
static u32 r25_bits(f32 v){u32 b=0;memcpy(&b,&v,sizeof(b));return b;}
static u32 r25_h32(u32 h,u32 v){h=(h^(u8)(v>>24))*16777619U;h=(h^(u8)(v>>16))*16777619U;h=(h^(u8)(v>>8))*16777619U;return(h^(u8)v)*16777619U;}
static u32 r25_kin_hash(Player *p,int index){
    u32 h=2166136261U;int j;
    for(j=0;j<3;++j)h=r25_h32(h,r25_bits(p->pos[j]));
    for(j=0;j<3;++j)h=r25_h32(h,r25_bits(p->velocity[j]));
    h=r25_h32(h,r25_bits(p->speed));h=r25_h32(h,r25_bits(p->currentSpeed));h=r25_h32(h,r25_bits(p->previousSpeed));
    for(j=0;j<3;++j)h=r25_h32(h,r25_bits(p->oldPos[j]));
    h=r25_h32(h,(u32)(u16)p->rotation[1]);h=r25_h32(h,(u32)(u16)p->slopeAccel);
    h=r25_h32(h,r25_bits(p->unk_098));h=r25_h32(h,r25_bits(p->unk_08C));h=r25_h32(h,r25_bits(p->boundingBoxSize));
    h=r25_h32(h,r25_bits(p->collision.surfaceDistance[2]));
    for(j=0;j<3;++j)h=r25_h32(h,r25_bits(p->collision.orientationVector[j]));
    return h;
}
static u32 r25_logic_hash(Player *p,int index){
    u32 h=2166136261U;(void)index;
    h=r25_h32(h,(u32)(u16)p->type);h=r25_h32(h,(u32)(u16)p->lapCount);h=r25_h32(h,p->effects);
    h=r25_h32(h,(u32)p->soundEffects);h=r25_h32(h,(u32)(u16)p->unk_044);h=r25_h32(h,(u32)(u16)p->currentRank);
    h=r25_h32(h,(u32)(u16)p->nearestPathPointId);h=r25_h32(h,(u32)(u16)p->currentItemCopy);return h;
}
static void r25_detail(u32 *w,Player *p,struct Controller *c,int index){
    w[0]=(u32)(u16)p->type;
    w[1]=((u32)(u16)p->lapCount<<16)|(u16)p->currentRank;
    w[2]=((u32)(u16)p->nearestPathPointId<<16)|(u16)p->currentItemCopy;
    w[3]=p->effects;w[4]=(u32)p->soundEffects;w[5]=(u32)(u16)p->unk_044;
    w[6]=r25_bits(p->pos[0]);w[7]=r25_bits(p->pos[1]);w[8]=r25_bits(p->pos[2]);
    w[9]=r25_bits(p->velocity[0]);w[10]=r25_bits(p->velocity[1]);w[11]=r25_bits(p->velocity[2]);
    w[12]=r25_bits(p->speed);w[13]=r25_bits(p->currentSpeed);w[14]=r25_bits(p->previousSpeed);
    w[15]=r25_bits(p->oldPos[0]);w[16]=r25_bits(p->oldPos[1]);w[17]=r25_bits(p->oldPos[2]);
    w[18]=((u32)(u16)p->rotation[1]<<16)|(u16)p->slopeAccel;
    w[19]=r25_bits(p->unk_098);w[20]=r25_bits(p->unk_08C);w[21]=r25_bits(p->boundingBoxSize);
    w[22]=r25_bits(p->collision.surfaceDistance[2]);w[23]=r25_bits(p->collision.orientationVector[0]);
    w[24]=r25_bits(p->collision.orientationVector[1]);w[25]=r25_bits(p->collision.orientationVector[2]);
    w[26]=r25_bits(p->unk_090);w[27]=0; /* reserved */
    w[28]=((u32)(u16)c->button<<16)|((u32)(u8)c->rawStickX<<8)|(u8)c->rawStickY;
    w[29]=((u32)(u16)c->buttonPressed<<16)|(u16)c->buttonDepressed;
}
/* R27 CPU trace: retain the first 1024 checkpoints plus the latest 2048.
 * This preserves the first post-GO split even if the watchdog fires later.
 * 672 KiB, no allocation, I/O, RNG calls or simulation writes during racing. */
#define R27_ANCHOR 1024U
#define R27_RECENT 2048U
#define R27_WORDS 56U
static u32 sR27Cpu[R27_ANCHOR + R27_RECENT][R27_WORDS];
static u32 sR27CpuTotal, sR27Tick, sR27LastFrame;
static int sR27RaceActive;
static void r27_trace_frame(u32 tick) {
    extern int xbox_netplay_diagnostics_enabled(void);
    int active = xbox_netplay_diagnostics_enabled() && xbox_netplay_crossplay() && gDemoMode == DEMO_MODE_INACTIVE &&
                 gGamestate == RACING && gModeSelection == GRAND_PRIX && D_800DC510 == 3;
    u32 frame = (u32)xbox_netplay_frame();
    sR27Tick = tick;
    if (active && (!sR27RaceActive || frame < sR27LastFrame)) sR27CpuTotal = 0;
    sR27RaceActive = active;
    sR27LastFrame = frame;
}
void mk64_r27_cpu_checkpoint(unsigned int stage, int playerId) {
    extern u16 gRandomSeed16;
    extern int xbox_netplay_diagnostics_enabled(void);
    u32 slot, *w; Player *p;
    extern void mk64_r27_cpu_ai(unsigned int *w, int playerId);
    if (!xbox_netplay_diagnostics_enabled() || !sR27RaceActive || playerId < 0 || playerId >= 8) return;
    slot = sR27CpuTotal < R27_ANCHOR ? sR27CpuTotal :
           R27_ANCHOR + ((sR27CpuTotal - R27_ANCHOR) & (R27_RECENT - 1U));
    w = sR27Cpu[slot]; p = &gPlayers[playerId];
    w[0] = (u32)xbox_netplay_frame(); w[1] = sR27Tick; w[2] = stage; w[3] = (u32)playerId;
    w[4] = gRandomSeed16; w[5] = r25_bits(gCourseTimer); w[6] = r25_bits(gVBlankTimer);
    w[7] = r25_kin_hash(p, playerId); w[8] = r25_logic_hash(p, playerId);
    r25_detail(w + 9, p, &gControllers[0], playerId); /* CPU has no controller input. */
    w[37] = 0; w[38] = 0;
    mk64_r27_cpu_ai(w, playerId);
    ++sR27CpuTotal;
}
unsigned int mk64_crossplay_r27_count(void) {
    return sR27CpuTotal < R27_ANCHOR + R27_RECENT ? sR27CpuTotal : R27_ANCHOR + R27_RECENT;
}
int mk64_crossplay_r27_get(unsigned int index, unsigned int *out, int outCount) {
    u32 slot, start;
    if (!out || outCount < R27_WORDS || index >= mk64_crossplay_r27_count()) return 0;
    if (index < R27_ANCHOR) slot = index;
    else {
        start = sR27CpuTotal > R27_ANCHOR + R27_RECENT ? sR27CpuTotal - R27_RECENT : R27_ANCHOR;
        slot = R27_ANCHOR + ((start + index - 2U * R27_ANCHOR) & (R27_RECENT - 1U));
    }
    memcpy(out, sR27Cpu[slot], sizeof(sR27Cpu[slot])); return R27_WORDS;
}

static void r25_capture(u32 phase,u32 tick){
    extern u16 gRandomSeed16;extern int xbox_netplay_diagnostics_enabled(void);R25Rec *r;u32 *w;int i;
    if(!xbox_netplay_diagnostics_enabled())return;
    r27_trace_frame(tick);
    if(!xbox_netplay_crossplay() || gDemoMode!=DEMO_MODE_INACTIVE || gGamestate!=RACING || gModeSelection!=GRAND_PRIX)return;
    r=&sR25Ring[sR25Total&(R25_RING-1U)];w=r->w;
    w[0]=(u32)xbox_netplay_frame();w[1]=phase;w[2]=tick;w[3]=(u32)gGlobalTimer;w[4]=(u32)D_800DC510;w[5]=(u32)gRandomSeed16;
    w[6]=r25_bits(gCourseTimer);w[7]=r25_bits(gVBlankTimer);w[8]=(u32)gDemoMode;w[9]=(u32)(u16)gTickSpeed;
    w[10]=(u32)gPlayerCountSelection1;w[11]=((u32)(u16)gActiveScreenMode<<16)|(u16)gModeSelection;
    for(i=0;i<8;++i)w[12+i]=r25_kin_hash(&gPlayers[i],i);
    for(i=0;i<8;++i)w[20+i]=r25_logic_hash(&gPlayers[i],i);
    r25_detail(&w[28],&gPlayers[0],&gControllers[0],0);r25_detail(&w[58],&gPlayers[1],&gControllers[1],1);
    ++sR25Total;
}
unsigned int xbox_crossplay_r25_count(void){return sR25Total<R25_RING?sR25Total:R25_RING;}
int xbox_crossplay_r25_get(unsigned int index,unsigned int *out,int outCount){
    u32 n,start,pos;if(!out||outCount<R25_WORDS)return 0;n=xbox_crossplay_r25_count();if(index>=n)return 0;
    start=sR25Total-n;pos=(start+index)&(R25_RING-1U);memcpy(out,sR25Ring[pos].w,sizeof(sR25Ring[pos].w));return R25_WORDS;
}
unsigned int xbox_netplay_state_hash(void){
    extern u16 gRandomSeed16;
    u32 h=2166136261U;int i,j,players=xbox_netplay_player_count();
    mk64_astra_diag_capture();
    if(players<2)players=2;if(players>4)players=4;
    if(xbox_netplay_crossplay() && gDemoMode==DEMO_MODE_INACTIVE && gGamestate==RACING && gModeSelection==GRAND_PRIX)players=8; /* R25: real GP only */
    if(xbox_netplay_crossplay() && (gDemoMode!=DEMO_MODE_INACTIVE || gGamestate!=RACING || D_800DC510!=3)){
        unsigned char state[XPLAY_STATE_BYTES];
        int n=xbox_crossplay_state_pack(state,sizeof(state));
        for(i=0;i<n;++i)h=cross_hash_u8(h,state[i]);
        return h;
    }
    if(xbox_netplay_crossplay() && gGamestate==RACING){
        /* R27: retain existing checks and add exact gameplay bits for real GP.
         * A one-ULP movement difference must reach the watchdog immediately. */
        h = cross_r18_race_hash(h,players);
        if (gModeSelection == GRAND_PRIX && D_800DC510 == 3 && gDemoMode == DEMO_MODE_INACTIVE) {
            h = r25_h32(h, (u32)gRandomSeed16);
            h = r25_h32(h, r25_bits(gCourseTimer));
            h = r25_h32(h, r25_bits(gVBlankTimer));
            for (i = 0; i < 8; ++i) {
                h = r25_h32(h, r25_kin_hash(&gPlayers[i], i));
                h = r25_h32(h, r25_logic_hash(&gPlayers[i], i));
            }
        }
        return h;
    }
    h=cross_hash_u32(h,(u32)gGlobalTimer);
    h=cross_phase_hash(h);
    if(gGamestate==RACING){
        h=cross_hash_f32(h,gCourseTimer);
        h=cross_hash_f32(h,gVBlankTimer);
        h=cross_hash_u16(h,gRandomSeed16);
        for(i=0;i<players;++i){
            h=cross_hash_u16(h,gPlayers[i].type);
            h=cross_hash_u16(h,(u16)gPlayers[i].lapCount);
            h=cross_hash_u32(h,gPlayers[i].effects);
            for(j=0;j<3;++j)h=cross_hash_f32(h,gPlayers[i].pos[j]);
            for(j=0;j<3;++j)h=cross_hash_f32(h,gPlayers[i].velocity[j]);
        }
    }
    return h;
}
#endif

static void send_display_list(struct SPTask *spTask) {
    gfx_run((Gfx *)spTask->task.t.data_ptr);
}

uint16_t __attribute__((aligned(32))) fb[3][4];

void create_thread(UNUSED OSThread* thread, UNUSED OSId id, void (*entry)(void*), void* arg, void* sp, OSPri pri) {
    kthread_attr_t main_attr;
    main_attr.create_detached = 1;
	main_attr.stack_size = 32768;
	main_attr.stack_ptr = sp;
	main_attr.prio = pri;
	main_attr.label = "thread";
    thd_create_ex(&main_attr, entry, arg);
}

// mio0encode
s32 func_80040174(UNUSED void*, UNUSED s32, UNUSED s32) {
	return -1;
}

s32 mio0encode(UNUSED s32, UNUSED s32, UNUSED s32) {
	return -1;
}

u8 *_audio_banksSegmentRomStart;
u8 *_audio_tablesSegmentRomStart;
u8 *_instrument_setsSegmentRomStart;
u8 *_sequencesSegmentRomStart;

//extern void init_all_sounds(void);
void _AudioInit(void);

__used void __stack_chk_fail(void) {
    unsigned int pr = (unsigned int)arch_get_ret_addr();
    printf("Stack smashed at PR=0x%08x\n", pr);
    printf("Successfully detected stack corruption!\n");
    exit(EXIT_SUCCESS);
}
#if 0
/* Callback function used to handle a breakpoint request. */
static bool on_break(const ubc_breakpoint_t *bp,
                     const irq_context_t *ctx,
                     void *ud) {
    /* Don't warn about unused bp */
    (void)bp;


    /* Print the location of the program counter when the breakpoint
       IRQ was signaled (minus 2 if we're breaking AFTER instruction
       execution!) */
    printf("\tBREAKPOINT HIT! [PC = %x]\n", (unsigned)CONTEXT_PC(*ctx) - 2);

    /* Userdata pointer used to hold a boolean used as the return value, which
       dictates whether a breakpoint persists or is removed after being
       handled. */
    return (bool)ud;
}
    #endif

void setup_audio_data(void) {
    char texfn[256];
    u8 *AUDIOBANKS_BUF = memalign(32,79936);
    if (!AUDIOBANKS_BUF) printf("can't malloc banks\n");
    /* Only the ALSeqFile header of audiotables is needed now (for gAlTbl and
       sample-address anchoring); the 2.4MB of VADPCM is dead -- the AICA driver
       plays from the transcoded adpcm_pool instead. */
    u8 *AUDIOTABLES_BUF = memalign(32,4096);
    if (!AUDIOTABLES_BUF) printf("can't malloc tables\n");
    u8 *INSTRUMENT_SETS_BUF = memalign(32,256);
    if (!INSTRUMENT_SETS_BUF) printf("can't malloc instruments\n");
    u8 *SEQUENCES_BUF = memalign(32,143728);
    if (!SEQUENCES_BUF) printf("can't malloc sequences\n");
    vid_border_color(64, 64, 64);
    // load sound data
    {
        sprintf(texfn, "%s/dc_data/audiobanks.bin", fnpre);
        FILE* file = fopen(texfn, "rb");
        if (!file) {
            perror("fopen");
            printf("\n");
            while(1){}
            exit(-1);
        }

        fseek(file, 0, SEEK_END);
        long filesize = ftell(file);
        //printf("audiobanks is %ld @ %08x\n", filesize, (uintptr_t)AUDIOBANKS_BUF);
        rewind(file);

        /* Guard the fixed AUDIOBANKS_BUF allocation: a file larger than the
           buffer would fread straight past it and corrupt the heap,
           surfacing as a crash in an unrelated free() much later.
           The audiotables load above already does this. */
        if (filesize > 79936) filesize = 79936;


        long toread = filesize;
        long didread = 0;

        while (didread < filesize) {
            long rv = fread(&AUDIOBANKS_BUF[didread], 1, toread - didread, file);

            if (rv == -1) {
                printf("FILE IS FUCKED\n");
                printf("\n");
                while(1){}
                exit(-1);
            }
            //printf("writing %08x size %ld\n", (uintptr_t)&AUDIOBANKS_BUF[didread], rv);

            toread -= rv;
            didread += rv;
        }

        fclose(file);
        _audio_banksSegmentRomStart = AUDIOBANKS_BUF;
    }

    vid_border_color(128, 128, 128);
    {
        sprintf(texfn, "%s/dc_data/audiotables.bin", fnpre);
        FILE* file = fopen(texfn, "rb");
        if (!file) {
            perror("fopen");
            printf("\n");
            while(1){}
            exit(-1);
        }

        fseek(file, 0, SEEK_END);
        long filesize = ftell(file);
        //printf("audiotables is %ld @ %08x\n", filesize, (uintptr_t)AUDIOTABLES_BUF);
        rewind(file);

        /* Only load the ALSeqFile header; the VADPCM sample data is unused. */
        if (filesize > 4096) filesize = 4096;

        long toread = filesize;
        long didread = 0;

        while (didread < filesize) {
            long rv = fread(&AUDIOTABLES_BUF[didread], 1, toread - didread, file);
            if (rv == -1) {
                printf("FILE IS FUCKED\n");
                printf("\n");
                while(1){}
                exit(-1);
            }
            //printf("writing %08x size %ld\n", (uintptr_t)&AUDIOTABLES_BUF[didread], rv);
            toread -= rv;
            didread += rv;
        }

        fclose(file);
        _audio_tablesSegmentRomStart = AUDIOTABLES_BUF;
    }

    vid_border_color(192, 192, 192);
    {
        sprintf(texfn, "%s/dc_data/instrument_sets.bin", fnpre);
        FILE* file = fopen(texfn, "rb");
        if (!file) {
            perror("fopen");
            printf("\n");
            while(1){}
            exit(-1);
        }

        fseek(file, 0, SEEK_END);
        long filesize = ftell(file);
        //printf("instrument_sets is %ld @ %08x\n", filesize, (uintptr_t)INSTRUMENT_SETS_BUF);
        rewind(file);

        /* Guard the fixed INSTRUMENT_SETS_BUF allocation: a file larger than the
           buffer would fread straight past it and corrupt the heap,
           surfacing as a crash in an unrelated free() much later.
           The audiotables load above already does this. */
        if (filesize > 256) filesize = 256;


        long toread = filesize;
        long didread = 0;

        while (didread < filesize) {
            long rv = fread(&INSTRUMENT_SETS_BUF[didread], 1, toread - didread, file);
            if (rv == -1) {
                printf("FILE IS FUCKED\n");
                printf("\n");
                while(1){}
                exit(-1);
            }
            //printf("writing %08x size %ld\n", (uintptr_t)&INSTRUMENT_SETS_BUF[didread], rv);
            toread -= rv;
            didread += rv;
        }

        fclose(file);
        _instrument_setsSegmentRomStart = INSTRUMENT_SETS_BUF;
    }
    vid_border_color(255, 255, 255);
    {
        sprintf(texfn, "%s/dc_data/sequences.bin", fnpre);
        FILE* file = fopen(texfn, "rb");
        if (!file) {
            perror("fopen");
            printf("\n");
            while(1){}
            exit(-1);
        }

        fseek(file, 0, SEEK_END);
        long filesize = ftell(file);
        //printf("sequences is %ld @ %08x\n", filesize, (uintptr_t)SEQUENCES_BUF);
        rewind(file);

        /* Guard the fixed SEQUENCES_BUF allocation: a file larger than the
           buffer would fread straight past it and corrupt the heap,
           surfacing as a crash in an unrelated free() much later.
           The audiotables load above already does this. */
        if (filesize > 143728) filesize = 143728;


        long toread = filesize;
        long didread = 0;

        while (didread < filesize) {
            long rv = fread(&SEQUENCES_BUF[didread], 1, toread - didread, file);
            if (rv == -1) {
                printf("FILE IS FUCKED\n");
                printf("\n");
                while(1){}
                exit(-1);
            }

            //printf("writing %08x size %ld\n", (uintptr_t)&SEQUENCES_BUF[didread], rv);

            toread -= rv;
            didread += rv;
        }

        fclose(file);
        _sequencesSegmentRomStart = SEQUENCES_BUF;
    }

    /* Load the offline-transcoded AICA-ADPCM sample pool resident, for the AICA
       hardware-mixing voice driver (src/audio/aica_synth.c). */
    {
        extern const unsigned char* gAicaAdpcmPoolBase;
        sprintf(texfn, "%s/dc_data/adpcm_pool.bin", fnpre);
        FILE* file = fopen(texfn, "rb");
        if (!file) {
            perror("fopen adpcm_pool.bin");
            printf("\n");
            while(1){}
            exit(-1);
        }
        fseek(file, 0, SEEK_END);
        long filesize = ftell(file);
        rewind(file);
        u8 *POOL_BUF = memalign(32, (filesize + 31) & ~31);
        if (!POOL_BUF) printf("can't malloc adpcm_pool\n");
        long toread = filesize, didread = 0;
        while (didread < filesize) {
            long rv = fread(&POOL_BUF[didread], 1, toread - didread, file);
            if (rv == -1) {
                printf("adpcm_pool read FAILED\n");
                while(1){}
                exit(-1);
            }
            toread -= rv;
            didread += rv;
        }
        fclose(file);
        gAicaAdpcmPoolBase = POOL_BUF;
    }

    _AudioInit();
    audio_init();
    sound_init();
    vid_border_color(0, 0, 0);
}

#include "dcprofiler.h"

s32 osAppNmiBuffer[16];
void isPrintfInit(void);
extern int must_inval_bg;
extern int stupid_fucking_faces_hack;

const uint32_t rainbow[] = {
    0xF800F800, // Red     (255,   0,   0)
    0xFD20FD20, // Orange  (255, 165,   0)
    0xFFE0FFE0, // Yellow  (255, 255,   0)
    0x07E007E0, // Green   (  0, 255,   0)
    0x001F001F, // Blue    (  0,   0, 255)
    0x72157215, // Indigo  ( 90,  30, 180)
    0x801F801F  // Violet  (148,   0, 211)
};

void rainbow_print(int x, int y, char *text) {
    int ci = 0;
    void *ptr = (void*)((uintptr_t)vram_s + ((y*640*2) + (x*2)));
    for (size_t i=0;i<strlen(text);i++) {
        if (ci == 18) ci = 0;
        bfont_draw_ex(ptr, 640, rainbow[ci%7], 0x00000000, 16, 1, text[i], 0, 0);
        if (text[i] != ' ') ci++;
        ptr = (void*)((uintptr_t)ptr + (12*2));
    }
    ptr = (void*)((uintptr_t)vram_s + (((y+1)*640*2) + ((x+1)*2)));
    ci = 0;
    for (size_t i=0;i<strlen(text);i++) {
        if (ci == 18) ci = 0;
        bfont_draw_ex(ptr, 640, rainbow[ci%7], 0x00000000, 16, 0, text[i], 0, 0);
        if (text[i] != ' ') ci++;
        ptr = (void*)((uintptr_t)ptr + (12*2));
    }
    ptr = (void*)((uintptr_t)vram_s + (((y-1)*640*2) + ((x-1)*2)));
    ci = 0;
    for (size_t i=0;i<strlen(text);i++) {
        if (ci == 18) ci = 0;
        bfont_draw_ex(ptr, 640, rainbow[ci%7], 0x00000000, 16, 0, text[i], 0, 0);
        if (text[i] != ' ') ci++;
        ptr = (void*)((uintptr_t)ptr + (12*2));
    }

}


#if defined(TARGET_XBOX)
// RXDK's startup (XapiTitleStartup) calls `void __cdecl main(void)`. The
// decomp's entry is the hosted int main(argc, argv) form, which that startup
// does not call correctly -- the title boots to a black screen. Rename the
// game's entry and give RXDK the signature it expects.
static int mk64_main(int argc, char **argv);

void __cdecl main(void) {
    // Peripheral enumeration is asynchronous; start it before anything
    // else so a pad is present by the time the game checks for one.
    xbox_input_init();
    mk64_main(0, NULL);
    for (;;) { }   // the game's main loop never returns
}

static int mk64_main(UNUSED int argc, UNUSED char **argv) {
#else
int main(UNUSED int argc, UNUSED char **argv) {
#endif
    thd_set_hz(300);

    must_inval_bg = 0;
    stupid_fucking_faces_hack = 0;

    wasSoftReset = (s16)0;

    gPhysicalFramebuffers[0] = fb[0];
    gPhysicalFramebuffers[1] = fb[1];
    gPhysicalFramebuffers[2] = fb[2];

    dbgio_enable();
//    dbglog_set_level(0);

//    thd_sleep(375);

#if defined(TARGET_XBOX)
    // The Dreamcast probes /pc (dev host) then /cd (the disc). On Xbox the
    // title's own media is D:, which is where deployPaths stages dc_data.
    // Forward slashes throughout: every other load here builds its path as
    // "%s/dc_data/...", so fnpre must work with that form too.
    FILE* fntest = fopen("D:/dc_data/common_data.bin", "rb");
    if (NULL == fntest) {
        printf("Cant load dc_data from D:/dc_data\n");
        while (1) { }
        exit(-1);
    }
    fnpre = "D:";
#else
    FILE* fntest = fopen("/pc/dc_data/common_data.bin", "rb");
    if (NULL == fntest) {
        fntest = fopen("/cd/dc_data/common_data.bin", "rb");
        if (NULL == fntest) {
            printf("Cant load from /pc or /cd");
            printf("\n");
            while(1){}
           exit(-1);
        } else {
            fnpre = "/cd";
        }
    } else {
        fnpre = "/pc";
    }
#endif

    fclose(fntest);
    thd_sleep(375);
//    dbgio_disable();
    setup_audio_data();

    //profiler_init("/pc/audiogmon.out");
    //profiler_start();

    rainbow_print(180+18, 260-24, "Welcome to Mario Kart :)");

//    thd_sleep(1500);
    thread5_game_loop(NULL);

    return 0;
}

void setup_mesg_queues(void) {
    return;
}

void start_sptask(UNUSED s32 taskType) {
    return;
}

/**
 * Initializes the Fast3D OSTask structure.
 * Loads F3DEX or F3DLX based on the number of players
 **/
void create_gfx_task_structure(void) {
    gGfxSPTask->msgqueue = NULL;
    // = &gGfxVblankQueue;
    gGfxSPTask->msg = (OSMesg) 2;
    gGfxSPTask->task.t.type = M_GFXTASK;
    gGfxSPTask->task.t.flags = OS_TASK_DP_WAIT;

    gGfxSPTask->task.t.ucode_boot = NULL;
    // = rspF3DBootStart;

    gGfxSPTask->task.t.ucode_boot_size = 0;
    // = ((u8*) rspF3DBootEnd - (u8*) rspF3DBootStart);

    // The split-screen multiplayer racing state uses F3DLX which has a simple subpixel calculation.
    // Singleplayer race mode and all other game states use F3DEX.
    // http://n64devkit.square7.ch/n64man/ucode/gspF3DEX.htm
    if (gGamestate != RACING || gPlayerCountSelection1 == 1) {
        gGfxSPTask->task.t.ucode = NULL;
        // = gspF3DEXTextStart;
        gGfxSPTask->task.t.ucode_data = NULL;
        // = gspF3DEXDataStart;
    } else {
        gGfxSPTask->task.t.ucode = NULL;
        // = gspF3DLXTextStart;
        gGfxSPTask->task.t.ucode_data = NULL;
        // = gspF3DLXDataStart;
    }
    gGfxSPTask->task.t.flags = 0;
    gGfxSPTask->task.t.flags = OS_TASK_DP_WAIT;
    gGfxSPTask->task.t.ucode_size = SP_UCODE_SIZE;
    gGfxSPTask->task.t.ucode_data_size = SP_UCODE_DATA_SIZE;
    gGfxSPTask->task.t.dram_stack = NULL;
    // = (u64*) &gGfxSPTaskStack;
    gGfxSPTask->task.t.dram_stack_size = SP_DRAM_STACK_SIZE8;
    gGfxSPTask->task.t.output_buff = NULL;
    // = (u64*) &gGfxSPTaskOutputBuffer;
    gGfxSPTask->task.t.output_buff_size = NULL;
    // = (u64*) ((u8*) gGfxSPTaskOutputBuffer + sizeof(gGfxSPTaskOutputBuffer));
    gGfxSPTask->task.t.data_ptr = (u64*) gGfxPool->gfxPool;
    gGfxSPTask->task.t.data_size = (gDisplayListHead - gGfxPool->gfxPool) * sizeof(Gfx);
    gGfxSPTask->task.t.yield_data_ptr = NULL;
    // = (u64*) &gGfxSPTaskYieldBuffer;
    gGfxSPTask->task.t.yield_data_size = OS_YIELD_DATA_SIZE;
}

int held;
int sd_x,sd_y;

void init_controllers(void) {
    gControllerBits = 0;
    gKeyboardBit = 0;

    maple_device_t *cont;
    int detected = 0;
    for (int i=0;i<4;i++) {
        cont = NULL;
        cont = maple_enum_type(i, MAPLE_FUNC_CONTROLLER);
        if (cont) {
            gControllerBits |= (1 << i);
            ++detected;
        }
    }

    if(detected < 4) {
        cont = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);
        if(cont) {
            gControllerBits |= gKeyboardBit = (1 << detected);
        }
    }

    if ((gControllerBits & 1) == 0) {
        sIsController1Unplugged = 1;
    } else {
        sIsController1Unplugged = 0;
    }

    // jnmartin84 - my new vars
    sd_x = sd_y = held = 0;
}

#if 1
//extern int player_index;

#define N64_CONT_A 0x8000
#define N64_CONT_B 0x4000
#define N64_CONT_G 0x2000
#define N64_CONT_START 0x1000
#define N64_CONT_UP 0x0800
#define N64_CONT_DOWN 0x0400
#define N64_CONT_LEFT 0x0200
#define N64_CONT_RIGHT 0x0100
#define N64_CONT_L 0x0020
#define N64_CONT_R 0x0010
#define N64_CONT_E 0x0008
#define N64_CONT_D 0x0004
#define N64_CONT_C 0x0002
#define N64_CONT_F 0x0001

/* Nintendo's official button names */
#undef A_BUTTON 
#undef B_BUTTON
#undef L_TRIG
#undef R_TRIG
#undef Z_TRIG
#undef START_BUTTON
#undef U_JPAD
#undef L_JPAD
#undef R_JPAD
#undef D_JPAD
#undef U_CBUTTONS
#undef L_CBUTTONS
#undef R_CBUTTONS
#undef D_CBUTTONS

#define A_BUTTON N64_CONT_A
#define B_BUTTON N64_CONT_B
#define L_TRIG N64_CONT_L
#define R_TRIG N64_CONT_R
#define Z_TRIG N64_CONT_G
#define START_BUTTON N64_CONT_START
#define U_JPAD N64_CONT_UP
#define L_JPAD N64_CONT_LEFT
#define R_JPAD N64_CONT_RIGHT
#define D_JPAD N64_CONT_DOWN
#define U_CBUTTONS N64_CONT_E
#define L_CBUTTONS N64_CONT_C
#define R_CBUTTONS N64_CONT_F
#define D_CBUTTONS N64_CONT_D
#endif

#undef CONT_C
#undef CONT_B
#undef CONT_A
#undef CONT_START
#undef CONT_DPAD_UP
#undef CONT_DPAD_DOWN
#undef CONT_DPAD_LEFT
#undef CONT_DPAD_RIGHT
#undef CONT_Z
#undef CONT_Y
#undef CONT_X
#undef CONT_D
#undef CONT_DPAD2_UP
#undef CONT_DPAD2_DOWN
#undef CONT_DPAD2_LEFT
#undef CONT_DPAD2_RIGHT


#define CONT_C              (1<<0)      /**< \brief C button Mask. */
#define CONT_B              (1<<1)      /**< \brief B button Mask. */
#define CONT_A              (1<<2)      /**< \brief A button Mask. */
#define CONT_START          (1<<3)      /**< \brief Start button Mask. */
#define CONT_DPAD_UP        (1<<4)      /**< \brief Main Dpad Up button Mask. */
#define CONT_DPAD_DOWN      (1<<5)      /**< \brief Main Dpad Down button Mask. */
#define CONT_DPAD_LEFT      (1<<6)      /**< \brief Main Dpad Left button Mask. */
#define CONT_DPAD_RIGHT     (1<<7)      /**< \brief Main Dpad right button Mask. */
#define CONT_Z              (1<<8)      /**< \brief Z button Mask. */
#define CONT_Y              (1<<9)      /**< \brief Y button Mask. */
#define CONT_X              (1<<10)     /**< \brief X button Mask. */
#define CONT_D              (1<<11)     /**< \brief D button Mask. */
#define CONT_DPAD2_UP       (1<<12)     /**< \brief Secondary Dpad Up button Mask. */
#define CONT_DPAD2_DOWN     (1<<13)     /**< \brief Secondary Dpad Down button Mask. */
#define CONT_DPAD2_LEFT     (1<<14)     /**< \brief Secondary Dpad Left button Mask. */
#define CONT_DPAD2_RIGHT    (1<<15)     /**< \brief Secondary Dpad Right button Mask. */
extern void __osPfsCloseAllFiles(void);
u16 ucheld;
u16 stick;

void update_keyboard(s32 index) {
	struct Controller* controller = &gControllers[index];
    maple_device_t *kbd;
    kbd_state_t *state;
    ucheld = 0;
    stick = 0;
    controller->rawStickX = 0;
    controller->rawStickY = 0;

    kbd = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);
    if(!kbd) return;

    state = maple_dev_status(kbd);

    if (strcmp("/pc", fnpre) == 0)
        if(state->key_states[KBD_KEY_ESCAPE].is_down) {
            __osPfsCloseAllFiles();
            exit(0);
        }

    if (state->key_states[KBD_KEY_SPACE].is_down)
        ucheld |= 0x8000; //A_BUTTON

    if (state->key_states[KBD_KEY_B].is_down ||
        state->key_states[KBD_KEY_C].is_down)
        ucheld |= 0x4000; //B_BUTTON

    if (state->key_states[KBD_KEY_ENTER].is_down)
       ucheld |= 0x1000; //START_BUTTON

    if (state->key_states[KBD_KEY_Q].is_down)
        ucheld |= 0x0020; //L_TRIG
    if (state->key_states[KBD_KEY_Z].is_down)
        ucheld |= 0x2000; //Z_TRIG
    if (state->key_states[KBD_KEY_E].is_down ||
        state->key_states[KBD_KEY_X].is_down)
        ucheld |= 0x0010; //R_TRIG

    if (state->key_states[KBD_KEY_UP].is_down) {
        controller->rawStickY = 60;
        ucheld |= U_JPAD;
    }
    if (state->key_states[KBD_KEY_DOWN].is_down) {
        controller->rawStickY = -60;
        ucheld |= D_JPAD;
    }
    if (state->key_states[KBD_KEY_LEFT].is_down) {
        controller->rawStickX = -60;
        ucheld |= L_JPAD;
    }
    if (state->key_states[KBD_KEY_RIGHT].is_down) {
        controller->rawStickX = 60;
        ucheld |= R_JPAD;
    }

    controller->buttonPressed = ucheld & (ucheld ^ controller->button);
    controller->buttonDepressed = controller->button & (ucheld ^ controller->button);
    controller->button = ucheld;

    if (state->key_states[KBD_KEY_A].is_down) {
        controller->rawStickX = -60;
        stick |= L_JPAD;
    }
    if (state->key_states[KBD_KEY_D].is_down) {
        controller->rawStickX = 60;
        stick |= R_JPAD;
    }
    if (state->key_states[KBD_KEY_S].is_down) {
        stick |= D_JPAD;
        controller->rawStickY = 60;
    }
    if (state->key_states[KBD_KEY_W].is_down) {
        stick |= U_JPAD;
        controller->rawStickY = 60;
    }

    controller->stickPressed = stick & (stick ^ controller->stickDirection);
    controller->stickDepressed = controller->stickDirection & (stick ^ controller->stickDirection);
    controller->stickDirection = stick;
}
#if 0
void update_controller(s32 index) {
	struct Controller* controller = &gControllers[index];
    maple_device_t *cont;
    cont_state_t *state;
    ucheld = 0;
    stick = 0;
    if (index > 3)
        return;
    cont = maple_enum_type(index, MAPLE_FUNC_CONTROLLER);
    if (!cont)
        return;
    state = maple_dev_status(cont);

    if (strcmp("/pc", fnpre) == 0) {
        if ((state->buttons & CONT_START) && 
        (state->buttons & CONT_A) &&
        (state->buttons & CONT_B) &&
        (state->buttons & CONT_X) &&
        (state->buttons & CONT_Y)) {
        //state->ltrig && state->rtrig) {
            //profiler_stop();
            //profiler_clean_up();
            // stop the AICA voice driver cleanly before teardown (the audio
            // thread is still running AicaSynth_Update on the sound RAM here)
            extern void AicaSynth_Shutdown(void);
            AicaSynth_Shutdown();
            // give vmu a chance to write and close
            __osPfsCloseAllFiles();
            exit(0);
        }
    }

    const char stickH =state->joyx;
    const char stickV = 0xff-((uint8_t)(state->joyy));
    controller->rawStickX = ((float)stickH/127)*80;
    controller->rawStickY = ((float)stickV/127)*80;

    if (state->buttons & CONT_A)
        ucheld |= 0x8000; //A_BUTTON
#if defined(BUTTON_SWAP_X)
    if (state->buttons & CONT_X)
        ucheld |= 0x0001; //C_RIGHT
    if (state->buttons & CONT_B)
        ucheld |= 0x4000; //B_BUTTON
#else
    if (state->buttons & CONT_X)
        ucheld |= 0x4000; //B_BUTTON
    if (state->buttons & CONT_B)
        ucheld |= 0x0001; //C_RIGHT
#endif

    if (state->ltrig) {
        if (gGamestate > 3) // DC L is N64 Z in-game
            ucheld |= 0x2000; //Z_TRIG
        else // DC L becomes N64 L in-menu
            ucheld |= 0x0020; //L_TRIG
    }
    if (state->buttons & CONT_START)
       ucheld |= 0x1000; //START_BUTTON

    if (state->buttons & CONT_DPAD_UP)
        ucheld |= 0x0800; //U_JPAD
    if (state->buttons & CONT_DPAD_DOWN)
        ucheld |= 0x0400; //D_JPAD
    if (state->buttons & CONT_DPAD_LEFT)
        ucheld |= 0x0200; //L_JPAD
    if (state->buttons & CONT_DPAD_RIGHT)
        ucheld |= 0x0100; //R_JPAD

    if (state->rtrig)
        ucheld |= 0x0010; //R_TRIG
    if (state->buttons & CONT_Y)
        ucheld |= 0x0008; //C_UP

    controller->buttonPressed = ucheld & (ucheld ^ controller->button);
    controller->buttonDepressed = controller->button & (ucheld ^ controller->button);
    controller->button = ucheld;

    stick = 0;
    if (controller->rawStickX < -50)
        stick |= L_JPAD;
    if (controller->rawStickX > 50)
        stick |= R_JPAD;
    if (controller->rawStickY < -50)
        stick |= D_JPAD;
    if (controller->rawStickY > 50)
        stick |= U_JPAD;

    controller->stickPressed = stick & (stick ^ controller->stickDirection);
    controller->stickDepressed = controller->stickDirection & (stick ^ controller->stickDirection);
    controller->stickDirection = stick;
}
#endif
#include <arch/arch.h>
extern void AicaSynth_Shutdown(void);

void update_controller(s32 index) {
    struct Controller* controller = &gControllers[index];
    maple_device_t *cont;
    cont_state_t *state;
    ucheld = 0;
    stick = 0;

    if (index > 3)
        return;
    if((1 << index) == gKeyboardBit) {
        update_keyboard(index);
        return;
    }
    cont = maple_enum_type(index, MAPLE_FUNC_CONTROLLER);
    if (!cont)
        return;
    state = maple_dev_status(cont);

    if (strcmp("/pc", fnpre) == 0) {
        if ((state->buttons & CONT_START) && 
        (state->buttons & CONT_A) &&
        (state->buttons & CONT_B) &&
        (state->buttons & CONT_X) &&
        (state->buttons & CONT_Y)) {
        //state->ltrig && state->rtrig) {
            //profiler_stop();
            //profiler_clean_up();
            // stop the AICA voice driver cleanly before teardown (the audio
            // thread is still running AicaSynth_Update on the sound RAM here)
            AicaSynth_Shutdown();
            // give vmu a chance to write and close
            __osPfsCloseAllFiles();
            exit(0);
        }
    }

    const char stickH =state->joyx;
    const char stickV = 0xff-((uint8_t)(state->joyy));
    controller->rawStickX = ((float)stickH/127)*80;
    controller->rawStickY = ((float)stickV/127)*80;

    if (state->buttons & CONT_A)
        ucheld |= 0x8000; //A_BUTTON
#if defined(BUTTON_SWAP_X)
    if (state->buttons & CONT_X)
        ucheld |= 0x0001; //C_RIGHT
    if (state->buttons & CONT_B)
        ucheld |= 0x4000; //B_BUTTON
#else
    if (state->buttons & CONT_X)
        ucheld |= 0x4000; //B_BUTTON
    if (state->buttons & CONT_B)
        ucheld |= 0x0001; //C_RIGHT
#endif

    if (state->ltrig) {
        if (gGamestate > 3) // DC L is N64 Z in-game
            ucheld |= 0x2000; //Z_TRIG
        else // DC L becomes N64 L in-menu
            ucheld |= 0x0020; //L_TRIG
    }
    if (state->buttons & CONT_START)
       ucheld |= 0x1000; //START_BUTTON

    if (state->buttons & CONT_DPAD_UP)
        ucheld |= 0x0800; //U_JPAD
    if (state->buttons & CONT_DPAD_DOWN)
        ucheld |= 0x0400; //D_JPAD
    if (state->buttons & CONT_DPAD_LEFT)
        ucheld |= 0x0200; //L_JPAD
    if (state->buttons & CONT_DPAD_RIGHT)
        ucheld |= 0x0100; //R_JPAD

    if (state->rtrig)
        ucheld |= 0x0010; //R_TRIG
    if (state->buttons & CONT_Y)
        ucheld |= 0x0008; //C_UP

    controller->buttonPressed = ucheld & (ucheld ^ controller->button);
    controller->buttonDepressed = controller->button & (ucheld ^ controller->button);
    controller->button = ucheld;

    stick = 0;
    if (controller->rawStickX < -50)
        stick |= L_JPAD;
    if (controller->rawStickX > 50)
        stick |= R_JPAD;
    if (controller->rawStickY < -50)
        stick |= D_JPAD;
    if (controller->rawStickY > 50)
        stick |= U_JPAD;

    controller->stickPressed = stick & (stick ^ controller->stickDirection);
    controller->stickDepressed = controller->stickDirection & (stick ^ controller->stickDirection);
    controller->stickDirection = stick;
}


void read_controllers(void) {
    OSMesg msg;
#if defined(TARGET_XBOX)
    u16 net_prev_button[4];
    u16 net_prev_stick[4];
    int net_i;
    for (net_i = 0; net_i < 4; ++net_i) {
        net_prev_button[net_i] = gControllers[net_i].button;
        net_prev_stick[net_i] = gControllers[net_i].stickDirection;
    }
#endif

    osRecvMesg(&gSIEventMesgQueue, &msg, OS_MESG_BLOCK);

    update_controller(0);
    update_controller(1);
    update_controller(2);
    update_controller(3);
#if defined(TARGET_XBOX)
    if (xbox_netplay_active()) {
        /* Match the Xbox 360 netplay clock exactly.  The normal platform
         * vblank cadence is presentation timing; online simulation time is
         * derived from the synchronized input frame so OG/360 cannot drift
         * merely because their video loops run at slightly different times. */
        gVBlankTimer = r22_crossplay_frame_time(xbox_netplay_frame());

        struct XboxNetPadCompat {
            u16 button;
            s8 stick_x;
            s8 stick_y;
            u8 err_no;
        } pads[4];

        memset(pads, 0, sizeof(pads));
        for (net_i = 0; net_i < 4; ++net_i) {
            int sx = (int)gControllers[net_i].rawStickX;
            int sy = (int)gControllers[net_i].rawStickY;
            if (sx < -128) sx = -128; if (sx > 127) sx = 127;
            if (sy < -128) sy = -128; if (sy > 127) sy = 127;
            /* update_controller() leaves the previous sample intact when a
             * physical controller disappears. Tell netplay explicitly whether
             * this physical port is present so a disconnected local pad cannot
             * keep sending stale held buttons/steering. Remote logical pads are
             * overwritten by xbox_netplay_controllers() after lockstep. */
            if (maple_enum_type(net_i, MAPLE_FUNC_CONTROLLER) != NULL) {
                pads[net_i].button = gControllers[net_i].button;
                pads[net_i].stick_x = (s8)sx;
                pads[net_i].stick_y = (s8)sy;
                pads[net_i].err_no = 0;
            } else {
                pads[net_i].button = 0;
                pads[net_i].stick_x = 0;
                pads[net_i].stick_y = 0;
                pads[net_i].err_no = 1;
            }
        }

        xbox_netplay_controllers(pads, 4);

        /* update_controller() sampled physical pads above. Replace that sample
         * with the synchronized frame and rebuild edge-triggered fields from
         * the previous synchronized frame, not from the temporary local one. */
        for (net_i = 0; net_i < 4; ++net_i) {
            struct Controller *controller = &gControllers[net_i];
            u16 now_button = pads[net_i].err_no ? 0 : pads[net_i].button;
            u16 now_stick = 0;

            controller->rawStickX = pads[net_i].err_no ? 0 : pads[net_i].stick_x;
            controller->rawStickY = pads[net_i].err_no ? 0 : pads[net_i].stick_y;
            controller->buttonPressed = now_button & (now_button ^ net_prev_button[net_i]);
            controller->buttonDepressed = net_prev_button[net_i] & (now_button ^ net_prev_button[net_i]);
            controller->button = now_button;

            if (controller->rawStickX < -50) now_stick |= L_JPAD;
            if (controller->rawStickX > 50) now_stick |= R_JPAD;
            if (controller->rawStickY < -50) now_stick |= D_JPAD;
            if (controller->rawStickY > 50) now_stick |= U_JPAD;
            controller->stickPressed = now_stick & (now_stick ^ net_prev_stick[net_i]);
            controller->stickDepressed = net_prev_stick[net_i] & (now_stick ^ net_prev_stick[net_i]);
            controller->stickDirection = now_stick;
        }
    } else {
        xbox_netplay_pump();
    }
#endif
    gControllerFive->button = (s16) (((gControllerOne->button | gControllerTwo->button) | gControllerThree->button) |
                                     gControllerFour->button);
    gControllerFive->buttonPressed =
        (s16) (((gControllerOne->buttonPressed | gControllerTwo->buttonPressed) | gControllerThree->buttonPressed) |
               gControllerFour->buttonPressed);
    gControllerFive->buttonDepressed = (s16) (((gControllerOne->buttonDepressed | gControllerTwo->buttonDepressed) |
                                               gControllerThree->buttonDepressed) |
                                              gControllerFour->buttonDepressed);
    gControllerFive->stickDirection =
        (s16) (((gControllerOne->stickDirection | gControllerTwo->stickDirection) | gControllerThree->stickDirection) |
               gControllerFour->stickDirection);
    gControllerFive->stickPressed =
        (s16) (((gControllerOne->stickPressed | gControllerTwo->stickPressed) | gControllerThree->stickPressed) |
               gControllerFour->stickPressed);
    gControllerFive->stickDepressed =
        (s16) (((gControllerOne->stickDepressed | gControllerTwo->stickDepressed) | gControllerThree->stickDepressed) |
               gControllerFour->stickDepressed);
}

void func_80000BEC(void) {
    gPhysicalZBuffer = VIRTUAL_TO_PHYSICAL(&gZBuffer);
}

void dispatch_audio_sptask(UNUSED struct SPTask* spTask) {
}

static void exec_display_list(UNUSED struct SPTask* spTask) {
	send_display_list(&gGfxPool->spTask);
}

/**
 * Set default RCP (Reality Co-Processor) settings.
 */
void init_rcp(void) {
    move_segment_table_to_dmem();
    init_rdp();
    set_viewport();
    select_framebuffer();
    init_z_buffer();
}

/**
 * End the master display list and initialize the graphics task structure for the next frame to be rendered.
 */
void end_master_display_list(void) {
    gDPFullSync(gDisplayListHead++);
    gSPEndDisplayList(gDisplayListHead++);
    create_gfx_task_structure();
}

// clear_frame_buffer from SM64, with a few edits
void clear_framebuffer(s32 color) {
    //gDPPipeSync(gDisplayListHead++);

    gDPSetRenderMode(gDisplayListHead++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
    gDPSetCycleType(gDisplayListHead++, G_CYC_FILL);

    gDPSetFillColor(gDisplayListHead++, color);
    //gDPFillRectangle(gDisplayListHead++, 0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1);

    //gDPPipeSync(gDisplayListHead++);

    gDPSetCycleType(gDisplayListHead++, G_CYC_1CYCLE);
}

void rendering_init(void) {
    gGfxPool = &gGfxPools[0];
    set_segment_base_addr(1, gGfxPool);
    gGfxSPTask = &gGfxPool->spTask;
    gDisplayListHead = gGfxPool->gfxPool;
    init_rcp();
    clear_framebuffer(0);
    end_master_display_list();
    // don't do this yet
    // exec_display_list(&gGfxPool->spTask);
    sRenderingFramebuffer++;
    gGlobalTimer++;
}

void config_gfx_pool(void) {
    gGfxPool = &gGfxPools[gGlobalTimer & 1];
    set_segment_base_addr(1, gGfxPool);
    gDisplayListHead = gGfxPool->gfxPool;
    gGfxSPTask = &gGfxPool->spTask;
}

/**
 * Send current master display list for rendering.
 * Tell the VI which colour framebuffer to display.
 * Yields to the VI framerate twice, locking the game at 30 FPS.
 * Selects the next framebuffer to be rendered and displayed.
 */
void display_and_vsync(void) {
    exec_display_list(&gGfxPool->spTask);

    if (++sRenderedFramebuffer == 3) {
        sRenderedFramebuffer = 0;
    }

    if (++sRenderingFramebuffer == 3) {
        sRenderingFramebuffer = 0;
    }

    gGlobalTimer++;
}

//void dma_copy(u8* dest, u8* romAddr, size_t size) {
//    n64_memcpy(segmented_to_virtual(dest), segmented_to_virtual(romAddr), size);
//}

#include "buffer_sizes.h"

extern u8 __attribute__((aligned(32))) COMMON_BUF[COMMON_BUF_SIZE];
extern u16 common_texture_minimap_kart_toad[];
extern u16 common_texture_minimap_kart_luigi[];
extern u16 common_texture_minimap_kart_peach[];
extern u16 common_texture_minimap_kart_donkey_kong[];
extern u16 common_texture_minimap_finish_line[];
extern u16 common_texture_minimap_kart_mario[];
extern u16 common_texture_minimap_kart_wario[];
extern u16 common_texture_minimap_kart_yoshi[];
extern u16 common_texture_minimap_kart_bowser[];
extern u16 common_texture_minimap_progress_dot[];
extern u16 common_tlut_bomb[];
extern u16 common_tlut_player_emblem[];
extern u16 common_tlut_finish_line_banner[];
extern u16 common_tlut_trees_import[];
extern u16 common_texture_hud_total_time[];
extern u16 common_texture_hud_time[];
extern u16 common_texture_hud_normal_digit[];
extern u16 common_texture_banana[];
extern u16 common_texture_flat_banana[];
extern u16 common_tlut_green_shell[];
extern u16 common_tlut_blue_shell[];
extern u16 common_texture_hud_123[];
extern u16 common_texture_hud_lap[];
extern u16 common_texture_hud_123[];
extern u16 common_texture_hud_lap_time[];
extern u16 common_texture_hud_lap_1_on_3[];
extern u16 common_texture_hud_lap_2_on_3[];
extern u16 common_texture_hud_lap_3_on_3[];
extern u16 common_tlut_portrait_mario[];
extern u16 common_tlut_portrait_luigi[];
extern u16 common_tlut_portrait_wario[];
extern u16 common_tlut_portrait_peach[];
extern u16 common_tlut_portrait_toad[];
extern u16 common_tlut_portrait_yoshi[];
extern u16 common_tlut_portrait_bowser[];
extern u16 common_tlut_portrait_donkey_kong[];
extern u16 common_tlut_portrait_bomb_kart_and_question_mark[];
extern u8 D_0D02AA58[];
extern u8 common_texture_portrait_mario[];
extern u8 common_texture_portrait_luigi[];
extern u8 common_texture_portrait_wario[];
extern u8 common_texture_portrait_peach[];
extern u8 common_texture_portrait_toad[];
extern u8 common_texture_portrait_yoshi[];
extern u8 common_texture_portrait_bowser[];
extern u8 common_texture_portrait_donkey_kong[];
extern u8 common_texture_item_box_question_mark[];
extern u16 common_texture_particle_leaf[];
extern u16 common_tlut_item_window_none[];

extern u16 common_tlut_item_window_banana[];

extern u16 common_tlut_item_window_banana_bunch[];

extern u16 common_tlut_item_window_mushroom[];

extern u16 common_tlut_item_window_double_mushroom[];

extern u16 common_tlut_item_window_triple_mushroom[];

extern u16 common_tlut_item_window_super_mushroom[];

extern u16 common_tlut_item_window_blue_shell[];

extern u16 common_tlut_item_window_boo[];

extern u16 common_tlut_item_window_green_shell[];
extern u16 common_tlut_item_window_triple_green_shell[];
extern u16 common_tlut_item_window_red_shell[];

extern u16 common_tlut_item_window_triple_red_shell[];

extern u16 common_tlut_item_window_star[];
extern u16 common_tlut_item_window_thunder_bolt[];

extern u16 common_tlut_item_window_fake_item_box[];
extern u16 common_tlut_lakitu_countdown[][256];
extern u16 common_tlut_lakitu_checkered_flag[];
extern u16 common_tlut_lakitu_second_lap[];
extern u16 common_tlut_lakitu_reverse[];
extern u16 common_tlut_lakitu_final_lap[];
extern u16 common_tlut_lakitu_fishing[];
extern u16 l_common_texture_minimap_kart_mario[][64];
int sgm_run = 0;
extern void load_ceremony_data(void);

/**
 * Setup main segments and framebuffers.
 */
static char texfn[256];

void setup_game_memory(void) {
#if defined(TARGET_XBOX)
    /* On Dreamcast this makes a segment-0 address resolve to RAM base +
       offset. Xbox has no fixed RAM base, so segment 0 is identity --
       which is what segmented_to_virtual now does for segments 0 and 1. */
    set_segment_base_addr(0, (void *) 0);
#else
    set_segment_base_addr(0, 0x8C010000);
#endif
    func_80000BEC();

    memset(COMMON_BUF, 0, sizeof(COMMON_BUF));
    set_segment_base_addr(2, SEG_DATA_START);

    sprintf(texfn, "%s/dc_data/common_data.bin", fnpre);

    FILE* file = NULL;
    file = fopen(texfn, "rb");
    if (!file) {
        perror("fopen");
        exit(-1);
    }

    fseek(file, 0, SEEK_END);
    long filesize = ftell(file);
    //printf("common data is %ld\n", filesize);
    rewind(file);

    long toread = filesize;
    long didread = 0;

    while (didread < filesize) {
        long rv = fread(&COMMON_BUF[didread], 1, toread - didread, file);
        if (rv == -1) {
            printf("FILE IS FUCKED\n");
            exit(-1);
        }
        toread -= rv;
        didread += rv;
    }

    fclose(file);
    file = NULL;

    set_segment_base_addr(0xD, (void*) COMMON_BUF);
    // Common course data does not get reloaded when the race state resets.
    if (!sgm_run) {
    extern u16 l_d_course_rainbow_road_static_tluts[][256];

    u16* tlut_ptr = (u16*) segmented_to_virtual(l_d_course_rainbow_road_static_tluts[0]);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(l_d_course_rainbow_road_static_tluts[1]);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }
        tlut_ptr = (u16*) segmented_to_virtual(l_d_course_rainbow_road_static_tluts[2]);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }
        tlut_ptr = (u16*) segmented_to_virtual(l_d_course_rainbow_road_static_tluts[3]);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }
        tlut_ptr = (u16*) segmented_to_virtual(l_d_course_rainbow_road_static_tluts[4]);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }
        tlut_ptr = (u16*) segmented_to_virtual(l_d_course_rainbow_road_static_tluts[5]);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }
        tlut_ptr = (u16*) segmented_to_virtual(l_d_course_rainbow_road_static_tluts[6]);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        // a whole bunch of stuff I have to endian-swap
        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_player_emblem);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_texture_particle_leaf);
        for (int i = 0; i < 32*16; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_bomb);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        /* Bomb-kart wheel texture (16x16 rgba16). Its swap was dropped when
         * the common_data SEGMENT copy was made ROM-true -- but that only
         * covers consumers reading through a 0x0Dxxxxxx address. These two
         * (func_8005669C and func_80056E24 in render_objects.c) pass the
         * COMPILED ARRAY by symbol, whose x86 little-endian image is the
         * ROM's bytes reversed: 0x318C (grey, alpha 0) arrived as 0x8C31 --
         * rgba5551 (17,16,24) with alpha set, the blue/purple checkered
         * wheels on the bomb karts in VS. */
        tlut_ptr = (u16*) segmented_to_virtual(D_0D02AA58);
        for (int i = 0; i < 16 * 16; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_lakitu_countdown);
        for (int i = 0; i < 768; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_lakitu_checkered_flag);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_lakitu_final_lap);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_lakitu_fishing);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_lakitu_reverse);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_lakitu_second_lap);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_texture_hud_lap_time);
        for (int i = 0; i < 32 * 16; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_texture_hud_total_time);
        for (int i = 0; i < 32 * 16; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_texture_hud_time);
        for (int i = 0; i < 512; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_texture_hud_normal_digit);
        for (int i = 0; i < 1664; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_texture_hud_123);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_texture_hud_lap);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_texture_hud_lap_1_on_3);
        for (int i = 0; i < 512; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_texture_hud_lap_2_on_3);
        for (int i = 0; i < 512; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_texture_hud_lap_3_on_3);
        for (int i = 0; i < 512; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        // 32*64
        tlut_ptr = (u16*) segmented_to_virtual(common_texture_item_box_question_mark);
        for (int i = 0; i < 32 * 64; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_finish_line_banner);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_trees_import);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_green_shell);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_blue_shell);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_portrait_mario);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_portrait_bomb_kart_and_question_mark);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_portrait_luigi);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_portrait_wario);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_portrait_yoshi);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_portrait_peach);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_portrait_toad);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_portrait_bowser);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_portrait_donkey_kong);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_item_window_none);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_item_window_banana);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_item_window_banana_bunch);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_item_window_mushroom);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_item_window_double_mushroom);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_item_window_triple_mushroom);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_item_window_super_mushroom);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_item_window_blue_shell);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_item_window_boo);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_item_window_green_shell);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_item_window_triple_green_shell);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_item_window_red_shell);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_item_window_triple_red_shell);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_item_window_star);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_item_window_thunder_bolt);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_tlut_item_window_fake_item_box);
        for (int i = 0; i < 256; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_texture_banana);
        for (int i = 0; i < 32 * 32; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = (u16*) segmented_to_virtual(common_texture_flat_banana);
        for (int i = 0; i < 64 * 32; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = segmented_to_virtual(l_common_texture_minimap_kart_mario[0]);
        for (int i = 0; i < 8 * 8; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = segmented_to_virtual(l_common_texture_minimap_kart_mario[1]);
        for (int i = 0; i < 8 * 8; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = segmented_to_virtual(l_common_texture_minimap_kart_mario[2]);
        for (int i = 0; i < 8 * 8; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = segmented_to_virtual(l_common_texture_minimap_kart_mario[3]);
        for (int i = 0; i < 8 * 8; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = segmented_to_virtual(common_texture_minimap_finish_line);
        for (int i = 0; i < 8 * 8; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = segmented_to_virtual(l_common_texture_minimap_kart_mario[4]);
        for (int i = 0; i < 8 * 8; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = segmented_to_virtual(l_common_texture_minimap_kart_mario[5]);
        for (int i = 0; i < 8 * 8; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = segmented_to_virtual(l_common_texture_minimap_kart_mario[6]);
        for (int i = 0; i < 8 * 8; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = segmented_to_virtual(l_common_texture_minimap_kart_mario[7]);
        for (int i = 0; i < 8 * 8; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        tlut_ptr = segmented_to_virtual(l_common_texture_minimap_kart_mario[8]);
        for (int i = 0; i < 8 * 8; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }


        tlut_ptr = segmented_to_virtual(common_texture_minimap_progress_dot);
        for (int i = 0; i < 8 * 8; i++) {
            uint16_t np = tlut_ptr[i];
            np = (np << 8) | ((np >> 8) & 0xff);
            tlut_ptr[i] = np;
        }

        // bomb kart wheel, 16x16 -- NO LONGER SWAPPED: dc_data/common_data.bin
        // (and the exe's embedded segment blob) are now extracted VERBATIM
        // from the ROM's own MIO0 block, i.e. already the raw BE byte stream
        // every consumer expects. This loop existed to fix the old
        // generated-from-C-arrays bin, and on ROM-true data it would corrupt
        // the wheel texture instead. The C-array swaps above are unaffected:
        // those arrays are separate compiled copies and still need it.

        sgm_run = 1;
    }
    load_ceremony_data();
}

/**
 * @brief
 *
 */
void game_init_clear_framebuffer(void) {
    gGamestateNext = 0; // = START_MENU_FROM_QUIT?
    clear_framebuffer(0);
}
extern int force_30fps;
static void r25_capture(u32 phase,u32 tick);
void race_logic_loop(void) {
    s16 i;
    u16 rotY;
#if defined(TARGET_XBOX)
    static int r12RaceFrames = 0;
    int r12Trace = xbox_netplay_active() && r12RaceFrames < 12;
    if (r12Trace)
        xbox_netplay_trace("R12_RACE %d ENTER net=%u rs=%u activeSM=%d selSM=%d pc=%d mode=%d course=%d pause=%u quit=%u p1=(%.2f,%.2f,%.2f) p2=(%.2f,%.2f,%.2f)\n",
                           r12RaceFrames, xbox_netplay_frame(), (unsigned)D_800DC510, (int)gActiveScreenMode,
                           (int)gScreenModeSelection, (int)gPlayerCountSelection1, (int)gModeSelection,
                           (int)gCurrentCourseId, (unsigned)gIsGamePaused, (unsigned)gIsInQuitToMenuTransition,
                           gPlayers[0].pos[0], gPlayers[0].pos[1], gPlayers[0].pos[2],
                           gPlayers[1].pos[0], gPlayers[1].pos[1], gPlayers[1].pos[2]);
#endif

#if defined(TARGET_XBOX) && MK64X_DEBUG_TOOLS
    /* Watermark: report frames that maxed the ORIGINAL 128-entry mtxObject
     * pool -- render_set_position silently skips the matrix load when full,
     * so course sections then draw with a stale transform (explosion). */
    {
        static u16 sMaxObj = 0;
        static int sPrObj = 0;
        if (gMatrixObjectCount > sMaxObj) {
            sMaxObj = gMatrixObjectCount;
            if (sMaxObj > 120 && sPrObj < 10) {
                sPrObj++;
                printf("MTXHI obj peak=%d\n", (int) sMaxObj);
            }
        }
    }
#endif
    gMatrixObjectCount = 0;
    gMatrixEffectCount = 0;
    if (gIsGamePaused != 0) {
        func_80290B14();
    }
    if (gIsInQuitToMenuTransition != 0) {
        func_802A38B4();
        return;
    }

#if defined(TARGET_XBOX)
    if (xbox_netplay_active()) sNumVBlanks = 2;
#endif
    if (sNumVBlanks >= 6) {
        sNumVBlanks = 5;
    }
    if (sNumVBlanks < 0) {
        sNumVBlanks = 1;
    }
#if defined(TARGET_XBOX)
    if (r12Trace) xbox_netplay_trace("R12_RACE %d PRE 802A4EF4\n", r12RaceFrames);
#endif
    func_802A4EF4();
    r25_capture(1,0xFFFFFFFFU);
#if defined(TARGET_XBOX)
    if (r12Trace) xbox_netplay_trace("R12_RACE %d POST 802A4EF4\n", r12RaceFrames);
#endif
    
    //gTickSpeed = 2;
    gTickSpeed = 1;

    switch (gActiveScreenMode) {
        case SCREEN_MODE_1P:
            // Uncap (60fps) ONLY for TRUE single-player. When a 3/4P battle/VS winner-pane
            // zoom completes, func_8028E438 flips gActiveScreenMode to SCREEN_MODE_1P for
            // the fullscreen RANKING screen while gamestate is still RACING — that pseudo-1P
            // must stay capped, or its flashing text (per-frame counters) runs at whatever
            // rate frames arrive (HW 2026-08-19). gPlayerCountSelection1 survives the switch.
            force_30fps = 1;//(gPlayerCountSelection1 == 1) ? 0 : 1;
            gTickSpeed = 2;//sNumVBlanks;

            // 60fps 30Hz gate (gRun30hz, see game_loop_one_iteration): ghost replay is a
            // frame-count-addressed input stream (2x at 60fps would desync).
            // DO NOT top-gate func_8005A070 OR func_80022744 — both strobe on HW:
            // 5A070 rebuilds HUD matrices in the double-buffered gGfxPool (Lakitu/HUD
            // flickered against 2-frame-old matrices), and the kart particle system
            // (exhaust smoke / boost flames) has a state->draw handshake (draw pass in
            // render_kart_particle_on_screen_one keys off per-frame state the update
            // pass refreshes), so gating updates starves draws every other frame (awful
            // flicker, HW 2026-08-19). Particles therefore run per-frame (cosmetically
            // 2x fast at 60); a real fix needs the state/draw split done INSIDE that
            // system. 5A070's safe leaves are gated inside code_80057C60.c
            // (func_8005C728 counters, update_object critters).
            if (gRun30hz) {
                staff_ghosts_loop();
            }

            if (gIsGamePaused == 0) {
                for (i = 0; i < gTickSpeed; i++) {
                    if (D_8015011E) {
                        gCourseTimer += COURSE_TIMER_ITER;
                    }
                    func_802909F0();
                    r25_capture(10,(u32)i);
                    evaluate_collision_for_players_and_actors();
                    r25_capture(11,(u32)i);
                    func_800382DC();
                    r25_capture(12,(u32)i);
                    func_8001EE98(gPlayerOneCopy, camera1, 0);
                    r25_capture(13,(u32)i);
                    func_80028F70();
                    func_8028F474();
                    r25_capture(17,(u32)i);
                    func_80059AC8();
                    r25_capture(18,(u32)i);
                    update_course_actors();
                    r25_capture(19,(u32)i);
                    course_update_water();
                    r25_capture(20,(u32)i);
                    func_8028FCBC();
                    r25_capture(21,(u32)i);
                }
                func_80022744();
                r25_capture(30,0xFFFFFFFFU);
            }
            func_8005A070();
            r25_capture(31,0xFFFFFFFFU);
#if defined(TARGET_XBOX)
            /* R22: everything after func_8005A070() is presentation.  The item
             * window/object update above remains authoritative gameplay. */
            r22_crossplay_present_begin();
#endif
            sNumVBlanks = 0;
            ////profiler_log_thread5_time(LEVEL_SCRIPT_EXECUTE);
            D_8015F788 = 0;
            render_player_one_1p_screen();
            if (!gEnableDebugMode) {
                D_800DC514 = false;
            } else {
                if (D_800DC514) {

                    if ((gControllerOne->buttonPressed & R_TRIG) && (gControllerOne->button & A_BUTTON) &&
                        (gControllerOne->button & B_BUTTON)) {
                        D_800DC514 = false;
                    }

                    rotY = camera1->rot[1];
                    gDebugPathCount = D_800DC5EC->pathCounter;
                    if (rotY < 0x2000) {
                        func_80057A50(40, 100, "SOUTH  ", gDebugPathCount);
                    } else if (rotY < 0x6000) {
                        func_80057A50(40, 100, "EAST   ", gDebugPathCount);
                    } else if (rotY < 0xA000) {
                        func_80057A50(40, 100, "NORTH  ", gDebugPathCount);
                    } else if (rotY < 0xE000) {
                        func_80057A50(40, 100, "WEST   ", gDebugPathCount);
                    } else {
                        func_80057A50(40, 100, "SOUTH  ", gDebugPathCount);
                    }

                } else {
                    if ((gControllerOne->buttonPressed & L_TRIG) && (gControllerOne->button & A_BUTTON) &&
                        (gControllerOne->button & B_BUTTON)) {
                        D_800DC514 = true;
                    }
                }
            }
            break;

        case SCREEN_MODE_2P_SPLITSCREEN_VERTICAL:
            force_30fps = 1;
#if defined(TARGET_XBOX)
            if (r12Trace) xbox_netplay_trace("R12_RACE %d 2PV branch enter sNumVBlanks=%d\n", r12RaceFrames, (int)sNumVBlanks);
#endif

            /* if (gCurrentCourseId == COURSE_DK_JUNGLE) {
                gTickSpeed = 3;
            } else {
                gTickSpeed = 2;
            } */

            // DC: framerate-compensating tick speed in place of the N64's static per-course
            // table above. sNumVBlanks = 60Hz vblanks since the last logic frame = exactly how
            // many 60Hz ticks this frame spans: 2 when we hold 30fps, 3 when we drop to 20.
            // Game speed stays correct at either rate, and frames that reach 30fps
            // automatically get the smooth 2-tick treatment as renderer perf improves.
            // Clamped [2,4]: 4 = the N64's own DK-Jungle-in-4P floor, also covers spikes.
            #if defined(TARGET_XBOX)
            gTickSpeed = xbox_netplay_active() ? 2 :
                ((sNumVBlanks < 2) ? 2 : ((sNumVBlanks > 4) ? 4 : sNumVBlanks));
#else
            gTickSpeed = (sNumVBlanks < 2) ? 2 : ((sNumVBlanks > 4) ? 4 : sNumVBlanks);
#endif

            if (gIsGamePaused == 0) {
                for (i = 0; i < gTickSpeed; i++) {
                    if (D_8015011E != 0) {
                        gCourseTimer += COURSE_TIMER_ITER;
                    }
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PV PRE 802909F0\n", r12RaceFrames);
#endif
                    func_802909F0();
                    r25_capture(10,(u32)i);
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PV POST 802909F0\n", r12RaceFrames);
#endif
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PV PRE collision\n", r12RaceFrames);
#endif
                    evaluate_collision_for_players_and_actors();
                    r25_capture(11,(u32)i);
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PV POST collision\n", r12RaceFrames);
#endif
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PV PRE 800382DC\n", r12RaceFrames);
#endif
                    func_800382DC();
                    r25_capture(12,(u32)i);
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PV POST 800382DC\n", r12RaceFrames);
#endif
                    func_8001EE98(gPlayerOneCopy, camera1, 0);
                    r25_capture(13,(u32)i);
                    func_80029060();
                    r25_capture(14,(u32)i);
                    func_8001EE98(gPlayerTwoCopy, camera2, 1);
                    r25_capture(15,(u32)i);
                    func_80029150();
                    r25_capture(16,(u32)i);
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PV PRE 8028F474\n", r12RaceFrames);
#endif
                    func_8028F474();
                    r25_capture(17,(u32)i);
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PV POST 8028F474\n", r12RaceFrames);
#endif
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PV PRE 80059AC8\n", r12RaceFrames);
#endif
                    func_80059AC8();
                    r25_capture(18,(u32)i);
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PV POST 80059AC8\n", r12RaceFrames);
#endif
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PV PRE course_actors\n", r12RaceFrames);
#endif
                    update_course_actors();
                    r25_capture(19,(u32)i);
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PV POST course_actors\n", r12RaceFrames);
#endif
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PV PRE water\n", r12RaceFrames);
#endif
                    course_update_water();
                    r25_capture(20,(u32)i);
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PV POST water\n", r12RaceFrames);
#endif
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PV PRE 8028FCBC\n", r12RaceFrames);
#endif
                    func_8028FCBC();
                    r25_capture(21,(u32)i);
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PV POST 8028FCBC\n", r12RaceFrames);
#endif
                }
#if defined(TARGET_XBOX)
                if (r12Trace) xbox_netplay_trace("R12_RACE %d 2PV PRE 80022744\n", r12RaceFrames);
#endif
                func_80022744();
                r25_capture(30,0xFFFFFFFFU);
#if defined(TARGET_XBOX)
                if (r12Trace) xbox_netplay_trace("R12_RACE %d 2PV POST 80022744\n", r12RaceFrames);
#endif
            }
#if defined(TARGET_XBOX)
            if (r12Trace) xbox_netplay_trace("R12_RACE %d 2PV PRE 8005A070\n", r12RaceFrames);
#endif
            func_8005A070();
            r25_capture(31,0xFFFFFFFFU);
#if defined(TARGET_XBOX)
            /* R22: everything after func_8005A070() is presentation.  The item
             * window/object update above remains authoritative gameplay. */
            r22_crossplay_present_begin();
#endif
#if defined(TARGET_XBOX)
            if (r12Trace) xbox_netplay_trace("R12_RACE %d 2PV POST 8005A070\n", r12RaceFrames);
#endif
            ////profiler_log_thread5_time(LEVEL_SCRIPT_EXECUTE);
            sNumVBlanks = 0;
            move_segment_table_to_dmem();
            init_rdp();
            if (D_800DC5B0 != 0) {
                select_framebuffer();
            }
            D_8015F788 = 0;
            if (gPlayerWinningIndex == 0) {
#if defined(TARGET_XBOX)
                if (r12Trace) xbox_netplay_trace("R12_RACE %d 2PV PRE renderP2V\n", r12RaceFrames);
#endif
                render_player_two_2p_screen_vertical();
#if defined(TARGET_XBOX)
                if (r12Trace) xbox_netplay_trace("R12_RACE %d 2PV POST renderP2V\n", r12RaceFrames);
#endif
#if defined(TARGET_XBOX)
                if (r12Trace) xbox_netplay_trace("R12_RACE %d 2PV PRE renderP1V\n", r12RaceFrames);
#endif
                render_player_one_2p_screen_vertical();
#if defined(TARGET_XBOX)
                if (r12Trace) xbox_netplay_trace("R12_RACE %d 2PV POST renderP1V\n", r12RaceFrames);
#endif
            } else {
#if defined(TARGET_XBOX)
                if (r12Trace) xbox_netplay_trace("R12_RACE %d 2PV PRE renderP1V\n", r12RaceFrames);
#endif
                render_player_one_2p_screen_vertical();
#if defined(TARGET_XBOX)
                if (r12Trace) xbox_netplay_trace("R12_RACE %d 2PV POST renderP1V\n", r12RaceFrames);
#endif
#if defined(TARGET_XBOX)
                if (r12Trace) xbox_netplay_trace("R12_RACE %d 2PV PRE renderP2V\n", r12RaceFrames);
#endif
                render_player_two_2p_screen_vertical();
#if defined(TARGET_XBOX)
                if (r12Trace) xbox_netplay_trace("R12_RACE %d 2PV POST renderP2V\n", r12RaceFrames);
#endif
            }
            break;

        case SCREEN_MODE_2P_SPLITSCREEN_HORIZONTAL:
            force_30fps = 1;
#if defined(TARGET_XBOX)
            if (r12Trace) xbox_netplay_trace("R12_RACE %d 2PH branch enter sNumVBlanks=%d\n", r12RaceFrames, (int)sNumVBlanks);
#endif

             /* if (gCurrentCourseId == COURSE_DK_JUNGLE ||
                gCurrentCourseId == COURSE_TOADS_TURNPIKE) {
                gTickSpeed = 3;
            } else {
                gTickSpeed = 2;
            } */

            // DC: framerate-compensating tick speed in place of the N64's static per-course
            // table above. sNumVBlanks = 60Hz vblanks since the last logic frame = exactly how
            // many 60Hz ticks this frame spans: 2 when we hold 30fps, 3 when we drop to 20.
            // Game speed stays correct at either rate, and frames that reach 30fps
            // automatically get the smooth 2-tick treatment as renderer perf improves.
            // Clamped [2,4]: 4 = the N64's own DK-Jungle-in-4P floor, also covers spikes.
            #if defined(TARGET_XBOX)
            gTickSpeed = xbox_netplay_active() ? 2 :
                ((sNumVBlanks < 2) ? 2 : ((sNumVBlanks > 4) ? 4 : sNumVBlanks));
#else
            gTickSpeed = (sNumVBlanks < 2) ? 2 : ((sNumVBlanks > 4) ? 4 : sNumVBlanks);
#endif

            if (gIsGamePaused == 0) {
                for (i = 0; i < gTickSpeed; i++) {
                    if (D_8015011E != 0) {
                        gCourseTimer += COURSE_TIMER_ITER;
                    }
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PH PRE 802909F0\n", r12RaceFrames);
#endif
                    func_802909F0();
                    r25_capture(10,(u32)i);
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PH POST 802909F0\n", r12RaceFrames);
#endif
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PH PRE collision\n", r12RaceFrames);
#endif
                    evaluate_collision_for_players_and_actors();
                    r25_capture(11,(u32)i);
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PH POST collision\n", r12RaceFrames);
#endif
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PH PRE 800382DC\n", r12RaceFrames);
#endif
                    func_800382DC();
                    r25_capture(12,(u32)i);
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PH POST 800382DC\n", r12RaceFrames);
#endif
                    func_8001EE98(gPlayerOneCopy, camera1, 0);
                    r25_capture(13,(u32)i);
                    func_80029060();
                    r25_capture(14,(u32)i);
                    func_8001EE98(gPlayerTwoCopy, camera2, 1);
                    r25_capture(15,(u32)i);
                    func_80029150();
                    r25_capture(16,(u32)i);
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PH PRE 8028F474\n", r12RaceFrames);
#endif
                    func_8028F474();
                    r25_capture(17,(u32)i);
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PH POST 8028F474\n", r12RaceFrames);
#endif
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PH PRE 80059AC8\n", r12RaceFrames);
#endif
                    func_80059AC8();
                    r25_capture(18,(u32)i);
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PH POST 80059AC8\n", r12RaceFrames);
#endif
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PH PRE course_actors\n", r12RaceFrames);
#endif
                    update_course_actors();
                    r25_capture(19,(u32)i);
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PH POST course_actors\n", r12RaceFrames);
#endif
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PH PRE water\n", r12RaceFrames);
#endif
                    course_update_water();
                    r25_capture(20,(u32)i);
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PH POST water\n", r12RaceFrames);
#endif
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PH PRE 8028FCBC\n", r12RaceFrames);
#endif
                    func_8028FCBC();
                    r25_capture(21,(u32)i);
#if defined(TARGET_XBOX)
                    if (r12Trace && i == 0) xbox_netplay_trace("R12_RACE %d 2PH POST 8028FCBC\n", r12RaceFrames);
#endif
                }
#if defined(TARGET_XBOX)
                if (r12Trace) xbox_netplay_trace("R12_RACE %d 2PH PRE 80022744\n", r12RaceFrames);
#endif
                func_80022744();
                r25_capture(30,0xFFFFFFFFU);
#if defined(TARGET_XBOX)
                if (r12Trace) xbox_netplay_trace("R12_RACE %d 2PH POST 80022744\n", r12RaceFrames);
#endif
            }
            ////profiler_log_thread5_time(LEVEL_SCRIPT_EXECUTE);
            sNumVBlanks = (u16) 0;
#if defined(TARGET_XBOX)
            if (r12Trace) xbox_netplay_trace("R12_RACE %d 2PH PRE 8005A070\n", r12RaceFrames);
#endif
            func_8005A070();
            r25_capture(31,0xFFFFFFFFU);
#if defined(TARGET_XBOX)
            /* R22: everything after func_8005A070() is presentation.  The item
             * window/object update above remains authoritative gameplay. */
            r22_crossplay_present_begin();
#endif
#if defined(TARGET_XBOX)
            if (r12Trace) xbox_netplay_trace("R12_RACE %d 2PH POST 8005A070\n", r12RaceFrames);
#endif
            move_segment_table_to_dmem();
            init_rdp();
            if (D_800DC5B0 != 0) {
                select_framebuffer();
            }
            D_8015F788 = 0;
            if (gPlayerWinningIndex == 0) {
#if defined(TARGET_XBOX)
                if (r12Trace) xbox_netplay_trace("R12_RACE %d 2PH PRE renderP2H\n", r12RaceFrames);
#endif
                render_player_two_2p_screen_horizontal();
#if defined(TARGET_XBOX)
                if (r12Trace) xbox_netplay_trace("R12_RACE %d 2PH POST renderP2H\n", r12RaceFrames);
#endif
#if defined(TARGET_XBOX)
                if (r12Trace) xbox_netplay_trace("R12_RACE %d 2PH PRE renderP1H\n", r12RaceFrames);
#endif
                render_player_one_2p_screen_horizontal();
#if defined(TARGET_XBOX)
                if (r12Trace) xbox_netplay_trace("R12_RACE %d 2PH POST renderP1H\n", r12RaceFrames);
#endif
            } else {
#if defined(TARGET_XBOX)
                if (r12Trace) xbox_netplay_trace("R12_RACE %d 2PH PRE renderP1H\n", r12RaceFrames);
#endif
                render_player_one_2p_screen_horizontal();
#if defined(TARGET_XBOX)
                if (r12Trace) xbox_netplay_trace("R12_RACE %d 2PH POST renderP1H\n", r12RaceFrames);
#endif
#if defined(TARGET_XBOX)
                if (r12Trace) xbox_netplay_trace("R12_RACE %d 2PH PRE renderP2H\n", r12RaceFrames);
#endif
                render_player_two_2p_screen_horizontal();
#if defined(TARGET_XBOX)
                if (r12Trace) xbox_netplay_trace("R12_RACE %d 2PH POST renderP2H\n", r12RaceFrames);
#endif
            }

            break;

        case SCREEN_MODE_3P_4P_SPLITSCREEN:
            force_30fps = 1;

            /* if (gPlayerCountSelection1 == 3) {
                switch (gCurrentCourseId) {
                    case COURSE_BOWSER_CASTLE:
                    case COURSE_MOO_MOO_FARM:
                    case COURSE_SKYSCRAPER:
                    case COURSE_DK_JUNGLE:
                        gTickSpeed = 3;
                        break;
                    default:
                        gTickSpeed = 2;
                        break;
                }
            } else {
                // Four players
                switch (gCurrentCourseId) {
                    case COURSE_BLOCK_FORT:
                    case COURSE_DOUBLE_DECK:
                    case COURSE_BIG_DONUT:
                        gTickSpeed = 2;
                        break;
                    case COURSE_DK_JUNGLE:
                        gTickSpeed = 4;
                        break;
                    default:
                        gTickSpeed = 3;
                        break;
                }
            } */

            // DC: framerate-compensating tick speed in place of the N64's static per-course
            // table above. sNumVBlanks = 60Hz vblanks since the last logic frame = exactly how
            // many 60Hz ticks this frame spans: 2 when we hold 30fps, 3 when we drop to 20.
            // Game speed stays correct at either rate, and frames that reach 30fps
            // automatically get the smooth 2-tick treatment as renderer perf improves.
            // Clamped [2,4]: 4 = the N64's own DK-Jungle-in-4P floor, also covers spikes.
            #if defined(TARGET_XBOX)
            gTickSpeed = xbox_netplay_active() ? 2 :
                ((sNumVBlanks < 2) ? 2 : ((sNumVBlanks > 4) ? 4 : sNumVBlanks));
#else
            gTickSpeed = (sNumVBlanks < 2) ? 2 : ((sNumVBlanks > 4) ? 4 : sNumVBlanks);
#endif

            if (gIsGamePaused == 0) {
                for (i = 0; i < gTickSpeed; i++) {
                    if (D_8015011E != 0) {
                        gCourseTimer += COURSE_TIMER_ITER;
                    }
                    func_802909F0();
                    r25_capture(10,(u32)i);
                    evaluate_collision_for_players_and_actors();
                    r25_capture(11,(u32)i);
                    func_800382DC();
                    r25_capture(12,(u32)i);
                    func_8001EE98(gPlayerOneCopy, camera1, 0);
                    r25_capture(13,(u32)i);
                    func_80029158();
                    func_8001EE98(gPlayerTwo, camera2, 1);
                    func_800291E8();
                    func_8001EE98(gPlayerThree, camera3, 2);
                    func_800291F0();
                    func_8001EE98(gPlayerFour, camera4, 3);
                    func_800291F8();
                    func_8028F474();
                    r25_capture(17,(u32)i);
                    func_80059AC8();
                    r25_capture(18,(u32)i);
                    update_course_actors();
                    r25_capture(19,(u32)i);
                    course_update_water();
                    r25_capture(20,(u32)i);
                    func_8028FCBC();
                    r25_capture(21,(u32)i);
                }
                func_80022744();
                r25_capture(30,0xFFFFFFFFU);
            }
            func_8005A070();
            r25_capture(31,0xFFFFFFFFU);
#if defined(TARGET_XBOX)
            /* R22: everything after func_8005A070() is presentation.  The item
             * window/object update above remains authoritative gameplay. */
            r22_crossplay_present_begin();
#endif
            sNumVBlanks = 0;
            ////profiler_log_thread5_time(LEVEL_SCRIPT_EXECUTE);
            move_segment_table_to_dmem();
            init_rdp();
            if (D_800DC5B0 != 0) {
                select_framebuffer();
            }
            D_8015F788 = 0;
            if (gPlayerWinningIndex == 0) {
                render_player_two_3p_4p_screen();
                render_player_three_3p_4p_screen();
                render_player_four_3p_4p_screen();
                render_player_one_3p_4p_screen();
            } else if (gPlayerWinningIndex == 1) {
                render_player_one_3p_4p_screen();
                render_player_three_3p_4p_screen();
                render_player_four_3p_4p_screen();
                render_player_two_3p_4p_screen();
            } else if (gPlayerWinningIndex == 2) {
                render_player_one_3p_4p_screen();
                render_player_two_3p_4p_screen();
                render_player_four_3p_4p_screen();
                render_player_three_3p_4p_screen();
            } else {
                render_player_one_3p_4p_screen();
                render_player_two_3p_4p_screen();
                render_player_three_3p_4p_screen();
                render_player_four_3p_4p_screen();
            }
            break;
    }

    if (!gEnableDebugMode) {
        gEnableResourceMeters = 0;
    } else {
        if (gEnableResourceMeters) {
            resource_display();
            if ((!(gControllerOne->button & L_TRIG)) && (gControllerOne->button & R_TRIG) &&
                (gControllerOne->buttonPressed & B_BUTTON)) {
                gEnableResourceMeters = 0;
            }
        } else {
            if ((!(gControllerOne->button & L_TRIG)) && (gControllerOne->button & R_TRIG) &&
                (gControllerOne->buttonPressed & B_BUTTON)) {
                gEnableResourceMeters = 1;
            }
        }
    }
    draw_splitscreen_separators();
#if defined(TARGET_XBOX)
    /* R41: native-proportion local fullscreen HUD. Offline/multi-local fall
     * straight through to MK64's stock frame-end HUD. */
    if (!xbox_render_online_local_hud()) {
        func_800591B4();
    }
#else
    func_800591B4();
#endif
    func_80093E20();
#if DVDL
    display_dvdl();
#endif
#if defined(TARGET_XBOX)
    /* Discard render/particle/culling side effects before the next lockstep frame. */
    r22_crossplay_present_end();
    r25_capture(32,0xFFFFFFFFU);
#endif
    gDPFullSync(gDisplayListHead++);
    gSPEndDisplayList(gDisplayListHead++);
#if defined(TARGET_XBOX)
    if (r12Trace) {
        xbox_netplay_trace("R12_RACE %d EXIT rs=%u tick=%d CT=%.3f VT=%.3f\n",
                           r12RaceFrames, (unsigned)D_800DC510, (int)gTickSpeed, gCourseTimer, gVBlankTimer);
        r12RaceFrames++;
    }
#endif
}

/**
 * mk64's game loop depends on a series of states.
 * It runs a wide branching series of code based on these states.
 * State 1) Clear framebuffer
 * State 2) Run menus
 * State 3) Process race related logic
 * State 4) Ending sequence
 * State 5) Credits
 *
 * Note that the state doesn't flip-flop at random but is permanent
 * until the state changes (ie. Exit menus and start a race).
 */

void game_state_handler(void) {
#if DVDL
    if ((gControllerOne->button & L_TRIG) && (gControllerOne->button & R_TRIG) && (gControllerOne->button & Z_TRIG) &&
        (gControllerOne->button & A_BUTTON)) {
        gGamestateNext = CREDITS_SEQUENCE;
    } else if ((gControllerOne->button & L_TRIG) && (gControllerOne->button & R_TRIG) &&
               (gControllerOne->button & Z_TRIG) && (gControllerOne->button & B_BUTTON)) {
        gGamestateNext = ENDING;
    }
#endif

    switch (gGamestate) {
        case 7:
            game_init_clear_framebuffer();
            break;
        case START_MENU_FROM_QUIT:
        case MAIN_MENU_FROM_QUIT:
        case PLAYER_SELECT_MENU_FROM_QUIT:
        case COURSE_SELECT_MENU_FROM_QUIT:
            // Display black
            // 
            update_menus();
            init_rcp();
            func_80094A64(gGfxPool);
#if DVDL
            display_dvdl();
#endif
            break;
        case RACING:
            race_logic_loop();
            break;
        case ENDING:
            podium_ceremony_loop();
            break;
        case CREDITS_SEQUENCE:
            credits_loop();
            break;
    }
}

void interrupt_gfx_sptask(void) {
    if (gActiveSPTask->task.t.type == M_GFXTASK) {
        gActiveSPTask->state = SPTASK_STATE_INTERRUPTED;
        osSpTaskYield();
    }
}

void receive_new_tasks(void) {
    UNUSED s32 pad;
    struct SPTask* spTask;

    while (osRecvMesg(&gSPTaskMesgQueue, (OSMesg*) &spTask, OS_MESG_NOBLOCK) != -1) {
        spTask->state = SPTASK_STATE_NOT_STARTED;
        switch (spTask->task.t.type) {
            case 2:
                sNextAudioSPTask = spTask;
                break;
            case 1:
                sNextDisplaySPTask = spTask;
                break;
        }
    }

    if (sCurrentAudioSPTask == NULL && sNextAudioSPTask != NULL) {
        sCurrentAudioSPTask = sNextAudioSPTask;
        sNextAudioSPTask = NULL;
    }
    if (sCurrentDisplaySPTask == NULL && sNextDisplaySPTask != NULL) {
        sCurrentDisplaySPTask = sNextDisplaySPTask;
        sNextDisplaySPTask = NULL;
    }
}

void set_vblank_handler(s32 index, struct VblankHandler* handler, OSMesgQueue* queue, OSMesg* msg) {
    handler->queue = queue;
    handler->msg = msg;
    switch (index) {
        case 1:
            gVblankHandler1 = handler;
            break;
        case 2:
            gVblankHandler2 = handler;
            break;
    }
}

void start_gfx_sptask(void) {
    if (gActiveSPTask == NULL && sCurrentDisplaySPTask != NULL &&
        sCurrentDisplaySPTask->state == SPTASK_STATE_NOT_STARTED) {
        ////profiler_log_gfx_time(TASKS_QUEUED);
        start_sptask(M_GFXTASK);
    }
}

void handle_vblank(void) {
#if defined(TARGET_XBOX)
    /* Online simulation time is driven only by consumed network frames. */
    if (!xbox_netplay_active()) {
        gVBlankTimer += V_BlANK_TIMER_ITER;
        sNumVBlanks++;
    }
#else
    gVBlankTimer += V_BlANK_TIMER_ITER;
    sNumVBlanks++;
#endif

    receive_new_tasks();

    // First try to kick off an audio task. If the gfx task is currently
    // running, we need to asynchronously interrupt it -- handle_sp_complete
    // will pick up on what we're doing and start the audio task for us.
    // If there is already an audio task running, there is nothing to do.
    // If there is no audio task available, try a gfx task instead.
    if (sCurrentAudioSPTask != NULL) {
        if (gActiveSPTask != NULL) {
            interrupt_gfx_sptask();
        } else {
            ////profiler_log_vblank_time();
            start_sptask(M_AUDTASK);
        }
    } else {
        if (gActiveSPTask == NULL && sCurrentDisplaySPTask != NULL &&
            sCurrentDisplaySPTask->state != SPTASK_STATE_FINISHED) {
            ////profiler_log_gfx_time(TASKS_QUEUED);
            start_sptask(M_GFXTASK);
        }
    }

/* This is where I would put my rumble code... If I had any. */
#if ENABLE_RUMBLE
    rumble_thread_update_vi();
#endif

    if (gVblankHandler1 != NULL) {
        osSendMesg(gVblankHandler1->queue, gVblankHandler1->msg, OS_MESG_NOBLOCK);
    }
    if (gVblankHandler2 != NULL) {
        osSendMesg(gVblankHandler2->queue, gVblankHandler2->msg, OS_MESG_NOBLOCK);
    }
}

void handle_dp_complete(void) {
    // Gfx SP task is completely done.
    if (sCurrentDisplaySPTask->msgqueue != NULL) {
        osSendMesg(sCurrentDisplaySPTask->msgqueue, sCurrentDisplaySPTask->msg, OS_MESG_NOBLOCK);
    }
    ////profiler_log_gfx_time(RDP_COMPLETE);
    sCurrentDisplaySPTask->state = SPTASK_STATE_FINISHED_DP;
    sCurrentDisplaySPTask = NULL;
}

void handle_sp_complete(void) {
    struct SPTask* curSPTask = gActiveSPTask;

    gActiveSPTask = NULL;

    if (curSPTask->state == SPTASK_STATE_INTERRUPTED) {
        // handle_vblank tried to start an audio task while there was already a
        // gfx task running, so it had to interrupt the gfx task. That interruption
        // just finished.
        if (osSpTaskYielded((OSTask*) curSPTask) == 0) {
            // The gfx task completed before we had time to interrupt it.
            // Mark it finished, just like below.
            curSPTask->state = SPTASK_STATE_FINISHED;
            ////profiler_log_gfx_time(RSP_COMPLETE);
        }
        // Start the audio task, as expected by handle_vblank.
        ////profiler_log_vblank_time();
        start_sptask(M_AUDTASK);
    } else {
        curSPTask->state = SPTASK_STATE_FINISHED;
        if (curSPTask->task.t.type == M_AUDTASK) {
            // After audio tasks come gfx tasks.
            ////profiler_log_vblank_time();
            if (sCurrentDisplaySPTask != NULL) {
                if (sCurrentDisplaySPTask->state != SPTASK_STATE_FINISHED) {
                    if (sCurrentDisplaySPTask->state != SPTASK_STATE_INTERRUPTED) {
                        ////profiler_log_gfx_time(TASKS_QUEUED);
                    }
                    start_sptask(M_GFXTASK);
                }
            }
            sCurrentAudioSPTask = NULL;
            if (curSPTask->msgqueue != NULL) {
                osSendMesg(curSPTask->msgqueue, curSPTask->msg, OS_MESG_NOBLOCK);
            }
        } else {
            // The SP process is done, but there is still a Display Processor notification
            // that needs to arrive before we can consider the task completely finished and
            // null out sCurrentDisplaySPTask. That happens in handle_dp_complete.
            ////profiler_log_gfx_time(RSP_COMPLETE);
        }
    };
}

void thread3_video(UNUSED void* arg0) {
#if 0
    s32 i;
    u64* framebuffer1;
    OSMesg msg;
    UNUSED s32 pad[4];

    gPhysicalFramebuffers[0] = (u16*) &gFramebuffer0;
    gPhysicalFramebuffers[1] = (u16*) &gFramebuffer1;
    gPhysicalFramebuffers[2] = (u16*) &gFramebuffer2;

    // Clear framebuffer.
    framebuffer1 = (u64*) &gFramebuffer1;
    for (i = 0; i < 19200; i++) {
        framebuffer1[i] = 0;
    }
    setup_mesg_queues();
    setup_game_memory();

    create_thread(&gAudioThread, 4, &thread4_audio, 0, gAudioThreadStack + ARRAY_COUNT(gAudioThreadStack), 20);
    osStartThread(&gAudioThread);

    create_thread(&gGameLoopThread, 5, &thread5_game_loop, 0, gGameLoopThreadStack + ARRAY_COUNT(gGameLoopThreadStack),
                  10);
    osStartThread(&gGameLoopThread);

    while (true) {
        osRecvMesg(&gIntrMesgQueue, &msg, OS_MESG_BLOCK);
        switch ((u32) msg) {
            case MESG_VI_VBLANK:
                handle_vblank();
                break;
            case MESG_SP_COMPLETE:
                handle_sp_complete();
                break;
            case MESG_DP_COMPLETE:
                handle_dp_complete();
                break;
            case MESG_START_GFX_SPTASK:
                start_gfx_sptask();
                break;
        }
    }
#endif
}

void func_800025D4(void) {
    func_80091B78();
    gActiveScreenMode = SCREEN_MODE_1P;
    set_perspective_and_aspect_ratio();
}

void func_80002600(void) {
    func_80091B78();
    gActiveScreenMode = SCREEN_MODE_1P;
    set_perspective_and_aspect_ratio();
}

void func_8000262C(void) {
    func_80091B78();
    gActiveScreenMode = SCREEN_MODE_1P;
    set_perspective_and_aspect_ratio();
}

void func_80002658(void) {
    func_80091B78();
    gActiveScreenMode = SCREEN_MODE_1P;
    set_perspective_and_aspect_ratio();
}

int credits_started = 0;

void update_gamestate(void) {
    // Default EVERY gamestate to the 30fps vblank cap: menus/logos/ceremony/credits all
    // assume 30Hz ticking (hardcoded gTickSpeed, per-frame anim counters). Racing is the
    // only state that may uncap — race_logic_loop re-asserts force_30fps per mode EVERY
    // frame (1P=0, split-screen=1), so this default cleanly covers entering AND leaving it.
    force_30fps = 1;
    switch (gGamestate) {
        case START_MENU_FROM_QUIT:
            func_80002658();
            gCurrentlyLoadedCourseId = COURSE_NULL;
            break;
        case MAIN_MENU_FROM_QUIT:
            func_800025D4();
            gCurrentlyLoadedCourseId = COURSE_NULL;
            break;
        case PLAYER_SELECT_MENU_FROM_QUIT:
            func_80002600();
            gCurrentlyLoadedCourseId = COURSE_NULL;
            break;
        case COURSE_SELECT_MENU_FROM_QUIT:
            func_8000262C();
            gCurrentlyLoadedCourseId = COURSE_NULL;
            break;
        case RACING:
#if defined(TARGET_XBOX)
            xbox_netplay_trace("R12_UPDATE_STATE RACING PRE setup_race\n");
#endif
            setup_race();
#if defined(TARGET_XBOX)
            xbox_netplay_trace("R12_UPDATE_STATE RACING POST setup_race\n");
#endif
            break;
        case ENDING:
            gCurrentlyLoadedCourseId = COURSE_NULL;
            load_ceremony_cutscene();
            break;
        case CREDITS_SEQUENCE:
            credits_started = 1;
            gCurrentlyLoadedCourseId = COURSE_NULL;
            load_credits();
            break;
    }
}

void SPINNING_THREAD(UNUSED void *arg);

/* static */ volatile uint64_t vblticker=0;

void vblfunc(uint32_t c, void *d) {
	(void)c;
	(void)d;
    vblticker++;
    /* Drive offline timing from the real 60 Hz platform vblank. During
       netplay, however, the Xbox 360 reference build derives gVBlankTimer
       exclusively from the consumed network frame. Keeping this increment
       online let OG Xbox transition/menu/object timers run ahead of the 360. */
#if defined(TARGET_XBOX)
    if (!xbox_netplay_active()) {
        gVBlankTimer += V_BlANK_TIMER_ITER;
        sNumVBlanks++;
    }
#else
    gVBlankTimer += V_BlANK_TIMER_ITER;
    sNumVBlanks++;
#endif
    genwait_wake_all((void *)&vblticker);
}

void thread5_game_loop(UNUSED void* arg) {
    setup_mesg_queues();
    setup_game_memory();

    gfx_init(wm_api, rendering_api, "Mario Kart 64", false);

    osCreateMesgQueue(&gGfxVblankQueue, gGfxMesgBuf, 1);
    osCreateMesgQueue(&gGameVblankQueue, &gGameMesgBuf, 1);

    init_controllers();
#if defined(TARGET_XBOX)
    /* R2: the online menu is intentionally AFTER graphics + controller init.
     * Offline never initializes XNet. Host/Join initialize networking only
     * after the player selects them. */
    xbox_netplay_boot_menu();
    if (xbox_netplay_active()) {
        /* Xbox 360's online osContInit reports every negotiated racer as a
         * connected logical N64 controller.  Our safe OG menu runs after
         * init_controllers(), so reproduce that result here instead. */
        unsigned net_players = (unsigned)xbox_netplay_player_count();
        if (net_players > 4U) net_players = 4U;
        gControllerBits = (u8)((1U << net_players) - 1U);
        sIsController1Unplugged = 0;
    }
#endif
    if (!wasSoftReset) {
        clear_nmi_buffer();
    }

    // These variables track stats such as player wins.
    // In the event of a console reset, it remembers them.
    gNmiUnknown1 = &pAppNmiBuffer[0]; // 2  u8's, tracks number of times player 1/2 won a VS race
    gNmiUnknown2 =
        &pAppNmiBuffer[2]; // 9  u8's, 3x3, tracks number of times player 1/2/3   has placed in 1st/2nd/3rd in a VS race
    gNmiUnknown3 = &pAppNmiBuffer[11]; // 12 u8's, 4x3, tracks number of times player 1/2/3/4 has placed in 1st/2nd/3rd
                                       // in a VS race
    gNmiUnknown4 = &pAppNmiBuffer[23]; // 2  u8's, tracking number of Battle mode wins by player 1/2
    gNmiUnknown5 = &pAppNmiBuffer[25]; // 3  u8's, tracking number of Battle mode wins by player 1/2/3
    gNmiUnknown6 = &pAppNmiBuffer[28]; // 4  u8's, tracking number of Battle mode wins by player 1/2/3/4
    rendering_init();
    read_controllers();
    func_800C5CB8();
	inited = 1;
    vblank_handler_add(&vblfunc, NULL);
    create_thread(NULL, 5, &SPINNING_THREAD, NULL, NULL, 12);
#define MEMTEST
#if defined(MEMTEST)
    for(int mi=0;mi<6*1048576;mi+=65536) {
        void *test_m = malloc(mi);
        if (test_m != NULL) {
            free(test_m);
            test_m = NULL;
            continue;
        } else {
            int bi = mi - 65536;
            for (; bi < 6 * 1048576; bi++) {
                test_m = malloc(bi);
                if (test_m != NULL) {
                    free(test_m);
                    test_m = NULL;
                    continue;
                } else {
                    printf("free ram for malloc: %d\n", bi);
                    goto run_game_loop;
                }
            }
        }
    }
run_game_loop:
#endif

    while (true) {
        game_loop_one_iteration();
		thd_pass();
    }
}

void _AudioInit(void) {
    if (audio_api == NULL) {
        audio_api = &audio_dc;
        audio_api->init();
    }
}

void SPINNING_THREAD(UNUSED void *arg) {
    uint64_t last_vbltick = vblticker;

    while (1) {
//        {
//            irq_disable_scoped();
            while (vblticker <= last_vbltick)
                genwait_wait((void*)&vblticker, NULL, 0, NULL);
//        }

        last_vbltick = vblticker;

        /* AICA hardware mixing: create_next_audio_buffer drives the AICA voices
           directly (AicaSynth_Update inside synthesis_execute). No software mix
           buffer is produced, so the KOS stream is no longer pushed/used. */
        create_next_audio_buffer(audio_buffer, SAMPLES_HIGH);
    }
}

/* MK64_CROSSPLAY_COMPONENT_DIAG_R15_MAIN
 * Cross-platform, read-only diagnostic snapshot.
 * Hashes integers in an explicit byte order so PPC/x86 host endianness
 * cannot itself create a mismatch.
 */
static u32 mkdiag_fnv_u32(u32 h, u32 v) {
    h = (h ^ ((v >> 24) & 0xFFU)) * 16777619U;
    h = (h ^ ((v >> 16) & 0xFFU)) * 16777619U;
    h = (h ^ ((v >>  8) & 0xFFU)) * 16777619U;
    h = (h ^ ( v        & 0xFFU)) * 16777619U;
    return h;
}
static u32 mkdiag_float_bits(f32 v) {
    union { f32 f; u32 u; } x;
    x.f = v;
    return x.u;
}
static u32 mkdiag_hash_vec3_raw(const f32 *v) {
    u32 h = 2166136261U;
    h = mkdiag_fnv_u32(h, mkdiag_float_bits(v[0]));
    h = mkdiag_fnv_u32(h, mkdiag_float_bits(v[1]));
    h = mkdiag_fnv_u32(h, mkdiag_float_bits(v[2]));
    return h;
}
static u32 mkdiag_quant_float_bits(f32 v) {
    u32 b = mkdiag_float_bits(v);
    /* Diagnostic only: discard the lowest 12 mantissa/storage bits.
     * This never changes gameplay state. */
    return b & 0xFFFFF000U;
}
static u32 mkdiag_hash_vec3_quant(const f32 *v) {
    u32 h = 2166136261U;
    h = mkdiag_fnv_u32(h, mkdiag_quant_float_bits(v[0]));
    h = mkdiag_fnv_u32(h, mkdiag_quant_float_bits(v[1]));
    h = mkdiag_fnv_u32(h, mkdiag_quant_float_bits(v[2]));
    return h;
}
static u32 mkdiag_player_meta_hash(int i) {
    u32 h = 2166136261U;
    h = mkdiag_fnv_u32(h, (u32)gPlayers[i].type);
    h = mkdiag_fnv_u32(h, (u32)(s32)gPlayers[i].lapCount);
    h = mkdiag_fnv_u32(h, (u32)gPlayers[i].effects);
    return h;
}

void mk64_crossplay_component_diag(unsigned int *out, int cap) {
    extern u16 gRandomSeed16;
    u32 core, full;
    int i;
    if (!out || cap < 36) return;
    for (i = 0; i < cap; ++i) out[i] = 0;

    out[0] = 0x4D4B4431U; /* MKD1 */
    out[1] = (u32)gGlobalTimer;
    out[2] = (u32)gGamestate;
    out[3] = (u32)gModeSelection;
    out[4] = (u32)gRandomSeed16;

    core = 2166136261U;
    core = mkdiag_fnv_u32(core, out[2]);
    core = mkdiag_fnv_u32(core, out[3]);
    core = mkdiag_fnv_u32(core, out[4]);
    out[5] = core;

    out[6]  = mkdiag_player_meta_hash(0);
    out[7]  = mkdiag_hash_vec3_raw(gPlayers[0].pos);
    out[8]  = mkdiag_hash_vec3_raw(gPlayers[0].velocity);
    out[9]  = mkdiag_hash_vec3_quant(gPlayers[0].pos);
    out[10] = mkdiag_hash_vec3_quant(gPlayers[0].velocity);

    out[11] = mkdiag_player_meta_hash(1);
    out[12] = mkdiag_hash_vec3_raw(gPlayers[1].pos);
    out[13] = mkdiag_hash_vec3_raw(gPlayers[1].velocity);
    out[14] = mkdiag_hash_vec3_quant(gPlayers[1].pos);
    out[15] = mkdiag_hash_vec3_quant(gPlayers[1].velocity);

    out[16] = mkdiag_float_bits(gPlayers[0].pos[0]);
    out[17] = mkdiag_float_bits(gPlayers[0].pos[1]);
    out[18] = mkdiag_float_bits(gPlayers[0].pos[2]);
    out[19] = mkdiag_float_bits(gPlayers[0].velocity[0]);
    out[20] = mkdiag_float_bits(gPlayers[0].velocity[1]);
    out[21] = mkdiag_float_bits(gPlayers[0].velocity[2]);

    out[22] = mkdiag_float_bits(gPlayers[1].pos[0]);
    out[23] = mkdiag_float_bits(gPlayers[1].pos[1]);
    out[24] = mkdiag_float_bits(gPlayers[1].pos[2]);
    out[25] = mkdiag_float_bits(gPlayers[1].velocity[0]);
    out[26] = mkdiag_float_bits(gPlayers[1].velocity[1]);
    out[27] = mkdiag_float_bits(gPlayers[1].velocity[2]);

    full = core;
    for (i = 6; i <= 15; ++i) full = mkdiag_fnv_u32(full, out[i]);
    out[28] = full;

    out[30] = (u32)gPlayers[0].type;
    out[31] = (u32)(s32)gPlayers[0].lapCount;
    out[32] = (u32)gPlayers[0].effects;
    out[33] = (u32)gPlayers[1].type;
    out[34] = (u32)(s32)gPlayers[1].lapCount;
    out[35] = (u32)gPlayers[1].effects;
}
