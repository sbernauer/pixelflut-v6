#ifndef _IMAGE_H_
#define _IMAGE_H_

struct fluter_image {
    unsigned int width;
    unsigned int height;

    uint32_t* pixels;
};

int load_image(struct fluter_image** fluter_image, char* file_name);

#endif
