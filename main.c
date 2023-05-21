/* PNGDECD - PNG2BMP for 16-bit DOS */
/* See LICENSE for the license --
 * this file is by Urja Rannikko 2023,
 * obviously with some influences from the PNGdec linux/main.cpp */

#include <stdio.h>
#include <errno.h>
#ifdef LINUX
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#else
#include <io.h>
#include <fcntl.h>
#include <sys\stat.h>
#endif

#include "pngdec.h"
#include "zlib.h"

#include "png.inl"

#ifndef O_BINARY
#define O_BINARY 0
#endif

static const char *errs(void) {
#ifdef LINUX
	return strerror(errno);
#else
	return sys_errlist[errno];
#endif
}

static void xout(const char* action, const char *param, int ev) {
	const char *e = errs();
	if (param)
		fprintf(stderr,"%s (%s): %s\n", action, param, e);
	else
		fprintf(stderr,"%s: %s\n", action, e);
	exit(ev);
}

static int32_t pngRead(PNGFILE *pFile, uint8_t *pBuf, int32_t iLen)
{
	return read(pFile->fHandle, pBuf, iLen);
}

static void pngSeek(PNGFILE *pFile, off_t iPosition)
{
	lseek(pFile->fHandle, iPosition, SEEK_SET);
}

/* Windows BMP header info (54 bytes) (<-! highlights things that we set.) */
static uint8_t winbmphdr[54 + 1024] =
        {0x42,0x4d,  // BM, File Header
         0,0,0,0,    // 2, File size  <-!
         0,0,0,0, 	 // 6, RSVD1/2
		 0x36,4,0,0, // 10, bfOffBits <-!
		 
		 0x28,0,0,0,// 14, Image Header, Header size (40)
         0,0,0,0,	// 18, Xsize <-!
         0,0,0,0,	// 22, Ysize <-!
         1,0,		// 26, Planes (fixed 1)
		 8,0,		// 28, Bpp <-!
		 0,0,0,0,	// 30, Uncompressed
		 0,0,0,0,	// 34, biSizeImage, can be 0 for uncompressed
		 0,0,0,0,	// 38, biXPelsPerMeter 
		 0,0,0,0,	// 42, biYPelsPerMeter
		 0,0,0,0,	// 46, biClrUsed (Color Map Size) <-!
		 0,0,0,0 	// 50, biClrImportant
		 };
/* ^^ + 1024 to include space for the palette */

static int bm_bitoff;
static int32_t BmpStride;
static uint8_t *BMPLine;	 
static int32_t pngHeight;
static int32_t desiredBackground = -1;

static uint8_t expandbits8(int idx, int bits)
{
		if (bits >= 8) return idx;
		if (bits == 1) {
			if (idx) return 0xFF;
			return 0;
		}
		if (bits == 2) {
			switch (idx) {
				default:
				case 0: return 0;
				case 1: return 0x55;
				case 2: return 0xAA;
				case 3: return 0xFF;
			}
		}
		/* Else: 4 bits */
		return (idx << 4) | idx;
}

static void wwrite(int fd, uint8_t*buf, unsigned len)
{
	unsigned written=0;
	do {
		int r = write(fd, buf+written, len-written);
		unsigned rl;
		if (r==-1)
			xout("write",NULL, 2);
		rl = r;
		if (rl == 0) {
			fprintf(stderr,"short write - disk full?");
			exit(2);
		}
		written += rl;
	} while (written < len);
}

