/* shimsvideo.c -- the intro-movie player.

   The Mac build plays Contents/Resources/intro.mov (H.264 video + AAC audio) through QTMovie
   before the main menu.  We don't link QuickTime; instead the host decodes the file with Windows
   Media Foundation (IMFSourceReader, which uses the OS's built-in H.264/AAC decoders -- no extra
   DLLs to ship), draws each frame to the game's GL window as a letterboxed textured quad, and plays
   the audio through a stream of its own (shimsaudio.c).  gmv_intro_play() is a blocking, skippable phase that the run loop
   (shimsappkit.c) calls at the exact point it used to fire the simulated QTMovieDidEndNotification;
   when it returns, the game advances to the menu as before.

   Audio/video sync: we read samples from both streams in timestamp order (ANY_STREAM), push video
   frames into a small look-ahead queue, and feed audio to the stream as it arrives.  Reading ahead to
   fill the video queue also queues the interleaved audio ahead of the device, so pacing a video
   frame (sleeping until its presentation time) never starves the audio.

   Linux has no Media Foundation: there the system's ffmpeg program decodes the file (ffpipe_linux.c), one run for
   the pictures (raw BGRA at the movie's own frame rate) and one for the sound (16-bit stereo); frame i is shown at
   i / rate.  With no ffmpeg installed the game goes on to its menu, as with NO_INTRO.

   Toggles:  GMV_NO_INTRO=1 skips playback entirely (old behaviour).
             GMV_INTRO_FLIP=1 flips the picture vertically (if a decoder delivers the other parity).
             GMV_INTRO_TRACE=1 logs stream setup and timing. */

#ifdef _WIN32
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#endif

#include "compat.h"
#ifdef _WIN32
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#else
#include <errno.h>
#include <stdio.h>
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <GL/gl.h>

#include "log.h"
#include "shims.h"

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif

static int g_trace;

/* ---- GL 2.x entries we may need to neutralise before immediate-mode drawing -------------- */
static void (APIENTRY *p_glUseProgram)(GLuint);
static void (APIENTRY *p_glActiveTexture)(GLenum);
static void (APIENTRY *p_glBindBuffer)(GLenum, GLuint);

static void resolve_gl2(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    p_glUseProgram  = (void (APIENTRY *)(GLuint))(uintptr_t)gl_real("glUseProgram");
    p_glActiveTexture = (void (APIENTRY *)(GLenum))(uintptr_t)gl_real("glActiveTexture");
    p_glBindBuffer   = (void (APIENTRY *)(GLenum, GLuint))(uintptr_t)gl_real("glBindBuffer");
}

/* ---- video frame look-ahead queue -------------------------------------------------------- */
#define VQ_CAP 8
struct vframe { unsigned char *buf; size_t cap; int w, h, flip; LONGLONG ts; };
static struct vframe g_vq[VQ_CAP];
static int g_vq_head, g_vq_tail, g_vq_count;

static struct vframe *vq_push_slot(void)
{
    if (g_vq_count >= VQ_CAP)
        return NULL;
    struct vframe *f = &g_vq[g_vq_tail];
    g_vq_tail = (g_vq_tail + 1) % VQ_CAP;
    g_vq_count++;
    return f;
}
static struct vframe *vq_front(void) { return g_vq_count ? &g_vq[g_vq_head] : NULL; }
static void vq_pop(void) { if (g_vq_count) { g_vq_head = (g_vq_head + 1) % VQ_CAP; g_vq_count--; } }

/* ---- audio ------------------------------------------------------------------------------- */
static void *g_snd;
static size_t g_snd_ahead;              /* most PCM to hold ahead of the device: one second */

static void audio_close(void)
{
    if (g_snd) {
        gmv_snd_close(g_snd);
        g_snd = NULL;
    }
}

