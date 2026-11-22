/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * libjpeg-turbo 3 decodes 12- and 16-bit JPEGs too (medical and lossless
 * images; browsers don't show them either), through a second and third copy
 * of most of the decoder: 92 KB of the player's code for covers that don't
 * exist. jdmaster.c reaches them only through these set-up functions; with
 * them defined here, failing as libjpeg does for a precision it can't handle,
 * the 12- and 16-bit objects stay out of the player. Such a cover fails like
 * a damaged one (library.c's error handler), and the album shows no art. If
 * libjpeg ever pulled those objects in for something else, the link would
 * fail on the duplicates rather than misbehave.
 */
#include <stdio.h>
#include <jpeglib.h>
#include <jerror.h>

#define NO_PRECISION(name) \
    void name(j_decompress_ptr cinfo); \
    void name(j_decompress_ptr cinfo) { ERREXIT1(cinfo, JERR_BAD_PRECISION, cinfo->data_precision); }
#define NO_PRECISION_B(name) \
    void name(j_decompress_ptr cinfo, boolean b); \
    void name(j_decompress_ptr cinfo, boolean b) { (void)b; ERREXIT1(cinfo, JERR_BAD_PRECISION, cinfo->data_precision); }

NO_PRECISION_B(j12init_d_main_controller)
NO_PRECISION_B(j12init_d_coef_controller)
NO_PRECISION_B(j12init_d_post_controller)
NO_PRECISION_B(j12init_d_diff_controller)
NO_PRECISION(j12init_inverse_dct)
NO_PRECISION(j12init_upsampler)
NO_PRECISION(j12init_color_deconverter)
NO_PRECISION(j12init_1pass_quantizer)
NO_PRECISION(j12init_2pass_quantizer)
NO_PRECISION(j12init_merged_upsampler)
NO_PRECISION(j12init_lossless_decompressor)
NO_PRECISION_B(j16init_d_main_controller)
NO_PRECISION_B(j16init_d_post_controller)
NO_PRECISION_B(j16init_d_diff_controller)
NO_PRECISION(j16init_upsampler)
NO_PRECISION(j16init_color_deconverter)
NO_PRECISION(j16init_lossless_decompressor)
