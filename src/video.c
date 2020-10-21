/*B-em v2.2 by Tom Walker
  Video emulation
  Incorporates 6845 CRTC, Video ULA and SAA5050*/

#include <allegro5/allegro_native_dialog.h>
#include <allegro5/allegro_primitives.h>
#include "b-em.h"

#include "bbctext.h"
#include "mem.h"
#include "model.h"
#include "serial.h"
#include "tape.h"
#include "via.h"
#include "sysvia.h"
#include "video.h"
#include "video_render.h"

int fullscreen = 0;

static int scrx, scry;
int interlline = 0;

static int colblack;
static int colwhite;

/*6845 CRTC*/
uint8_t crtc[32];
static const uint8_t crtc_mask[32] = { 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0x1F, 0x7F, 0x7F, 0xF3, 0x1F, 0x7F, 0x1F, 0x3F, 0xFF, 0x3F, 0xFF, 0x3F, 0xFF };

int crtc_i;

int hc, vc, sc;
static int vadj;
uint16_t ma, ttxbank;
static uint16_t maback;
static int vdispen, dispen;
static int crtc_mode;

void crtc_reset()
{
    hc = vc = sc = vadj = 0;
    crtc[9] = 10;
}

static void set_intern_dtype(enum vid_disptype dtype)
{
    if (crtc_mode == 0 && (crtc[8] & 1))
        dtype = VDT_INTERLACE;
    else if (dtype == VDT_INTERLACE && !(crtc[8] & 1))
        dtype = VDT_SCALE;
    vid_dtype_intern = dtype;
}

static void crtc_setreg(int reg, uint8_t val)
{
    val &= crtc_mask[reg];
    crtc[reg] = val;
    if (crtc_i == 6 && vc == val)
        vdispen = 0;
    else if (reg == 8)
        set_intern_dtype(vid_dtype_user);
    else if (reg == 12)
        ttxbank = (MASTER|BPLUS) ? 0x7c00 : 0x3C00 | ((val & 0x8) << 11);
}

void crtc_write(uint16_t addr, uint8_t val)
{
//        log_debug("Write CRTC %04X %02X %04X\n",addr,val,pc);
    if (!(addr & 1))
        crtc_i = val & 31;
    else
        crtc_setreg(crtc_i, val);
}

uint8_t crtc_read(uint16_t addr)
{
    if (!(addr & 1))
        return crtc_i;
    return crtc[crtc_i];
}

void crtc_latchpen()
{
    crtc[0x10] = (ma >> 8) & 0x3F;
    crtc[0x11] = ma & 0xFF;
}

void crtc_savestate(FILE * f)
{
    uint8_t bytes[25];
    for (int c = 0; c < 18; c++)
        bytes[c] = crtc[c];
    bytes[18] = vc;
    bytes[19] = sc;
    bytes[20] = hc;
    bytes[21] = ma;
    bytes[22] = ma >> 8;
    bytes[23] = maback;
    bytes[24] = maback >> 8;
    fwrite(bytes, sizeof(bytes), 1, f);
}

void crtc_loadstate(FILE * f)
{
    uint8_t bytes[25];
    fread(bytes, sizeof(bytes), 1, f);
    vc = bytes[18];
    sc = bytes[19];
    hc = bytes[20];
    ma = bytes[21] | (bytes[22] << 8);
    maback = bytes[23] | (bytes[24] << 8);
    for (int c = 0; c < 18; c++)
        crtc_setreg(c, bytes[c]);
}


/*Video ULA (VIDPROC)*/
uint8_t ula_ctrl;
static int ula_pal[16];         // maps from actual physical colour to bitmap display
uint8_t ula_palbak[16];         // palette RAM in orginal ULA maps actual colour to logical colour
static int ula_mode;
int nula_collook[16];           // maps palette (logical) colours to 12-bit RGB

static uint8_t table4bpp[4][256][16];

static int nula_pal_write_flag = 0;
static uint8_t nula_pal_first_byte;
uint8_t nula_flash[8];

uint8_t nula_palette_mode;
uint8_t nula_horizontal_offset;
uint8_t nula_left_blank;
uint8_t nula_disable;
uint8_t nula_attribute_mode;
uint8_t nula_attribute_text;

static int nula_left_cut;
static int nula_left_edge;
static int mode7_need_new_lookup;

static inline uint32_t makecol(int red, int green, int blue)
{
    return 0xff000000 | (red << 16) | (green << 8) | blue;
}

static inline int get_pixel(ALLEGRO_LOCKED_REGION *region, int x, int y)
{
    return *((uint32_t *)((char *)region->data + region->pitch * y + x * region->pixel_size));
}

#ifdef PIXEL_BOUNDS_CHECK

static inline void put_pixel_checked(ALLEGRO_LOCKED_REGION *region, int x, int y, uint32_t colour, int line)
{
    if (x < 0 || x > 1280)
        log_debug("video: pixel out of bounds, x=%d at %d", x, line);
    if (y < 0 || y > 800)
        log_debug("video: pixel out of bounds, y=%d at %d", y, line);
    *((uint32_t *)((char *)region->data + region->pitch * y + x * region->pixel_size)) = colour;
}

static inline void put_pixels_checked(ALLEGRO_LOCKED_REGION *region, int x, int y, int count, uint32_t colour, int line)
{
    char *ptr = (char *)region->data + region->pitch * y + x * region->pixel_size;
    if (x < 0 || (x + count) > 1280)
        log_debug("video: pixel out of bounds, x=%d at %d", x, line);
    if (y < 0 || y > 800)
        log_debug("video: pixel out of bounds, y=%d at %d", y, line);
    while (count--) {
        *(uint32_t *)ptr = colour;
        ptr += region->pixel_size;
    }
}