/* Queue one decoded PCM chunk, waiting (while pumping) if the device is a full second behind. */
static void audio_submit(const unsigned char *pcm, size_t len)
{
    if (!g_snd || !len)
        return;
    int spins = 0;
    while ((size_t)gmv_snd_queued(g_snd) > g_snd_ahead) {
#ifdef _WIN32
        MSG m;
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
#endif
        Sleep(1);
        if (++spins > 2000)             /* ~2s hard cap: never hang on a stuck device */
            return;
    }
    gmv_snd_write(g_snd, pcm, len);
}

/* PAD_MENU: a pad button pressed since the intro began also skips it (one already held then does not). */
static unsigned g_pad_held;
static int g_pad_skipped;

static int intro_pad_press(void)
{
    static DWORD last;
    float x, y;
    unsigned b;
    int hat;
    DWORD now = GetTickCount();
    if (now - last < 10)                        /* the pacing loop spins; read the pads every 10 ms */
        return 0;
    last = now;
    const char *e = gmv_getenv("GMV_PAD_MENU");
    if (!(e && *e && *e != '0') || !gmv_joy_peek(&x, &y, &b, &hat))
        return 0;
    if (b & ~g_pad_held) {
        g_pad_skipped = 1;
        return 1;
    }
    g_pad_held &= b;                            /* released ones count again */
    return 0;
}

/* After a pad skip, wait (up to 1 s) for the buttons to come up, so the press does not reach the
   game's first pad read as a new press. */
static void intro_pad_release(void)
{
    for (int i = 0; i < 100; i++) {
        float x, y;
        unsigned b;
        int hat;
        if (!gmv_joy_peek(&x, &y, &b, &hat) || !b)
            return;
        gmv_input_drain(NULL);
        Sleep(10);
    }
}

/* ---- the events during playback; sets *skip on a skip key/click/pad button (closing the window ends the game there) --- */
static int intro_pump(int *skip)
{
    if (intro_pad_press())
        *skip = 1;
    gmv_input_drain(skip);
    return 0;
}

/* ---- draw one frame as a letterboxed, aspect-correct textured quad ----------------------- */
static void draw_frame(GLuint tex, const struct vframe *f)
{
    int winw = 1024, winh = 768;
    gmv_gl_render_size(&winw, &winh);               /* the screen the frame is drawn into */
    if (winw < 1) winw = 1;
    if (winh < 1) winh = 1;

    resolve_gl2();
    if (p_glUseProgram) p_glUseProgram(0);          /* immediate mode: no shader program bound */
    if (p_glActiveTexture) p_glActiveTexture(GL_TEXTURE0);
    if (p_glBindBuffer) p_glBindBuffer(0x8892 /*GL_ARRAY_BUFFER*/, 0);

    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, f->w, f->h, 0, GL_BGRA, GL_UNSIGNED_BYTE, f->buf);

    glViewport(0, 0, winw, winh);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);  glPushMatrix(); glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_LIGHTING);
    glEnable(GL_TEXTURE_2D);
    glColor4f(1.f, 1.f, 1.f, 1.f);

    glClearColor(0.f, 0.f, 0.f, 1.f);               /* black letterbox bars */
    glClear(GL_COLOR_BUFFER_BIT);

    /* fit the frame inside the window preserving aspect ratio */
    float wa = (float)winw / (float)winh, va = (float)f->w / (float)f->h;
    float sx = 1.f, sy = 1.f;
    if (va > wa) sy = wa / va;                       /* pillar/letterbox */
    else         sx = va / wa;

    /* texcoords: row 0 of our buffer is the visual top unless the source was bottom-up (f->flip) */
    float t0 = f->flip ? 1.f : 0.f, t1 = f->flip ? 0.f : 1.f;
    glBegin(GL_QUADS);
        glTexCoord2f(0.f, t0); glVertex2f(-sx,  sy);
        glTexCoord2f(1.f, t0); glVertex2f( sx,  sy);
        glTexCoord2f(1.f, t1); glVertex2f( sx, -sy);
        glTexCoord2f(0.f, t1); glVertex2f(-sx, -sy);
    glEnd();

    glDisable(GL_TEXTURE_2D);
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW);  glPopMatrix();

    gmv_gl_present_raw();
}

