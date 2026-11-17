/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Single compilation unit for the vendored stb image decoder (public domain / MIT). */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb/stb_image.h"