static inline void nula_putpixel_checked(ALLEGRO_LOCKED_REGION *region, int x, int y, uint32_t colour, int line)
{
    if (crtc_mode && (nula_horizontal_offset || nula_left_blank) && (x < nula_left_cut || x >= nula_left_edge + (crtc[1] * crtc_mode * 8)))
        put_pixel_checked(region, x, y, colblack, line);
    else if (x < 1280)
        put_pixel_checked(region, x, y, colour, line);
}

#define put_pixel(region, x, y, colour) put_pixel_checked(region, x, y, colour, __LINE__)
#define put_pixels(region, x, y, count, colour) put_pixels_checked(region, x, y, count, colour, __LINE__)
#define nula_putpixel(region, x, y, colour) nula_putpixel_checked(region, x, y, colour, __LINE__)

#else

static inline void put_pixel(ALLEGRO_LOCKED_REGION *region, int x, int y, uint32_t colour)
{
    *((uint32_t *)((char *)region->data + region->pitch * y + x * region->pixel_size)) = colour;
}

static inline void put_pixels(ALLEGRO_LOCKED_REGION *region, int x, int y, int count, uint32_t colour)
{
    char *ptr = (char *)region->data + region->pitch * y + x * region->pixel_size;
    while (count--) {
        *(uint32_t *)ptr = colour;
        ptr += region->pixel_size;
    }
}

static inline void nula_putpixel(ALLEGRO_LOCKED_REGION *region, int x, int y, uint32_t colour)
{
    if (crtc_mode && (nula_horizontal_offset || nula_left_blank) && (x < nula_left_cut || x >= nula_left_edge + (crtc[1] * crtc_mode * 8)))
        put_pixel(region, x, y, colblack);
    else if (x < 1280)
        put_pixel(region, x, y, colour);
}

#endif

static void nula_default_palette(void)
{
    nula_collook[0]  = 0xff000000; // black
    nula_collook[1]  = 0xffff0000; // red
    nula_collook[2]  = 0xff00ff00; // green
    nula_collook[3]  = 0xffffff00; // yellow
    nula_collook[4]  = 0xff0000ff; // blue
    nula_collook[5]  = 0xffff00ff; // magenta
    nula_collook[6]  = 0xff00ffff; // cyan
    nula_collook[7]  = 0xffffffff; // white
    nula_collook[8]  = 0xff000000; // black
    nula_collook[9]  = 0xffff0000; // red
    nula_collook[10] = 0xff00ff00; // green
    nula_collook[11] = 0xffffff00; // yellow
    nula_collook[12] = 0xff0000ff; // blue
    nula_collook[13] = 0xffff00ff; // magenta
    nula_collook[14] = 0xff00ffff; // cyan
    nula_collook[15] = 0xffffffff; // white

    mode7_need_new_lookup = 1;
}

void nula_reset(void)
{
    // Reset NULA
    nula_palette_mode = 0;
    nula_horizontal_offset = 0;
    nula_left_blank = 0;
    nula_attribute_mode = 0;
    nula_attribute_text = 0;

    // Reset palette
    nula_default_palette();

    // Reset flash
    for (int c = 0; c < 8; c++)
        nula_flash[c] = 1;
}

void videoula_write(uint16_t addr, uint8_t val)
{
    int c;
    if (nula_disable)
        addr &= ~2;             // nuke additional NULA addresses

    switch (addr & 3) {
    case 0:
        {
            // Video control register.
            // log_debug("video: ULA write VCR from %04X: %02X %i %i\n",pc,val,hc,vc);

            if ((ula_ctrl ^ val) & 1) {
                // Flashing colour control bit has changed.
                if (val & 1) {
                    for (c = 0; c < 16; c++) {
                        if ((ula_palbak[c] & 8) && nula_flash[(ula_palbak[c] & 7) ^ 7])
                            ula_pal[c] = nula_collook[ula_palbak[c] & 15];
                        else
                            ula_pal[c] = nula_collook[(ula_palbak[c] & 15) ^ 7];
                    }
                } else {
                    for (c = 0; c < 16; c++)
                        ula_pal[c] = nula_collook[(ula_palbak[c] & 15) ^ 7];
                }
            }
            ula_ctrl = val;
            ula_mode = (ula_ctrl >> 2) & 3;
            if (val & 2)
                crtc_mode = 0;  // Teletext
            else if (val & 0x10)
                crtc_mode = 1;  // High frequency
            else
                crtc_mode = 2;  // Low frequency
            set_intern_dtype(vid_dtype_user);
        }
        break;

    case 1:
        {
            // Palette register.
            // log_debug("video: ULA write palette from %04X: %02X map l=%x->p=%x %i %i\n",pc,val, val >> 4, (val & 0x0f) ^ 0x07, hc, vc);
            uint8_t code = val >> 4;
            ula_palbak[code] = val & 15;
            ula_pal[code] = nula_collook[(val & 15) ^ 7];
            if ((val & 8) && (ula_ctrl & 1) && nula_flash[val & 7])
                ula_pal[code] = nula_collook[val & 15];
        }
        break;

    case 2:                     // &FE22 = NULA CONTROL REG
        {
            uint8_t code = val >> 4;
            uint8_t param = val & 0xf;

            switch (code) {
            case 1:
                nula_palette_mode = param & 1;
                break;

            case 2:
                nula_horizontal_offset = param & 7;
                break;

            case 3:
                nula_left_blank = param & 15;
                break;

            case 4:
                nula_reset();
                break;

            case 5:
                nula_disable = 1;
                break;

            case 6:
                nula_attribute_mode = param & 1;
                break;

            case 7:
                nula_attribute_text = param & 1;
                break;

            case 8:
                nula_flash[0] = param & 8;
                nula_flash[1] = param & 4;
                nula_flash[2] = param & 2;
                nula_flash[3] = param & 1;
                break;

            case 9:
                nula_flash[4] = param & 8;
                nula_flash[5] = param & 4;
                nula_flash[6] = param & 2;
                nula_flash[7] = param & 1;
                break;

            default:
                break;
            }

        }
        break;

    case 3:                     // &FE23 = NULA PALETTE REG
        {
            if (nula_pal_write_flag) {
                // Commit the write to palette
                int c = (nula_pal_first_byte >> 4);
                int r = nula_pal_first_byte & 0x0f;
                int g = (val & 0xf0) >> 4;
                int b = val & 0x0f;
                nula_collook[c] = makecol(r | r << 4, g | g << 4, b | b << 4);
                // Manual states colours 8-15 are set solid by default
                if (c & 8)
                    nula_flash[c - 8] = 0;
                // Reset all colour lookups
                for (c = 0; c < 16; c++) {
                    ula_pal[c] = nula_collook[(ula_palbak[c] & 15) ^ 7];
                    if ((ula_palbak[c] & 8) && (ula_ctrl & 1) && nula_flash[(ula_palbak[c] & 7) ^ 7])
                        ula_pal[c] = nula_collook[ula_palbak[c] & 15];
                }
                mode7_need_new_lookup = 1;
            } else {
                // Remember the first byte
                nula_pal_first_byte = val;
            }

            nula_pal_write_flag = !nula_pal_write_flag;
        }
        break;

    }
}

