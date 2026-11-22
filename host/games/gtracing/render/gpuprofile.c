/* gpuprofile.c -- GPU_PROFILE: the picture settings left on "auto" follow the kind of GPU the game runs on.

   Optimisation pass 5: MSAA 4x and the 0.65x motion blur (GT_FULL_BLUR=0) on an integrated GPU (Intel / AMD),
   unless the ini sets them, with a Motion Blur row on the Display page.  Why: a built-in GPU shares the CPU's memory
   and has a fraction of a graphics card's fill rate; its AMD driver here hung with 8x MSAA and every effect on
   (memory msaa8-igpu), and the full motion blur (FULL_BLUR) fills a whole-screen target at speed.  A port
   addition, not a fault of the game.

   The kind: the DXGI adapter the OpenGL driver is (matched by vendor, then by name) is asked by Direct3D 11 whether
   its memory is the CPU's (D3D11_FEATURE_D3D11_OPTIONS2 UnifiedMemoryArchitecture): true on Intel and AMD built-in
   GPUs, false on graphics cards.  Without an answer (no Direct3D 11.3, no matching adapter) the name decides
   (Intel but not Arc A/B, AMD "Radeon(TM) Graphics" / Vega N / 6x0M-8x0M).  GT_GPU_PROFILE=igpu / full skips the
   question (testing, or a wrong guess); auto (default) asks.

   What "auto" then means (gtracing.ini: a key missing, blank or "auto"; a value of the player's always wins):
     GT_MSAA        4x on both (8x is never chosen for the player);
     GT_FULL_BLUR   a graphics card: the full blur (FULL_BLUR); a built-in GPU: the iPad 3's lighter
                    blur (a target 0.65x the screen, two samples fewer: fullblur.c). */
#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#ifdef _WIN32
#include <dxgi.h>
#include <d3d11.h>
#endif

#include "gl.h"
#include "rt.h"
#include "features.h"


static int integrated = -1;
static SRWLOCK lock = SRWLOCK_INIT;

#ifndef _WIN32
/* Android: every phone GPU shares the CPU's memory: the built-in profile (GT_GPU_PROFILE=full: the full blur) */
static void detect(void)
{
    const char *e = getenv("GT_GPU_PROFILE"), *r = gles_gpu_renderer();
    integrated = !(e && !_stricmp(e, "full"));
    rt_log("[gpu] %s: %s profile; settings on auto: %s motion blur\n", r && *r ? r : "(no GL yet)",
           integrated ? "mobile (built-in)" : "full (GT_GPU_PROFILE=full)", integrated ? "the iPad 3's lighter" : "the full");
}
#else
static unsigned vendor_id(const char *v)
{
    if (strstr(v, "NVIDIA"))
        return 0x10de;
    if (strstr(v, "ATI") || strstr(v, "AMD"))
        return 0x1002;
    if (strstr(v, "Intel"))
        return 0x8086;
    return 0;
}

/* the name alone (no answer from Direct3D) */
static int by_name(unsigned vid, const char *r)
{
    if (vid == 0x8086)                 /* Intel: built in, but the Arc A / B cards (Arc(TM) Graphics is built in) */
        return !(strstr(r, "Arc(TM) A") || strstr(r, "Arc(TM) B") || strstr(r, "Arc A") || strstr(r, "Arc B") ||
                 strstr(r, "Iris(R) Xe MAX"));
    if (vid == 0x1002) {               /* AMD: the APUs' names */
        if (strstr(r, " RX ") || strstr(r, "Pro") || strstr(r, "Series") || strstr(r, "FirePro"))
            return 0;
        if (strstr(r, "Radeon(TM) Graphics") || strstr(r, "Radeon Graphics") || strstr(r, "Vega"))
            return 1;
        for (const char *p = r; (p = strstr(p, "Radeon")) != NULL; p++) {   /* 610M .. 890M */
            const char *d = strchr(p, ' ');
            if (d && d[1] >= '6' && d[1] <= '8' && d[2] >= '0' && d[2] <= '9' && d[3] >= '0' && d[3] <= '9' &&
                d[4] == 'M')
                return 1;
        }
        return 0;
    }
    return 0;
}

static const GUID iid_factory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};

