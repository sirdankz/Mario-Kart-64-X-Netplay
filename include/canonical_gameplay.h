// Copyright (c) 2026 sirdankz
// SPDX-License-Identifier: GPL-3.0-only
// See NETPLAY-LICENSE.md for license scope.
#ifndef MK64_CANONICAL_GAMEPLAY_H
#define MK64_CANONICAL_GAMEPLAY_H
/* R27: only simulation translation units include this header. Preserve OG C
 * operation order and actual division; never contract PPC multiply/add pairs.
 * Graphics, audio, input, startup and transport retain their platform settings.
 * The OG compiler already uses SSE binary32 arithmetic on Pentium III.
 */
#if defined(_MSC_VER) && defined(XBOX360_PORT)
#pragma float_control(precise, on)
#pragma fp_contract(off)
#elif defined(__clang__)
#pragma clang fp contract(off)
#pragma clang fp reassociate(off)
#endif

/* Explicit binary32 boundaries survive inlining and future optimizer changes. */
static float mk64_f32_add(float a, float b) { volatile float r = a + b; return r; }
static float mk64_f32_mul(float a, float b) { volatile float r = a * b; return r; }
static float mk64_f32_div(float a, float b) {
    volatile float denominator = b;
    volatile float r = a / denominator;
    return r;
}
static float mk64_canonical_speed_square_div25(float speed) {
    return mk64_f32_div(mk64_f32_mul(speed, speed), 25.0f);
}
/* OG uses double literals here. Keep those semantics, but remove x87 excess
 * precision and PPC fusion at the individual double-operation boundaries. */
static float mk64_canonical_accel_step(float speed, float acceleration, int slope,
                                      float boost, int boosted) {
    volatile double grade = 0.05 * (slope / 182);
    volatile double delta = (double)acceleration + grade;
    volatile double scaled;
    volatile double sum;
    volatile float result;
    if (boosted) { scaled = delta * (double)boost; } else { scaled = delta; }
    sum = (double)speed + scaled;
    result = (float)sum;
    return result;
}
/* OG CPU velocity integration uses double literals after a binary32 force
 * sum. Preserve that promotion and round each double result explicitly. */
static float mk64_canonical_cpu_velocity(float velocity, float force, float friction) {
    volatile double drag = 0.12 * (double)friction;
    volatile double loss = (double)velocity * drag;
    volatile double net = (double)force - loss;
    volatile double delta = net / 6000.0;
    volatile double sum = (double)velocity + delta;
    volatile float result = (float)sum;
    return result;
}
/* Round the ratio before scaling the lookup index, as the OG SSE path does. */
static int mk64_canonical_atan_index(float y, float x) {
    return (int)mk64_f32_add(mk64_f32_mul(mk64_f32_div(y, x), 1024.0f), 0.5f);
}
/* R27 RAM-only CPU subphase trace; implemented beside the R25 tracer. */
void mk64_r27_cpu_checkpoint(unsigned int stage, int playerId);
#endif
