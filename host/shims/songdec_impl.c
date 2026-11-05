/* songdec_impl.c -- the code of the decoders in vendor/ (dr_mp3, dr_flac, dr_wav, stb_vorbis), compiled once here.
   songdec.c holds what the host does with them.  vendor/ is a system include folder, so their warnings stay quiet. */

#define DR_MP3_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#define DR_WAV_IMPLEMENTATION
#define DR_FLAC_NO_OGG          /* Ogg goes to stb_vorbis; an Ogg FLAC is rare enough to leave to MF/ffmpeg */
#include <dr_mp3.h>
#include <dr_flac.h>
#include <dr_wav.h>

#define STB_VORBIS_NO_PUSHDATA_API
#include <stb_vorbis.c>