/* 1 / 0 the adapter's memory is / is not the CPU's, -1 no answer; name: the adapter's */
static int by_d3d(unsigned vid, const char *renderer, char *name, size_t nn)
{
    typedef HRESULT(WINAPI * T_factory)(REFIID, void **);
    typedef HRESULT(WINAPI * T_device)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *,
                                       UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
    HMODULE dx = LoadLibraryA("dxgi.dll"), d3 = LoadLibraryA("d3d11.dll");
    T_factory mk = dx ? (T_factory)(void *)GetProcAddress(dx, "CreateDXGIFactory1") : NULL;
    T_device dev_mk = d3 ? (T_device)(void *)GetProcAddress(d3, "D3D11CreateDevice") : NULL;
    IDXGIFactory1 *f = NULL;
    if (!mk || !dev_mk || FAILED(mk(&iid_factory1, (void **)&f)) || !f)
        return -1;
    IDXGIAdapter1 *best = NULL;
    int best_score = -1;
    IDXGIAdapter1 *a;
    for (UINT i = 0; IDXGIFactory1_EnumAdapters1(f, i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        int score = -1;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            (!vid || d.VendorId == vid)) {
            char n[128];
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, n, sizeof n, NULL, NULL);
            score = 1 + (*n && !strncmp(renderer, n, strlen(n)));   /* the GL name starts with the adapter's */
            if (score > best_score) {
                snprintf(name, nn, "%s", n);
            }
        }
        if (score > best_score) {
            if (best)
                IDXGIAdapter1_Release(best);
            best = a, best_score = score;
        } else
            IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(f);
    if (!best)
        return -1;
    ID3D11Device *dev = NULL;
    HRESULT hr = dev_mk((IDXGIAdapter *)best, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL,
                        NULL);
    IDXGIAdapter1_Release(best);
    if (FAILED(hr) || !dev)
        return -1;
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 o2;
    memset(&o2, 0, sizeof o2);
    hr = ID3D11Device_CheckFeatureSupport(dev, D3D11_FEATURE_D3D11_OPTIONS2, &o2, sizeof o2);
    ID3D11Device_Release(dev);
    return FAILED(hr) ? -1 : o2.UnifiedMemoryArchitecture ? 1 : 0;
}

/* the answer, worked out once: on its own thread from gles_load (gpu_profile_start), so a Direct3D device's
   creation (~200 ms measured on the NVIDIA card) never holds the start up; the blur asks at the first event */
static void detect(void)
{
    {
        const char *e = getenv("GT_GPU_PROFILE"), *v = gles_gpu_vendor(), *r = gles_gpu_renderer();
        char name[128] = "";
        /* GT_GPU_TEST_AS=vendor|renderer (testing): the question asked for another of the PC's GPUs than the one
           OpenGL draws on (a laptop whose screen is on its graphics card cannot draw a window on the built-in one) */
        static char tv[64], tr[128];
        const char *ta = dev_getenv("GT_GPU_TEST_AS"), *bar = ta ? strchr(ta, '|') : NULL;
        if (bar) {
            snprintf(tv, sizeof tv, "%.*s", (int)(bar - ta), ta);
            snprintf(tr, sizeof tr, "%s", bar + 1);
            v = tv, r = tr;
            rt_log("[gpu] test: asked as %s / %s (GT_GPU_TEST_AS)\n", v, r);
        }
        if (e && !_stricmp(e, "igpu")) {
            integrated = 1;
            rt_log("[gpu] %s: built-in GPU profile (GT_GPU_PROFILE=igpu)\n", *r ? r : "(no GL yet)");
        } else if (e && !_stricmp(e, "full")) {
            integrated = 0;
            rt_log("[gpu] %s: graphics card profile (GT_GPU_PROFILE=full)\n", *r ? r : "(no GL yet)");
        } else if (vendor_id(v) == 0x10de) {   /* NVIDIA makes no built-in PC GPUs: nothing to ask */
            integrated = 0;
            rt_log("[gpu] %s: graphics card (NVIDIA); settings on auto: anti-aliasing 4x, the full motion blur\n", r);
        } else {
            LARGE_INTEGER t0, t1, fq;
            QueryPerformanceCounter(&t0);
            unsigned vid = vendor_id(v);
            int k = by_d3d(vid, r, name, sizeof name);
            QueryPerformanceCounter(&t1);
            QueryPerformanceFrequency(&fq);
            integrated = k >= 0 ? k : by_name(vid, r);
            rt_log("[gpu] %s: %s (%s, %.0f ms); settings on auto: anti-aliasing 4x, %s motion blur\n",
                   *r ? r : "(no GL yet)", integrated ? "built-in GPU" : "graphics card",
                   k >= 0 ? (k ? "its memory is the CPU's (Direct3D 11)" : "memory of its own (Direct3D 11)")
                          : "by its name",
                   (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart,
                   integrated ? "the iPad 3's lighter" : "the full");
            if (*name && strncmp(r, name, strlen(name)))
                rt_log("[gpu] (Direct3D's adapter: %s)\n", name);
        }
    }
}
#endif

static HANDLE worker;
static DWORD WINAPI worker_main(void *p)
{
    (void)p;
    AcquireSRWLockExclusive(&lock);
    if (integrated < 0)
        detect();
    ReleaseSRWLockExclusive(&lock);
    return 0;
}

/* gles.c's gles_load, once the driver's names are known */
int gpu_setting_auto(const char *key);
void gpu_profile_start(void)
{
    if (!worker && integrated < 0 && gpu_setting_auto("GT_FULL_BLUR"))   /* only the blur's auto needs the answer */
        worker = CreateThread(NULL, 0, worker_main, NULL, 0, NULL);
}

static int gpu_integrated(void)
{
    if (integrated >= 0)
        return integrated;
    if (worker)
        WaitForSingleObject(worker, 5000);
    AcquireSRWLockExclusive(&lock);
    if (integrated < 0)
        detect();                      /* no worker (no GL yet) or it did not finish: here */
    ReleaseSRWLockExclusive(&lock);
    return integrated;
}

/* a setting left on auto: a key missing, blank or "auto" */
int gpu_setting_auto(const char *key)
{
    const char *e = getenv(key);
    return !e || !*e || !_stricmp(e, "auto");
}

int gpu_auto_msaa(void) { return 4; }
int gpu_auto_full_blur(void) { return !gpu_integrated(); }