/* ---- the playback clock: the first frame shown starts it; ts in 100 ns units ------------- */
static int g_clock_started;
static LONGLONG g_base_ts;
static LARGE_INTEGER g_freq, g_t0;

/* Wait until the front frame's time (or a skip), draw it and drop it from the queue. */
static void show_front(GLuint tex, struct vframe *f, int *skip)
{
    if (!g_clock_started) {
        g_clock_started = 1;
        g_base_ts = f->ts;
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_t0);
    }

    /* pace: present when wall-clock elapsed reaches this frame's media time */
    double target_ms = (double)(f->ts - g_base_ts) / 10000.0;
    for (;;) {
        if (intro_pump(skip) || *skip) break;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double elapsed_ms = (double)(now.QuadPart - g_t0.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
        if (elapsed_ms + 1.0 >= target_ms)
            break;
        double wait = target_ms - elapsed_ms;
        Sleep(wait > 8.0 ? 4 : 0);
    }

    draw_frame(tex, f);
    vq_pop();
}

static void intro_start(void)
{
    g_clock_started = 0;
    g_pad_skipped = 0;
    float x, y;
    int hat;
    if (!gmv_joy_peek(&x, &y, &g_pad_held, &hat))
        g_pad_held = 0;
}

static void intro_end(GLuint tex, int skip, int presented)
{
    if (g_trace) gmv_trace("[intro] presented %d frames (%s)\n", presented, skip ? "skipped" : "ended");
    if (g_pad_skipped) {
        gmv_log("[intro] skipped by a pad button\n");
        intro_pad_release();
    }

    /* let any already-queued audio finish briefly, unless the user skipped */
    if (!skip && g_snd) {
        for (int i = 0; i < 40; i++) {   /* up to ~0.4s tail */
            if (gmv_snd_queued(g_snd) <= 0) break;
            int s2 = 0; intro_pump(&s2);
            Sleep(10);
        }
    }

    glDeleteTextures(1, &tex);
    audio_close();
    for (int i = 0; i < VQ_CAP; i++) { free(g_vq[i].buf); g_vq[i].buf = NULL; g_vq[i].cap = 0; }
    g_vq_head = g_vq_tail = g_vq_count = 0;
}

/* The movie to play, or NULL (off, none, or not there). */
static const char *intro_path(int *flip)
{
    g_trace = 0;
    { const char *e = gmv_dbgenv("GMV_INTRO_TRACE"); g_trace = (e && *e && *e != '0'); }
    { const char *e = gmv_getenv("GMV_NO_INTRO"); if (e && *e && *e != '0') { gmv_log("[intro] skipped (NO_INTRO)\n"); return NULL; } }

    const char *path = gmv_intro_movie_path();
    if (!path || !*path) {
        if (g_trace) gmv_log("[intro] no movie path captured; nothing to play\n");
        return NULL;
    }
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        gmv_log("[intro] file not found: %s\n", path);
        return NULL;
    }
    { const char *e = gmv_getenv("GMV_INTRO_FLIP"); *flip = (e && *e && *e != '0'); }
    return path;
}

#ifdef _WIN32
/* ---- media-type setup helpers ------------------------------------------------------------ */
static HRESULT set_output(IMFSourceReader *r, DWORD stream, const GUID *major, const GUID *sub)
{
    IMFMediaType *t = NULL;
    HRESULT hr = MFCreateMediaType(&t);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, major);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, sub);
    if (SUCCEEDED(hr)) hr = IMFSourceReader_SetCurrentMediaType(r, stream, NULL, t);
    if (t) IMFMediaType_Release(t);
    return hr;
}

