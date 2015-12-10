/*
uPNG -- derived from LodePNG version 20100808

Copyright(c) 2005-2010 Lode Vandevenne
Copyright(c) 2010 Sean Middleditch

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
  claim that you wrote the original software. If you use this software
  in a product, an acknowledgment in the product documentation would be
  appreciated but is not required.

  2. Altered source versions must be plainly marked as such, and must not be
  misrepresented as being the original software.

  3. This notice may not be removed or altered from any source
  distribution.
*/

#if !defined(UPNG_H)
#define UPNG_H

#include <stdint.h>
#include <stdbool.h>

typedef enum upng_error {
 UPNG_EOK   = 0, /* success(no error) */
 UPNG_ENOMEM   = 1, /* memory allocation failed */
 UPNG_ENOTFOUND  = 2, /* resource not found(file missing) */
 UPNG_ENOTPNG  = 3, /* image data does not have a PNG header */
 UPNG_EMALFORMED  = 4, /* image data is not a valid PNG image */
 UPNG_EUNSUPPORTED = 5, /* critical PNG chunk type is not supported */
 UPNG_EUNINTERLACED = 6, /* image int32_terlacing is not supported */
 UPNG_EUNFORMAT  = 7, /* image color format is not supported */
 UPNG_EPARAM   = 8, /* invalid parameter to method call */
 UPNG_EDONE   = 9 /* completed decoding all information to end of file (IEND) */
} upng_error;

typedef enum upng_format {
 UPNG_BADFORMAT,
 UPNG_INDEXED1,
 UPNG_INDEXED2,
 UPNG_INDEXED4,
 UPNG_INDEXED8,
 UPNG_RGB8,
 UPNG_RGB16,
 UPNG_RGBA8,
 UPNG_RGBA16,
 UPNG_LUMINANCE1,
 UPNG_LUMINANCE2,
 UPNG_LUMINANCE4,
 UPNG_LUMINANCE8,
 UPNG_LUMINANCE_ALPHA1,
 UPNG_LUMINANCE_ALPHA2,
 UPNG_LUMINANCE_ALPHA4,
 UPNG_LUMINANCE_ALPHA8
} upng_format;

typedef struct upng_t upng_t;

typedef struct __attribute__((__packed__)) rgb {
  uint8_t r;
  uint8_t g;
  uint8_t b;
} rgb;


upng_t*  upng_new_from_bytes(uint8_t* source_buffer, uint32_t source_size);

void  upng_free(upng_t* upng);

upng_error upng_load(upng_t* upng);
upng_error upng_decode_image(upng_t* upng);

upng_error upng_get_error(const upng_t* upng);
uint32_t upng_get_error_line(const upng_t* upng);

uint32_t upng_get_width(const upng_t* upng);
uint32_t upng_get_height(const upng_t* upng);
int32_t upng_get_x_offset(const upng_t* upng);
int32_t upng_get_y_offset(const upng_t* upng);
uint32_t upng_get_bpp(const upng_t* upng);
uint32_t upng_get_bitdepth(const upng_t* upng);
uint32_t upng_get_components(const upng_t* upng);
uint32_t upng_get_pixelsize(const upng_t* upng);
upng_format upng_get_format(const upng_t* upng);
uint32_t upng_get_size(const upng_t* upng);

//returns palette and count of entries in palette for indexed images
int32_t upng_get_palette(const upng_t *upng, rgb **palette);

//returns(optional) alpha_palette and count of entries in alpha_palette for indexed images
int32_t upng_get_alpha_palette(const upng_t *upng, uint8_t **alpha_palette);

const uint8_t* upng_get_buffer(const upng_t* upng);

typedef enum apng_dispose_ops {
  APNG_DISPOSE_OP_NONE = 0,
  APNG_DISPOSE_OP_BACKGROUND,
  APNG_DISPOSE_OP_PREVIOUS
} apng_dispose_ops;

typedef enum apng_blend_ops {
  APNG_BLEND_OP_SOURCE = 0,
  APNG_BLEND_OP_OVER
} apng_blend_ops;



typedef struct apng_fctl {
  uint32_t sequence_number;
  uint32_t width;
  uint32_t height;
  uint32_t x_offset;
  uint32_t y_offset;
  uint16_t delay_num;
  uint16_t delay_den;
  uint8_t dispose_op;
  uint8_t blend_op;
} apng_fctl;

//returns if the png is an apng after the upng_load() function
bool upng_is_apng(const upng_t* upng);

//retuns the apng num_frames
uint32_t upng_apng_num_frames(const upng_t* upng);

// retuns the apng num_plays (0 indicates infinite looping)
uint32_t upng_apng_num_plays(const upng_t* upng);

//Pass in a apng_fctl to get the next frames frame control information
bool upng_get_apng_fctl(const upng_t* upng, apng_fctl *apng_frame_control);

#endif /*defined(UPNG_H)*/
