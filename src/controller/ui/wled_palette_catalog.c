#include "wled_palette_catalog.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/*
 * Curated WLED palette catalog. IDs match WLED's built-in palette indices so a
 * dropdown selection maps directly to {"seg":[{"pal":<id>}]} JSON.
 *
 * preview[] holds four representative 0xRRGGBB colors used to draw a small
 * swatch strip next to the picker (the panel has multi-stop gradients compiled
 * out, so previews are built from solid blocks, not a gradient fill).
 */
static const wled_palette_catalog_item_t s_items[] = {
    {0,  "Default",       {0xFF0000, 0x00FF00, 0x0000FF, 0xFF00FF}},
    {6,  "Party",         {0xFF2000, 0xFF00FF, 0x8000FF, 0x0040FF}},
    {7,  "Cloud",         {0x0000FF, 0x4169E1, 0x87CEEB, 0xFFFFFF}},
    {8,  "Lava",          {0x000000, 0x9B0000, 0xFF3300, 0xFFCC00}},
    {9,  "Ocean",         {0x000080, 0x0066AA, 0x00CCCC, 0xCCFFFF}},
    {10, "Forest",        {0x004400, 0x228B22, 0x66BB22, 0xCCEE88}},
    {11, "Rainbow",       {0xFF0000, 0x00FF00, 0x0000FF, 0xFF0000}},
    {12, "Rainbow Bands", {0xFF0000, 0xFFFF00, 0x00FF00, 0x0000FF}},
    {13, "Sunset",        {0x4A0E4E, 0xC04000, 0xFF6600, 0xFFCC33}},
    {14, "Rivendell",     {0x607848, 0x8A9A5B, 0xB5C18E, 0xE0E0C0}},
    {15, "Breeze",        {0x0A3A5A, 0x1E7A8C, 0x4FB0A0, 0xA0E0C0}},
    {16, "Red & Blue",    {0xFF0000, 0x550055, 0x0000FF, 0xFF0000}},
    {17, "Yellowout",     {0xFFFF00, 0xFFAA00, 0x885500, 0x000000}},
    {18, "Analogous",     {0xFF0000, 0xFF6600, 0xCC0066, 0x660099}},
    {19, "Splash",        {0x00FFAA, 0x00AAFF, 0xAA00FF, 0xFF00AA}},
    {20, "Pastel",        {0xFFB3BA, 0xFFDFBA, 0xBAFFC9, 0xBAE1FF}},
    {21, "Sunset 2",      {0x661100, 0xCC3300, 0xFF8800, 0xFFDD66}},
    {22, "Beech",         {0x1D4E89, 0x4A90C2, 0xE8D8A8, 0x88B04B}},
    {23, "Vintage",       {0x4A3520, 0x8A6D3B, 0xC2A878, 0xE8D8B0}},
    {24, "Departure",     {0x2B1700, 0x6B4500, 0x4A8A2B, 0xA0D060}},
    {25, "Landscape",     {0x000050, 0x008040, 0xA0C020, 0xFFFFE0}},
    {26, "Beach",         {0x00A0C0, 0x40D0E0, 0xE0D090, 0xC0A060}},
    {27, "Sherbet",       {0xFF66CC, 0xFF99AA, 0xFFCC99, 0x99FFCC}},
    {28, "Hult",          {0xFCF4DD, 0x00A0A0, 0xCC00CC, 0x202020}},
    {35, "Fire",          {0x000000, 0x880000, 0xFF4400, 0xFFDD00}},
    {36, "Icefire",       {0x000000, 0x0044AA, 0xFFAA00, 0xFFFFCC}},
    {37, "Cyane",         {0x00FFFF, 0x00AAFF, 0x5566FF, 0xAA00FF}},
    {39, "Autumn",        {0x8B2500, 0xC65D00, 0xDAA520, 0x556B2F}},
    {40, "Magenta",       {0xFF00FF, 0xCC00AA, 0x880066, 0x440033}},
    {46, "April Night",   {0x011A3A, 0x0A4A6A, 0xC09000, 0x00A040}},
    {48, "C9",            {0xB80400, 0x0B6700, 0xC41000, 0x0050A0}},
    {50, "Aurora",        {0x00FF66, 0x00AAFF, 0x6600FF, 0x004422}},
    {51, "Atlantica",     {0x00264A, 0x0A5A6A, 0x2A9A8A, 0x80C090}},
    {54, "Temperature",   {0x1A0DAB, 0x00A0A0, 0xF0D000, 0xD00000}},
    {57, "Candy",         {0xEE4488, 0xFFAA55, 0x66CCFF, 0xFFFFFF}},
};

static char s_options[768];
static bool s_options_built;

static void build_options(void)
{
    size_t off = 0;
    size_t count = sizeof(s_items) / sizeof(s_items[0]);
    for (size_t i = 0; i < count && off < sizeof(s_options); i++) {
        int n = snprintf(s_options + off, sizeof(s_options) - off, "%s%s",
                         i ? "\n" : "", s_items[i].name);
        if (n < 0) break;
        off += (size_t)n;
    }
    s_options[sizeof(s_options) - 1] = '\0';
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
    return s_options;
}
