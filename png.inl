// Modified by Urja Rannikko in 2023 to be a 16-bit DOS application
//
// PNG Decoder, originally
// written by Larry Bank
// bitbank@pobox.com
// Arduino port started 5/3/2021
// Original PNG code written 20+ years ago :)
// 
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
#include "zlib.h"

int PNG_getLastError(PNGIMAGE *pPNG)
{
    return pPNG->iError;
} /* PNG_getLastError() */

uint8_t *PNG_getPalette(PNGIMAGE *pPNG)
{
    return pPNG->ucPalette;
} /* PNG_getPalette() */

//
// Verify it's a PNG file and then parse the IHDR chunk
// to get basic image size/type/etc
//
static int PNGParseInfo(PNGIMAGE *pPage)
{
    uint8_t s[32];
    int iBytesRead;
    const uint8_t signature[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	int32_t iL;
	
    pPage->iHasAlpha = pPage->iInterlaced = 0;
    // Read a few bytes to just parse the size/pixel info
    iBytesRead = (*pPage->pfnRead)(&pPage->PNGFile, s, 32);
    if (iBytesRead < 32) { // a PNG file this tiny? probably bad
        pPage->iError = PNG_INVALID_FILE;
        return pPage->iError;
    }

	if (memcmp(s, signature, 8) != 0) { // check that it's a PNG file
		pPage->iError = PNG_INVALID_FILE;
		return pPage->iError;
    }
	
	iL = MOTOLONG(&s[8]);
	
	if (iL < 13) {
		pPage->iError = PNG_INVALID_FILE;
		return pPage->iError;
	}
	
	pPage->iBackground = -1; /* unspecified */
	
    if (MOTOLONG(&s[12]) == 0x49484452UL/*'IHDR'*/) {
        pPage->iWidth = MOTOLONG(&s[16]);
        pPage->iHeight = MOTOLONG(&s[20]);
        pPage->ucBpp = s[24]; // bits per pixel
        pPage->ucPixelType = s[25]; // pixel type
        pPage->iInterlaced = s[28];
		
        if ((s[26] != 0) || (s[27] != 0) || pPage->iInterlaced) {
            pPage->iError = PNG_UNSUPPORTED_FEATURE;
            return pPage->iError;
		}
		/* Validate BPP properly. */
		switch (pPage->ucBpp) {
			case 1:
			case 2:
			case 4:
			case 8:
			case 16:
				break;
			default:
				pPage->iError = PNG_INVALID_FILE;
				return pPage->iError;
		}
        // calculate the number of bytes per line of pixels
		switch (pPage->ucPixelType) {
            case PNG_PIXEL_GRAYSCALE: // grayscale
            case PNG_PIXEL_INDEXED: // indexed
                pPage->iPitch = (pPage->iWidth * pPage->ucBpp + 7)/8; // bytes per pixel
                break;
            case PNG_PIXEL_TRUECOLOR: // truecolor
				pPage->iBackground = 0x999999L;
                pPage->iPitch = ((3 * pPage->ucBpp) * pPage->iWidth + 7)/8;
                break;
            case PNG_PIXEL_GRAY_ALPHA: // grayscale + alpha
				pPage->iBackground = 0x99;
                pPage->iPitch = ((2 * pPage->ucBpp) * pPage->iWidth + 7)/8;
                pPage->iHasAlpha = 1;
                break;
            case PNG_PIXEL_TRUECOLOR_ALPHA: // truecolor + alpha
				pPage->iBackground = 0x999999L;
                pPage->iPitch = ((4 * pPage->ucBpp) * pPage->iWidth + 7)/8;
                pPage->iHasAlpha = 1;
				break;
			default:
				pPage->iError = PNG_INVALID_FILE;
				return pPage->iError;
        } // switch
		
    } else {
		pPage->iError = PNG_INVALID_FILE;
		return pPage->iError;
	}

	if (pPage->iPitch < 0) {
		pPage->iError = PNG_INVALID_FILE;
		return pPage->iError;
	}

	/* The glory of 16-bit x86 :P */
    if (pPage->iPitch >= 65534)
       return PNG_TOO_BIG;

    return PNG_SUCCESS;
}

//
// De-filter the current line of pixels
//
PNG_STATIC void DeFilter(uint8_t *pCurr, uint8_t *pPrev, int iWidth, int iPitch)
{
    uint8_t ucFilter = *pCurr++;
    int x, iBpp;
    if (iPitch <= iWidth)
        iBpp = 1;
    else
        iBpp = iPitch / iWidth;
    
    pPrev++; // skip filter of previous line
    switch (ucFilter) { // switch on filter type
        case PNG_FILTER_NONE:
            // nothing to do :)
            break;
        case PNG_FILTER_SUB:
            for (x=iBpp; x<iPitch; x++) {
                pCurr[x] += pCurr[x-iBpp];
            }
            break;
        case PNG_FILTER_UP:
	    for (x = 0; x < iPitch; x++) {
               pCurr[x] += pPrev[x];
            }
            break;
        case PNG_FILTER_AVG:
            for (x = 0; x < iBpp; x++) {
               pCurr[x] = (pCurr[x] +
                  pPrev[x] / 2 );
            }
            for (x = iBpp; x < iPitch; x++) {
               pCurr[x] = pCurr[x] +
                  (pPrev[x] + pCurr[x-iBpp]) / 2;
            }
            break;
        case PNG_FILTER_PAETH:
            if (iBpp == 1) {
                int a, c;
                uint8_t *pEnd = &pCurr[iPitch];
                // First pixel/byte
                c = *pPrev++;
                a = *pCurr + c;
                *pCurr++ = (uint8_t)a;
                while (pCurr < pEnd) {
                   int b, pa, pb, pc, p;
                   a &= 0xff; // From previous iteration
                   b = *pPrev++;
                   p = b - c;
                   pc = a - c;
                   // assume no native ABS() instruction
                   pa = p < 0 ? -p : p;
                   pb = pc < 0 ? -pc : pc;
                   pc = (p + pc) < 0 ? -(p + pc) : p + pc;
                   // choose the best predictor
                   if (pb < pa) {
                      pa = pb; a = b;
                   }
                   if (pc < pa) a = c;
                   // Calculate current pixel
                   c = b;
                   a += *pCurr;
                   *pCurr++ = (uint8_t)a;
                }
            } else { // multi-byte
                uint8_t *pEnd = &pCurr[iBpp];
                // first pixel is treated the same as 'up'
		while (pCurr < pEnd) {
                   int a = *pCurr + *pPrev++;
                   *pCurr++ = (uint8_t)a;
                }
                pEnd = pEnd + (iPitch - iBpp);
                while (pCurr < pEnd) {
                   int a, b, c, pa, pb, pc, p;
                   c = pPrev[-iBpp];
                   a = pCurr[-iBpp];
                   b = *pPrev++;
                   p = b - c;
                   pc = a - c;
                    // asume no native ABS() instruction
                   pa = p < 0 ? -p : p;
                   pb = pc < 0 ? -pc : pc;
                   pc = (p + pc) < 0 ? -(p + pc) : p + pc;
                   if (pb < pa) {
                      pa = pb; a = b;
                   }
                   if (pc < pa) a = c;
                   a += *pCurr;
                   *pCurr++ = (uint8_t)a;
                }
            } // multi-byte
            break;
    } // switch on filter type
} /* DeFilter() */
//
// PNGInit
// Parse the PNG file header and confirm that it's a valid file
//
// returns 0 for success, nonzero for failure
//
int PNG_init(PNGIMAGE *pPNG)
{
    return PNGParseInfo(pPNG); // gather info for image
} /* PNG_init() */



//
// Decode the PNG file
//
// You must call open() before calling decode()
// This function can be called repeatedly without having
// to close and re-open the file
//

int PNG_decode(PNGIMAGE *pPage, long User, int iOptions)
{
	int more = 0;
    int err, y;
    int iBytesRead;
	int32_t iOffset; /* Needs to be i32 to allow us to skip >32k chunks (iLen can be added to it) */
	int32_t iLen=0;
	off_t iFileOffset;
    uint32_t iMarker=0;
    uint8_t *pCurr, *pPrev;
    z_stream d_stream; /* decompression stream */
    uint8_t *s = pPage->ucFileBuf;
    struct inflate_state *state;
	
	int hbo = 1; /* highest bits offset, for bKGD and tRNS chunks - 0 for 16bit bpp */
	
    // we need the draw callback and the linebuffers
    if ((pPage->pfnDraw == NULL)||(pPage->uLine1 == NULL)||(pPage->uLine2 == NULL)) {
		pPage->iError = PNG_NO_BUFFER;
		return pPage->iError;
	}

    // buffers to maintain the current and previous lines
	pCurr = pPage->uLine1;
	pPrev = pPage->uLine2;
		
    pPage->iError = PNG_SUCCESS;
    // Inflate the compressed image data
    // The allocation functions are disabled and zlib has been modified
    // to not use malloc/free and instead the buffer is part of the PNG class
    d_stream.zalloc = (alloc_func)0;
    d_stream.zfree = (free_func)0;
    d_stream.opaque = (voidpf)0;
    // Insert the memory pointer here to avoid having to use malloc() inside zlib
    state = (struct inflate_state FAR *)pPage->ucZLIB;
    d_stream.state = (struct internal_state FAR *)state;
    state->window = &pPage->ucZLIB[sizeof(struct inflate_state)]; // point to 32k dictionary buffer
    err = inflateInit(&d_stream);
    
    iFileOffset = 8; // skip PNG file signature
    iOffset = 0; // internal buffer offset starts at 0
    (*pPage->pfnSeek)(&pPage->PNGFile, iFileOffset);
	iBytesRead = 0;
    y = 0;
    d_stream.avail_out = 0;
    d_stream.next_out = 0;
	if (pPage->ucBpp > 8) hbo = 0;
	
    while ((!pPage->iError)&&(y < pPage->iHeight)) { // continue until fully decoded
		int32_t left = iBytesRead - iOffset;
        if ((left < 8)||(more)) { // need to read more data
			//printf("left %d more %d Offset %d\n", left, more, iOffset);
			if (left > 0) {
				memmove(s, s+iOffset, left);
			} else {
				/* This is the case where we skip some file data. (-= left which is negative -> adds to iFileOffset) */
				iFileOffset -= left;
				left = 0;
			}
            (*pPage->pfnSeek)(&pPage->PNGFile, iFileOffset);
            iBytesRead = (*pPage->pfnRead)(&pPage->PNGFile, s+left, PNG_FILE_BUF_SIZE - left);
			if (iBytesRead < 0) {
				pPage->iError = PNG_IO_ERROR;
				break;
			}
            iFileOffset += iBytesRead;
			iBytesRead += left;
            iOffset = 0;
			more = 0;
			/*
			printf("iFO %ld BR %d\n", iFileOffset, iBytesRead);
			printf("%02X %02X %02X %02X  %02X %02X %02X %02X  - %02X %02X %02X %02X  %02X %02X %02X %02X\n",
				s[0x0], s[0x1],s[0x2],s[0x3],s[0x4],s[0x5],s[0x6],s[0x7],s[0x8],s[0x9],s[0xA],s[0xB],s[0xC],s[0xD],s[0xE],s[0xF]); */
			/* We didn't get enough to parse a chunk, file ended before output finished. */
			if (iBytesRead < 8) {
				pPage->iError = PNG_DECODE_ERROR;
				//printf("decode1\n");
				break;
			}

		}

		if (!iMarker) {
			iLen = MOTOLONG(&s[iOffset]); // chunk length
			//printf("zMark iL %d Offset %d\n", iLen, iOffset);
			if (iLen < 0 || iLen + (iFileOffset - iBytesRead) > pPage->PNGFile.iSize) // invalid data
			{
				pPage->iError = PNG_DECODE_ERROR;
				//printf("decode2\n");
				break;
			}
			iMarker = MOTOLONG(&s[iOffset+4]);
			iOffset += 8; // point to the marker data
		}

		/* Skip unknown chunks (by ... not skipping if one of the later-handled ones.) */
		switch (iMarker) {
            case 0x504c5445: //'PLTE' palette colors
			case 0x74524e53: //'tRNS' transparency info
            case 0x49444154: //'IDAT' image data block
			case 0x44474b62: //'bKGD' background color
				break;
			default:
				iMarker = 0;
				iOffset += (iLen + 4); // skip data + CRC
				break;
		}
		
		if (!iMarker)
			continue;

		left = iBytesRead - iOffset;
		
		/* Check that we have enough data for the chunk, if it is not IDAT. */
		if ((iMarker != 0x49444154) && (left < iLen)) {
			/* If it were impossible to fetch this chunk into our buffer, abort. */
			if (iLen > PNG_FILE_BUF_SIZE) {
				pPage->iError = PNG_DECODE_ERROR;
				break;
			}
			more = 1;
			continue;
		}
		
        switch (iMarker)
        {
			case 0x44474b62: // 'bKGD'
				switch (pPage->ucPixelType) {
					case PNG_PIXEL_INDEXED:
						pPage->iBackground = s[iOffset];
						break;
					case PNG_PIXEL_GRAYSCALE: 
					case PNG_PIXEL_GRAY_ALPHA:
						pPage->iBackground = s[iOffset+hbo];
						break;
						
					case PNG_PIXEL_TRUECOLOR: // truecolor + alpha
					case PNG_PIXEL_TRUECOLOR_ALPHA: // truecolor + alpha
						pPage->iBackground = s[iOffset + 0+hbo]; // lower part of 2-byte value is transparent color value
						pPage->iBackground |= (s[iOffset + 2+hbo] << 8);
						pPage->iBackground |= (s[iOffset + 4+hbo] << 16);
						break;
				}
				break;

            case 0x504c5445: //'PLTE' palette colors
				if ((iLen > 768)||(iLen%3)) {
					pPage->iError = PNG_DECODE_ERROR;
					break;
				}					
				pPage->iPaletteCnt = iLen/3;
                memcpy(pPage->ucPalette, &s[iOffset], iLen);
                memset(&pPage->ucPalette[768], 0xff, 256); // assume all colors are opaque unless specified
				iMarker = 0;
                break;
			case 0x74524e53: //'tRNS' transparency info
                if (pPage->ucPixelType == PNG_PIXEL_INDEXED) // if palette exists
                {
					if (iLen > 256) {
						pPage->iError = PNG_DECODE_ERROR;
						break;
					}

					memcpy(&pPage->ucPalette[768], &s[iOffset], iLen);
                    pPage->iHasAlpha = 1;
                }
                else if (iLen == 2) // for grayscale images
                {
					if (pPage->ucBpp > 8) {
						memcpy(pPage->iTrans, &(s[iOffset]), 2);
						pPage->iTransLen = 2;
					} else {
						pPage->iTrans[0] = s[iOffset+1];
						pPage->iTransLen = 1;
					}
                }
                else if (iLen == 6) // transparent color for 24-bpp image
                {
					if (pPage->ucBpp > 8) {
						memcpy(pPage->iTrans, &(s[iOffset]), 6);
						pPage->iTransLen = 6;
					} else {
						pPage->iTrans[0] = s[iOffset+1];
						pPage->iTrans[1] = s[iOffset+3];
						pPage->iTrans[2] = s[iOffset+5];
						pPage->iTransLen = 3;
					}
                }
				iMarker = 0;
                break;
            case 0x49444154: //'IDAT' image data block
                while (iLen) {
					int32_t chunk;
                    if (iOffset >= iBytesRead) {
                        // we ran out of data; get some more
						if (d_stream.avail_in) {
							iOffset -= d_stream.avail_in;
							iLen += d_stream.avail_in;
						}
						more = 1;
						break;
                    }
					chunk = iBytesRead - iOffset;
					if (chunk > iLen) chunk = iLen;
                    d_stream.next_in  = &pPage->ucFileBuf[iOffset];
                    d_stream.avail_in = chunk;
					
                    iLen -= chunk;
                    iOffset += chunk;
                    err = 0;
                    while (err == Z_OK) {
                        if (d_stream.avail_out == 0) { // reset for next line
                            d_stream.avail_out = pPage->iPitch+1; /* +1 for the filter mode */
                            d_stream.next_out = pCurr;
						} // otherwise it is a continuation of an unfinished line
                        err = inflate(&d_stream, Z_NO_FLUSH, iOptions & PNG_CHECK_CRC);
                        if ((err == Z_OK || err == Z_STREAM_END) && d_stream.avail_out == 0) {// successfully decoded line
							uint8_t *tmp;
							PNGDRAW pngd;					
                            DeFilter(pCurr, pPrev, pPage->iWidth, pPage->iPitch);
							pngd.User = User;
							pngd.iPitch = pPage->iPitch;
							pngd.iWidth = pPage->iWidth;
							pngd.iPaletteCnt = pPage->iPaletteCnt;
							pngd.pPalette = pPage->ucPalette;
							pngd.pPixels = pCurr+1;
							pngd.iPixelType = pPage->ucPixelType;
							pngd.iHasAlpha = pPage->iHasAlpha;
							pngd.iBpp = pPage->ucBpp;
							pngd.iPaletteCnt = pPage->iPaletteCnt;
							pngd.iBackground = pPage->iBackground;
							pngd.iTransLen = pPage->iTransLen;
							if (pngd.iTransLen)
								memcpy(pngd.iTrans, pPage->iTrans, pngd.iTransLen);
							
							pngd.y = y;
							(*pPage->pfnDraw)(&pngd);
                            y++;
							// swap current and previous lines
							tmp = pCurr; pCurr = pPrev; pPrev = tmp;
                        }
                    }
                    if (err == Z_STREAM_END && d_stream.avail_out == 0) {
                        // successful decode, stop here
                        y = pPage->iHeight;
						iMarker = 0;
						break;
                    } else if (err == Z_DATA_ERROR || err == Z_STREAM_ERROR) {
                        pPage->iError = PNG_DECODE_ERROR; // force loop to exit with error
						//printf("decode3\n");
						break;
                    }
                } // while (iLen)
				if (!iLen)
					iMarker = 0;
                break;

        } // switch
		if (!iMarker) {
			iOffset += (iLen + 4); // skip data + CRC
		}
    } // while y < height and no error
    err = inflateEnd(&d_stream);
    return pPage->iError;
} /* DecodePNG() */
