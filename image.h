#ifndef _IMAGE_H_
#define _IMAGE_H_

#if IMAGICK < 7
	#include <wand/MagickWand.h>
#else
	#include <MagickWand/MagickWand.h>
#endif

struct fluter_image {
    unsigned int width;
    unsigned int height;

    uint32_t* pixels;
};

int load_image(struct fluter_image** ret, char* file_name);

#endif