void videoula_savestate(FILE * f)
{
    unsigned char bytes[97], *ptr = bytes;
    *ptr++ = ula_ctrl;
    for (int c = 0; c < 16; c++)
        *ptr++ = ula_palbak[c];
    for (int c = 0; c < 16; c++) {
        uint32_t v = nula_collook[c];
        *ptr++ = (v >> 16) & 0xff; // red
        *ptr++ = (v >> 8) & 0xff;  // green
        *ptr++ = v & 0xff;         // blue
        *ptr++ = (v >> 24) & 0xff; // alpha
    }
    *ptr++ = nula_pal_write_flag;
    *ptr++ = nula_pal_first_byte;
    for (int c = 0; c < 8; c++)
        *ptr++ = nula_flash[c];
    *ptr++ = nula_palette_mode;
    *ptr++ = nula_horizontal_offset;
    *ptr++ = nula_left_blank;
    *ptr++ = nula_disable;
    *ptr++ = nula_attribute_mode;
    *ptr++ = nula_attribute_text;
    fwrite(bytes, ptr-bytes, 1, f);
}

void videoula_loadstate(FILE * f)
{
    unsigned char bytes[97], *ptr = bytes;
    fread(bytes, sizeof(bytes), 1, f);
    videoula_write(0, *ptr++);
    for (int c = 0; c < 16; c++)
        videoula_write(1, *ptr++ | (c << 4));
    for (int c= 0; c < 16; c++) {
        nula_collook[c] = (ptr[3] << 24) | (ptr[0] << 16) | (ptr[1] << 8) | ptr[2];
        ptr += 4;
    }
    nula_pal_write_flag = *ptr++;
    nula_pal_first_byte = *ptr++;
    for (int c = 0; c < 8; c++)
        nula_flash[c] = *ptr++;
    nula_palette_mode = *ptr++;
    nula_horizontal_offset = *ptr++;
    nula_left_blank = *ptr++;
    nula_disable = *ptr++;
    nula_attribute_mode = *ptr++;
    nula_attribute_text = *ptr++;
}

/*Mode 7 (SAA5050)*/
static uint8_t mode7_chars[MODE7_CHAR_ARRAY_SIZE], mode7_charsi[MODE7_CHAR_ARRAY_SIZE], mode7_graph[MODE7_CHAR_ARRAY_SIZE], mode7_graphi[MODE7_CHAR_ARRAY_SIZE], mode7_sepgraph[MODE7_CHAR_ARRAY_SIZE], mode7_sepgraphi[MODE7_CHAR_ARRAY_SIZE];
static int mode7_lookup[8][8][MODE7_DISPLAY_WIDTH];

static int mode7_col = 7, mode7_bg = 0;
static int mode7_sep = 0;
static int mode7_dbl, mode7_nextdbl, mode7_wasdbl;
static int mode7_gfx;
static int mode7_flash, mode7_flashon = 0, mode7_flashtime = 0;
static uint8_t mode7_buf[2];
static uint8_t *mode7_p[2] = { mode7_chars, mode7_charsi };

static uint8_t mode7_heldchar, mode7_holdchar;
static uint8_t *mode7_heldp[2];