static void pngDraw_init(PNGDRAW *d)
{
	uint8_t *bm_palette = winbmphdr + 54;
	int palettecnt = 0;
	int i;

	switch (d->iPixelType) {
		/* Synth a palette */
		case PNG_PIXEL_GRAYSCALE:				
			switch (d->iBpp) {
					case 1: palettecnt = 2; break;
					case 2: palettecnt = 4; break;
					case 4: palettecnt = 16; break;
					case 8: palettecnt = 256; break;
					case 16: palettecnt = 256; break;
			}
			for (i = 0; i < palettecnt; i++) {
				uint8_t v = expandbits8(i, d->iBpp);
				bm_palette[i*4+0] = v;
				bm_palette[i*4+1] = v;
				bm_palette[i*4+2] = v;
			}
			winbmphdr[28] = palettecnt > 16 ? 8 : 4;
			if (d->iTransLen==1) { // bpp 1-8: we can adjust the palette for the background
				uint8_t br,bg,bb;
				if (desiredBackground>=0) {
					br =  desiredBackground       & 0xFF;
					bg = (desiredBackground >> 8) & 0xFF;
					bb = (desiredBackground >> 16)& 0xFF;
				} else if (d->iBackground>=0) {
					br = expandbits8(d->iBackground, d->iBpp);
					bg = br;
					bb = br;
				} else {
					br = bg = bb = 0x99;
				}
				bm_palette[d->iTrans[0]*4 +0] = bb;
				bm_palette[d->iTrans[0]*4 +1] = bg;
				bm_palette[d->iTrans[0]*4 +2] = br;
			}
			if (d->iTransLen==2) { // 16bpp: figure out the background color
				uint8_t bgr = 0x99;
				// we limit the background color to grayscale for this
				// otherwise we'd need to steal a palette entry and prevent other colors from mapping to it...
				// which would be extra work for the pixel copy loop, so I'd rather not.
				if (desiredBackground>=0) {
					uint16_t bgr_;
					uint8_t br,bg,bb;
					br =  desiredBackground       & 0xFF;
					bg = (desiredBackground >> 8) & 0xFF;
					bb = (desiredBackground >> 16)& 0xFF;
					/* Doing this properly was such a rabbit hole that i decided to just "fuck it, use a third" */
					bgr_ = br+bg+bb;
					bgr = (bgr_+1) / 3;
				} else if (d->iBackground>=0) {
					bgr = d->iBackground;
				}
				/* Use desiredBackground to store the computed value :) */
				desiredBackground = bgr;
			}
			break;
			
		case PNG_PIXEL_INDEXED:
			palettecnt = d->iPaletteCnt;
			if (!palettecnt) {
				fprintf(stderr,"PNG: No palette\n");
				exit(4);
			}
			switch (d->iBpp) {
					case 1:
					case 2:
					case 4:
						winbmphdr[28] = 4;
						break;
			}
			for (i = 0; i < palettecnt; i++) {
				bm_palette[i*4 + 0] = d->pPalette[(i*3)+2];
				bm_palette[i*4 + 1] = d->pPalette[(i*3)+1];
				bm_palette[i*4 + 2] = d->pPalette[(i*3)+0];
				bm_palette[i*4 + 3] = 0;
			}
			/* Transform the palette with alpha to a palette without alpha. */
			if (d->iHasAlpha) {
				uint8_t br,bg,bb;
				if (desiredBackground>=0) {
					br =  desiredBackground       & 0xFF;
					bg = (desiredBackground >> 8) & 0xFF;
					bb = (desiredBackground >> 16)& 0xFF;
				} else if (d->iBackground>=0) {
					br = d->pPalette[d->iBackground*3 + 0];
					bg = d->pPalette[d->iBackground*3 + 1];
					bb = d->pPalette[d->iBackground*3 + 2];
				} else {
					br = bg = bb = 0x99;
				}
				for (i = 0; i < palettecnt; i++) {
					uint8_t a = d->pPalette[768 + i];
					if (a==255) continue;
					if (a==0) {
						bm_palette[i*4 + 0] = bb;
						bm_palette[i*4 + 1] = bg;
						bm_palette[i*4 + 2] = br;
					} else {
						uint16_t b_r=br, b_g=bg, b_b=bb;
						uint16_t r,g,b;
						b = bm_palette[i*4 + 0];
						g = bm_palette[i*4 + 1];
						r = bm_palette[i*4 + 2];
						bm_palette[i*4+2] = ((r * a) + (b_r * (255-a))) >> 8;
                        bm_palette[i*4+1] = ((g * a) + (b_g * (255-a))) >> 8;
                        bm_palette[i*4+0] = ((b * a) + (b_b * (255-a))) >> 8;
					}
				}
			}
			break;

		case PNG_PIXEL_TRUECOLOR:
		case PNG_PIXEL_GRAY_ALPHA:
		case PNG_PIXEL_TRUECOLOR_ALPHA:
			winbmphdr[28] = 24;
			break;
			
		default:
			printf("ooops1\n");
			exit(9);
			break;
	}
	*(short*)(winbmphdr+46) = palettecnt;
	bm_bitoff = 54 + (4*palettecnt);
	*(int32_t*)(winbmphdr+2) = bm_bitoff + pngHeight * BmpStride;
	*(short*)(winbmphdr+10) = bm_bitoff;
	*(int32_t*)(winbmphdr+18) = d->iWidth;
	*(int32_t*)(winbmphdr+22) = pngHeight;
	
	wwrite(d->User, winbmphdr, bm_bitoff);
}

