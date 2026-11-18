/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Single compilation unit for the vendored stb image decoder (public domain / MIT). */
#define STB_IMAGE_IMPLEMENTATION
/* JPEGs go through libjpeg where it's built in (the H2's player): it decodes
 * all that stb's decoder does (baseline and progressive) and more, scaling as
 * it goes, so there stb's copy would be 15 KB of fallback that never helps. */
#ifndef FATHOM_HAVE_LIBJPEG
#define STBI_ONLY_JPEG
#endif
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb/stb_image.h"