void mode7_makechars()
{
    int c, d, y;
    int offs1 = 0, offs2 = 0;
    float x;
    int x2;
    int stat;
    uint8_t* p = teletext_characters;
    for (c = 0; c < BBC_NEW_TEXT_SIZE; c++)
        teletext_characters[c] *= 15;
    offs2 = 0;
    for (c = 0; c < MODE7_CHAR_COUNT ; c++) {
        for (y = 0; y < MODE7_CHAR_HEIGHT; y++) {
            int mask = 0x800;
            for (d = 0; d < MODE7_NEW_CHAR_WIDTH; d++) {
                int idx = (c * MODE7_CHAR_NEW_HEIGHT + 2 * y) * MODE7_NEW_CHAR_WIDTH + d;
                int idxi = idx + MODE7_NEW_CHAR_WIDTH;
                if (y < MODE7_CHAR_HEIGHT - 1) {
                    mode7_chars[offs2 + d] = teletext_characters[idx];
                    mode7_charsi[offs2 + d] = teletext_characters[idxi];
                    if (c >= 32 && c < 64) {
                        mode7_graph[offs2 + d] = mode7_sepgraph[offs2 + d] = mode7_chars[offs2 + d];
                        mode7_graphi[offs2 + d] = mode7_sepgraphi[offs2 + d] = mode7_charsi[offs2 + d];
                    }
                    else {
                        int row = 0;
                        // left block has a value of 0x0fc0, right 0x003f and both 0xfff
                        if (y < 3) {
                            if (c & 0x01) row |= 0x0fc0;
                            if (c & 0x02) row |= 0x003f;
                        }
                        else if (y < 6) {
                            if (c & 0x04) row |= 0x0fc0;
                            if (c & 0x08) row |= 0x003f;

                        }
                        else {
                            if (c & 0x10) row |= 0x0fc0;
                            if (c & 0x40) row |= 0x003f;
                        }

                        mode7_graph[offs2 + d] = (row & mask) ? 15 : 0;
                        mode7_graphi[offs2 + d] = (row & mask) ? 15 : 0;
                        row &= 0x03cf;
                        mode7_sepgraph[offs2 + d] = (row & mask) ? 15 : 0;
                        mode7_sepgraphi[offs2 + d] = (row & mask) ? 15 : 0;
                    }
                }
                else {
                    mode7_chars[offs2 + d] = 0;
                    mode7_charsi[offs2 + d] = 0;
                    mode7_graph[offs2 + d] = 0;
                    mode7_graphi[offs2 + d] = 0;
                    mode7_sepgraph[offs2 + d] = 0;
                    mode7_sepgraphi[offs2 + d] = 0;
                }
                mask >>= 1;
            }
            offs2 += MODE7_DISPLAY_WIDTH;
        }
    }
}

static void mode7_gen_nula_lookup(void)
{
    int fg_ix, fg_pix, fg_red, fg_grn, fg_blu;
    int bg_ix, bg_pix, bg_red, bg_grn, bg_blu;
    int weight, lu_red, lu_grn, lu_blu;

    for (fg_ix = 0; fg_ix < 8; fg_ix++) {
        fg_pix = nula_collook[fg_ix];
        fg_red = (fg_pix >> 16) & 0xff;
        fg_grn = (fg_pix >> 8) & 0xff;
        fg_blu = fg_pix & 0xff;
        for (bg_ix = 0; bg_ix < 8; bg_ix++) {
            bg_pix = nula_collook[bg_ix];
            bg_red = (bg_pix >> 16) & 0xff;
            bg_grn = (bg_pix >> 8) & 0xff;
            bg_blu = bg_pix & 0xff;
            for (weight = 0; weight < 16; weight++) {
                lu_red = bg_red + (((fg_red - bg_red) * weight) / 15);
                lu_grn = bg_grn + (((fg_grn - bg_grn) * weight) / 15);
                lu_blu = bg_blu + (((fg_blu - bg_blu) * weight) / 15);
                mode7_lookup[fg_ix][bg_ix][weight] = makecol(lu_red, lu_grn, lu_blu);
            }
        }
    }
    mode7_need_new_lookup = 0;
}

