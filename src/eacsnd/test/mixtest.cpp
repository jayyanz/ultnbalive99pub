/*
 * mixtest: checks eacsnd's 3D maths (mix3d.inc) on the build machine.
 *   g++ -O1 -Wall -o mixtest mixtest.cpp && ./mixtest
 * Panning: direction to the right speakers, constant power, rear sounds on
 * surround layouts, stereo fold-down; voice mixing: rate, looping, end, gain ramp.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "../mix3d.inc"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static double power(const float *g, int n) { double p = 0; for (int i = 0; i < n; i++) p += (double)g[i] * g[i]; return p; }

/* direction at azimuth deg (+ = right), level */
static void dir(double deg, double *x, double *y, double *z) { double r = deg * M3D_PI / 180; *x = sin(r); *y = 0; *z = cos(r); }

int main(void)
{
    m3d_layout L;
    float g[8];
    double x, y, z;

    /* stereo: centre equal, right louder on the right, rear folds to its side */
    m3d_layout_init(&L, 2);
    CHECK(!L.surround, "stereo has no rear");
    dir(0, &x, &y, &z); m3d_gains(&L, MODE3D_SPEAKERS, x, y, z, g);
    CHECK(fabs(g[0] - g[1]) < 1e-5 && fabs(power(g, 2) - 1) < 1e-5, "centre %f %f", g[0], g[1]);
    dir(90, &x, &y, &z); m3d_gains(&L, MODE3D_SPEAKERS, x, y, z, g);
    CHECK(g[0] < 1e-5 && fabs(g[1] - 1) < 1e-5, "hard right %f %f", g[0], g[1]);
    dir(-45, &x, &y, &z); m3d_gains(&L, MODE3D_SPEAKERS, x, y, z, g);
    CHECK(g[0] > g[1] && fabs(power(g, 2) - 1) < 1e-5, "front left %f %f", g[0], g[1]);
    dir(-135, &x, &y, &z); m3d_gains(&L, MODE3D_SPEAKERS, x, y, z, g);
    CHECK(g[0] > g[1], "rear left heard on the left %f %f", g[0], g[1]);
    dir(90, &x, &y, &z); m3d_gains(&L, MODE3D_OFF, x, y, z, g);
    CHECK(fabs(g[0] - g[1]) < 1e-5, "Mode3D=off centred");

    /* 5.1 (SDL order FL FR FC LFE BL BR): front to FC, rear right to FR/BR, LFE never */
    m3d_layout_init(&L, 6);
    CHECK(L.surround && L.n == 5, "5.1 layout n=%d", L.n);
    dir(0, &x, &y, &z); m3d_gains(&L, MODE3D_SPEAKERS, x, y, z, g);
    CHECK(fabs(g[2] - 1) < 1e-5 && g[3] == 0, "5.1 front to centre: FC %f", g[2]);
    dir(110, &x, &y, &z); m3d_gains(&L, MODE3D_SPEAKERS, x, y, z, g);
    CHECK(fabs(g[5] - 1) < 1e-5, "5.1 at BR exactly: %f", g[5]);
    dir(180, &x, &y, &z); m3d_gains(&L, MODE3D_SPEAKERS, x, y, z, g);
    CHECK(fabs(g[4] - g[5]) < 1e-5 && g[4] > 0.5 && g[0] == 0 && g[1] == 0, "5.1 behind: BL %f BR %f", g[4], g[5]);
    for (int d = -180; d <= 180; d += 7) {
        dir(d, &x, &y, &z); m3d_gains(&L, MODE3D_SPEAKERS, x, y, z, g);
        CHECK(fabs(power(g, 6) - 1) < 1e-4 && g[3] == 0, "5.1 constant power at %d: %f", d, power(g, 6));
    }
    dir(90, &x, &y, &z); m3d_gains(&L, MODE3D_STEREO, x, y, z, g);
    CHECK(fabs(g[1] - 1) < 1e-5 && g[4] == 0 && g[5] == 0, "Mode3D=stereo on 5.1 uses FL/FR only");

    /* 7.1 and 4.0 constant power all round */
    int lays[] = { 4, 5, 7, 8 };
    for (int li = 0; li < 4; li++) {
        m3d_layout_init(&L, lays[li]);
        for (int d = -180; d <= 180; d += 5) {
            dir(d, &x, &y, &z); m3d_gains(&L, MODE3D_SPEAKERS, x, y, z, g);
            CHECK(fabs(power(g, lays[li]) - 1) < 1e-4, "%d ch power at %d: %f", lays[li], d, power(g, lays[li]));
        }
    }

    /* distance: DirectSound default rolloff, 1/d beyond 1 */
    m3d_layout_init(&L, 2);
    m3d_gains(&L, MODE3D_SPEAKERS, 0, 0, 4, g);
    CHECK(fabs(power(g, 2) - 1.0 / 16) < 1e-5, "distance 4: power %f", power(g, 2));

    /* volume: EA's table is 20*log10(v/127) in hundredths of a dB */
    CHECK(m3d_volume(0) == 0 && m3d_volume(127) == 1 && fabs(m3d_volume(64) - 64.0 / 127) < 1e-6, "volume");
    CHECK(fabs(2000 * log10(m3d_volume(1)) - (-4207)) < 10, "volume 1 = -42 dB like EA's -4200");

    /* voice: half rate plays twice as long; non-looping ends; looping wraps */
    int16_t src[100];
    for (int i = 0; i < 100; i++) src[i] = (int16_t)(i * 300);
    static float out[2 * 1000];
    m3d_voice v;
    memset(&v, 0, sizeof v);
    v.data = src; v.frames = 100; v.step = 0.5;
    v.cur[0] = v.cur[1] = v.target[0] = v.target[1] = 1.0f;
    memset(out, 0, sizeof out);
    int n = m3d_mix(&v, out, 2, 1000);
    CHECK(n == 200 && v.ended, "half rate: %d frames, ended %d", n, v.ended);
    CHECK(fabs(out[2 * 3] - (1.5f * 300 / 32768)) < 1e-4, "interpolation %f", out[6]);
    memset(&v, 0, sizeof v);
    v.data = src; v.frames = 100; v.step = 1.0; v.loop = 1;
    v.cur[0] = v.cur[1] = v.target[0] = v.target[1] = 1.0f;
    memset(out, 0, sizeof out);
    n = m3d_mix(&v, out, 2, 1000);
    CHECK(n == 1000 && !v.ended && fabs(out[2 * 250] - src[50] / 32768.0f) < 1e-5, "loop: %d, %f", n, out[500]);
    /* gain ramp: from 0 to 1 over 64 frames, no jump */
    memset(&v, 0, sizeof v);
    int16_t dc[1000];
    for (int i = 0; i < 1000; i++) dc[i] = 16384;
    v.data = dc; v.frames = 1000; v.step = 1.0;
    v.target[0] = 1.0f;
    memset(out, 0, sizeof out);
    m3d_mix(&v, out, 2, 200);
    CHECK(out[0] > 0 && out[0] < 0.02f && fabs(out[2 * 63] - 0.5f) < 1e-3 && fabs(out[2 * 150] - 0.5f) < 1e-3,
          "ramp %f %f %f", out[0], out[126], out[300]);

    printf(fails ? "%d FAILED\n" : "mixtest: all passed\n", fails);
    return fails != 0;
}
