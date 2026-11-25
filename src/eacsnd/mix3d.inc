/*
 * mix3d.inc: the 3D voice maths of our eacsnd.dll, kept free of Windows and SDL so
 * test/mixtest.cpp can check it on the build machine. Included once by eacsnd.cpp.
 *
 * Coordinates are DirectSound 3D's, because the game was written for EA's DLL, which
 * passed them straight to IDirectSound3DBuffer::SetPosition: left-handed, +x right,
 * +y up, +z ahead; the listener stays at the origin facing +z (EA's DLL never set it).
 * The game's position callback gives a unit direction (two angles, no distance), so in
 * practice only the direction matters; distance still follows DirectSound's default
 * inverse-distance rolloff (minimum distance 1, rolloff 1) in case a longer vector comes.
 *
 * Speaker layouts are SDL3's channel orders (SDL_audio.h):
 *   1 FC | 2 FL FR | 3 FL FR LFE | 4 FL FR BL BR | 5 FL FR LFE BL BR |
 *   6 FL FR FC LFE BL BR | 7 FL FR FC LFE BC SL SR | 8 FL FR FC LFE BL BR SL SR
 * Layouts with speakers behind the listener get vector-based amplitude panning (VBAP,
 * Pulkki 1997) between the two speakers either side of the sound; front-only layouts
 * get a constant-power left/right pan from the sound's sideways component, so a sound
 * behind is heard on the side it is on, as DirectSound did on two speakers.
 */
#define M3D_MAXCH 8
#define M3D_PI 3.14159265358979323846

enum { MODE3D_SPEAKERS, MODE3D_STEREO, MODE3D_OFF };

typedef struct {
    int channels;            /* output channels (1..8) */
    int fl, fr;              /* channel of front left and right (-1 if none) */
    int n;                   /* speakers used for VBAP, sorted by azimuth */
    int ch[M3D_MAXCH];       /* their channels */
    double az[M3D_MAXCH];    /* their azimuths in radians, -pi..pi, + = right */
    int surround;            /* 1 if a speaker is behind the listener: VBAP */
} m3d_layout;

/* speaker azimuth in degrees for channel c of an n-channel SDL layout; 999 = LFE (not used) */
static double m3d_speaker_deg(int n, int c)
{
    static const double l1[] = { 0 };
    static const double l2[] = { -30, 30 };
    static const double l3[] = { -30, 30, 999 };
    static const double l4[] = { -30, 30, -135, 135 };
    static const double l5[] = { -30, 30, 999, -135, 135 };
    static const double l6[] = { -30, 30, 0, 999, -110, 110 };
    static const double l7[] = { -30, 30, 0, 999, 180, -90, 90 };
    static const double l8[] = { -30, 30, 0, 999, -150, 150, -90, 90 };
    static const double *const t[] = { l1, l2, l3, l4, l5, l6, l7, l8 };
    if (n < 1 || n > 8 || c < 0 || c >= n) return 999;
    return t[n - 1][c];
}

static void m3d_layout_init(m3d_layout *L, int channels)
{
    int c, i, j;
    if (channels < 1) channels = 1;
    if (channels > M3D_MAXCH) channels = M3D_MAXCH;
    L->channels = channels;
    L->fl = channels >= 2 ? 0 : -1;
    L->fr = channels >= 2 ? 1 : -1;
    L->n = 0;
    L->surround = 0;
    for (c = 0; c < channels; c++) {
        double d = m3d_speaker_deg(channels, c);
        if (d > 360) continue;
        L->ch[L->n] = c;
        L->az[L->n] = d * M3D_PI / 180.0;
        if (d > 90.5 || d < -90.5) L->surround = 1;
        L->n++;
    }
    for (i = 1; i < L->n; i++)      /* insertion sort by azimuth */
        for (j = i; j > 0 && L->az[j - 1] > L->az[j]; j--) {
            double ta = L->az[j]; int tc = L->ch[j];
            L->az[j] = L->az[j - 1]; L->ch[j] = L->ch[j - 1];
            L->az[j - 1] = ta; L->ch[j - 1] = tc;
        }
}

/* constant-power left/right pan: p = -1 full left .. +1 full right */
static void m3d_pan_lr(const m3d_layout *L, double p, float *g)
{
    double t;
    if (p < -1) p = -1;
    if (p > 1) p = 1;
    if (L->fl < 0) { g[0] = 1.0f; return; }   /* mono output */
    t = (p + 1.0) * M3D_PI / 4.0;
    g[L->fl] = (float)cos(t);
    g[L->fr] = (float)sin(t);
}

/*
 * Gains for one 3D sound at (x, y, z), into g[channels] (zeroed here). mode is
 * MODE3D_SPEAKERS, _STEREO or _OFF. The result has a total power of 1 times the
 * distance gain, so a sound does not get louder or quieter as it moves round.
 */