static inline void mode7_render(ALLEGRO_LOCKED_REGION *region, uint8_t dat)
{
    int t, c;
    int off;
    int mcolx = mode7_col;
    int holdoff = 0, holdclear = 0;
    uint8_t *mode7_px[2];
    int mode7_flashx = mode7_flash, mode7_dblx = mode7_dbl;
    int *on;

    if (scrx < (1280-32)) {
        if (mode7_need_new_lookup)
            mode7_gen_nula_lookup();

        t = mode7_buf[0];
        mode7_buf[0] = mode7_buf[1];
        mode7_buf[1] = dat;
        dat = t;
        mode7_px[0] = mode7_p[0];
        mode7_px[1] = mode7_p[1];

        if (!mode7_dbl && mode7_nextdbl)
            on = mode7_lookup[mode7_bg & 7][mode7_bg & 7];
        if (dat == 255) {
            for (c = 0; c < MODE7_DISPLAY_WIDTH; c++)
                put_pixel(region, scrx + c + 16, scry, colblack);
            return;
        }

        if (dat < 0x20) {
            switch (dat) {
            case 1:
            case 2:
            case 3:
            case 4:
            case 5:
            case 6:
            case 7:
                mode7_gfx = 0;
                mode7_col = dat;
                mode7_p[0] = mode7_chars;
                mode7_p[1] = mode7_charsi;
                holdclear = 1;
                break;
            case 8:
                mode7_flash = 1;
                break;
            case 9:
                mode7_flash = 0;
                break;
            case 12:
            case 13:
                mode7_dbl = dat & 1;
                if (mode7_dbl)
                    mode7_wasdbl = 1;
                break;
            case 17:
            case 18:
            case 19:
            case 20:
            case 21:
            case 22:
            case 23:
                mode7_gfx = 1;
                mode7_col = dat & 7;
                if (mode7_sep) {
                    mode7_p[0] = mode7_sepgraph;
                    mode7_p[1] = mode7_sepgraphi;
                } else {
                    mode7_p[0] = mode7_graph;
                    mode7_p[1] = mode7_graphi;
                }
                break;
            case 24:
                mode7_col = mcolx = mode7_bg;
                break;
            case 25:
                if (mode7_gfx) {
                    mode7_p[0] = mode7_graph;
                    mode7_p[1] = mode7_graphi;
                }
                mode7_sep = 0;
                break;
            case 26:
                if (mode7_gfx) {
                    mode7_p[0] = mode7_sepgraph;
                    mode7_p[1] = mode7_sepgraphi;
                }
                mode7_sep = 1;
                break;
            case 28:
                mode7_bg = 0;
                break;
            case 29:
                mode7_bg = mode7_col;
                break;
            case 30:
                mode7_holdchar = 1;
                break;
            case 31:
                holdoff = 1;
                break;
            }
            if (mode7_holdchar) {
                dat = mode7_heldchar;
                if (dat >= 0x40 && dat < 0x60)
                    dat = 32;
                mode7_px[0] = mode7_heldp[0];
                mode7_px[1] = mode7_heldp[1];
            } else
                dat = 0x20;
            if (mode7_dblx != mode7_dbl)
                dat = 32;           /*Double height doesn't respect held characters */
        } else if (mode7_p[0] != mode7_chars) {
            mode7_heldchar = dat;
            mode7_heldp[0] = mode7_px[0];
            mode7_heldp[1] = mode7_px[1];
        }

        if (mode7_dblx && !mode7_nextdbl)
            t = ((dat - MODE7_FIRST_CHAR) * MODE7_DISPLAY_WIDTH * MODE7_CHAR_HEIGHT) + ((sc >> 1) * MODE7_DISPLAY_WIDTH);
        else if (mode7_dblx)
            t = ((dat - MODE7_FIRST_CHAR) * MODE7_DISPLAY_WIDTH * MODE7_CHAR_HEIGHT) + ((sc >> 1) * MODE7_DISPLAY_WIDTH) + (5 * MODE7_DISPLAY_WIDTH);
        else
            t = ((dat - MODE7_FIRST_CHAR) * MODE7_DISPLAY_WIDTH * MODE7_CHAR_HEIGHT) + (sc * MODE7_DISPLAY_WIDTH);

        off = mode7_lookup[0][mode7_bg & 7][0];
        if (!mode7_dbl && mode7_nextdbl)
            on = mode7_lookup[mode7_bg & 7][mode7_bg & 7];
        else
            on = mode7_lookup[mcolx & 7][mode7_bg & 7];

        int interindex = (vid_dtype_intern == VDT_INTERLACE) && interlline;
        for (c = 0; c < MODE7_DISPLAY_WIDTH; c++) {
            if (mode7_flashx && !mode7_flashon)
                put_pixel(region, scrx + c + 16, scry, off);
            else if (mode7_dblx)
                put_pixel(region, scrx + c + 16, scry, on[mode7_px[sc & 1][t] & 15]);
            else
                put_pixel(region, scrx + c + 16, scry, on[mode7_px[interindex][t] & 15]);
            t++;
        }

        if ((scrx + 16) < firstx)
            firstx = scrx + 16;
        if ((scrx + 32) > lastx)
            lastx = scrx + 32;

        if (holdoff) {
            mode7_holdchar = 0;
            mode7_heldchar = 32;
        }
        if (holdclear)
            mode7_heldchar = 32;
    }
}

uint16_t vidbank;
static const int screenlen[4] = { 0x4000, 0x5000, 0x2000, 0x2800 };

static int vsynctime;
static int interline;
static int hvblcount;
static int frameodd;
static int con, cdraw, coff;
static int cursoron;
static int frcount;
static int charsleft;

static int vidclocks = 0;
static int oddclock = 0;
static int vidbytes = 0;

static int oldr8;

int firstx, firsty, lastx, lasty;

static ALLEGRO_DISPLAY *display;
ALLEGRO_BITMAP *b, *b16, *b32;

ALLEGRO_LOCKED_REGION *region;

ALLEGRO_COLOR border_col;

