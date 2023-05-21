// Modified by Urja Rannikko in 2023 to be a 16-bit DOS application
//
// PNG Decoder, originally
// written by Larry Bank
// bitbank@pobox.com
// Arduino port started 5/3/2021
// Original PNG code written 20+ years ago :)
//
// Copyright 2021 BitBank Software, Inc. All Rights Reserved.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//===========================================================================
//
#ifndef __PNGDEC__
#define __PNGDEC__
#include <stdlib.h>
#include <string.h>
#ifdef LINUX
#include <stdint.h>
#else
#include <limits.h>
typedef unsigned char uint8_t;
typedef signed char int8_t;
typedef unsigned short uint16_t;
typedef signed short int16_t;
typedef signed long int32_t;
typedef unsigned long uint32_t;
typedef signed long off_t;
#endif

#include <stdio.h>
#include "zutil.h"
#include "inftrees.h"
#include "inflate.h"

//
// PNG Decoder
// Written by Larry Bank
// Copyright (c) 2021 BitBank Software, Inc.


#ifndef FALSE
#define FALSE 0
#define TRUE 1
#endif
/* Defines and variables */
#define PNG_FILE_BUF_SIZE 2048

// PNG filter type
enum {
    PNG_FILTER_NONE=0,
    PNG_FILTER_SUB,
    PNG_FILTER_UP,
    PNG_FILTER_AVG,
    PNG_FILTER_PAETH,
    PNG_FILTER_COUNT
};

// decode options
enum {
    PNG_CHECK_CRC = 1,
};

// source pixel type
enum {
	PNG_PIXEL_GRAYSCALE=0,
    PNG_PIXEL_TRUECOLOR=2,
    PNG_PIXEL_INDEXED=3,
    PNG_PIXEL_GRAY_ALPHA=4,
    PNG_PIXEL_TRUECOLOR_ALPHA=6
};

// Error codes returned by getLastError()
enum {
    PNG_SUCCESS = 0,
    PNG_INVALID_PARAMETER, // 1
    PNG_DECODE_ERROR,	// 2
    PNG_IO_ERROR,	// 3
    PNG_NO_BUFFER,	// 4
    PNG_UNSUPPORTED_FEATURE, // 5
    PNG_INVALID_FILE, // 6
    PNG_TOO_BIG // 7
};

typedef struct png_draw_tag
{
    int y; // starting x,y of this line
    int iWidth; // size of this line
    int iPitch; // bytes per line
    int iPixelType; // PNG pixel type (0,2,3,4,6)
    int iBpp; // bits per color stimulus
    int iHasAlpha; // flag indicating the presence of an alpha palette
	// Transparent pixel value like seen in the data proper
	uint8_t iTrans[6];
	uint8_t iTransLen;
	// Background pixel value (1,2,4,8 or 24 bits gray/color) or palette index (1,2,4,8)
	int32_t iBackground;
	int iPaletteCnt;
    long User; // user supplied value (output fd for this app)
    uint8_t *pPalette;
    uint8_t *pPixels;
} PNGDRAW;

typedef struct png_file_tag
{
  off_t iPos; // current file position
  off_t iSize; // file size
  long fHandle;
} PNGFILE;

// Callback function prototypes
typedef int32_t (PNG_READ_CALLBACK)(PNGFILE *pFile, uint8_t *pBuf, int32_t iLen);
typedef void (PNG_SEEK_CALLBACK)(PNGFILE *pFile, off_t iPosition);
typedef void (PNG_DRAW_CALLBACK)(PNGDRAW *);


//
// our private structure to hold an image decode state
//
typedef struct png_image_tag
{
    int32_t iWidth, iHeight; // image size
    uint8_t ucBpp, ucPixelType;
    int32_t iPitch; // bytes per line
    int iHasAlpha;
	int iPaletteCnt;
    int iInterlaced;
    uint8_t iTrans[6]; // transparent color index/value
	uint8_t iTransLen;
	uint32_t iBackground;
    int iError;
    PNG_READ_CALLBACK *pfnRead;
    PNG_SEEK_CALLBACK *pfnSeek;
    PNG_DRAW_CALLBACK *pfnDraw;

    PNGFILE PNGFile;
    uint8_t ucZLIB[32768 + sizeof(struct inflate_state)]; // put this here to avoid needing malloc/free
    uint8_t ucPalette[1024];
	
    uint8_t ucFileBuf[PNG_FILE_BUF_SIZE]; // holds temp file data
	uint8_t *uLine1;
	uint8_t *uLine2;
	
} PNGIMAGE;

#define PNG_STATIC

int PNG_init(PNGIMAGE* pPNG);
int PNG_decode(PNGIMAGE *pPNG, long User, int iOptions);
int PNG_getLastError(PNGIMAGE *pPNG);
int PNG_getBpp(PNGIMAGE *pPNG);
int PNG_getLastError(PNGIMAGE *pPNG);
uint8_t *PNG_getPalette(PNGIMAGE *pPNG);
int PNG_getPixelType(PNGIMAGE *pPNG);
int PNG_hasAlpha(PNGIMAGE *pPNG);
int PNG_isInterlaced(PNGIMAGE *pPNG);


// Due to unaligned memory causing an exception, we have to do these macros the slow way
// x86: unaligned is fine <3
#define INTELSHORT(p) (*(short*)p)
#define INTELLONG(p) (*(long*)p)
#define MOTOSHORT(p) (((*(p))<<8) + (*(p+1)))
#define MOTOLONG(p) (((uint32_t)(*p)<<24) + ((uint32_t)(*(p+1))<<16) + ((*(p+2))<<8) + (*(p+3)))

#endif // __PNGDEC__
