#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>

#include "framebuffer.h"

int fb_alloc(struct framebuffer** framebuffer, uint16_t width, uint16_t height) {
    printf("Allocating framebuffer of size (%u,%u)\n", width, height);
    struct framebuffer* fb = malloc(sizeof(struct framebuffer));
    if(!fb) {
        return -ENOMEM;
    }

    fb->width = width;
    fb->height = height;

    fb->pixels = calloc(width * height, sizeof(uint32_t));
    if(!fb->pixels) {
        return -ENOMEM;
    }

    *framebuffer = fb;
    return 0;
}

// Only sets pixel if it is within bounds
void fb_set(struct framebuffer* framebuffer, uint16_t x, uint16_t y, uint32_t rgba) {
    if (x < framebuffer->width && y < framebuffer->height) {
        framebuffer->pixels[x + y * framebuffer->width] = rgba;
    }
}

// Does *not* check for bounds
uint32_t fb_get(struct framebuffer* framebuffer, uint16_t x, uint16_t y) {
    return framebuffer->pixels[x + y * framebuffer->width];
}
