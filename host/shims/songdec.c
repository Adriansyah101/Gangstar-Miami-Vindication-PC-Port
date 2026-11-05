/* songdec.c -- the custom radio's own decoders for mp3, flac, wav and ogg (vendor/: dr_mp3, dr_flac, dr_wav,
   stb_vorbis; compiled in songdec_impl.c).

   They are the same code on Windows and Linux, so these songs play the same on both, and Linux needs no ffmpeg for
   them.  Media Foundation (Windows) and ffmpeg (Linux) stay for m4a, aac and wma, and for a file of these four kinds
   that the decoders here will not open.  A song comes out as 16-bit stereo at its own rate: mono is doubled, and of
   more channels the first two are kept (stb_vorbis mixes them down itself), as the Media Foundation path does. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <wchar.h>
#else
#include <strings.h>
#endif

#include <dr_mp3.h>
#include <dr_flac.h>
#include <dr_wav.h>
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

#include "shims.h"

enum { SD_MP3 = 1, SD_FLAC, SD_WAV, SD_OGG };

struct gmv_song {
    int kind;
    unsigned rate, ch;
    union { drmp3 mp3; drflac *flac; drwav wav; stb_vorbis *ogg; } u;
    int16_t *tmp;               /* the song's own channels before they become stereo */
    size_t tmpcap;
};

static const char *const sd_names[] = { "", "dr_mp3", "dr_flac", "dr_wav", "stb_vorbis" };

#ifdef _WIN32
static int sd_kind(const wchar_t *path)
{
    const wchar_t *dot = wcsrchr(path, L'.');
    if (!dot || wcschr(dot, L'\\') || wcschr(dot, L'/'))
        return 0;
    return !_wcsicmp(dot, L".mp3") ? SD_MP3 : !_wcsicmp(dot, L".flac") ? SD_FLAC : !_wcsicmp(dot, L".wav") ? SD_WAV
         : !_wcsicmp(dot, L".ogg") ? SD_OGG : 0;
}
#else
static int sd_kind(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot || strchr(dot, '/'))
        return 0;
    return !strcasecmp(dot, ".mp3") ? SD_MP3 : !strcasecmp(dot, ".flac") ? SD_FLAC : !strcasecmp(dot, ".wav") ? SD_WAV
         : !strcasecmp(dot, ".ogg") ? SD_OGG : 0;
}
#endif

int gmv_song_builtin(const gmv_pathch *path)
{
    return sd_kind(path) != 0;
}

gmv_song *gmv_song_open(const gmv_pathch *path)
{
    int kind = sd_kind(path);
    if (!kind)
        return NULL;
    gmv_song *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->kind = kind;
    int ok = 0;
    switch (kind) {
#ifdef _WIN32
    case SD_MP3:  ok = drmp3_init_file_w(&s->u.mp3, path, NULL); break;
    case SD_FLAC: ok = (s->u.flac = drflac_open_file_w(path, NULL)) != NULL; break;
    case SD_WAV:  ok = drwav_init_file_w(&s->u.wav, path, NULL); break;
    case SD_OGG: {
        FILE *f = _wfopen(path, L"rb");
        int err = 0;
        ok = f && (s->u.ogg = stb_vorbis_open_file(f, 1, &err, NULL)) != NULL;
        if (f && !ok) fclose(f);
        break;
    }
#else
    case SD_MP3:  ok = drmp3_init_file(&s->u.mp3, path, NULL); break;
    case SD_FLAC: ok = (s->u.flac = drflac_open_file(path, NULL)) != NULL; break;
    case SD_WAV:  ok = drwav_init_file(&s->u.wav, path, NULL); break;
    case SD_OGG: {
        int err = 0;
        ok = (s->u.ogg = stb_vorbis_open_filename(path, &err, NULL)) != NULL;
        break;
    }
#endif
    }
    if (ok) {
        switch (kind) {
        case SD_MP3:  s->rate = s->u.mp3.sampleRate;    s->ch = s->u.mp3.channels;  break;
        case SD_FLAC: s->rate = s->u.flac->sampleRate;  s->ch = s->u.flac->channels; break;
        case SD_WAV:  s->rate = s->u.wav.sampleRate;    s->ch = s->u.wav.channels;  break;
        case SD_OGG: {
            stb_vorbis_info i = stb_vorbis_get_info(s->u.ogg);
            s->rate = i.sample_rate;
            s->ch = (unsigned)i.channels;
            break;
        }
        }
        if (s->rate && s->ch)
            return s;
    }
    if (ok)
        gmv_song_close(s);
    else
        free(s);
    return NULL;
}

unsigned gmv_song_rate(const gmv_song *s) { return s->rate; }
unsigned gmv_song_channels(const gmv_song *s) { return s->ch; }
const char *gmv_song_decoder(const gmv_song *s) { return sd_names[s->kind]; }

size_t gmv_song_read(gmv_song *s, int16_t *out, size_t frames)
{
    if (s->kind == SD_OGG)
        return (size_t)stb_vorbis_get_samples_short_interleaved(s->u.ogg, 2, out, (int)(frames * 2));
    int16_t *in = out;
    if (s->ch > 2) {                            /* more than fits in out: read into tmp */
        if (s->tmpcap < frames * s->ch) {
            free(s->tmp);
            s->tmpcap = frames * s->ch;
            s->tmp = malloc(s->tmpcap * sizeof *s->tmp);
            if (!s->tmp) {
                s->tmpcap = 0;
                return 0;
            }
        }
        in = s->tmp;
    }
    size_t n = 0;
    switch (s->kind) {
    case SD_MP3:  n = (size_t)drmp3_read_pcm_frames_s16(&s->u.mp3, frames, in); break;
    case SD_FLAC: n = (size_t)drflac_read_pcm_frames_s16(s->u.flac, frames, in); break;
    case SD_WAV:  n = (size_t)drwav_read_pcm_frames_s16(&s->u.wav, frames, in); break;
    }
    if (s->ch == 1) {
        for (size_t i = n; i-- > 0;) {          /* backwards: mono and stereo share out */
            out[2 * i] = out[2 * i + 1] = in[i];
        }
    } else if (s->ch > 2) {
        for (size_t i = 0; i < n; i++) {
            out[2 * i]     = in[i * s->ch];
            out[2 * i + 1] = in[i * s->ch + 1];
        }
    }
    return n;
}

void gmv_song_close(gmv_song *s)
{
    if (!s)
        return;
    switch (s->kind) {
    case SD_MP3:  drmp3_uninit(&s->u.mp3); break;
    case SD_FLAC: drflac_close(s->u.flac); break;
    case SD_WAV:  drwav_uninit(&s->u.wav); break;
    case SD_OGG:  stb_vorbis_close(s->u.ogg); break;
    }
    free(s->tmp);
    free(s);
}
