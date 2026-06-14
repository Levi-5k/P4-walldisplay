#include "wled_palette_catalog.h"

#include "esp_heap_caps.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const wled_palette_catalog_item_t s_items[] = {
    {0, "Default", {0x9B00D5, 0xDA9B00, 0xEA006D, 0x0032FC}},
    {1, "* Random Cycle", {0xFF2D55, 0xFF2D55, 0xFF2D55, 0xFF2D55}},
    {2, "* Color 1", {0xFF4400, 0xFF4400, 0xFF4400, 0xFF4400}},
    {3, "* Colors 1&2", {0xFF4400, 0xFF4400, 0x006DFF, 0x006DFF}},
    {4, "* Color Gradient", {0xF8F7FF, 0x006DFF, 0xFF4400, 0xFF4400}},
    {5, "* Colors Only", {0xFF4400, 0x006DFF, 0xF8F7FF, 0xFF4400}},
    {6, "Party", {0x9B00D5, 0xDA9B00, 0xEA006D, 0x0032FC}},
    {7, "Cloud", {0x0000FF, 0x00008B, 0x87CEEB, 0x87CEEB}},
    {8, "Lava", {0x000000, 0x880000, 0xFF6700, 0x8B0000}},
    {9, "Ocean", {0x191970, 0x0E2BA8, 0x3F91C8, 0x87CEFA}},
    {10, "Forest", {0x006400, 0x398C22, 0x73CD32, 0x228B22}},
    {11, "Rainbow", {0xFF0000, 0x6BF100, 0x4600F3, 0xEB0072}},
    {12, "Rainbow Bands", {0xFF0000, 0x005000, 0x000060, 0x000000}},
    {13, "Sunset", {0xB50000, 0xD3554D, 0x7300B4, 0x0000CF}},
    {14, "Rivendell", {0x18452C, 0x416342, 0x869065, 0xC8CCA6}},
    {15, "Breeze", {0x103033, 0x1BA1A9, 0xA4DAEE, 0x009198}},
    {16, "Red & Blue", {0x290E63, 0xA22044, 0x843665, 0x54306C}},
    {17, "Yellowout", {0xDEBF08, 0xBB9106, 0x986203, 0x753401}},
    {18, "Analogous", {0x2600FF, 0x6800FF, 0xB100A2, 0xFF0000}},
    {19, "Splash", {0xBA3FFF, 0xD51B8D, 0xE9B9C8, 0xCD26B0}},
    {20, "Pastel", {0x3D87B8, 0xC8EF9C, 0xFEE280, 0xFFFC71}},
    {21, "Sunset 2", {0xAF793E, 0xF3C44D, 0x798C82, 0x001A7D}},
    {22, "Beach", {0xFFFEEC, 0x30E9E9, 0x26AFCF, 0x0081BE}},
    {23, "Vintage", {0x291218, 0xC5B135, 0x420416, 0x291218}},
    {24, "Departure", {0x352200, 0xD5A86F, 0x00FF00, 0x008000}},
    {25, "Landscape", {0x000000, 0x56BC24, 0xAFCEEE, 0x1C6BE1}},
    {26, "Beech", {0x0C2D00, 0x63B484, 0x346E46, 0x052707}},
    {27, "Sherbet", {0xFF6629, 0xFF355A, 0xFFFFF9, 0x9DFF89}},
    {28, "Hult", {0xFBD8FC, 0xF168F2, 0x2E9FD1, 0x18B8AE}},
    {29, "Hult 64", {0x18B8AE, 0x42964F, 0x4E903D, 0x008075}},
    {30, "Drywet", {0x776121, 0xA6EE7F, 0x0778EC, 0x043365}},
    {31, "Jul", {0xE2060C, 0x2D5748, 0x918444, 0xB10309}},
    {32, "Grintage", {0x1D0803, 0x753D12, 0xC3B53A, 0x75812A}},
    {33, "Rewhi", {0xB1A0C7, 0xE29C70, 0xB1637A, 0x84659F}},
    {34, "Tertiary", {0x0019FF, 0x37B44D, 0x8CB20D, 0xFF1929}},
    {35, "Fire", {0x000000, 0x9B0000, 0xFC9427, 0xFFFFFF}},
    {36, "Icefire", {0x000000, 0x0049B1, 0x47BCFF, 0xFFFFFF}},
    {37, "Cyane", {0x3D9B2C, 0x95AD7A, 0xA5D9CB, 0x54B6D7}},
    {38, "Light Pink", {0x4F206D, 0x9BA5CF, 0xB789D2, 0xC66FB8}},
    {39, "Autumn", {0x5A0E05, 0xB54D16, 0xA93E10, 0x4A0502}},
    {40, "Magenta", {0x000000, 0x0300FF, 0xFF00FF, 0xFFFFFF}},
    {41, "Magred", {0x000000, 0xA200A4, 0xFF00A2, 0xFF0000}},
    {42, "Yelmag", {0x000000, 0xFF0003, 0xFF00FF, 0xFFFF00}},
    {43, "Yelblu", {0x0000FF, 0x00ACFF, 0x4CFFA2, 0xFFFF00}},
    {44, "Orange & Teal", {0x00965C, 0x358649, 0xCA5813, 0xFF4800}},
    {45, "Tiamat", {0x01020E, 0x0B6A4F, 0x7488B5, 0xFFF9FF}},
    {46, "April Night", {0x01052D, 0x134927, 0x803117, 0x01052D}},
    {47, "Orangery", {0xFF5F17, 0x9D2703, 0xBE2007, 0xD52504}},
    {48, "C9", {0xB80400, 0x902C02, 0x046002, 0x070758}},
    {49, "Sakura", {0xC4130A, 0xF53E35, 0xF3445B, 0xDF0D11}},
    {50, "Aurora", {0x01052D, 0x00DA0F, 0x00F32D, 0x01052D}},
    {51, "Atlantica", {0x001C70, 0x0AC76C, 0x118557, 0x28AA50}},
    {52, "C9 2", {0x067E02, 0x041E72, 0xC43902, 0x895502}},
    {53, "C9 New", {0xFF0500, 0xC43902, 0x067E02, 0x041E72}},
    {54, "Temperature", {0x145CAB, 0x5ACDC6, 0xFBC704, 0x972623}},
    {55, "Aurora 2", {0x11B10D, 0x5ADB2B, 0xAD6E7D, 0xAB65DD}},
    {56, "Retro Clown", {0xF2A826, 0xE66745, 0xC94588, 0xA136E1}},
    {57, "Candy", {0xF3F217, 0xAA5764, 0x5D1697, 0x000075}},
    {58, "Toxy Reaf", {0x02EF7E, 0x32AB9C, 0x6167BB, 0x9123D9}},
    {59, "Fairy Reaf", {0xDC13BB, 0x6E80CC, 0x2CE4DC, 0xFFFFFF}},
    {60, "Semi Blue", {0x000000, 0x2935A9, 0x3F297D, 0x000000}},
    {61, "Pink Candy", {0xFFFFFF, 0xA523D6, 0xC40FB3, 0xFFFFFF}},
    {62, "Red Reaf", {0x244472, 0x80ACE0, 0xE82A35, 0x5E0E09}},
    {63, "Aqua Flash", {0x000000, 0xD1FA7B, 0xC2F992, 0x000000}},
    {64, "Yelblu Hot", {0x2B1E39, 0x4F0064, 0xCF5618, 0xF6F71B}},
    {65, "Lite Light", {0x000000, 0x361C3A, 0x220924, 0x000000}},
    {66, "Red Flash", {0x000000, 0xD00A07, 0xCE0A07, 0x000000}},
    {67, "Blink Red", {0x040704, 0x651634, 0xAF48D3, 0x4D1D4E}},
    {68, "Red Shift", {0x62165D, 0xA9273C, 0xE5611E, 0x020002}},
    {69, "Red Tide", {0xFB2E00, 0xF15D0A, 0xB64D16, 0x7E0804}},
    {70, "Candy2", {0x6D6666, 0xDE8428, 0xFBBC29, 0x14130D}},
    {71, "Traffic Light", {0x000000, 0x00FF00, 0xFFFF00, 0xFF0000}},
};