ALLEGRO_DISPLAY *video_init(void)
{
    int c;
    int temp, temp2, left;

#ifdef ALLEGRO_GTK_TOPLEVEL
    al_set_new_display_flags(ALLEGRO_WINDOWED | ALLEGRO_GTK_TOPLEVEL | ALLEGRO_RESIZABLE);
#else
    al_set_new_display_flags(ALLEGRO_WINDOWED | ALLEGRO_RESIZABLE);
#endif
    al_set_new_display_option(ALLEGRO_VSYNC, 2, ALLEGRO_REQUIRE);
    log_debug("video: vsync=%d", al_get_new_display_option(ALLEGRO_VSYNC, &temp));

    video_set_window_size(true);

    al_set_new_display_option(ALLEGRO_VSYNC, 2, ALLEGRO_SUGGEST);
    if ((display = al_create_display(winsizex, winsizey)) == NULL) {
        log_fatal("video: unable to create display");
        exit(1);
    }

    al_set_new_bitmap_flags(ALLEGRO_VIDEO_BITMAP);
    b16 = al_create_bitmap(832, 614);
    b32 = al_create_bitmap(1536, 800);

    colblack = 0xff000000;
    colwhite = 0xffffffff;
    border_col = al_map_rgb(0, 0, 0);

    nula_default_palette();

    for (c = 0; c < 8; c++)
        nula_flash[c] = 1;
    for (temp = 0; temp < 256; temp++) {
        temp2 = temp;
        for (c = 0; c < 16; c++) {
            left = 0;
            if (temp2 & 2)
                left |= 1;
            if (temp2 & 8)
                left |= 2;
            if (temp2 & 32)
                left |= 4;
            if (temp2 & 128)
                left |= 8;
            table4bpp[3][temp][c] = left;
            temp2 <<= 1;
            temp2 |= 1;
        }
        for (c = 0; c < 16; c++) {
            table4bpp[2][temp][c] = table4bpp[3][temp][c >> 1];
            table4bpp[1][temp][c] = table4bpp[3][temp][c >> 2];
            table4bpp[0][temp][c] = table4bpp[3][temp][c >> 3];
        }
    }
    b = al_create_bitmap(1280, 800);
    al_set_target_bitmap(b);
    al_clear_to_color(al_map_rgb(0, 0,0));
    region = al_lock_bitmap(b, ALLEGRO_PIXEL_FORMAT_ARGB_8888, ALLEGRO_LOCK_READWRITE);
    return display;
}

void video_set_disptype(enum vid_disptype dtype)
{
    vid_dtype_user = dtype;
    set_intern_dtype(dtype);
}

static const uint8_t cursorlook[7] = { 0, 0, 0, 0x80, 0x40, 0x20, 0x20 };
static const int cdrawlook[4] = { 3, 2, 1, 0 };

static const int cmask[4] = { 0, 0, 16, 32 };

static int lasthc0 = 0, lasthc;
static int ccount = 0;

static int vid_cleared;

static int firstdispen = 0;

void video_reset()
{
    interline = 0;
    vsynctime = 0;
    hvblcount = 0;
    frameodd = 0;
    con = cdraw = 0;
    cursoron = 0;
    charsleft = 0;
    vidbank = 0;

    nula_left_cut = 0;
    nula_left_edge = 0;
    nula_left_blank = 0;
    nula_horizontal_offset = 0;

}

#if 0
static inline int is_free_run(void) {
    ALLEGRO_KEYBOARD_STATE keystate;

    al_get_keyboard_state(&keystate);
    return al_key_down(&keystate, ALLEGRO_KEY_PGUP);
}
#endif
static inline int is_free_run(void) {
    return 0;
}

