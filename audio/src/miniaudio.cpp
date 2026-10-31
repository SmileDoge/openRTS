// miniaudio implementation with Ogg Vorbis through stb_vorbis (bundled with
// miniaudio), as its documentation describes. WAV, MP3 and FLAC are built in.
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio_config.hpp"

#undef STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