/* Resolve which real stream index is video and which is audio (ANY_STREAM reports the real index). */
static void resolve_streams(IMFSourceReader *r, DWORD *vidx, DWORD *aidx)
{
    *vidx = (DWORD)-1;
    *aidx = (DWORD)-1;
    for (DWORD i = 0; i < 16; i++) {
        BOOL sel = FALSE;
        if (FAILED(IMFSourceReader_GetStreamSelection(r, i, &sel)) )
            break;                      /* past the last stream */
        if (!sel)
            continue;
        IMFMediaType *t = NULL;
        if (FAILED(IMFSourceReader_GetCurrentMediaType(r, i, &t)) || !t)
            continue;
        GUID mj = {0};
        if (SUCCEEDED(IMFMediaType_GetGUID(t, &MF_MT_MAJOR_TYPE, &mj))) {
            if (IsEqualGUID(&mj, &MFMediaType_Video) && *vidx == (DWORD)-1) *vidx = i;
            else if (IsEqualGUID(&mj, &MFMediaType_Audio) && *aidx == (DWORD)-1) *aidx = i;
        }
        IMFMediaType_Release(t);
    }
}

/* Decode one video sample into a queue slot (normalised to a packed BGRA buffer). */
static void take_video(IMFSample *s, int w, int h, LONG stride, int flip, LONGLONG ts)
{
    struct vframe *f = vq_push_slot();
    if (!f)
        return;                          /* queue full: shouldn't happen (caller drains first) */
    size_t need = (size_t)w * (size_t)h * 4u;
    if (f->cap < need) {
        unsigned char *nb = realloc(f->buf, need);
        if (!nb) { g_vq_tail = (g_vq_tail + VQ_CAP - 1) % VQ_CAP; g_vq_count--; return; }
        f->buf = nb;
        f->cap = need;
    }
    f->w = w; f->h = h; f->ts = ts; f->flip = flip;

    IMFMediaBuffer *mb = NULL;
    if (FAILED(IMFSample_ConvertToContiguousBuffer(s, &mb)) || !mb) {
        g_vq_tail = (g_vq_tail + VQ_CAP - 1) % VQ_CAP; g_vq_count--;  /* undo push */
        return;
    }
    BYTE *p = NULL; DWORD maxlen = 0, curlen = 0;
    if (SUCCEEDED(IMFMediaBuffer_Lock(mb, &p, &maxlen, &curlen)) && p) {
        LONG pitch = stride ? (stride < 0 ? -stride : stride) : (LONG)w * 4;
        for (int y = 0; y < h; y++)
            memcpy(f->buf + (size_t)y * w * 4, p + (size_t)y * pitch, (size_t)w * 4);
        IMFMediaBuffer_Unlock(mb);
    } else {
        g_vq_tail = (g_vq_tail + VQ_CAP - 1) % VQ_CAP; g_vq_count--;  /* undo push */
    }
    IMFMediaBuffer_Release(mb);
}

/* Feed one audio sample to the stream. */
static void take_audio(IMFSample *s)
{
    IMFMediaBuffer *mb = NULL;
    if (FAILED(IMFSample_ConvertToContiguousBuffer(s, &mb)) || !mb)
        return;
    BYTE *p = NULL; DWORD maxlen = 0, curlen = 0;
    if (SUCCEEDED(IMFMediaBuffer_Lock(mb, &p, &maxlen, &curlen)) && p && curlen) {
        audio_submit(p, curlen);
        IMFMediaBuffer_Unlock(mb);
    }
    IMFMediaBuffer_Release(mb);
}

