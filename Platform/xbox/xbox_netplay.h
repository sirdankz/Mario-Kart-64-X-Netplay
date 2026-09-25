// Copyright (c) 2026 sirdankz
// SPDX-License-Identifier: GPL-3.0-only
// See NETPLAY-LICENSE.md for license scope.
#ifndef MK64X_XBOX_NETPLAY_H
#define MK64X_XBOX_NETPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Original-Xbox transport/session layer for MK64 netplay.
 *
 * R3 adds the Xbox 360-compatible pre-game flow and runtime input lockstep:
 *   OFFLINE
 *   HOST 2-4 PLAYER GAME
 *   JOIN 2-4 PLAYER GAME
 *
 * Networking is not touched until the player explicitly chooses Host or Join.
 * This keeps normal offline boot identical to the base port.
 */
int xbox_netplay_boot_menu(void);
void xbox_netplay_pump(void);
/* R3: synchronizes N64-format controller pads with the Xbox 360 v4 stream. */
void xbox_netplay_controllers(void *pads, int count);
void xbox_netplay_set_menu_sync(int enabled);
void xbox_netplay_shutdown(void);

int xbox_netplay_active(void);
int xbox_netplay_hosting(void);
int xbox_netplay_crossplay(void);
int xbox_netplay_player_count(void);
int xbox_netplay_local_slot(void);
int xbox_netplay_local_count(void);
unsigned int xbox_netplay_frame(void);
/* R14: race state from the 360 STATE_SYNC snapshot applied for the current
 * network frame. Returns -1 until such a snapshot has been applied. */
int xbox_netplay_host_race_state(void);
/* R12 persistent race-load diagnostic logger. Flushes each line immediately. */
void xbox_netplay_trace(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* MK64X_XBOX_NETPLAY_H */
