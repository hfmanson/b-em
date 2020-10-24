#ifndef __INC_BBCTEXT_H
#define __INC_BBCTEXT_H

#include <stdint.h>

#define MODE7_DISPLAY_WIDTH 16
#define MODE7_NEW_CHAR_WIDTH 12
#define MODE7_CHAR_HEIGHT 10
#define MODE7_CHAR_NEW_HEIGHT 18
#define MODE7_FIRST_CHAR 0x20
#define MODE7_LAST_CHAR 0x7F
#define MODE7_CHAR_COUNT (MODE7_LAST_CHAR + 1 - MODE7_FIRST_CHAR)
#define MODE7_CHAR_ARRAY_SIZE (MODE7_CHAR_COUNT * MODE7_DISPLAY_WIDTH * MODE7_CHAR_HEIGHT)
#define BBC_NEW_TEXT_SIZE (MODE7_CHAR_COUNT * MODE7_NEW_CHAR_WIDTH * MODE7_CHAR_NEW_HEIGHT)


/* Normal Character Set */
uint8_t teletext_characters[];
/* Graphics Character Set */
uint8_t teletext_graphics[];
/* Separated Graphics Character Set */
uint8_t teletext_separated_graphics[];

#endif