#define WLED_PALETTE_OPTIONS_MAX 1024
static char *s_options;
static bool s_options_built;
static const char s_options_fallback[] = "Default";

static void build_options(void)
{
    if (!s_options) {
        s_options = heap_caps_malloc(WLED_PALETTE_OPTIONS_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_options) return;
    }
    size_t off = 0;
    size_t count = sizeof(s_items) / sizeof(s_items[0]);
    for (size_t i = 0; i < count && off < WLED_PALETTE_OPTIONS_MAX; i++) {
        int n = snprintf(s_options + off, WLED_PALETTE_OPTIONS_MAX - off, "%s%s",
                         i ? "\n" : "", s_items[i].name);
        if (n < 0) break;
        off += (size_t)n;
    }
    s_options[WLED_PALETTE_OPTIONS_MAX - 1] = '\0';
    s_options_built = true;
}

size_t wled_palette_catalog_count(void)
{
    return sizeof(s_items) / sizeof(s_items[0]);
}

const wled_palette_catalog_item_t *wled_palette_catalog_item(size_t index)
{
    return index < wled_palette_catalog_count() ? &s_items[index] : NULL;
}

const wled_palette_catalog_item_t *wled_palette_catalog_find(uint8_t id)
{
    for (size_t i = 0; i < wled_palette_catalog_count(); i++) {
        if (s_items[i].id == id) return &s_items[i];
    }
    return NULL;
}

int wled_palette_catalog_index_for_id(uint8_t id)
{
    for (size_t i = 0; i < wled_palette_catalog_count(); i++) {
        if (s_items[i].id == id) return (int)i;
    }
    return -1;
}

uint8_t wled_palette_catalog_id_for_index(uint16_t index)
{
    return index < wled_palette_catalog_count() ? s_items[index].id : 0;
}

const char *wled_palette_catalog_options(void)
{
    if (!s_options_built) build_options();
    return s_options_built ? s_options : s_options_fallback;
}