void gmv_intro_play(void)
{
    int flip = 0;
    const char *path = intro_path(&flip);
    if (!path)
        return;

    gmv_trace("[intro] playing %s via Media Foundation\n", path);
    gmv_gl_make_current_raw();

    /* path (may use forward slashes) -> wide, backslashes */
    WCHAR wpath[1024];
    int n = MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, 1024);
    if (n <= 0) { gmv_log("[intro] path conversion failed\n"); return; }
    for (WCHAR *w = wpath; *w; w++) if (*w == L'/') *w = L'\\';

    HRESULT hrco = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    int co_owned = (hrco == S_OK);
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        gmv_log("[intro] MFStartup failed (0x%08lx); skipping intro\n", (unsigned long)hr);
        if (co_owned) CoUninitialize();
        return;
    }

    /* Ask the Source Reader to insert the Video Processor MFT so we can request RGB32 output; the
       H.264 decoder itself only emits YUV (NV12), so without this SetCurrentMediaType(RGB32) returns
       MF_E_INVALIDMEDIATYPE. */
    IMFAttributes *rattr = NULL;
    if (SUCCEEDED(MFCreateAttributes(&rattr, 1)))
        IMFAttributes_SetUINT32(rattr, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    IMFSourceReader *reader = NULL;
    hr = MFCreateSourceReaderFromURL(wpath, rattr, &reader);
    if (rattr) IMFAttributes_Release(rattr);
    if (FAILED(hr) || !reader) {
        gmv_log("[intro] MFCreateSourceReaderFromURL failed (0x%08lx); skipping\n", (unsigned long)hr);
        MFShutdown();
        if (co_owned) CoUninitialize();
        return;
    }

    IMFSourceReader_SetStreamSelection(reader, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    IMFSourceReader_SetStreamSelection(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

    int have_audio = 0;
    if (SUCCEEDED(set_output(reader, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                             &MFMediaType_Audio, &MFAudioFormat_PCM))) {
        IMFSourceReader_SetStreamSelection(reader, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
        have_audio = 1;
    }

    hr = set_output(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                    &MFMediaType_Video, &MFVideoFormat_RGB32);
    if (FAILED(hr)) {
        gmv_log("[intro] cannot set RGB32 output (0x%08lx); skipping\n", (unsigned long)hr);
        IMFSourceReader_Release(reader);
        MFShutdown();
        if (co_owned) CoUninitialize();
        return;
    }

    /* read back the actual video geometry + stride */
    int vw = 0, vh = 0; LONG stride = 0;
    {
        IMFMediaType *vt = NULL;
        if (SUCCEEDED(IMFSourceReader_GetCurrentMediaType(reader,
                (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &vt)) && vt) {
            UINT64 fs = 0;                       /* MF_MT_FRAME_SIZE: width<<32 | height */
            if (SUCCEEDED(IMFMediaType_GetUINT64(vt, &MF_MT_FRAME_SIZE, &fs))) {
                vw = (int)(fs >> 32);
                vh = (int)(fs & 0xffffffffULL);
            }
            UINT32 st = 0;
            if (SUCCEEDED(IMFMediaType_GetUINT32(vt, &MF_MT_DEFAULT_STRIDE, &st)))
                stride = (LONG)(INT32)st;        /* stored unsigned but is a signed stride */
            else
                stride = (LONG)vw * 4;
            IMFMediaType_Release(vt);
        }
    }
    if (vw <= 0 || vh <= 0 || vw > 8192 || vh > 8192) {
        gmv_log("[intro] bad video size %dx%d; skipping\n", vw, vh);
        IMFSourceReader_Release(reader);
        MFShutdown();
        if (co_owned) CoUninitialize();
        return;
    }
    if (stride < 0) flip = !flip;       /* bottom-up source -> show upright */
    if (g_trace) gmv_trace("[intro] video %dx%d stride=%ld flip=%d audio=%d\n", vw, vh, stride, flip, have_audio);

    /* open the stream from the actual PCM format */
    if (have_audio) {
        IMFMediaType *at = NULL;
        if (SUCCEEDED(IMFSourceReader_GetCurrentMediaType(reader,
                (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &at)) && at) {
            WAVEFORMATEX *wfx = NULL; UINT32 cb = 0;
            if (SUCCEEDED(MFCreateWaveFormatExFromMFMediaType(at, &wfx, &cb, 0)) && wfx) {
                g_snd = gmv_snd_open(wfx->nSamplesPerSec, wfx->nChannels, wfx->wBitsPerSample);
                g_snd_ahead = wfx->nAvgBytesPerSec;
                if (!g_snd) {
                    have_audio = 0;
                    gmv_log("[intro] no audio stream; playing silent\n");
                } else if (g_trace) {
                    gmv_trace("[intro] audio %luHz %dch %dbit\n",
                            (unsigned long)wfx->nSamplesPerSec, wfx->nChannels, wfx->wBitsPerSample);
                }
                CoTaskMemFree(wfx);
            } else {
                have_audio = 0;
            }
            IMFMediaType_Release(at);
        }
    }

    DWORD vidx = (DWORD)-1, aidx = (DWORD)-1;
    resolve_streams(reader, &vidx, &aidx);

    GLuint tex = 0;
    glGenTextures(1, &tex);

    int skip = 0, eos = 0, presented = 0;
    intro_start();

    for (;;) {
        /* fill the look-ahead queue (reading ahead also buffers audio ahead of the device) */
        while (g_vq_count < VQ_CAP && !eos && !skip) {
            if (intro_pump(&skip)) break;
            DWORD actual = 0, flags = 0;
            LONGLONG ts = 0;
            IMFSample *sample = NULL;
            HRESULT rhr = IMFSourceReader_ReadSample(reader, (DWORD)MF_SOURCE_READER_ANY_STREAM,
                                                     0, &actual, &flags, &ts, &sample);
            if (FAILED(rhr)) { eos = 1; break; }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { eos = 1; if (sample) IMFSample_Release(sample); break; }
            if (!sample)
                continue;               /* a stream tick / gap */
            if (actual == vidx)
                take_video(sample, vw, vh, stride, flip, ts);
            else if (actual == aidx && have_audio)
                take_audio(sample);
            IMFSample_Release(sample);
        }

        struct vframe *f = vq_front();
        if (!f) {
            if (eos || skip) break;
            continue;
        }
        show_front(tex, f, &skip);
        presented++;
        if (skip)
            break;
    }

    intro_end(tex, skip, presented);

    IMFSourceReader_Release(reader);
    MFShutdown();
    if (co_owned) CoUninitialize();

    gmv_trace("[intro] done\n");
}
#else
/* ---- Linux: two ffmpeg runs, the pictures and the sound ---------------------------------- */
#define INTRO_RATE 48000                        /* the sound's rate out of ffmpeg (SDL converts it) */

/* One field of ffprobe's "key=value" lines, or 0. */
static long probe_field(const char *s, const char *key, long *den)
{
    size_t n = strlen(key);
    for (const char *p = s; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL)
        if (!strncmp(p, key, n) && p[n] == '=') {
            char *end;
            long v = strtol(p + n + 1, &end, 10);
            if (den)
                *den = *end == '/' ? strtol(end + 1, NULL, 10) : 1;
            return v;
        }
    return 0;
}

void gmv_intro_play(void)
{
    int flip = 0;
    const char *path = intro_path(&flip);
    if (!path)
        return;
    if (path[0] == '.' && path[1] == '/' && gmv_in_bundle(path + 1))
        path++;

    const char *probe[] = {"ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                           "stream=width,height,r_frame_rate", "-of", "default=nw=1", path, NULL};
    char *info = gmv_ff_output(probe);
    if (!info) {
        if (errno == ENOENT)
            gmv_log("[intro] not played: this system has no ffmpeg (the intro needs ffmpeg and ffprobe)\n");
        else
            gmv_log("[intro] not played: ffprobe could not read the movie\n");
        return;
    }
    long fden = 1, fnum = probe_field(info, "r_frame_rate", &fden);
    int vw = (int)probe_field(info, "width", NULL), vh = (int)probe_field(info, "height", NULL);
    free(info);
    if (vw <= 0 || vh <= 0 || vw > 8192 || vh > 8192 || fnum <= 0 || fden <= 0) {
        gmv_log("[intro] bad video size %dx%d or frame rate %ld/%ld; skipping\n", vw, vh, fnum, fden);
        return;
    }
    char rate[48];
    snprintf(rate, sizeof rate, "%ld/%ld", fnum, fden);

    gmv_trace("[intro] playing %s via ffmpeg\n", path);
    gmv_gl_make_current_raw();

    const char *vargs[] = {"ffmpeg", "-nostdin", "-v", "error", "-i", path, "-map", "0:v:0", "-an", "-f", "rawvideo",
                           "-pix_fmt", "bgra", "-r", rate, "pipe:1", NULL};
    const char *aargs[] = {"ffmpeg", "-nostdin", "-v", "error", "-i", path, "-map", "0:a:0", "-vn", "-f", "s16le",
                           "-ac", "2", "-ar", "48000", "pipe:1", NULL};   /* INTRO_RATE */
    int vfd, afd = -1, vpid = gmv_ff_start(vargs, &vfd), apid = -1;
    if (vpid < 0) {
        gmv_log("[intro] ffmpeg did not start (%s); skipping\n", strerror(errno));
        return;
    }
    g_snd = gmv_snd_open(INTRO_RATE, 2, 16);
    g_snd_ahead = INTRO_RATE * 4;
    if (g_snd)
        apid = gmv_ff_start(aargs, &afd);
    if (apid < 0) {
        audio_close();
        gmv_log("[intro] no audio stream; playing silent\n");
    }
    if (g_trace) gmv_trace("[intro] video %dx%d at %s fps flip=%d audio=%d\n", vw, vh, rate, flip, apid >= 0);

    GLuint tex = 0;
    glGenTextures(1, &tex);

    size_t fbytes = (size_t)vw * vh * 4;
    long long frame = 0;
    int skip = 0, eos = 0, presented = 0;
    intro_start();

    for (;;) {
        /* the pictures a few frames ahead */
        while (g_vq_count < VQ_CAP && !eos && !skip) {
            if (intro_pump(&skip)) break;
            struct vframe *f = vq_push_slot();
            if (f->cap < fbytes) {
                unsigned char *nb = realloc(f->buf, fbytes);
                if (nb) {
                    f->buf = nb;
                    f->cap = fbytes;
                }
            }
            if (f->cap < fbytes || gmv_ff_read(vfd, f->buf, fbytes) < fbytes) {
                g_vq_tail = (g_vq_tail + VQ_CAP - 1) % VQ_CAP; g_vq_count--;  /* undo push */
                eos = 1;
                break;
            }
            f->w = vw; f->h = vh; f->flip = flip;
            f->ts = (LONGLONG)(frame++ * 10000000LL * fden / fnum);
        }

        /* the sound up to a second ahead of the device */
        while (afd >= 0 && !skip && (size_t)gmv_snd_queued(g_snd) < g_snd_ahead) {
            unsigned char pcm[16384];
            size_t k = gmv_ff_read(afd, pcm, sizeof pcm) & ~(size_t)3;
            if (k)
                audio_submit(pcm, k);
            if (k < sizeof pcm) {
                gmv_ff_stop(apid, afd);
                afd = apid = -1;
            }
        }

        struct vframe *f = vq_front();
        if (!f) {
            if (eos || skip) break;
            continue;
        }
        show_front(tex, f, &skip);
        presented++;
        if (skip)
            break;
    }

    intro_end(tex, skip, presented);
    gmv_ff_stop(vpid, vfd);
    if (afd >= 0)
        gmv_ff_stop(apid, afd);
    gmv_trace("[intro] done\n");
}
#endif
