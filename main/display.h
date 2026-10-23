#pragma once
#include <stdint.h>
#include <stddef.h>

void displayInit(void);
void displayClock(int hour, int minute, int day, int month, int year, int dayOfWeek);
void displayMediaWithArt(const char *title, const char *artist, bool playing,
                         const uint8_t *artBitmap, unsigned artW, unsigned artH);
void displaySpotifyLogo(void);
void displayCassette(void);
void display_set_cover_art_jpeg(const uint8_t *jpeg, size_t jpeg_len);
bool display_has_cover_art(void);
unsigned display_get_cover_art_width(void);
unsigned display_get_cover_art_height(void);
unsigned display_get_cover_art_data(uint8_t *buffer, unsigned bufsize);
void display_print_cover_art(void);