static void pngDraw(PNGDRAW *d)
{
	int32_t i;
	off_t linesdown;
	off_t dyp;
	uint8_t *line = BMPLine;
	int32_t linepitch = BmpStride;
	uint8_t br,bb,bg;
	
	if (d->y==0) /* Initialize */
		pngDraw_init(d);
	
	/* BMP is a horrible format. The Arachne BMP reader is even more horrible. */
	linesdown = (pngHeight - d->y) -1;
	dyp = bm_bitoff + (linesdown * BmpStride);
	lseek(d->User, dyp, SEEK_SET);

	/* Determine the background color (for these 3 formats) */
	switch  (d->iPixelType) {
		case PNG_PIXEL_TRUECOLOR:
		case PNG_PIXEL_TRUECOLOR_ALPHA:
			if (desiredBackground>=0) {
				br =  desiredBackground       & 0xFF;
				bg = (desiredBackground >> 8) & 0xFF;
				bb = (desiredBackground >> 16)& 0xFF;
			} else {
				br =  d->iBackground       & 0xFF;
				bg = (d->iBackground >> 8) & 0xFF;
				bb = (d->iBackground >> 16)& 0xFF;
			}
			break;

		case PNG_PIXEL_GRAY_ALPHA:
			if (desiredBackground>=0) {
				br =  desiredBackground       & 0xFF;
				bg = (desiredBackground >> 8) & 0xFF;
				bb = (desiredBackground >> 16)& 0xFF;
			} else {
				br = d->iBackground;
				bg = d->iBackground;
				bb = d->iBackground;
			}
			break;
	}
	
	switch (d->iPixelType) {
		case PNG_PIXEL_GRAYSCALE:				
		case PNG_PIXEL_INDEXED:
			switch(d->iBpp) {
				case 1:
					for (i = 0; i < d->iPitch; i++) {
						uint8_t v = d->pPixels[i];
						line[i*4 + 0] = (v & 0x80) >> 3 | (v & 0x40) >> 6;
						line[i*4 + 1] = (v & 0x20) >> 1 | (v & 0x10) >> 4;
						line[i*4 + 2] = (v & 0x08) << 1 | (v & 0x04) >> 2;
						line[i*4 + 3] = (v & 0x02) << 3 | (v & 0x01);
					}
					break;
				case 2:
					for (i = 0; i < d->iPitch; i++) {
						uint8_t v = d->pPixels[i];
						line[i*2 + 0] = (v & 0xC0) >> 2 | (v & 0x30) >> 4;
						line[i*2 + 1] = (v & 0x0C) << 2 | (v & 0x03);
					}
					break;
				case 4:
				case 8:
					line = d->pPixels;
					linepitch = d->iPitch;
					break;
				case 16: /* 16b grayscale, operated like 8bit grayscale except we need to splice the bytes out of there */
					if (d->iTransLen==2) { // iTRNS comparison, full 16bpp
						for (i = 0; i < d->iPitch; i += 2) {
							if (memcmp(d->iTrans, d->pPixels+i, 2)==0) {
								line[i/2] = desiredBackground;
							} else {
								line[i/2] = d->pPixels[i];
							}
						}
					} else {
						for (i = 0;i < d->iPitch; i += 2) {
							line[i/2] = d->pPixels[i];
						}
					}
					break;
					
			}
			break;

		case PNG_PIXEL_TRUECOLOR:
			/* BMP is a horrible format. */
			if (d->iBpp > 8) {
				if (d->iTransLen==6) {
					for (i = 0; i < d->iPitch; i+= 6) {
						if (memcmp(d->iTrans, d->pPixels+i, 6)==0) {
							line[(i/2)+2] = br;
							line[(i/2)+1] = bg;
							line[(i/2)+0] = bb;
						} else {
							line[(i/2)+2] = d->pPixels[i+0];
							line[(i/2)+1] = d->pPixels[i+2];
							line[(i/2)+0] = d->pPixels[i+4];
						}
					}
				} else {
					for (i = 0; i < d->iPitch; i+= 6) {
						line[(i/2)+2] = d->pPixels[i+0];
						line[(i/2)+1] = d->pPixels[i+2];
						line[(i/2)+0] = d->pPixels[i+4];
					}
				}
			} else {
				if (d->iTransLen==3) {
					for (i = 0; i < d->iPitch; i+= 3) {
						if (memcmp(d->iTrans, d->pPixels+i, 3)==0) {
							line[i+2] = br;
							line[i+1] = bg;
							line[i+0] = bb;
						} else {
							line[i+2] = d->pPixels[i+0];
							line[i+1] = d->pPixels[i+1];
							line[i+0] = d->pPixels[i+2];
						}
					}
				} else {
					for (i = 0; i < d->iPitch; i+= 3) {
						line[i+2] = d->pPixels[i+0];
						line[i+1] = d->pPixels[i+1];
						line[i+0] = d->pPixels[i+2];
					}
				}
			}
			break;
			
		/* And the alpha here is not implemented because i dont parse bKGD yet :P, and also because lazy,
		 * and also because i'd like to hook up the background color from arachne, but also that 
		 * it'd really make more sense to integrate the whole thing into arachne, so why bother with
		 * fancy features... */
			
		case PNG_PIXEL_GRAY_ALPHA:
			if (d->iBpp > 8) {
				for (i = 0; i < d->iPitch; i += 4) {
					uint8_t v = d->pPixels[i];
					uint8_t a = d->pPixels[i+2];
					if (a == 255) {
						line[(i/4)*3 + 0] = v;
						line[(i/4)*3 + 1] = v;
						line[(i/4)*3 + 2] = v;
					} else if (a == 0) {
						line[(i/4)*3 + 0] = bb;
						line[(i/4)*3 + 1] = bg;
						line[(i/4)*3 + 2] = br;						
					} else {
						uint16_t vb=v, b_r=br,b_g=bg, b_b=bb;
						line[(i/4)*3 + 0] =	((vb * a) + (b_b * (255-a))) >> 8;
						line[(i/4)*3 + 1] =	((vb * a) + (b_g * (255-a))) >> 8;
						line[(i/4)*3 + 2] =	((vb * a) + (b_r * (255-a))) >> 8;
					}
				}
			} else {
				for (i = 0; i < d->iPitch; i += 2) {
					uint8_t v = d->pPixels[i];
					uint8_t a = d->pPixels[i+1];
					if (a == 255) {
						line[(i/2)*3 + 0] = v;
						line[(i/2)*3 + 1] = v;
						line[(i/2)*3 + 2] = v;
					} else if (a == 0) {
						line[(i/2)*3 + 0] = bb;
						line[(i/2)*3 + 1] = bg;
						line[(i/2)*3 + 2] = br;						
					} else {
						uint16_t vb=v, b_r=br,b_g=bg, b_b=bb;
						line[(i/2)*3 + 0] =	((vb * a) + (b_b * (255-a))) >> 8;
						line[(i/2)*3 + 1] =	((vb * a) + (b_g * (255-a))) >> 8;
						line[(i/2)*3 + 2] =	((vb * a) + (b_r * (255-a))) >> 8;
					}
				}
			}
			break;
				
		case PNG_PIXEL_TRUECOLOR_ALPHA:
			if (d->iBpp > 8) {
				for (i = 0; i < d->iPitch; i += 8) {
					uint8_t a = d->pPixels[i + 6];
					if (a==255) {
						line[(i/8)*3 + 2] = d->pPixels[i + 0];
						line[(i/8)*3 + 1] = d->pPixels[i + 2];
						line[(i/8)*3 + 0] = d->pPixels[i + 4];
					} else if (a==0) {
						line[(i/8)*3 + 0] = bb;
						line[(i/8)*3 + 1] = bg;
						line[(i/8)*3 + 2] = br;						
					} else {
						uint16_t r, g, b, b_r=br,b_g=bg, b_b=bb;
						r = d->pPixels[i + 0];
						g = d->pPixels[i + 2];
						b = d->pPixels[i + 4];
						line[(i/8)*3 + 0] = ((b * a) + (b_b * (255-a))) >> 8;
						line[(i/8)*3 + 1] = ((g * a) + (b_g * (255-a))) >> 8;
						line[(i/8)*3 + 2] = ((r * a) + (b_r * (255-a))) >> 8;
					}
				}
			} else {
				for (i = 0; i < d->iPitch; i += 4) {
					uint8_t a = d->pPixels[i + 3];
					if (a==255) {
						line[(i/4)*3 + 2] = d->pPixels[i + 0];
						line[(i/4)*3 + 1] = d->pPixels[i + 1];
						line[(i/4)*3 + 0] = d->pPixels[i + 2];
					} else if (a==0) {
						line[(i/4)*3 + 0] = bb;
						line[(i/4)*3 + 1] = bg;
						line[(i/4)*3 + 2] = br;						
					} else {
						uint16_t r, g, b, b_r=br,b_g=bg, b_b=bb;
						r = d->pPixels[i + 0];
						g = d->pPixels[i + 1];
						b = d->pPixels[i + 2];
						line[(i/4)*3 + 0] = ((b * a) + (b_b * (255-a))) >> 8;
						line[(i/4)*3 + 1] = ((g * a) + (b_g * (255-a))) >> 8;
						line[(i/4)*3 + 2] = ((r * a) + (b_r * (255-a))) >> 8;
					}						
				}
			}
			break;
	}
	
	wwrite(d->User, line, linepitch);
	if (linepitch < BmpStride) {
		uint8_t z[4] = { 0,0,0,0 };
		wwrite(d->User, z, BmpStride - linepitch);
	}
}

