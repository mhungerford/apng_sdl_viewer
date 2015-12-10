/*
uPNG -- derived from LodePNG version 20100808

Copyright (c) 2005-2010 Lode Vandevenne
Copyright (c) 2010 Sean Middleditch

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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>
#include <stdbool.h>

#include "upng.h"

//smaller decompressor
//saves about 900 bytes, but still crashing watch
//#include "tinfl.h"
//#define TINFL 1

#define MAKE_BYTE(b) ((b) & 0xFF)
#define MAKE_DWORD(a,b,c,d) \
  ((MAKE_BYTE(a) << 24) | (MAKE_BYTE(b) << 16) | (MAKE_BYTE(c) << 8) | MAKE_BYTE(d))

#define MAKE_DWORD_PTR(p) MAKE_DWORD((p)[0], (p)[1], (p)[2], (p)[3])
#define MAKE_DSHORT_PTR(p) ((MAKE_BYTE((p)[0]) << 8) | MAKE_BYTE((p)[1]))

#define CHUNK_IHDR MAKE_DWORD('I','H','D','R')
#define CHUNK_IDAT MAKE_DWORD('I','D','A','T')
#define CHUNK_PLTE MAKE_DWORD('P','L','T','E')
#define CHUNK_OFFS MAKE_DWORD('o','F','F','s') //Offset chunk
#define CHUNK_IEND MAKE_DWORD('I','E','N','D')
#define CHUNK_TRNS MAKE_DWORD('t','R','N','S') //Alpha transparency for palettized images
#define CHUNK_ACTL MAKE_DWORD('a','c','T','L') //animation control (APNG)
#define CHUNK_ADTL MAKE_DWORD('a','d','T','L') //animation duration (APNG)
#define CHUNK_FDAT MAKE_DWORD('f','d','A','T') // Frame Data (APNG)
#define CHUNK_FCTL MAKE_DWORD('f','c','T','L') //frame control (APNG)

#define FIRST_LENGTH_CODE_INDEX 257
#define LAST_LENGTH_CODE_INDEX 285

/*256 literals, the end code, some length codes, and 2 unused codes */
#define NUM_DEFLATE_CODE_SYMBOLS 288	
/*the distance codes have their own symbols, 30 used, 2 unused */
#define NUM_DISTANCE_SYMBOLS 32	
/*the code length codes. 0-15: code lengths, 16: copy previous 3-6 times, 
 * 17: 3-10 zeros, 18: 11-138 zeros */
#define NUM_CODE_LENGTH_CODES 19	
/* largest number of symbols used by any tree type */
#define MAX_SYMBOLS 288 

#define DEFLATE_CODE_BITLEN 15
#define DISTANCE_BITLEN 15
#define CODE_LENGTH_BITLEN 7
#define MAX_BIT_LENGTH 15 // bug? 15 /* largest bitlen used by any tree type */

#define DEFLATE_CODE_BUFFER_SIZE (NUM_DEFLATE_CODE_SYMBOLS * 2)
#define DISTANCE_BUFFER_SIZE (NUM_DISTANCE_SYMBOLS * 2)
#define CODE_LENGTH_BUFFER_SIZE (NUM_DISTANCE_SYMBOLS * 2)

#define SET_ERROR(upng,code) do {(upng)->error = (code); (upng)->error_line = __LINE__;} while (0)

#define upng_chunk_data_length(chunk) MAKE_DWORD_PTR(chunk)
#define upng_chunk_type(chunk) MAKE_DWORD_PTR((chunk) + 4)
#define upng_chunk_data(chunk) ((chunk) + 8)

#define upng_chunk_type_critical(chunk_type) (((chunk_type) & 0x20000000) == 0)

typedef enum upng_state {
	UPNG_ERROR		= -1,
	UPNG_DECODED	= 0,
  UPNG_LOADED   = 1, // Global data loaded (Palette) (APNG control data)
	UPNG_HEADER		= 2,
	UPNG_NEW		= 3
} upng_state;

typedef enum upng_color {
	UPNG_LUM		= 0,
	UPNG_RGB		= 2,
	UPNG_PLT		= 3,
	UPNG_LUMA		= 4,
	UPNG_RGBA		= 6
} upng_color;

typedef struct upng_source {
	uint8_t*	buffer;
	uint32_t			size;
	char					owning;
} upng_source;

struct upng_t {
	uint32_t		width;
	uint32_t		height;

  int32_t x_offset;
  int32_t y_offset;

  rgb *palette;
  uint8_t palette_entries;

  uint8_t *alpha_palette;
  uint8_t alpha_palette_entries;

	upng_color		color_type;
	uint32_t		color_depth;
	upng_format		format;

  uint8_t* cursor; //data cursor for parsing linearly
	uint8_t*	buffer;
	uint32_t	size;

  // APNG information for image at current frame
  bool is_apng;
  apng_fctl* apng_frame_control;
  uint32_t apng_num_frames;
  uint32_t apng_duration_ms;

	upng_error		error;
	uint32_t		error_line;

	upng_state		state;
	upng_source		source;
};

#ifndef TINFL
typedef struct huffman_tree {
	uint16_t* tree2d;
	uint16_t maxbitlen;	/*maximum number of bits a single code can get */
	uint16_t numcodes;	/*number of symbols in the alphabet = number of codes */
} huffman_tree;

/*the base lengths represented by codes 257-285 */
static const uint16_t LENGTH_BASE[29] = {	
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
	67, 83, 99, 115, 131, 163, 195, 227, 258
};

/*the extra bits used by codes 257-285 (added to base length) */
static const uint16_t LENGTH_EXTRA[29] = {	
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5,
	5, 5, 5, 0
};

/*the base backwards distances 
 * (the bits of distance codes appear after length codes and use their own huffman tree) */
static const uint16_t DISTANCE_BASE[30] = {	
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
	769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};

/*the extra bits of backwards distances (added to base) */
static const uint16_t DISTANCE_EXTRA[30] = {	
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
	11, 11, 12, 12, 13, 13
};

/*the order in which "code length alphabet code lengths" are stored, 
 * out of this the huffman tree of the dynamic huffman tree lengths is generated */
