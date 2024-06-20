#if IMAGICK < 7
    #include <wand/MagickWand.h>
#else
    #include <MagickWand/MagickWand.h>
#endif

#include "image.h"

int load_image(struct fluter_image** ret, char* file_name) {
    int err = 0;
    MagickBooleanType isOK;

    struct fluter_image* fluter_image = malloc(sizeof(struct fluter_image));

    MagickWand* m_wand = NewMagickWand();

    MagickWandGenesis();

    isOK = MagickReadImage(m_wand, file_name);
    if (isOK == MagickFalse) {
        return -ENOENT;
    }

    int width = MagickGetImageWidth(m_wand);
    int height = MagickGetImageHeight(m_wand);
    printf("Loaded image with (%u, %u) pixels\n", width, height);

    unsigned char* pixel_colors = (char *)malloc(width * height * 4);
    isOK = MagickExportImagePixels(m_wand, 0, 0, width, height, "RGBA", CharPixel, pixel_colors);
    if (isOK == MagickFalse) {
        fprintf(stderr, "Failed to export pixels from image %s\n", file_name);
        return -1;
    }

    if(m_wand) {
        DestroyMagickWand(m_wand);	
    }
    
    MagickWandTerminus();

    fluter_image->width = width;
    fluter_image->height = height;
    // They have the same memory layout
    fluter_image->pixels = (uint32_t*)pixel_colors;

    *ret = fluter_image;
    return 0;
}