static void m3d_gains(const m3d_layout *L, int mode, double x, double y, double z, float *g)
{
    double d = sqrt(x * x + y * y + z * z), dist_gain = d > 1.0 ? 1.0 / d : 1.0, h, az;
    int c;
    for (c = 0; c < L->channels; c++) g[c] = 0.0f;
    if (mode == MODE3D_OFF || d < 1e-9) {
        m3d_pan_lr(L, 0.0, g);
    } else {
        h = sqrt(x * x + z * z);                 /* horizontal part */
        if (mode == MODE3D_STEREO || !L->surround || h < 1e-9) {
            m3d_pan_lr(L, x / d, g);             /* sideways component: straight up/down is centred */
        } else {
            int k, a = -1, b = -1;
            double s1x, s1z, s2x, s2z, det, g1, g2, p;
            az = atan2(x, z);
            for (k = 0; k < L->n; k++) {         /* the arc [az[k], az[k+1]] holding az, wrapping round */
                int k2 = (k + 1) % L->n;
                double lo = L->az[k], hi = L->az[k2], t = az;
                if (hi <= lo) hi += 2 * M3D_PI;
                if (t < lo) t += 2 * M3D_PI;
                if (t >= lo && t <= hi) { a = k; b = k2; break; }
            }
            if (a < 0) { m3d_pan_lr(L, x / d, g); }
            else {
                s1x = sin(L->az[a]); s1z = cos(L->az[a]);
                s2x = sin(L->az[b]); s2z = cos(L->az[b]);
                det = s1x * s2z - s2x * s1z;
                if (det > -1e-9 && det < 1e-9) { g1 = 1; g2 = 0; }
                else {
                    double sx = x / h, sz = z / h;
                    g1 = (sx * s2z - sz * s2x) / det;
                    g2 = (s1x * sz - s1z * sx) / det;
                }
                if (g1 < 0) g1 = 0;
                if (g2 < 0) g2 = 0;
                p = sqrt(g1 * g1 + g2 * g2);
                if (p < 1e-9) { g1 = 1; p = 1; }
                g[L->ch[a]] = (float)(g1 / p);
                g[L->ch[b]] += (float)(g2 / p);
            }
        }
    }
    for (c = 0; c < L->channels; c++) g[c] *= (float)dist_gain;
}

/* the game's volume 0..127 as an amplitude (EA's DLL: 0 = silent, 127 = full, linear in amplitude) */
static float m3d_volume(int v)
{
    if (v <= 0) return 0.0f;
    if (v >= 127) return 1.0f;
    return (float)v / 127.0f;
}

/*
 * One playing 3D sound. Source data is 16-bit mono at its own rate; the voice steps
 * through it at rate / out_rate per output frame (linear interpolation) and adds into
 * out (float, channels interleaved). Gains move from cur to target over the first
 * ramp frames of each call, so position and volume changes do not click.
 */
typedef struct {
    const int16_t *data;
    int frames;              /* source length */
    int loop;
    double pos;              /* position in source frames */
    double step;             /* source frames per output frame */
    float cur[M3D_MAXCH], target[M3D_MAXCH];
    int ended;               /* reached the end (not looping) */
} m3d_voice;

/* returns the number of output frames written (fewer than n if the sound ended) */
static int m3d_mix(m3d_voice *v, float *out, int channels, int n)
{
    int i, c, ramp = n < 64 ? n : 64;
    float step_g[M3D_MAXCH];
    if (!v->data || v->frames <= 0 || v->step <= 0) { v->ended = 1; return 0; }
    for (c = 0; c < channels; c++) step_g[c] = (v->target[c] - v->cur[c]) / (float)(ramp > 0 ? ramp : 1);
    for (i = 0; i < n; i++) {
        int k = (int)v->pos, k2;
        float f, s;
        if (k >= v->frames) {
            if (!v->loop) { v->ended = 1; break; }
            v->pos -= v->frames * (double)(k / v->frames);
            k = (int)v->pos;
        }
        k2 = k + 1;
        if (k2 >= v->frames) k2 = v->loop ? 0 : -1;
        f = (float)(v->pos - k);
        s = (float)v->data[k] * (1.0f - f) + (k2 >= 0 ? (float)v->data[k2] * f : 0.0f);
        s *= 1.0f / 32768.0f;
        if (i < ramp) for (c = 0; c < channels; c++) v->cur[c] += step_g[c];
        for (c = 0; c < channels; c++) out[i * channels + c] += s * v->cur[c];
        v->pos += v->step;
    }
    if (i >= ramp) for (c = 0; c < channels; c++) v->cur[c] = v->target[c];
    return i;
}