static const uint16_t CLCL[NUM_CODE_LENGTH_CODES]	= { 
  16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

static const uint16_t FIXED_DEFLATE_CODE_TREE[NUM_DEFLATE_CODE_SYMBOLS * 2] = {
	289, 370, 290, 307, 546, 291, 561, 292, 293, 300, 294, 297, 295, 296, 0, 1,
	2, 3, 298, 299, 4, 5, 6, 7, 301, 304, 302, 303, 8, 9, 10, 11, 305, 306, 12,
	13, 14, 15, 308, 339, 309, 324, 310, 317, 311, 314, 312, 313, 16, 17, 18,
	19, 315, 316, 20, 21, 22, 23, 318, 321, 319, 320, 24, 25, 26, 27, 322, 323,
	28, 29, 30, 31, 325, 332, 326, 329, 327, 328, 32, 33, 34, 35, 330, 331, 36,
	37, 38, 39, 333, 336, 334, 335, 40, 41, 42, 43, 337, 338, 44, 45, 46, 47,
	340, 355, 341, 348, 342, 345, 343, 344, 48, 49, 50, 51, 346, 347, 52, 53,
	54, 55, 349, 352, 350, 351, 56, 57, 58, 59, 353, 354, 60, 61, 62, 63, 356,
	363, 357, 360, 358, 359, 64, 65, 66, 67, 361, 362, 68, 69, 70, 71, 364,
	367, 365, 366, 72, 73, 74, 75, 368, 369, 76, 77, 78, 79, 371, 434, 372,
	403, 373, 388, 374, 381, 375, 378, 376, 377, 80, 81, 82, 83, 379, 380, 84,
	85, 86, 87, 382, 385, 383, 384, 88, 89, 90, 91, 386, 387, 92, 93, 94, 95,
	389, 396, 390, 393, 391, 392, 96, 97, 98, 99, 394, 395, 100, 101, 102, 103,
	397, 400, 398, 399, 104, 105, 106, 107, 401, 402, 108, 109, 110, 111, 404,
	419, 405, 412, 406, 409, 407, 408, 112, 113, 114, 115, 410, 411, 116, 117,
	118, 119, 413, 416, 414, 415, 120, 121, 122, 123, 417, 418, 124, 125, 126,
	127, 420, 427, 421, 424, 422, 423, 128, 129, 130, 131, 425, 426, 132, 133,
	134, 135, 428, 431, 429, 430, 136, 137, 138, 139, 432, 433, 140, 141, 142,
	143, 435, 483, 436, 452, 568, 437, 438, 445, 439, 442, 440, 441, 144, 145,
	146, 147, 443, 444, 148, 149, 150, 151, 446, 449, 447, 448, 152, 153, 154,
	155, 450, 451, 156, 157, 158, 159, 453, 468, 454, 461, 455, 458, 456, 457,
	160, 161, 162, 163, 459, 460, 164, 165, 166, 167, 462, 465, 463, 464, 168,
	169, 170, 171, 466, 467, 172, 173, 174, 175, 469, 476, 470, 473, 471, 472,
	176, 177, 178, 179, 474, 475, 180, 181, 182, 183, 477, 480, 478, 479, 184,
	185, 186, 187, 481, 482, 188, 189, 190, 191, 484, 515, 485, 500, 486, 493,
	487, 490, 488, 489, 192, 193, 194, 195, 491, 492, 196, 197, 198, 199, 494,
	497, 495, 496, 200, 201, 202, 203, 498, 499, 204, 205, 206, 207, 501, 508,
	502, 505, 503, 504, 208, 209, 210, 211, 506, 507, 212, 213, 214, 215, 509,
	512, 510, 511, 216, 217, 218, 219, 513, 514, 220, 221, 222, 223, 516, 531,
	517, 524, 518, 521, 519, 520, 224, 225, 226, 227, 522, 523, 228, 229, 230,
	231, 525, 528, 526, 527, 232, 233, 234, 235, 529, 530, 236, 237, 238, 239,
	532, 539, 533, 536, 534, 535, 240, 241, 242, 243, 537, 538, 244, 245, 246,
	247, 540, 543, 541, 542, 248, 249, 250, 251, 544, 545, 252, 253, 254, 255,
	547, 554, 548, 551, 549, 550, 256, 257, 258, 259, 552, 553, 260, 261, 262,
	263, 555, 558, 556, 557, 264, 265, 266, 267, 559, 560, 268, 269, 270, 271,
	562, 565, 563, 564, 272, 273, 274, 275, 566, 567, 276, 277, 278, 279, 569,
	572, 570, 571, 280, 281, 282, 283, 573, 574, 284, 285, 286, 287, 0, 0
};

static const uint16_t FIXED_DISTANCE_TREE[NUM_DISTANCE_SYMBOLS * 2] = {
	33, 48, 34, 41, 35, 38, 36, 37, 0, 1, 2, 3, 39, 40, 4, 5, 6, 7, 42, 45, 43,
	44, 8, 9, 10, 11, 46, 47, 12, 13, 14, 15, 49, 56, 50, 53, 51, 52, 16, 17,
	18, 19, 54, 55, 20, 21, 22, 23, 57, 60, 58, 59, 24, 25, 26, 27, 61, 62, 28,
	29, 30, 31, 0, 0
};
#endif

static uint8_t read_bit(uint32_t *bitpointer, const uint8_t *bitstream) {
	uint8_t result = 
    (uint8_t)((bitstream[(*bitpointer) >> 3] >> ((*bitpointer) & 0x7)) & 1);
	(*bitpointer)++;
	return result;
}

#ifndef TINFL
static uint32_t read_bits(uint32_t *bitpointer, 
    const uint8_t *bitstream, uint32_t nbits) {
	uint32_t result = 0, i;
	for (i = 0; i < nbits; i++)
		result |= ((uint32_t)read_bit(bitpointer, bitstream)) << i;
	return result;
}

/* the buffer must be numcodes*2 in size! */
static void huffman_tree_init(huffman_tree* tree, uint16_t* buffer, 
    uint16_t numcodes, uint16_t maxbitlen) {
	tree->tree2d = buffer;

	tree->numcodes = numcodes;
	tree->maxbitlen = maxbitlen;
}

/*given the code lengths (as stored in the PNG file), generate the tree as defined by Deflate.
 * maxbitlen is the maximum bits that a code in the tree can have. return value is error.*/
static void huffman_tree_create_lengths(upng_t* upng, huffman_tree* tree, const uint16_t *bitlen) {
	uint16_t* tree1d = malloc(sizeof(uint16_t) * MAX_SYMBOLS);
  uint16_t blcount[MAX_BIT_LENGTH];
  uint16_t nextcode[MAX_BIT_LENGTH];
  if (!tree1d) {
		SET_ERROR(upng, UPNG_ENOMEM);
		return;
  }

	uint16_t bits, n, i;
	uint16_t nodefilled = 0;	/*up to which node it is filled */
	uint16_t treepos = 0;	/*position in the tree (1 of the numcodes columns) */

	/* initialize local vectors */
	memset(blcount, 0, sizeof(uint16_t) * MAX_BIT_LENGTH);
	memset(nextcode, 0, sizeof(uint16_t) * MAX_BIT_LENGTH);

	/*step 1: count number of instances of each code length */
	for (bits = 0; bits < tree->numcodes; bits++) {
		blcount[bitlen[bits]]++;
	}

	/*step 2: generate the nextcode values */
	for (bits = 1; bits <= tree->maxbitlen; bits++) {
		nextcode[bits] = (nextcode[bits - 1] + blcount[bits - 1]) << 1;
	}

	/*step 3: generate all the codes */
	for (n = 0; n < tree->numcodes; n++) {
		if (bitlen[n] != 0) {
			tree1d[n] = nextcode[bitlen[n]]++;
		}
	}

	/*convert tree1d[] to tree2d[][]. In the 2D array, a value of 32767 means uninited, 
   * a value >= numcodes is an address to another bit, a value < numcodes is a code. 
   * The 2 rows are the 2 possible bit values (0 or 1), there are as many columns as codes - 1
	 * a good huffmann tree has N * 2 - 1 nodes, of which N - 1 are internal nodes. 
   * Here, the internal nodes are stored (what their 0 and 1 option point to).
   * There is only memory for such good tree currently, if there are more nodes
   * (due to too long length codes), error 55 will happen */
	for (n = 0; n < tree->numcodes * 2; n++) {
		tree->tree2d[n] = 32767;	/*32767 here means the tree2d isn't filled there yet */
	}

	for (n = 0; n < tree->numcodes; n++) {	/*the codes */
		for (i = 0; i < bitlen[n]; i++) {	/*the bits for this code */
			uint8_t bit = (uint8_t)((tree1d[n] >> (bitlen[n] - i - 1)) & 1);
			/* check if oversubscribed */
			if (treepos > tree->numcodes - 2) {
				SET_ERROR(upng, UPNG_EMALFORMED);
				return;
			}

			if (tree->tree2d[2 * treepos + bit] == 32767) {	/*not yet filled in */
				if (i + 1 == bitlen[n]) {	/*last bit */
					tree->tree2d[2 * treepos + bit] = n;	/*put the current code in it */
					treepos = 0;
				} else {	
          /*put address of the next step in here, 
           * first that address has to be found of course (it's just nodefilled + 1)... */
					nodefilled++;
          /*addresses encoded with numcodes added to it */
					tree->tree2d[2 * treepos + bit] = nodefilled + tree->numcodes;	
					treepos = nodefilled;
				}
			} else {
				treepos = tree->tree2d[2 * treepos + bit] - tree->numcodes;
			}
		}
	}

	for (n = 0; n < tree->numcodes * 2; n++) {
		if (tree->tree2d[n] == 32767) {
			tree->tree2d[n] = 0;	/*remove possible remaining 32767's */
		}
	}
	free(tree1d);
}

static uint16_t huffman_decode_symbol(upng_t *upng, const uint8_t *in, 
    uint32_t *bp, const huffman_tree* codetree, uint32_t inlength) {
	uint16_t treepos = 0, ct;
	uint8_t bit;
	for (;;) {
		/* error: end of input memory reached without endcode */
		if (((*bp) & 0x07) == 0 && ((*bp) >> 3) > inlength) {
			SET_ERROR(upng, UPNG_EMALFORMED);
			return 0;
		}

		bit = read_bit(bp, in);

		ct = codetree->tree2d[(treepos << 1) | bit];
		if (ct < codetree->numcodes) {
			return ct;
		}

		treepos = ct - codetree->numcodes;
		if (treepos >= codetree->numcodes) {
			SET_ERROR(upng, UPNG_EMALFORMED);
			return 0;
		}
	}
}

/* get the tree of a deflated block with dynamic tree, 
 * the tree itself is also Huffman compressed with a known tree*/
static void get_tree_inflate_dynamic(upng_t* upng, huffman_tree* codetree, huffman_tree* codetreeD,
    huffman_tree* codelengthcodetree, const uint8_t *in, uint32_t *bp, 
    uint32_t inlength) {
	uint16_t codelengthcode[NUM_CODE_LENGTH_CODES];
	uint16_t* bitlen = (uint16_t*)malloc(sizeof(uint16_t) * NUM_DEFLATE_CODE_SYMBOLS);
	uint16_t bitlenD[NUM_DISTANCE_SYMBOLS];
  
  if (!bitlen) {
		SET_ERROR(upng, UPNG_ENOMEM);
		return;
  }

  uint16_t n, hlit, hdist, hclen, i;

	/* make sure that length values that aren't filled in will be 0, or a wrong tree will be generated
	 * C-code note: use no "return" between ctor and dtor of an uivector! */
	if ((*bp) >> 3 >= inlength - 2) {
		SET_ERROR(upng, UPNG_EMALFORMED);
		return;
	}

	/* clear bitlen arrays */
	memset(bitlen, 0, sizeof(uint16_t) * NUM_DEFLATE_CODE_SYMBOLS);
	memset(bitlenD, 0, sizeof(uint16_t) * NUM_DISTANCE_SYMBOLS);

	/*the bit pointer is or will go past the memory */
  /*number of literal/length codes + 257. 
   * Unlike the spec, the value 257 is added to it here already */
	hlit = read_bits(bp, in, 5) + 257;	
  /*number of distance codes. Unlike the spec, the value 1 is added to it here already */
	hdist = read_bits(bp, in, 5) + 1;	
  /*number of code length codes. Unlike the spec, the value 4 is added to it here already */
	hclen = read_bits(bp, in, 4) + 4;	

	for (i = 0; i < NUM_CODE_LENGTH_CODES; i++) {
		if (i < hclen) {
			codelengthcode[CLCL[i]] = read_bits(bp, in, 3);
		} else {
			codelengthcode[CLCL[i]] = 0;	/*if not, it must stay 0 */
		}
	}

	huffman_tree_create_lengths(upng, codelengthcodetree, codelengthcode);


	/* bail now if we encountered an error earlier */
	if (upng->error != UPNG_EOK) {
		return;
	}


	/*now we can use this tree to read the lengths for the tree that this function will return */
	i = 0;
  /*i is the current symbol we're reading in the part that contains 
   * the code lengths of lit/len codes and dist codes */
	while (i < hlit + hdist) {	
		uint16_t code = huffman_decode_symbol(upng, in, bp, codelengthcodetree, inlength);
		if (upng->error != UPNG_EOK) {
			break;
		}

		if (code <= 15) {	/*a length code */
			if (i < hlit) {
				bitlen[i] = code;
			} else {
				bitlenD[i - hlit] = code;
			}
			i++;
		} else if (code == 16) {	/*repeat previous */
			uint16_t replength = 3;	/*read in the 2 bits that indicate repeat length (3-6) */
			uint16_t value;	/*set value to the previous code */

			if ((*bp) >> 3 >= inlength) {
				SET_ERROR(upng, UPNG_EMALFORMED);
				break;
			}
			/*error, bit pointer jumps past memory */
			replength += read_bits(bp, in, 2);

			if ((i - 1) < hlit) {
				value = bitlen[i - 1];
			} else {
				value = bitlenD[i - hlit - 1];
			}

			/*repeat this value in the next lengths */
			for (n = 0; n < replength; n++) {
				/* i is larger than the amount of codes */
				if (i >= hlit + hdist) {
					SET_ERROR(upng, UPNG_EMALFORMED);
					break;
				}

				if (i < hlit) {
					bitlen[i] = value;
				} else {
					bitlenD[i - hlit] = value;
				}
				i++;
			}
		} else if (code == 17) {	/*repeat "0" 3-10 times */
			uint16_t replength = 3;	/*read in the bits that indicate repeat length */
			if ((*bp) >> 3 >= inlength) {
				SET_ERROR(upng, UPNG_EMALFORMED);
				break;
			}

			/*error, bit pointer jumps past memory */
			replength += read_bits(bp, in, 3);

			/*repeat this value in the next lengths */
			for (n = 0; n < replength; n++) {
				/* error: i is larger than the amount of codes */
				if (i >= hlit + hdist) {
					SET_ERROR(upng, UPNG_EMALFORMED);
					break;
				}

				if (i < hlit) {
					bitlen[i] = 0;
				} else {
					bitlenD[i - hlit] = 0;
				}
				i++;
			}
		} else if (code == 18) {	/*repeat "0" 11-138 times */
			uint16_t replength = 11;	/*read in the bits that indicate repeat length */
			/* error, bit pointer jumps past memory */
			if ((*bp) >> 3 >= inlength) {
				SET_ERROR(upng, UPNG_EMALFORMED);
				break;
			}

			replength += read_bits(bp, in, 7);

			/*repeat this value in the next lengths */
			for (n = 0; n < replength; n++) {
				/* i is larger than the amount of codes */
				if (i >= hlit + hdist) {
					SET_ERROR(upng, UPNG_EMALFORMED);
					break;
				}
				if (i < hlit)
					bitlen[i] = 0;
				else
					bitlenD[i - hlit] = 0;
				i++;
			}
		} else {
			/* somehow an unexisting code appeared. This can never happen. */
			SET_ERROR(upng, UPNG_EMALFORMED);
			break;
		}
	}

	if (upng->error == UPNG_EOK && bitlen[256] == 0) {
		SET_ERROR(upng, UPNG_EMALFORMED);
	}

	/*the length of the end code 256 must be larger than 0 */
	/*now we've finally got hlit and hdist, so generate the code trees, and the function is done */
	if (upng->error == UPNG_EOK) {
		huffman_tree_create_lengths(upng, codetree, bitlen);
	}
	if (upng->error == UPNG_EOK) {
		huffman_tree_create_lengths(upng, codetreeD, bitlenD);
	}
	free(bitlen);
}

/*inflate a block with dynamic of fixed Huffman tree*/
static void inflate_huffman(upng_t* upng, uint8_t* out, uint32_t outsize, 
    const uint8_t *in, uint32_t *bp, uint32_t *pos, uint32_t inlength, 
    uint16_t btype) {
  //Converted to malloc, was overflowing 2k stack on Pebble
	uint16_t* codetree_buffer = (uint16_t*)malloc(sizeof(uint16_t) * DEFLATE_CODE_BUFFER_SIZE);
	uint16_t codetreeD_buffer[DISTANCE_BUFFER_SIZE];
  if (codetree_buffer == NULL) {
		SET_ERROR(upng, UPNG_ENOMEM);
		return;
  }
	uint16_t done = 0;

	huffman_tree codetree;
	huffman_tree codetreeD;


	if (btype == 1) {
		/* fixed trees */
		huffman_tree_init(&codetree, 
                      (uint16_t*)FIXED_DEFLATE_CODE_TREE, NUM_DEFLATE_CODE_SYMBOLS, 
                      DEFLATE_CODE_BITLEN);
		huffman_tree_init(&codetreeD, 
                      (uint16_t*)FIXED_DISTANCE_TREE, NUM_DISTANCE_SYMBOLS, 
                      DISTANCE_BITLEN);
	} else if (btype == 2) {
		/* dynamic trees */
		uint16_t codelengthcodetree_buffer[CODE_LENGTH_BUFFER_SIZE];
		huffman_tree codelengthcodetree;


		huffman_tree_init(&codetree, codetree_buffer, NUM_DEFLATE_CODE_SYMBOLS, DEFLATE_CODE_BITLEN);

		
    huffman_tree_init(&codetreeD, codetreeD_buffer, NUM_DISTANCE_SYMBOLS, DISTANCE_BITLEN);
		huffman_tree_init(&codelengthcodetree, codelengthcodetree_buffer, NUM_CODE_LENGTH_CODES, 
                      CODE_LENGTH_BITLEN);
    
    get_tree_inflate_dynamic(upng, &codetree, &codetreeD, &codelengthcodetree, in, bp, inlength);
	}


	while (done == 0) {
		uint16_t code = huffman_decode_symbol(upng, in, bp, &codetree, inlength);
		if (upng->error != UPNG_EOK) {
			return;
		}

		if (code == 256) {
			/* end code */
			done = 1;
		} else if (code <= 255) {
			/* literal symbol */
			if ((*pos) >= outsize) {
				SET_ERROR(upng, UPNG_EMALFORMED);
				return;
			}

			/* store output */
			out[(*pos)++] = (uint8_t)(code);
		} else if (code >= FIRST_LENGTH_CODE_INDEX && code <= LAST_LENGTH_CODE_INDEX) {	/*length code */
			/* part 1: get length base */
			uint32_t length = LENGTH_BASE[code - FIRST_LENGTH_CODE_INDEX];
			uint16_t codeD, distance, numextrabitsD;
			uint32_t start, forward, backward, numextrabits;

			/* part 2: get extra bits and add the value of that to length */
			numextrabits = LENGTH_EXTRA[code - FIRST_LENGTH_CODE_INDEX];

			/* error, bit pointer will jump past memory */
			if (((*bp) >> 3) >= inlength) {
				SET_ERROR(upng, UPNG_EMALFORMED);
				return;
			}
			length += read_bits(bp, in, numextrabits);

			/*part 3: get distance code */
			codeD = huffman_decode_symbol(upng, in, bp, &codetreeD, inlength);
			if (upng->error != UPNG_EOK) {
				return;
			}

			/* invalid distance code (30-31 are never used) */
			if (codeD > 29) {
				SET_ERROR(upng, UPNG_EMALFORMED);
				return;
			}

			distance = DISTANCE_BASE[codeD];

			/*part 4: get extra bits from distance */
			numextrabitsD = DISTANCE_EXTRA[codeD];

			/* error, bit pointer will jump past memory */
			if (((*bp) >> 3) >= inlength) {
				SET_ERROR(upng, UPNG_EMALFORMED);
				return;
			}

			distance += read_bits(bp, in, numextrabitsD);

			/*part 5: fill in all the out[n] values based on the length and dist */
			start = (*pos);
			backward = start - distance;

			if ((*pos) + length > outsize) {
				SET_ERROR(upng, UPNG_EMALFORMED);
				return;
			}

			for (forward = 0; forward < length; forward++) {
				out[(*pos)++] = out[backward];
				backward++;

				if (backward >= start) {
					backward = start - distance;
				}
			}
		}
	}

  free(codetree_buffer);
  return;
}
#endif //ifdef TINFL

static void inflate_uncompressed(upng_t* upng, uint8_t* out, uint32_t outsize, 
    const uint8_t *in, uint32_t *bp, uint32_t *pos, uint32_t inlength) {
	uint32_t p;
	uint16_t len, nlen, n;

	/* go to first boundary of byte */
	while (((*bp) & 0x7) != 0) {
		(*bp)++;
	}
	p = (*bp) / 8;		/*byte position */

	/* read len (2 bytes) and nlen (2 bytes) */
	if (p >= inlength - 4) {
		SET_ERROR(upng, UPNG_EMALFORMED);
		return;
	}

	len = in[p] + 256 * in[p + 1];
	p += 2;
	nlen = in[p] + 256 * in[p + 1];
	p += 2;

	/* check if 16-bit nlen is really the one's complement of len */
	if (len + nlen != 65535) {
		SET_ERROR(upng, UPNG_EMALFORMED);
		return;
	}

	if ((*pos) + len >= outsize) {
		SET_ERROR(upng, UPNG_EMALFORMED);
		return;
	}

	/* read the literal data: len bytes are now stored in the out buffer */
	if (p + len > inlength) {
		SET_ERROR(upng, UPNG_EMALFORMED);
		return;
	}

	for (n = 0; n < len; n++) {
		out[(*pos)++] = in[p++];
	}

	(*bp) = p * 8;
}

/*inflate the deflated data (cfr. deflate spec); return value is the error*/
static upng_error uz_inflate_data(upng_t* upng, uint8_t* out, uint32_t outsize, 
    const uint8_t *in, uint32_t insize, uint32_t inpos) {
  /*bit pointer in the "in" data, current byte is bp >> 3, 
   * current bit is bp & 0x7 (from lsb to msb of the byte) */
	uint32_t bp = 0;	
	uint32_t pos = 0;	/*byte position in the out buffer */

	uint16_t done = 0;

	while (done == 0) {
		uint16_t btype;

		/* ensure next bit doesn't point past the end of the buffer */
		if ((bp >> 3) >= insize) {
			SET_ERROR(upng, UPNG_EMALFORMED);
			return upng->error;
		}

		/* read block control bits */
		done = read_bit(&bp, &in[inpos]);
		btype = read_bit(&bp, &in[inpos]) | (read_bit(&bp, &in[inpos]) << 1);

		/* process control type appropriateyly */
		if (btype == 3) {
			SET_ERROR(upng, UPNG_EMALFORMED);
			return upng->error;
		} else if (btype == 0) {
			inflate_uncompressed(upng, out, outsize, &in[inpos], &bp, &pos, insize);	/*no compression */
		} else {
#ifndef TINFL			
      /*compression, btype 01 or 10 */
      inflate_huffman(upng, out, outsize, &in[inpos], &bp, &pos, insize, btype);	
#else
      tinfl_decompressor inflator;
      tinfl_init(&inflator);
      tinfl_decompress(&inflator, &in[inpos], (size_t*)&insize, out, out, (uint8_t*)&outsize, 0);
			inflate_uncompressed(upng, out, outsize, &in[inpos], &bp, &pos, insize);	/*no compression */
#endif
		}

		/* stop if an error has occured */
		if (upng->error != UPNG_EOK) {
			return upng->error;
		}
	}

	return upng->error;
}

static upng_error uz_inflate(upng_t* upng, uint8_t *out, uint32_t outsize, 
    const uint8_t *in, uint32_t insize) {
	/* we require two bytes for the zlib data header */
	if (insize < 2) {
		SET_ERROR(upng, UPNG_EMALFORMED);
		return upng->error;
	}

	/* 256 * in[0] + in[1] must be a multiple of 31, 
   * the FCHECK value is supposed to be made that way */
	if ((in[0] * 256 + in[1]) % 31 != 0) {
		SET_ERROR(upng, UPNG_EMALFORMED);
		return upng->error;
	}

	/*error: only compression method 8: inflate with sliding window of 32k 
   * is supported by the PNG spec */
	if ((in[0] & 15) != 8 || ((in[0] >> 4) & 15) > 7) {
		SET_ERROR(upng, UPNG_EMALFORMED);
		return upng->error;
	}

	/* the specification of PNG says about the zlib stream: 
   * "The additional flags shall not specify a preset dictionary." */
	if (((in[1] >> 5) & 1) != 0) {
		SET_ERROR(upng, UPNG_EMALFORMED);
		return upng->error;
	}

	/* create output buffer */
	uz_inflate_data(upng, out, outsize, in, insize, 2);

	return upng->error;
}

/*Paeth predicter, used by PNG filter type 4*/
static int32_t paeth_predictor(int32_t a, int32_t b, int32_t c) {
	int32_t p = a + b - c;
	int32_t pa = p > a ? p - a : a - p;
	int32_t pb = p > b ? p - b : b - p;
	int32_t pc = p > c ? p - c : c - p;

	if (pa <= pb && pa <= pc)
		return a;
	else if (pb <= pc)
		return b;
	else
		return c;
}

static void unfilter_scanline(upng_t* upng, uint8_t *recon, const uint8_t *scanline, 
    const uint8_t *precon, uint32_t bytewidth, uint8_t filterType, 
    uint32_t length) {
	/*
	   For PNG filter method 0
	   unfilter a PNG image scanline by scanline. 
     When the pixels are smaller than 1 byte, the filter works byte per byte (bytewidth = 1)
	   precon is the previous unfiltered scanline, recon the result, scanline the current one
	   the incoming scanlines do NOT include the filtertype byte, 
     that one is given in the parameter filterType instead
	   recon and scanline MAY be the same memory address! precon must be disjoint.
	 */

	uint32_t i;
	switch (filterType) {
	case 0:
		for (i = 0; i < length; i++)
			recon[i] = scanline[i];
		break;
	case 1:
		for (i = 0; i < bytewidth; i++)
			recon[i] = scanline[i];
		for (i = bytewidth; i < length; i++)
			recon[i] = scanline[i] + recon[i - bytewidth];
		break;
	case 2:
		if (precon)
			for (i = 0; i < length; i++)
				recon[i] = scanline[i] + precon[i];
		else
			for (i = 0; i < length; i++)
				recon[i] = scanline[i];
		break;
	case 3:
		if (precon) {
			for (i = 0; i < bytewidth; i++)
				recon[i] = scanline[i] + precon[i] / 2;
			for (i = bytewidth; i < length; i++)
				recon[i] = scanline[i] + ((recon[i - bytewidth] + precon[i]) / 2);
		} else {
			for (i = 0; i < bytewidth; i++)
				recon[i] = scanline[i];
			for (i = bytewidth; i < length; i++)
				recon[i] = scanline[i] + recon[i - bytewidth] / 2;
		}
		break;
	case 4:
		if (precon) {
			for (i = 0; i < bytewidth; i++)
				recon[i] = (uint8_t)(scanline[i] + paeth_predictor(0, precon[i], 0));
			for (i = bytewidth; i < length; i++)
				recon[i] = (uint8_t)(scanline[i] + paeth_predictor(recon[i - bytewidth], 
              precon[i], precon[i - bytewidth]));
		} else {
			for (i = 0; i < bytewidth; i++)
				recon[i] = scanline[i];
			for (i = bytewidth; i < length; i++)
				recon[i] = (uint8_t)(scanline[i] + paeth_predictor(recon[i - bytewidth], 0, 0));
		}
		break;
	default:
		SET_ERROR(upng, UPNG_EMALFORMED);
		break;
	}
}

static void unfilter(upng_t* upng, uint8_t *out, const uint8_t *in, 
    uint32_t w, uint32_t h, uint32_t bpp) {
	/*
	   For PNG filter method 0
	   this function unfilters a single image 
     (e.g. without interlacing this is called once, with Adam7 it's called 7 times)
	   out must have enough bytes allocated already, 
     in must have the scanlines + 1 filtertype byte per scanline
	   w and h are image dimensions or dimensions of reduced image, bpp is bpp per pixel
	   in and out are allowed to be the same memory address!
	 */

	uint32_t y;
	uint8_t *prevline = 0;

  /*bytewidth is used for filtering, is 1 when bpp < 8, number of bytes per pixel otherwise */
	uint32_t bytewidth = (bpp + 7) / 8;	
	uint32_t linebytes = (w * bpp + 7) / 8;

	for (y = 0; y < h; y++) {
		uint32_t outindex = linebytes * y;
		uint32_t inindex = (1 + linebytes) * y;	/*the extra filterbyte added to each row */
		uint8_t filterType = in[inindex];

		unfilter_scanline(upng, &out[outindex], &in[inindex + 1], prevline, bytewidth, filterType, 
                      linebytes);
		if (upng->error != UPNG_EOK) {
			return;
		}

		prevline = &out[outindex];
	}
}

static void remove_padding_bits(uint8_t *out, const uint8_t *in, 
    uint32_t olinebits, uint32_t ilinebits, uint32_t h) {
	/*
	   After filtering there are still padding bpp if scanlines have non multiple of 8 bit amounts.
     They need to be removed (except at last scanline of (Adam7-reduced) image)
     before working with pure image buffers for the Adam7 code,
     the color convert code and the output to the user.
	   in and out are allowed to be the same buffer, in may also be higher but still overlapping; 
     in must have >= ilinebits*h bpp, out must have >= olinebits*h bpp, 
     olinebits must be <= ilinebits
	   also used to move bpp after earlier such operations happened, 
     e.g. in a sequence of reduced images from Adam7
	   only useful if (ilinebits - olinebits) is a value in the range 1..7
	 */
	uint32_t y;
	uint32_t diff = ilinebits - olinebits;
	uint32_t obp = 0, ibp = 0;	/*bit pointers */
	for (y = 0; y < h; y++) {
		uint32_t x;
		for (x = 0; x < olinebits; x++) {
			uint8_t bit = (uint8_t)((in[(ibp) >> 3] >> (7 - ((ibp) & 0x7))) & 1);
			ibp++;

			if (bit == 0)
				out[(obp) >> 3] &= (uint8_t)(~(1 << (7 - ((obp) & 0x7))));
			else
				out[(obp) >> 3] |= (1 << (7 - ((obp) & 0x7)));
			++obp;
		}
		ibp += diff;
	}
}

/*out must be buffer big enough to contain full image, 
 * and in must contain the full decompressed data from the IDAT chunks*/
static void post_process_scanlines(upng_t* upng, uint8_t *out, uint8_t *in, 
    uint32_t bpp, uint32_t w, uint32_t h) {
	if (bpp == 0) {
		SET_ERROR(upng, UPNG_EMALFORMED);
		return;
	}

	if (bpp < 8 && w * bpp != ((w * bpp + 7) / 8) * 8) {
		unfilter(upng, in, in, w, h, bpp);
		if (upng->error != UPNG_EOK) {
			return;
		}
		//remove_padding_bits(out, in, w * bpp, ((w * bpp + 7) / 8) * 8, h);
    //fix for non-byte-aligned images
    uint32_t aligned_width = ((w * bpp + 7) / 8) * 8;
		remove_padding_bits(in, in, aligned_width, aligned_width, h);
	} else {
    /*we can immediatly filter into the out buffer, no other steps needed */
		unfilter(upng, in, in, w, h, bpp);	
	}
}

static upng_format determine_format(upng_t* upng) {
	switch (upng->color_type) {
  case UPNG_PLT:
		switch (upng->color_depth) {
		case 1:
			return UPNG_INDEXED1;
		case 2:
			return UPNG_INDEXED2;
		case 4:
			return UPNG_INDEXED4;
		case 8:
			return UPNG_INDEXED8;
		default:
			return UPNG_BADFORMAT;
		}
	case UPNG_LUM:
		switch (upng->color_depth) {
		case 1:
			return UPNG_LUMINANCE1;
		case 2:
			return UPNG_LUMINANCE2;
		case 4:
			return UPNG_LUMINANCE4;
		case 8:
			return UPNG_LUMINANCE8;
		default:
			return UPNG_BADFORMAT;
		}
	case UPNG_RGB:
		switch (upng->color_depth) {
		case 8:
			return UPNG_RGB8;
		case 16:
			return UPNG_RGB16;
		default:
			return UPNG_BADFORMAT;
		}
	case UPNG_LUMA:
		switch (upng->color_depth) {
		case 1:
			return UPNG_LUMINANCE_ALPHA1;
		case 2:
			return UPNG_LUMINANCE_ALPHA2;
		case 4:
			return UPNG_LUMINANCE_ALPHA4;
		case 8:
			return UPNG_LUMINANCE_ALPHA8;
		default:
			return UPNG_BADFORMAT;
		}
	case UPNG_RGBA:
		switch (upng->color_depth) {
		case 8:
			return UPNG_RGBA8;
		case 16:
			return UPNG_RGBA16;
		default:
			return UPNG_BADFORMAT;
		}
	default:
		return UPNG_BADFORMAT;
	}
}

static void upng_free_source(upng_t* upng) {
	if (upng->source.owning != 0) {
		free((void*)upng->source.buffer);
	}

	upng->source.buffer = NULL;
	upng->source.size = 0;
	upng->source.owning = 0;
}

/*read the information from the header and store it in the upng_Info. return value is error*/
upng_error upng_header(upng_t* upng) {
	/* if we have an error state, bail now */
	if (upng->error != UPNG_EOK) {
		return upng->error;
	}

	/* if the state is not NEW (meaning we are ready to parse the header), stop now */
	if (upng->state != UPNG_NEW) {
		return upng->error;
	}

	/* minimum length of a valid PNG file is 29 bytes
	 * FIXME: verify this against the specification, or
	 * better against the actual code below */
	if (upng->source.size < 29) {
		SET_ERROR(upng, UPNG_ENOTPNG);
		return upng->error;
	}

	/* check that PNG header matches expected value */
	if (upng->source.buffer[0] != 137 || upng->source.buffer[1] != 80 
      || upng->source.buffer[2] != 78 || upng->source.buffer[3] != 71 
      || upng->source.buffer[4] != 13 || upng->source.buffer[5] != 10 
      || upng->source.buffer[6] != 26 || upng->source.buffer[7] != 10) {
		SET_ERROR(upng, UPNG_ENOTPNG);
		return upng->error;
	}

	/* check that the first chunk is the IHDR chunk */
	if (MAKE_DWORD_PTR(upng->source.buffer + 12) != CHUNK_IHDR) {
		SET_ERROR(upng, UPNG_EMALFORMED);
		return upng->error;
	}

	/* read the values given in the header */
	upng->width = MAKE_DWORD_PTR(upng->source.buffer + 16);
	upng->height = MAKE_DWORD_PTR(upng->source.buffer + 20);
	upng->color_depth = upng->source.buffer[24];
	upng->color_type = (upng_color)upng->source.buffer[25];

	/* determine our color format */
	upng->format = determine_format(upng);
	if (upng->format == UPNG_BADFORMAT) {
		SET_ERROR(upng, UPNG_EUNFORMAT);
		return upng->error;
	}

	/* check that the compression method (byte 27) is 0 (only allowed value in spec) */
	if (upng->source.buffer[26] != 0) {
		SET_ERROR(upng, UPNG_EMALFORMED);
		return upng->error;
	}

	/* check that the compression method (byte 27) is 0 (only allowed value in spec) */
	if (upng->source.buffer[27] != 0) {
		SET_ERROR(upng, UPNG_EMALFORMED);
		return upng->error;
	}

	/* check that the compression method (byte 27) is 0 
   * (spec allows 1, but uPNG does not support it) */
	if (upng->source.buffer[28] != 0) {
		SET_ERROR(upng, UPNG_EUNINTERLACED);
		return upng->error;
	}

	upng->state = UPNG_HEADER;
	return upng->error;
}


upng_error upng_load(upng_t* upng) {
  /* if we have an error state, bail now */
	if (upng->error != UPNG_EOK) {
		return upng->error;
	}

	/* parse the main header, if necessary */
  if (upng->state != UPNG_HEADER) {
	  upng_header(upng);
	  if ((upng->error != UPNG_EOK) || (upng->state != UPNG_HEADER)) {
		  return upng->error;
	  }
  }
  
  /* first byte of the first chunk after the header */
	upng->cursor = upng->source.buffer + 33;

	/* scan through the chunks, finding the size of all IDAT chunks, and also
	 * verify general well-formed-ness */
	while (upng->cursor < upng->source.buffer + upng->source.size) {
    uint32_t chunk_type = upng_chunk_type(upng->cursor);
		uint32_t data_length = upng_chunk_data_length(upng->cursor);
		const uint8_t *data = upng_chunk_data(upng->cursor);

		/* sanity check data_length */
		if (data_length > INT_MAX) {
      upng->cursor += data_length + 12; //forward cursor to next chunk
			SET_ERROR(upng, UPNG_EMALFORMED);
			return upng->error;
		}

		/* make sure chunk header is not larger than the total compressed */
		if ((uint32_t)(upng->cursor - upng->source.buffer + 12) > upng->source.size) {
      upng->cursor += data_length + 12; //forward cursor to next chunk
			SET_ERROR(upng, UPNG_EMALFORMED);
			return upng->error;
		}

		/* make sure chunk header+paylaod is not larger than the total compressed */
		if ((uint32_t)(upng->cursor - upng->source.buffer + data_length + 12) > upng->source.size) {
      upng->cursor += data_length + 12; //forward cursor to next chunk
			SET_ERROR(upng, UPNG_EMALFORMED);
			return upng->error;
		}


		/* parse chunks */
    switch (chunk_type) {
      case CHUNK_OFFS:
        upng->x_offset = MAKE_DWORD_PTR(data);
        upng->y_offset = MAKE_DWORD_PTR(data + 4);
        break;
      case CHUNK_PLTE:
        upng->palette_entries = data_length / 3; //3 bytes per color entry
        if(upng->palette) {
          free(upng->palette);
          upng->palette = NULL;
        }
        upng->palette = malloc(data_length);
        memcpy(upng->palette, data, data_length);
        break;
      case CHUNK_TRNS:
        upng->alpha_palette_entries = data_length; //1 byte per color entry
        if(upng->alpha_palette) {
          free(upng->alpha_palette);
          upng->alpha_palette = NULL;
        }
        upng->alpha_palette = malloc(data_length);
        memcpy(upng->alpha_palette, data, data_length);
        break;
      case CHUNK_FCTL:
        if(upng->apng_frame_control) {
          free(upng->apng_frame_control);
        }
        upng->apng_frame_control = malloc(data_length);
        upng->apng_frame_control->sequence_number = MAKE_DWORD_PTR(data);
        upng->apng_frame_control->width = MAKE_DWORD_PTR(data + 4);
        upng->apng_frame_control->height = MAKE_DWORD_PTR(data + 8);
        upng->apng_frame_control->x_offset = MAKE_DWORD_PTR(data + 12);
        upng->apng_frame_control->y_offset = MAKE_DWORD_PTR(data + 16);
        upng->apng_frame_control->delay_num = MAKE_DSHORT_PTR(data + 20);
        upng->apng_frame_control->delay_den = MAKE_DSHORT_PTR(data + 22);
        upng->apng_frame_control->dispose_op = *(data + 23);
        upng->apng_frame_control->blend_op = *(data + 24);
        break; 
      case CHUNK_ACTL:
        upng->is_apng = true;
        upng->apng_num_frames = MAKE_DWORD_PTR(data);
        // We ignore apng num_plays
        break;
      case CHUNK_IDAT:
        // Stop at these chunks and leave for another stage
        upng->state = UPNG_LOADED;
        return upng->error;
        break;
      case CHUNK_IEND:
        SET_ERROR(upng, UPNG_EMALFORMED);
        upng->state = UPNG_ERROR;
        return upng->error;
        break;
      default:
        if (upng_chunk_type_critical(chunk_type)) {
          SET_ERROR(upng, UPNG_EUNSUPPORTED);
          upng->cursor += data_length + 12; //forward cursor to next chunk
          return upng->error;
        }
        break;
    }
    upng->cursor += data_length + 12; //forward cursor to next chunk
  }

  SET_ERROR(upng, UPNG_EMALFORMED);
	upng->state = UPNG_ERROR;
	return upng->error;
}

/*read a PNG, the result will be in the same color type as the PNG (hence "generic")*/
upng_error upng_decode_image(upng_t* upng) {
	uint8_t* compressed = NULL;
	uint8_t* inflated = NULL;
	uint32_t compressed_size = 0;
	uint32_t inflated_size = 0;
  bool cursor_at_next_frame = false;

	/* if we have an error state, bail now */
	if (upng->error != UPNG_EOK) {
		return upng->error;
	}
  
  /* parse the main header and additional global data, if necessary */
  if (upng->state != UPNG_LOADED && upng->state != UPNG_DECODED) {
	  upng_load(upng);
	  if (upng->error != UPNG_EOK || upng->state != UPNG_LOADED) {
		  return upng->error;
	  }
  }


	/* release old result, if any */
	if (upng->buffer != 0) {
		free(upng->buffer);
		upng->buffer = 0;
		upng->size = 0;
	}


  /* scan through the chunks, finding the size of all IDAT chunks, and also
	 * verify general well-formed-ness */
	while ((upng->cursor < upng->source.buffer + upng->source.size) && !cursor_at_next_frame) {
    uint32_t chunk_type = upng_chunk_type(upng->cursor);
		uint32_t data_length = upng_chunk_data_length(upng->cursor);
		const uint8_t *data = upng_chunk_data(upng->cursor);

		/* sanity check data_length */
		if (data_length > INT_MAX) {
      upng->cursor += data_length + 12; //forward cursor to next chunk
			SET_ERROR(upng, UPNG_EMALFORMED);
			return upng->error;
		}

		/* make sure chunk header is not larger than the total compressed */
		if ((uint32_t)(upng->cursor - upng->source.buffer + 12) > upng->source.size) {
      upng->cursor += data_length + 12; //forward cursor to next chunk
			SET_ERROR(upng, UPNG_EMALFORMED);
			return upng->error;
		}

		/* make sure chunk header+payload is not larger than the total compressed */
		if ((uint32_t)(upng->cursor - upng->source.buffer + data_length + 12) > upng->source.size) {
      upng->cursor += data_length + 12; //forward cursor to next chunk
			SET_ERROR(upng, UPNG_EMALFORMED);
			return upng->error;
		}


		/* parse chunks */
    switch (chunk_type) {
      case CHUNK_FCTL:
        if(upng->apng_frame_control) {
          free(upng->apng_frame_control);
        }
        upng->apng_frame_control = malloc(data_length);
        upng->apng_frame_control->sequence_number = MAKE_DWORD_PTR(data);
        upng->apng_frame_control->width = MAKE_DWORD_PTR(data + 4);
        upng->apng_frame_control->height = MAKE_DWORD_PTR(data + 8);
        upng->apng_frame_control->x_offset = MAKE_DWORD_PTR(data + 12);
        upng->apng_frame_control->y_offset = MAKE_DWORD_PTR(data + 16);
        upng->apng_frame_control->delay_num = *(uint16_t*)(data + 20);
        upng->apng_frame_control->delay_den = *(uint16_t*)(data + 22);
        upng->apng_frame_control->dispose_op = *(data + 23);
        upng->apng_frame_control->blend_op = *(data + 24);
        break; 
      case CHUNK_FDAT:
        /* first 4 bytes in fdAT is sequence number, so skip 4 bytes */
        // TODO : fix for multiple consecutive fdAT chunks
        compressed = (uint8_t*)(data + 4);
        compressed_size = (data_length - 4);
        cursor_at_next_frame = true; //stop processing chunks at the IDAT/fdAT chunks
        break;
      case CHUNK_IDAT:
        // TODO : fix for multiple consecutive IDAT chunks
        compressed = (uint8_t*)(data);
        compressed_size = data_length;
        cursor_at_next_frame = true; //stop processing chunks at the IDAT/fdAT chunks
        break;
      case CHUNK_IEND:
        SET_ERROR(upng, UPNG_EDONE);
        upng->state = UPNG_ERROR; //force future calls to fail
        return upng->error;
        break;
      default:
        if (upng_chunk_type_critical(chunk_type)) {
          SET_ERROR(upng, UPNG_EUNSUPPORTED);
          upng->cursor += data_length + 12; //forward cursor to next chunk
          return upng->error;
        }
        break;
    }
    upng->cursor += data_length + 12; //forward cursor to next chunk
  }


  uint32_t width = upng->width;
  uint32_t height = upng->height;
  if (upng->apng_frame_control) {
    width = upng->apng_frame_control->width;
    height = upng->apng_frame_control->height;
  }

	/* allocate space to store inflated (but still filtered) data */
  int32_t width_aligned_bytes = (width * upng_get_bpp(upng) + 7) / 8;
	inflated_size = (width_aligned_bytes * height) + height; //pad byte

#ifdef CCM
  //Hard-codec CCM usage, avoid compositor buffer (ie. +32k to be safe)
	inflated = (void*)0x1000a0d8;//(uint8_t*)malloc(inflated_size);
	//inflated = (void*)0x10008000;//(uint8_t*)malloc(inflated_size);
#else
  inflated = (uint8_t*)malloc(inflated_size);
#endif

	if (inflated == NULL) {
		SET_ERROR(upng, UPNG_ENOMEM);
		return upng->error;
	}

	/* decompress image data */
	if (uz_inflate(upng, inflated, inflated_size, compressed, compressed_size) != UPNG_EOK) {
		free(compressed);
		return upng->error;
	}

	/* unfilter scanlines */
	post_process_scanlines(upng, inflated, inflated, upng_get_bpp(upng), width, height);
  upng->buffer = inflated;
	upng->size = inflated_size;

	if (upng->error != UPNG_EOK) {
		free(upng->buffer);
		upng->buffer = NULL;
		upng->size = 0;
	} else {
		upng->state = UPNG_DECODED;
	}

	return upng->error;
}

static upng_t* upng_new(void) {
	upng_t* upng;

	upng = (upng_t*)malloc(sizeof(upng_t));
	if (upng == NULL) {
		return NULL;
	}

  upng->cursor = NULL;
	upng->buffer = NULL;
	upng->size = 0;

	upng->width = upng->height = 0;

  upng->x_offset = 0;
  upng->y_offset = 0;

  upng->palette = NULL;
  upng->palette_entries = 0;

  upng->alpha_palette = NULL;
  upng->alpha_palette_entries = 0;

	upng->color_type = UPNG_RGBA;
	upng->color_depth = 8;
	upng->format = UPNG_RGBA8;

  upng->apng_frame_control = NULL;
  upng->apng_duration_ms = 0;
  upng->apng_num_frames = 0;

	upng->state = UPNG_NEW;

	upng->error = UPNG_EOK;
	upng->error_line = 0;

	upng->source.buffer = NULL;
	upng->source.size = 0;
	upng->source.owning = 0;

	return upng;
}

upng_t* upng_new_from_bytes(uint8_t* buffer, uint32_t size) {
	upng_t* upng = upng_new();
	if (upng == NULL) {
		return NULL;
	}

	upng->source.buffer = buffer;
	upng->source.size = size;
	upng->source.owning = 0;

	return upng;
}

void upng_free(upng_t* upng) {
#ifndef CCM   // Hack to use CCM, disable free
	/* deallocate image buffer */
	if (upng->buffer != NULL) {
		free(upng->buffer);
	}
#endif

  /* deallocate palette buffer, if necessary */
  if (upng->palette) {
    free(upng->palette);
  }

	/* deallocate source buffer, if necessary */
	upng_free_source(upng);

	/* deallocate struct itself */
	free(upng);
}

upng_error upng_get_error(const upng_t* upng) {
	return upng->error;
}

uint32_t upng_get_error_line(const upng_t* upng) {
	return upng->error_line;
}

uint32_t upng_get_width(const upng_t* upng) {
	return upng->width;
}

uint32_t upng_get_height(const upng_t* upng) {
	return upng->height;
}

int32_t upng_get_x_offset(const upng_t* upng) {
	return upng->x_offset;
}

int32_t upng_get_y_offset(const upng_t* upng) {
	return upng->y_offset;
}

int32_t upng_get_palette(const upng_t* upng, rgb **palette) {
  *palette = upng->palette;
	return upng->palette_entries;
}

int32_t upng_get_alpha_palette(const upng_t* upng, uint8_t **alpha_palette) {
  *alpha_palette = upng->alpha_palette;
	return upng->alpha_palette_entries;
}

uint32_t upng_get_bpp(const upng_t* upng) {
	return upng_get_bitdepth(upng) * upng_get_components(upng);
}

uint32_t upng_get_components(const upng_t* upng) {
	switch (upng->color_type) {
  case UPNG_PLT:
    return 1;
	case UPNG_LUM:
		return 1;
	case UPNG_RGB:
		return 3;
	case UPNG_LUMA:
		return 2;
	case UPNG_RGBA:
		return 4;
	default:
		return 0;
	}
}

uint32_t upng_get_bitdepth(const upng_t* upng) {
	return upng->color_depth;
}

uint32_t upng_get_pixelsize(const upng_t* upng) {
	return (upng_get_bitdepth(upng) * upng_get_components(upng));
}

upng_format upng_get_format(const upng_t* upng) {
	return upng->format;
}

const uint8_t* upng_get_buffer(const upng_t* upng) {
	return upng->buffer;
}

uint32_t upng_get_size(const upng_t* upng) {
	return upng->size;
}

//returns if the png is an apng after the upng_load() function
bool upng_is_apng(const upng_t* upng) {
  return upng->is_apng;
}

//retuns the apng num_frames
uint32_t upng_apng_num_frames(const upng_t* upng) {
  return upng->apng_num_frames;
}

//Pass in a apng_fctl to get the next frames frame control information
bool upng_get_apng_fctl(const upng_t* upng, apng_fctl *apng_frame_control) {
  bool retval = false;
  if (upng->is_apng && apng_frame_control != NULL) {
    *apng_frame_control = *upng->apng_frame_control;
    retval = true;
  }
  return retval;
}



