// Copyright (c) 2026 sirdankz
// SPDX-License-Identifier: GPL-3.0-only
// See NETPLAY-LICENSE.md for license scope.
/* R27: shared gameplay approach helpers, despite their original location in
 * render_player.c. CPU steering, suspension and item effects call these too.
 * Volatile binary32 intermediates prevent multiply-subtract contraction without
 * changing compiler policy for any rendering function. */
void move_s32_towards(s32* value, s32 target, f32 percent) {
    volatile f32 delta = (f32)(*value - target);
    volatile f32 step = delta * percent;
    volatile f32 result = (f32)*value - step;
    *value = (s32)result;
}
void move_f32_towards(f32* value, f32 target, f32 percent) {
    volatile f32 delta = *value - target;
    volatile f32 step = delta * percent;
    volatile f32 result = *value - step;
    f32 next = result;
    if (next < 0.001f && -0.001f < next) next = 0.0f;
    *value = next;
}
void move_s16_towards(s16* value, s16 target, f32 percent) {
    volatile f32 delta = (f32)(*value - target);
    volatile f32 step = delta * percent;
    volatile f32 result = (f32)*value - step;
    *value = (s16)result;
}
void move_u16_towards(u16* value, s16 target, f32 percent) {
    volatile f32 delta = (f32)(*value - target);
    volatile f32 step = delta * percent;
    volatile f32 result = (f32)*value - step;
    *value = (u16)result;
}