int main(int argc, char **argv)
{
	PNGIMAGE *pPNG;
	const int argoff = 1;
	int ifd, ofd;
	int r;
	if (argc < 3) {
		fprintf(stderr,"usage: png2bmp <in.png> <out.bmp>\n");
		return 1;
	}
	
	/* N.B. We can exit() without freeing everything, it's fine. */
	pPNG = calloc(1, sizeof(PNGIMAGE));
	if (!pPNG) xout("malloc", "PNGIMAGE", 3);
	
	ifd = open(argv[argoff], O_RDONLY|O_BINARY);
	if (ifd < 0) xout("open", argv[argoff], 1);
	
    pPNG->pfnRead = pngRead;
    pPNG->pfnSeek = pngSeek;
    pPNG->pfnDraw = pngDraw;
    pPNG->PNGFile.fHandle = ifd;
    pPNG->PNGFile.iSize = lseek(pPNG->PNGFile.fHandle, 0, SEEK_END);
	lseek(pPNG->PNGFile.fHandle, 0, SEEK_SET);
	
	r = PNG_init(pPNG);
	if (r) {
		fprintf(stderr, "PNG Error (Header): %d\n", pPNG->iError);
		exit(4);
	}
	
    pPNG->uLine1 = malloc(pPNG->iPitch+1);
	if (!pPNG->uLine1)
		xout("malloc", "Line1", 3);
		
    pPNG->uLine2 = malloc(pPNG->iPitch+1);
	if (!pPNG->uLine2)
		xout("malloc", "Line2", 3);

	/*printf("PNG Info: %ldx%ld %d bpp color type %d\n",
		(long)pPNG->iWidth, (long)pPNG->iHeight, pPNG->ucBpp, pPNG->ucPixelType); */
	pngHeight = pPNG->iHeight;
	
	switch (pPNG->ucPixelType) {
		case PNG_PIXEL_GRAYSCALE:
		case PNG_PIXEL_INDEXED:
			switch (pPNG->ucBpp) {
					case 1:
					case 2:
					case 4:
						BmpStride = (pPNG->iWidth + 1) / 2;
						break;
					case 8:
					case 16:
						BmpStride = pPNG->iWidth;
						break;
			}
			break;
		case PNG_PIXEL_GRAY_ALPHA:
		case PNG_PIXEL_TRUECOLOR:
		case PNG_PIXEL_TRUECOLOR_ALPHA:
			BmpStride = pPNG->iWidth*3;
			break;
	}
	BmpStride = (BmpStride + 3) & ~3;
	
	if (BmpStride >= 65535) {
		fprintf(stderr,"BMP output too wide (%ld bytes per line)\n", (long)BmpStride);
		exit(3);
	}
	
	BMPLine = malloc(BmpStride);
	if (!BMPLine) xout("malloc", "BMPLine", 3);
	
	ofd = open(argv[argoff+1], O_RDWR|O_BINARY|O_CREAT, 0644);
	if (ofd < 0) xout("create", argv[argoff+1], 2);
	
	r = PNG_decode(pPNG, ofd, 0);
	if (r) {
		fprintf(stderr, "PNG Error (Decode): %d\n", pPNG->iError);
		exit(4);
	}
	close(ofd);
	return 0;
}
