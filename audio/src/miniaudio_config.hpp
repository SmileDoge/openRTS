#pragma once

// miniaudio with the parts openRTS does not use switched off. Included by the
// audio sources; miniaudio.cpp holds the implementation.
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include <miniaudio.h>
