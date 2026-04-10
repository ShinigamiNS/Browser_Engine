#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <setjmp.h>
#include <windows.h>

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

/* --------------------------------------------------------------------------
 * Thread + VEH + setjmp: handles both crashes (segfault) and infinite loops.
 *
 * VEH runs in the context of the thread that raised the exception, so longjmp
 * from within it jumps back into that same thread's stack frame.
 * --------------------------------------------------------------------------*/

/* Per-call state — only one nsvg call runs at a time (main thread waits). */
static volatile int  g_nsvg_active = 0;
static jmp_buf       g_nsvg_jmp;

static LONG NTAPI nsvg_veh(PEXCEPTION_POINTERS ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (g_nsvg_active &&
        (code == EXCEPTION_ACCESS_VIOLATION     ||
         code == EXCEPTION_STACK_OVERFLOW       ||
         code == EXCEPTION_INT_DIVIDE_BY_ZERO   ||
         code == EXCEPTION_ILLEGAL_INSTRUCTION  ||
         code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED)) {
        g_nsvg_active = 0;
        longjmp(g_nsvg_jmp, 1); /* jumps within the faulting thread */
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---- parse wrapper ---- */

struct ParseTask {
    char*       buf;
    const char* units;
    float       dpi;
    NSVGimage*  result;
};

static DWORD WINAPI parse_thread(LPVOID p) {
    struct ParseTask* t = (struct ParseTask*)p;
    PVOID veh = AddVectoredExceptionHandler(1, nsvg_veh);
    g_nsvg_active = 1;
    if (setjmp(g_nsvg_jmp) == 0) {
        t->result = nsvgParse(t->buf, t->units, t->dpi);
    }
    /* If longjmp fired, result stays NULL — thread exits cleanly */
    g_nsvg_active = 0;
    RemoveVectoredExceptionHandler(veh);
    return 0;
}

NSVGimage* safe_nsvgParse(char* buf, const char* units, float dpi) {
    struct ParseTask task;
    task.buf    = buf;
    task.units  = units;
    task.dpi    = dpi;
    task.result = NULL;

    HANDLE h = CreateThread(NULL, 512*1024, parse_thread, &task, 0, NULL);
    if (!h) return NULL;

    /* 4-second timeout handles infinite loops */
    DWORD wait = WaitForSingleObject(h, 4000);
    if (wait == WAIT_TIMEOUT) {
        TerminateThread(h, 0);
        CloseHandle(h);
        return NULL;
    }
    CloseHandle(h);
    return task.result;
}

/* ---- rasterize wrapper ---- */

struct RastTask {
    NSVGrasterizer* rast;
    NSVGimage*      image;
    float           tx, ty, scale;
    unsigned char*  dst;
    int             w, h, stride;
    int             done; /* set to 1 on success */
};

static DWORD WINAPI rast_thread(LPVOID p) {
    struct RastTask* t = (struct RastTask*)p;
    PVOID veh = AddVectoredExceptionHandler(1, nsvg_veh);
    g_nsvg_active = 1;
    if (setjmp(g_nsvg_jmp) == 0) {
        nsvgRasterize(t->rast, t->image, t->tx, t->ty, t->scale,
                      t->dst, t->w, t->h, t->stride);
        t->done = 1;
    }
    g_nsvg_active = 0;
    RemoveVectoredExceptionHandler(veh);
    return 0;
}

int safe_nsvgRasterize(NSVGrasterizer* rast, NSVGimage* image,
                       float tx, float ty, float scale,
                       unsigned char* dst, int w, int h, int stride) {
    struct RastTask task;
    task.rast   = rast;  task.image = image;
    task.tx     = tx;    task.ty    = ty;  task.scale = scale;
    task.dst    = dst;
    task.w      = w;     task.h     = h;   task.stride = stride;
    task.done   = 0;

    HANDLE hh = CreateThread(NULL, 512*1024, rast_thread, &task, 0, NULL);
    if (!hh) return 0;

    DWORD wait = WaitForSingleObject(hh, 4000);
    if (wait == WAIT_TIMEOUT) {
        TerminateThread(hh, 0);
        CloseHandle(hh);
        return 0;
    }
    CloseHandle(hh);
    return task.done;
}