void video_poll(int clocks, int timer_enable)
{
    int c, oldvc;
    uint16_t addr;
    uint8_t dat;

    while (clocks--) {
        scrx += 8;
        vidclocks++;
        oddclock = !oddclock;
        if (!(ula_ctrl & 0x10) && !oddclock)
            continue;

        if (hc == crtc[1]) { // reached horizontal displayed count.
            if (dispen && ula_ctrl & 2)
                charsleft = 3;
            else
                charsleft = 0;
            dispen = 0;
        }
        if (hc == crtc[2]) { // reached horizontal sync position.
            if (ula_ctrl & 0x10)
                scrx = 128 - ((crtc[3] & 15) * 4);
            else
                scrx = 128 - ((crtc[3] & 15) * 8);
            scry++;
            if (scry >= 384) {
                scry = 0;
                video_doblit(crtc_mode, crtc[4]);
            }
        }

        switch(vid_dtype_intern) {
            case VDT_INTERLACE:
                scry = (scry << 1) + interlline;
                break;
            case VDT_LINEDOUBLE:
                scry <<= 1;
            default:
                break;
        }
        if (dispen) {
            if (!((ma ^ (crtc[15] | (crtc[14] << 8))) & 0x3FFF) && con)
                cdraw = cdrawlook[crtc[8] >> 6];

            if (ma & 0x2000)
                dat = ram[ttxbank | (ma & 0x3FF) | vidbank];
            else {
                if ((crtc[8] & 3) == 3)
                    addr = (ma << 3) | ((sc & 3) << 1) | interlline;
                else
                    addr = (ma << 3) | (sc & 7);
                if (addr & 0x8000)
                    addr -= screenlen[scrsize];
                dat = ram[(addr & 0x7FFF) | vidbank];
            }

            if (scrx < (1280-16)) {
                if ((crtc[8] & 0x30) == 0x30 || ((sc & 8) && !(ula_ctrl & 2))) {
                    // Gaps between lines in modes 3 & 6.
                    put_pixels(region, scrx, scry, (ula_ctrl & 0x10) ? 8 : 16, colblack);
                } else
                    switch (crtc_mode) {
                    case 0:
                        mode7_render(region, dat & 0x7F);
                        break;
                    case 1:
                        {
                            if (scrx < firstx)
                                firstx = scrx;
                            if ((scrx + 8) > lastx)
                                lastx = scrx + 8;
                            if (nula_attribute_mode && ula_mode > 1) {
                                if (ula_mode == 3) {
                                    // 1bpp
                                    if (nula_attribute_text) {
                                        int attribute = ((dat & 7) << 1);
                                        float pc = 0.0f;
                                        for (c = 0; c < 7; c++, pc += 0.75f) {
                                            int output = ula_pal[attribute | (dat >> (7 - (int) pc) & 1)];
                                            nula_putpixel(region, scrx + c, scry, output);
                                        }
                                        // Very loose approximation of the text attribute mode
                                        nula_putpixel(region, scrx + 7, scry, ula_pal[attribute]);
                                    } else {
                                        int attribute = ((dat & 3) << 2);
                                        float pc = 0.0f;
                                        for (c = 0; c < 8; c++, pc += 0.75f) {
                                            int output = ula_pal[attribute | (dat >> (7 - (int) pc) & 1)];
                                            nula_putpixel(region, scrx + c, scry, output);
                                        }
                                    }
                                } else {
                                    int attribute = (((dat & 16) >> 1) | ((dat & 1) << 2));
                                    float pc = 0.0f;
                                    for (c = 0; c < 8; c++, pc += 0.75f) {
                                        int a = 3 - ((int) pc) / 2;
                                        int output = ula_pal[attribute | ((dat >> (a + 3)) & 2) | ((dat >> a) & 1)];
                                        nula_putpixel(region, scrx + c, scry, output);
                                    }
                                }
                            } else {
                                for (c = 0; c < 8; c++) {
                                    nula_putpixel(region, scrx + c, scry, nula_palette_mode ? nula_collook[table4bpp[ula_mode][dat][c]] : ula_pal[table4bpp[ula_mode][dat][c]]);
                                }
                            }
                        }
                        break;
                    case 2:
                        {
                            if (scrx < firstx)
                                firstx = scrx;
                            if ((scrx + 16) > lastx)
                                lastx = scrx + 16;
                            if (nula_attribute_mode && ula_mode > 1) {
                                // In low frequency clock can only have 1bpp modes
                                if (nula_attribute_text) {
                                    int attribute = ((dat & 7) << 1);
                                    float pc = 0.0f;
                                    for (c = 0; c < 14; c++, pc += 0.375f) {
                                        int output = ula_pal[attribute | (dat >> (7 - (int) pc) & 1)];
                                        nula_putpixel(region, scrx + c, scry, output);
                                    }

                                    // Very loose approximation of the text attribute mode
                                    nula_putpixel(region, scrx + 14, scry, ula_pal[attribute]);
                                    nula_putpixel(region, scrx + 15, scry, ula_pal[attribute]);
                                } else {
                                    int attribute = ((dat & 3) << 2);
                                    float pc = 0.0f;
                                    for (c = 0; c < 16; c++, pc += 0.375f) {
                                        int output = ula_pal[attribute | (dat >> (7 - (int) pc) & 1)];
                                        nula_putpixel(region, scrx + c, scry, output);
                                    }
                                }
                            } else {
                                for (c = 0; c < 16; c++) {
                                    nula_putpixel(region, scrx + c, scry, nula_palette_mode ? nula_collook[table4bpp[ula_mode][dat][c]] : ula_pal[table4bpp[ula_mode][dat][c]]);
                                }
                            }
                        }
                        break;
                    }
                if (cdraw) {
                    if (cursoron && (ula_ctrl & cursorlook[cdraw])) {
                        for (c = ((ula_ctrl & 0x10) ? 8 : 16); c >= 0; c--) {
                            nula_putpixel(region, scrx + c, scry, get_pixel(region, scrx + c, scry) ^ 0x00ffffff);
                        }
                    }
                    cdraw++;
                    if (cdraw == 7)
                        cdraw = 0;
                }
            }
            ma++;
            vidbytes++;
        } else {
            if (charsleft) {
                if (charsleft != 1)
                    mode7_render(region, 255);
                charsleft--;

            } else if (scrx < (1280-32)) {
                put_pixels(region, scrx, scry, (ula_ctrl & 0x10) ? 8 : 16, colblack);
                if (!crtc_mode)
                    put_pixels(region, scrx + 16, scry, 16, colblack);
            }
            if (cdraw && scrx < (1280-16)) {
                if (cursoron && (ula_ctrl & cursorlook[cdraw])) {
                    for (c = ((ula_ctrl & 0x10) ? 8 : 16); c >= 0; c--) {
                        nula_putpixel(region, scrx + c, scry, get_pixel(region, scrx + c, scry) ^ colwhite);
                    }
                }
                cdraw++;
                if (cdraw == 7)
                    cdraw = 0;
            }
        }

        switch(vid_dtype_intern) {
            case VDT_INTERLACE:
            case VDT_LINEDOUBLE:
                scry >>= 1;
            default:
                break;
        }
        if (hvblcount) {
            hvblcount--;
            if (!hvblcount && timer_enable)
                sysvia_set_ca1(0);
        }

        if (interline && hc == (crtc[0] >> 1)) {
            hc = interline = 0;
            lasthc0 = 1;

            if (ula_ctrl & 0x10)
                scrx = 128 - ((crtc[3] & 15) * 4);
            else
                scrx = 128 - ((crtc[3] & 15) * 8);
        } else if (hc == crtc[0]) {
            mode7_col = 7;
            mode7_bg = 0;
            mode7_holdchar = 0;
            mode7_heldchar = 0x20;
            mode7_p[0] = mode7_chars;
            mode7_p[1] = mode7_charsi;
            mode7_flash = 0;
            mode7_sep = 0;
            mode7_gfx = 0;
            mode7_heldp[0] = mode7_p[0];
            mode7_heldp[1] = mode7_p[1];

            hc = 0;

            if (crtc_mode) {
                // NULA left edge
                nula_left_edge = scrx + crtc_mode * 8;

                // NULA left cut
                nula_left_cut = nula_left_edge + nula_left_blank * crtc_mode * 8;

                // NULA horizontal offset - "delay" the pixel clock
                for (c = 0; c < nula_horizontal_offset * crtc_mode; c++, scrx++) {
                    put_pixel(region, scrx + crtc_mode * 8, scry, colblack);
                }
            }

            if (sc == (crtc[11] & 31) || ((crtc[8] & 3) == 3 && sc == ((crtc[11] & 31) >> 1))) {
                con = 0;
                coff = 1;
            }
            if (vadj) {
                sc++;
                sc &= 31;
                ma = maback;
                vadj--;
                if (!vadj) {
                    vdispen = 1;
                    ma = maback = (crtc[13] | (crtc[12] << 8)) & 0x3FFF;
                    sc = 0;
                }
            } else if (sc == crtc[9] || ((crtc[8] & 3) == 3 && sc == (crtc[9] >> 1))) {
                // Reached the bottom of a row of characters.
                maback = ma;
                sc = 0;
                con = 0;
                coff = 0;
                if (mode7_nextdbl)
                    mode7_nextdbl = 0;
                else
                    mode7_nextdbl = mode7_wasdbl;
                oldvc = vc;
                vc++;
                vc &= 127;
                if (vc == crtc[6]) // vertical displayed total.
                    vdispen = 0;
                if (oldvc == crtc[4]) {
                    // vertical total reached.
                    vc = 0;
                    vadj = crtc[5];
                    if (!vadj) {
                        vdispen = 1;
                        ma = maback = (crtc[13] | (crtc[12] << 8)) & 0x3FFF;
                    }
                    frcount++;
                    if (!(crtc[10] & 0x60))
                        cursoron = 1;
                    else
                        cursoron = frcount & cmask[(crtc[10] & 0x60) >> 5];
                }
                if (vc == crtc[7]) {
                    // Reached vertical sync position.
                    int intsync = crtc[8] & 1;
                    if (!intsync && oldr8) {
                        ALLEGRO_COLOR black = al_map_rgb(0, 0, 0);
                        al_set_target_bitmap(b32);
                        al_clear_to_color(black);
                        al_unlock_bitmap(b);
                        al_set_target_bitmap(b);
                        al_clear_to_color(black);
                        region = al_lock_bitmap(b, ALLEGRO_PIXEL_FORMAT_ARGB_8888, ALLEGRO_LOCK_READWRITE);
                    }
                    frameodd ^= 1;
                    if (frameodd)
                        interline = intsync;
                    interlline = frameodd && intsync;
                    oldr8 = intsync;
                    if (vidclocks > 1024 && !ccount) {
                        video_doblit(crtc_mode, crtc[4]);
                        vid_cleared = 0;
                    } else if (vidclocks <= 1024 && !vid_cleared) {
                        vid_cleared = 1;
                        al_unlock_bitmap(b);
                        al_set_target_bitmap(b);
                        al_clear_to_color(al_map_rgb(0, 0, 0));
                        region = al_lock_bitmap(b, ALLEGRO_PIXEL_FORMAT_ARGB_8888, ALLEGRO_LOCK_READWRITE);
                        video_doblit(crtc_mode, crtc[4]);
                    }
                    ccount++;
                    if (ccount == 10 || ((!motor || !fasttape) && !is_free_run()))
                        ccount = 0;
                    scry = 0;
                    if (timer_enable)
                        sysvia_set_ca1(1);

                    vsynctime = (crtc[3] >> 4) + 1;
                    if (!(crtc[3] >> 4))
                        vsynctime = 17;

                    mode7_flashtime++;
                    if ((mode7_flashon && mode7_flashtime == 32) || (!mode7_flashon && mode7_flashtime == 16)) {
                        mode7_flashon = !mode7_flashon;
                        mode7_flashtime = 0;
                    }

                    vidclocks = vidbytes = 0;
                }
            } else {
                sc++;
                sc &= 31;
                ma = maback;
            }

            mode7_dbl = mode7_wasdbl = 0;
            if ((sc == (crtc[10] & 31) || ((crtc[8] & 3) == 3 && sc == ((crtc[10] & 31) >> 1))) && !coff)
                con = 1;

            if (vsynctime) {
                vsynctime--;
                if (!vsynctime) {
                    hvblcount = 1;
                    if (frameodd)
                        interline = (crtc[8] & 1);
                }
            }

            dispen = vdispen;
            if (dispen || vadj) {
                if (scry < firsty)
                    firsty = scry;
                if ((scry + 1) > lasty)
                    lasty = scry;
            }

            firstdispen = 1;
            lasthc0 = 1;
        } else {
            hc++;
            hc &= 255;
        }
        lasthc = hc;
    }
}

void video_savestate(FILE * f)
{
    unsigned char bytes[9];
    bytes[0] = scrx;
    bytes[1] = scrx >> 8;
    bytes[2] = scry;
    bytes[3] = scry >> 8;
    bytes[4] = oddclock;
    bytes[5] = vidclocks;
    bytes[6] = vidclocks >> 8;
    bytes[7] = vidclocks >> 16;
    bytes[8] = vidclocks >> 24;
    fwrite(bytes, sizeof(bytes), 1, f);
}

void video_loadstate(FILE * f)
{
    unsigned char bytes[9];
    fread(bytes, sizeof(bytes), 1, f);
    scrx = bytes[0] | (bytes[1] << 8);
    scry = bytes[2] | (bytes[3] << 8);
    oddclock = bytes[4];
    vidclocks = bytes[5] | (bytes[6] << 8) | (bytes[7] << 16) | (bytes[8] << 24);
}
