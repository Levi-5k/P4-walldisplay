#include "wled_effect_catalog.h"

#include "esp_heap_caps.h"

#include <stdbool.h>
#include <stdio.h>

#define WLED_EFFECT_OPTIONS_MAX 8192

static const wled_effect_catalog_item_t s_wled_effects[] = {
    {0, "Solid", {"Speed", "Intensity", "", "", ""}, {"", "", ""}},
    {1, "Blink", {"Speed", "Duty cycle", "", "", ""}, {"", "", ""}},
    {2, "Breathe", {"Speed", "", "", "", ""}, {"", "", ""}},
    {3, "Wipe", {"Speed", "Intensity", "", "", ""}, {"", "", ""}},
    {4, "Wipe Random", {"Speed", "", "", "", ""}, {"", "", ""}},
    {5, "Random Colors", {"Speed", "Fade time", "", "", ""}, {"", "", ""}},
    {6, "Sweep", {"Speed", "Intensity", "", "", ""}, {"", "", ""}},
    {7, "Dynamic", {"Speed", "Intensity", "", "", ""}, {"Smooth", "", ""}},
    {8, "Colorloop", {"Speed", "Saturation", "", "", ""}, {"", "", ""}},
    {9, "Rainbow", {"Speed", "Size", "", "", ""}, {"", "", ""}},
    {10, "Scan", {"Speed", "# of dots", "", "", ""}, {"", "Overlay", ""}},
    {11, "Scan Dual", {"Speed", "# of dots", "", "", ""}, {"", "Overlay", ""}},
    {12, "Fade", {"Speed", "", "", "", ""}, {"", "", ""}},
    {13, "Theater", {"Speed", "Gap size", "", "", ""}, {"", "", ""}},
    {14, "Theater Rainbow", {"Speed", "Gap size", "", "", ""}, {"", "", ""}},
    {15, "Running", {"Speed", "Wave width", "", "", ""}, {"", "", ""}},
    {16, "Saw", {"Speed", "Width", "", "", ""}, {"", "", ""}},
    {17, "Twinkle", {"Speed", "Intensity", "", "", ""}, {"", "", ""}},
    {18, "Dissolve", {"Repeat speed", "Dissolve speed", "", "", ""}, {"Random", "Complete", ""}},
    {19, "Dissolve Rnd", {"Repeat speed", "Dissolve speed", "", "", ""}, {"", "", ""}},
    {20, "Sparkle", {"Speed", "", "", "", ""}, {"", "Overlay", ""}},
    {21, "Sparkle Dark", {"Speed", "Intensity", "", "", ""}, {"", "Overlay", ""}},
    {22, "Sparkle+", {"Speed", "Intensity", "", "", ""}, {"", "Overlay", ""}},
    {23, "Strobe", {"Speed", "", "", "", ""}, {"", "", ""}},
    {24, "Strobe Rainbow", {"Speed", "", "", "", ""}, {"", "", ""}},
    {25, "Strobe Mega", {"Speed", "Intensity", "", "", ""}, {"", "", ""}},
    {26, "Blink Rainbow", {"Frequency", "Blink duration", "", "", ""}, {"", "", ""}},
    {27, "Android", {"Speed", "Width", "", "", ""}, {"", "", ""}},
    {28, "Chase", {"Speed", "Width", "", "", ""}, {"", "", ""}},
    {29, "Chase Random", {"Speed", "Width", "", "", ""}, {"", "", ""}},
    {30, "Chase Rainbow", {"Speed", "Width", "", "", ""}, {"", "", ""}},
    {31, "Chase Flash", {"Speed", "", "", "", ""}, {"", "", ""}},
    {32, "Chase Flash Rnd", {"Speed", "", "", "", ""}, {"", "", ""}},
    {33, "Rainbow Runner", {"Speed", "Size", "", "", ""}, {"", "", ""}},
    {34, "Colorful", {"Speed", "Saturation", "", "", ""}, {"", "", ""}},
    {35, "Traffic Light", {"Speed", "US style", "", "", ""}, {"", "", ""}},
    {36, "Sweep Random", {"Speed", "", "", "", ""}, {"", "", ""}},
    {37, "Chase 2", {"Speed", "Width", "", "", ""}, {"", "", ""}},
    {38, "Aurora", {"Speed", "Intensity", "", "", ""}, {"", "", ""}},
    {39, "Stream", {"Speed", "Zone size", "", "", ""}, {"", "", ""}},
    {40, "Scanner", {"Speed", "Trail", "Delay", "", ""}, {"Dual", "Bi-delay", ""}},
    {41, "Lighthouse", {"Speed", "Fade rate", "", "", ""}, {"", "", ""}},
    {42, "Fireworks", {"", "Frequency", "", "", ""}, {"", "", ""}},
    {43, "Rain", {"Speed", "Spawning rate", "", "", ""}, {"", "", ""}},
    {44, "Tetrix", {"Speed", "Width", "", "", ""}, {"One color", "", ""}},
    {45, "Fire Flicker", {"Speed", "Intensity", "", "", ""}, {"", "", ""}},
    {46, "Gradient", {"Speed", "Spread", "", "", ""}, {"", "", ""}},
    {47, "Loading", {"Speed", "Fade", "", "", ""}, {"", "", ""}},
    {48, "Rolling Balls", {"Speed", "# of balls", "", "", ""}, {"Collide", "Overlay", "Trails"}},
    {49, "Fairy", {"Speed", "# of flashers", "", "", ""}, {"", "", ""}},
    {50, "Two Dots", {"Speed", "Dot size", "", "", ""}, {"", "Overlay", ""}},
    {51, "Fairytwinkle", {"Speed", "Intensity", "", "", ""}, {"", "", ""}},
    {52, "Running Dual", {"Speed", "Wave width", "", "", ""}, {"", "", ""}},
    {53, "Image", {"Speed", "Blur", "", "", ""}, {"", "", ""}},
    {54, "Chase 3", {"Speed", "Size", "", "", ""}, {"", "", ""}},
    {55, "Tri Wipe", {"Speed", "", "", "", ""}, {"", "", ""}},
    {56, "Tri Fade", {"Speed", "", "", "", ""}, {"", "", ""}},
    {57, "Lightning", {"Speed", "Intensity", "", "", ""}, {"", "Overlay", ""}},
    {58, "ICU", {"Speed", "Intensity", "", "", ""}, {"", "Overlay", ""}},
    {59, "Multi Comet", {"Speed", "Fade", "", "", ""}, {"", "", ""}},
    {60, "Scanner Dual", {"Speed", "Trail", "Delay", "", ""}, {"Dual", "Bi-delay", ""}},
    {61, "Stream 2", {"Speed", "", "", "", ""}, {"", "", ""}},
    {62, "Oscillate", {"Speed", "Intensity", "", "", ""}, {"", "", ""}},
    {63, "Pride 2015", {"Speed", "", "", "", ""}, {"", "", ""}},
    {64, "Juggle", {"Speed", "Trail", "", "", ""}, {"", "", ""}},
    {65, "Palette", {"Shift", "Size", "Rotation", "", ""}, {"Animate Shift", "Animate Rotation", "Anamorphic"}},
    {66, "Fire 2012", {"Cooling", "Spark rate", "", "2D Blur", "Boost"}, {"", "", ""}},
    {67, "Colorwaves", {"Speed", "Hue", "", "", ""}, {"", "", ""}},
    {68, "Bpm", {"Speed", "", "", "", ""}, {"", "", ""}},
    {69, "Fill Noise", {"Speed", "", "", "", ""}, {"", "", ""}},
    {70, "Noise 1", {"Speed", "", "", "", ""}, {"", "", ""}},
    {71, "Noise 2", {"Speed", "", "", "", ""}, {"", "", ""}},
    {72, "Noise 3", {"Speed", "", "", "", ""}, {"", "", ""}},
    {73, "Noise 4", {"Speed", "", "", "", ""}, {"", "", ""}},
    {74, "Colortwinkles", {"Fade speed", "Spawn speed", "", "", ""}, {"", "", ""}},
    {75, "Lake", {"Speed", "", "", "", ""}, {"", "", ""}},
    {76, "Meteor", {"Speed", "Trail", "", "", ""}, {"Gradient", "", "Smooth"}},
    {77, "Copy Segment", {"", "Color shift", "Lighten", "Brighten", "ID"}, {"Axis(2D)", "FullStack(last frame)", ""}},
    {78, "Railway", {"Speed", "Smoothness", "", "", ""}, {"", "", ""}},
    {79, "Ripple", {"Speed", "Wave #", "Blur", "", ""}, {"", "Overlay", ""}},
    {80, "Twinklefox", {"Speed", "Twinkle rate", "", "", ""}, {"Cool", "", ""}},
    {81, "Twinklecat", {"Speed", "Twinkle rate", "", "", ""}, {"Cool", "Reverse", ""}},
    {82, "Halloween Eyes", {"Eye off time", "Eye on time", "", "", ""}, {"", "Overlay", ""}},
    {83, "Solid Pattern", {"Fg size", "Bg size", "", "", ""}, {"", "", ""}},
    {84, "Solid Pattern Tri", {"", "Size", "", "", ""}, {"", "", ""}},
    {85, "Spots", {"Spread", "Width", "", "", ""}, {"", "Overlay", ""}},
    {86, "Spots Fade", {"Spread", "Width", "", "", ""}, {"", "Overlay", ""}},
    {87, "Glitter", {"Speed", "Intensity", "", "", ""}, {"", "Overlay", ""}},
    {88, "Candle", {"Speed", "Intensity", "", "", ""}, {"", "", ""}},
    {89, "Fireworks Starburst", {"Chance", "Fragments", "", "", ""}, {"", "Overlay", ""}},
    {90, "Fireworks 1D", {"Gravity", "Firing side", "", "", ""}, {"", "", ""}},
    {91, "Bouncing Balls", {"Gravity", "# of balls", "", "", ""}, {"", "Overlay", ""}},
    {92, "Sinelon", {"Speed", "Trail", "", "", ""}, {"", "", ""}},
    {93, "Sinelon Dual", {"Speed", "Trail", "", "", ""}, {"", "", ""}},
    {94, "Sinelon Rainbow", {"Speed", "Trail", "", "", ""}, {"", "", ""}},
    {95, "Popcorn", {"Speed", "Intensity", "", "", ""}, {"", "Overlay", ""}},
    {96, "Drip", {"Gravity", "# of drips", "", "", ""}, {"", "Overlay", ""}},
    {97, "Plasma", {"Phase", "Intensity", "", "", ""}, {"", "", ""}},
    {98, "Percent", {"Speed", "% of fill", "", "", ""}, {"One color", "", ""}},
    {99, "Ripple Rainbow", {"Speed", "Wave #", "", "", ""}, {"", "", ""}},
    {100, "Heartbeat", {"Speed", "Intensity", "", "", ""}, {"", "", ""}},
    {101, "Pacifica", {"Speed", "Angle", "", "", ""}, {"", "", ""}},
    {102, "Candle Multi", {"Speed", "Intensity", "", "", ""}, {"", "", ""}},
    {103, "Solid Glitter", {"", "Intensity", "", "", ""}, {"", "", ""}},
    {104, "Sunrise", {"Time [min]", "Width", "", "", ""}, {"", "", ""}},
    {105, "Phased", {"Speed", "Intensity", "", "", ""}, {"", "", ""}},
    {106, "Twinkleup", {"Speed", "Intensity", "", "", ""}, {"", "", ""}},
    {107, "Noise Pal", {"Speed", "Scale", "", "", ""}, {"", "", ""}},
    {108, "Sine", {"Speed", "Scale", "", "", ""}, {"", "", ""}},
    {109, "Phased Noise", {"Speed", "Intensity", "", "", ""}, {"", "", ""}},
    {110, "Flow", {"Speed", "Zones", "", "", ""}, {"", "", ""}},
    {111, "Chunchun", {"Speed", "Gap size", "", "", ""}, {"", "", ""}},
    {112, "Dancing Shadows", {"Speed", "# of shadows", "", "", ""}, {"", "", ""}},
    {113, "Washing Machine", {"Speed", "Intensity", "", "", ""}, {"", "", ""}},
    {114, "Rotozoomer", {"Speed", "Scale", "", "", ""}, {"Alt", "", ""}},
    {115, "Blends", {"Shift speed", "Blend speed", "", "", ""}, {"", "", ""}},
    {116, "TV Simulator", {"Speed", "Intensity", "", "", ""}, {"", "", ""}},
    {117, "Dynamic Smooth", {"Speed", "Intensity", "", "", ""}, {"", "", ""}},
    {118, "Spaceships", {"Speed", "Blur", "", "", ""}, {"Smear", "", ""}},
    {119, "Crazy Bees", {"Speed", "Blur", "", "", ""}, {"Smear", "", ""}},
    {120, "Ghost Rider", {"Fade rate", "Blur", "", "", ""}, {"", "", ""}},
    {121, "Blobs", {"Speed", "# blobs", "Blur", "Trail", ""}, {"", "", ""}},
    {122, "Scrolling Text", {"Speed", "Y Offset", "Trail", "Font size", "Rotate"}, {"Gradient", "Custom Font", "Reverse"}},
    {123, "Drift Rose", {"Fade", "Blur", "", "", ""}, {"Smear", "", ""}},
    {124, "Distortion Waves", {"Speed", "Scale", "", "", ""}, {"Fill", "Zoom", "Alt"}},
    {125, "Soap", {"Speed", "Smoothness", "Density", "", ""}, {"", "", ""}},
    {126, "Octopus", {"Speed", "", "Offset X", "Offset Y", "Legs"}, {"fasttan", "", ""}},
    {127, "Waving Cell", {"Speed", "Blur", "Amplitude 1", "Amplitude 2", "Amplitude 3"}, {"", "Flow", ""}},
    {128, "Pixels", {"Fade rate", "# of pixels", "", "", ""}, {"", "", ""}},
    {129, "Pixelwave", {"Speed", "Sensitivity", "", "", ""}, {"", "", ""}},
    {130, "Juggles", {"Speed", "# of balls", "", "", ""}, {"", "", ""}},
    {131, "Matripix", {"Speed", "Brightness", "", "", ""}, {"", "", ""}},
    {132, "Gravimeter", {"Rate of fall", "Sensitivity", "", "", ""}, {"", "", ""}},
    {133, "Plasmoid", {"Phase", "# of pixels", "", "", ""}, {"", "", ""}},
    {134, "Puddles", {"Fade rate", "Puddle size", "", "", ""}, {"", "", ""}},
    {135, "Midnoise", {"Fade rate", "Max. length", "", "", ""}, {"", "", ""}},
    {136, "Noisemeter", {"Fade rate", "Width", "", "", ""}, {"", "", ""}},
    {137, "Freqwave", {"Speed", "Sound effect", "Low bin", "High bin", "Pre-amp"}, {"", "", ""}},
    {138, "Freqmatrix", {"Speed", "Sound effect", "Low bin", "High bin", "Sensitivity"}, {"", "", ""}},
    {139, "GEQ", {"Fade speed", "Ripple decay", "# of bands", "", "Bin"}, {"Color bars", "", ""}},
    {140, "Waterfall", {"Speed", "Adjust color", "Select bin", "Volume (min)", ""}, {"", "", ""}},
    {141, "Freqpixels", {"Fade rate", "Starting color and # of pixels", "", "", ""}, {"", "", ""}},
    {142, "RSVD", {"Speed", "Intensity", "Custom 1", "Custom 2", "Custom 3"}, {"", "", ""}},
    {143, "Noisefire", {"Speed", "Intensity", "", "", ""}, {"", "", ""}},
    {144, "Puddlepeak", {"Fade rate", "Puddle size", "Select bin", "Volume (min)", ""}, {"", "", ""}},
    {145, "Noisemove", {"Move speed", "Fade rate", "", "", ""}, {"", "", ""}},
    {146, "Noise2D", {"Speed", "Scale", "", "", ""}, {"", "", ""}},
    {147, "Perlin Move", {"Speed", "# of pixels", "Fade rate", "", ""}, {"", "", ""}},
    {148, "Ripple Peak", {"Fade rate", "Max # of ripples", "Select bin", "Volume (min)", ""}, {"", "", ""}},
    {149, "Firenoise", {"X scale", "Y scale", "", "", ""}, {"Palette", "", ""}},
    {150, "Squared Swirl", {"", "Fade", "", "", "Blur"}, {"", "", ""}},
    {151, "PacMan", {"Speed", "# of PowerDots", "Blink distance", "Blur", "# of Ghosts"}, {"Dots", "Smear", "Compact"}},
    {152, "DNA", {"Scroll speed", "Blur", "", "", ""}, {"Smear", "", ""}},
    {153, "Matrix", {"Speed", "Spawning rate", "Trail", "", ""}, {"Custom color", "", ""}},
    {154, "Metaballs", {"Speed", "", "", "", ""}, {"", "", ""}},
    {155, "Freqmap", {"Fade rate", "Starting color", "", "", ""}, {"", "", ""}},
    {156, "Gravcenter", {"Rate of fall", "Sensitivity", "", "", ""}, {"", "", ""}},
    {157, "Gravcentric", {"Rate of fall", "Sensitivity", "", "", ""}, {"", "", ""}},
    {158, "Gravfreq", {"Rate of fall", "Sensitivity", "", "", ""}, {"", "", ""}},
    {159, "DJ Light", {"Speed", "", "", "", ""}, {"", "", ""}},
    {160, "Funky Plank", {"Scroll speed", "", "# of bands", "", ""}, {"", "", ""}},
    {161, "Shimmer", {"Speed", "Interval", "Size", "Granular", "Flow"}, {"Zebra", "Reverse", "Sporadic"}},
    {162, "Pulser", {"Speed", "Blur", "", "", ""}, {"", "", ""}},
    {163, "Blurz", {"Fade rate", "Blur", "", "", ""}, {"", "", ""}},
    {164, "Drift", {"Rotation speed", "Blur", "", "", ""}, {"Twin", "Smear", ""}},
    {165, "Waverly", {"Amplification", "Sensitivity", "", "", ""}, {"", "Blur", ""}},
    {166, "Sun Radiation", {"Variance", "Brightness", "", "", ""}, {"", "", ""}},
    {167, "Colored Bursts", {"Speed", "# of lines", "", "", "Blur"}, {"Gradient", "Smear", "Dots"}},
    {168, "Julia", {"", "Max iterations per pixel", "X center", "Y center", "Area size"}, {" Blur", "", ""}},
    {169, "RSVD", {"Speed", "Intensity", "Custom 1", "Custom 2", "Custom 3"}, {"", "", ""}},
    {170, "RSVD", {"Speed", "Intensity", "Custom 1", "Custom 2", "Custom 3"}, {"", "", ""}},
    {171, "RSVD", {"Speed", "Intensity", "Custom 1", "Custom 2", "Custom 3"}, {"", "", ""}},
    {172, "Game Of Life", {"Speed", "", "Blur", "", ""}, {"", "", "Mutation"}},
    {173, "Tartan", {"X scale", "Y scale", "", "", "Sharpness"}, {"", "", ""}},
    {174, "Polar Lights", {"Speed", "Scale", "", "", ""}, {"Flip Palette", "", ""}},
    {175, "Swirl", {"Speed", "Sensitivity", "Blur", "", ""}, {"", "", ""}},
    {176, "Lissajous", {"X frequency", "Fade rate", "Blur", "", "Speed"}, {"Smear", "", ""}},
    {177, "Frizzles", {"X frequency", "Y frequency", "Blur", "", ""}, {"Smear", "", ""}},
    {178, "Plasma Ball", {"Speed", "", "Fade", "Blur", ""}, {"", "", ""}},
    {179, "Flow Stripe", {"Hue speed", "Effect speed", "", "", ""}, {"", "", ""}},
    {180, "Hiphotic", {"X scale", "Y scale", "", "", "Speed"}, {"", "", ""}},
    {181, "Sindots", {"Speed", "Dot distance", "Fade rate", "Blur", ""}, {"Smear", "", ""}},
    {182, "DNA Spiral", {"Scroll speed", "Y frequency", "Blur", "", ""}, {"Smear", "", ""}},
    {183, "Black Hole", {"Fade rate", "Outer Y freq.", "Outer X freq.", "Inner X freq.", "Inner Y freq."}, {"Solid", "", "Blur"}},
    {184, "Wavesins", {"Speed", "Brightness variation", "Starting color", "Range of colors", "Color variation"}, {"", "", ""}},
    {185, "Rocktaves", {"", "", "", "", ""}, {"", "", ""}},
    {186, "Akemi", {"Color speed", "Dance", "", "", ""}, {"", "", ""}},
    {187, "PS Volcano", {"Speed", "Intensity", "Move", "Bounce", "Spread"}, {"AgeColor", "Walls", "Collide"}},
    {188, "PS Fire", {"Speed", "Intensity", "Flame Height", "Wind", "Spread"}, {"Smooth", "Cylinder", "Turbulence"}},
    {189, "PS Fireworks", {"Launches", "Explosion Size", "Fuse", "Blur", "Gravity"}, {"Cylinder", "Ground", "Fast"}},
    {190, "PS Vortex", {"Rotation Speed", "Particle Speed", "Arms", "Flip", "Nozzle"}, {"Smear", "Direction", "Random Flip"}},
    {191, "PS Fuzzy Noise", {"Speed", "Particles", "Bounce", "Friction", "Scale"}, {"Cylinder", "Smear", "Collide"}},
    {192, "PS Ballpit", {"Speed", "Intensity", "Size", "Hardness", "Saturation"}, {"Cylinder", "Walls", "Ground"}},
    {193, "PS Box", {"Speed", "Particles", "Tilt", "Hardness", "Size"}, {"Random", "Washing Machine", "Sloshing"}},
    {194, "PS Attractor", {"Mass", "Particles", "Size", "Collide", "Friction"}, {"AgeColor", "Move", "Swallow"}},
    {195, "PS Impact", {"Launches", "Intensity", "Force", "Hardness", "Blur"}, {"Cylinder", "Walls", "Collide"}},
    {196, "PS Waterfall", {"Speed", "Intensity", "Variation", "Collide", "Position"}, {"Cylinder", "Walls", "Ground"}},
    {197, "PS Spray", {"Speed", "Intensity", "Left/Right", "Up/Down", "Angle"}, {"Gravity", "Cylinder/Square", "Collide"}},
    {198, "PS GEQ 2D", {"Speed", "Intensity", "Diverge", "Bounce", "Gravity"}, {"Cylinder", "Walls", "Floor"}},
    {199, "PS GEQ Nova", {"Speed", "Intensity", "Rotation Speed", "Color Change", "Nozzle"}, {"", "Direction", ""}},
    {200, "PS Ghost Rider", {"Speed", "Spiral", "Blur", "Color Cycle", "Spread"}, {"AgeColor", "Walls", ""}},
    {201, "PS Blobs", {"Speed", "Blobs", "Size", "Life", "Blur"}, {"Wobble", "Collide", "Pulsate"}},
    {202, "PS DripDrop", {"Speed", "Intensity", "Splash", "Blur", "Gravity"}, {"Rain", "PushSplash", "Smooth"}},
    {203, "PS Pinball", {"Speed", "Intensity", "Size", "Blur", "Gravity"}, {"Collide", "Rolling", "Position Color"}},
    {204, "PS Dancing Shadows", {"Speed", "Intensity", "Blur", "Color Cycle", ""}, {"Smear", "Position Color", "Smooth"}},
    {205, "PS Fireworks 1D", {"Gravity", "Explosion", "Firing side", "Blur", "Color"}, {"Colorful", "Trail", "Smooth"}},
    {206, "PS Sparkler", {"Move", "Intensity", "Saturation", "Blur", "Sparklers"}, {"Slide", "Bounce", "Large"}},
    {207, "PS Hourglass", {"Interval", "Intensity", "Color", "Blur", "Gravity"}, {"Colorflip", "Start", "Fast Reset"}},
    {208, "PS Spray 1D", {"Speed(+/-)", "Intensity", "Position", "Blur", "Gravity(+/-)"}, {"AgeColor", "Bounce", "Position Color"}},
    {209, "PS 1D Balance", {"Speed", "Intensity", "Hardness", "Blur", "Tilt"}, {"Position Color", "Wrap", "Random"}},
    {210, "PS Chase", {"Speed", "Density", "Size", "Hue", "Blur"}, {"Playful", "", "Position Color"}},
    {211, "PS Starburst", {"Chance", "Fragments", "Size", "Blur", "Cooling"}, {"Gravity", "Colorful", "Push"}},
    {212, "PS GEQ 1D", {"Speed", "Intensity", "Size", "Blur", ""}, {"", "", ""}},
    {213, "PS Fire 1D", {"Speed", "Intensity", "Cooling", "Blur", ""}, {"", "", ""}},
    {214, "PS Sonic Stream", {"Speed", "Intensity", "Color", "Blur", "Bin"}, {"Mod", "Filter", "Push"}},
    {215, "PS Sonic Boom", {"Speed", "Intensity", "Color", "Position", "Bin"}, {"Mod", "Filter", "Blur"}},
    {216, "PS Springy", {"Stiffness", "Damping", "Density", "Hue", "Mode"}, {"Smear", "XL", "AR"}},
    {217, "PS Galaxy", {"Speed", "Intensity", "Size", "", "Color"}, {"", "Starfield", "Trace"}},
    {218, "Color Clouds", {"Speed", "Intensity", "Clouds", "Colors", "Distance"}, {"", "", "Cozy"}},
    {219, "Slow Transition", {"Time (min)", "", "", "", ""}, {"", "Sweep", ""}},
};

static char *s_wled_effect_options;
static bool s_wled_effect_options_ready;
static const char s_wled_effect_options_fallback[] = "Solid";

size_t wled_effect_catalog_count(void)
{
    return sizeof(s_wled_effects) / sizeof(s_wled_effects[0]);
}

const wled_effect_catalog_item_t *wled_effect_catalog_item(size_t index)
{
    if (index >= wled_effect_catalog_count()) return NULL;
    return &s_wled_effects[index];
}

int wled_effect_catalog_index_for_id(uint8_t id)
{
    for (size_t i = 0; i < wled_effect_catalog_count(); i++) {
        if (s_wled_effects[i].id == id) return (int)i;
    }
    return -1;
}

const wled_effect_catalog_item_t *wled_effect_catalog_find(uint8_t id)
{
    int index = wled_effect_catalog_index_for_id(id);
    return index >= 0 ? &s_wled_effects[index] : NULL;
}

uint8_t wled_effect_catalog_id_for_index(uint16_t index)
{
    if (index >= wled_effect_catalog_count()) return s_wled_effects[0].id;
    return s_wled_effects[index].id;
}

uint8_t wled_effect_catalog_adjacent_id(uint8_t current_id, int delta)
{
    int index = wled_effect_catalog_index_for_id(current_id);
    if (index < 0) {
        if (delta < 0) {
            for (int i = (int)wled_effect_catalog_count() - 1; i >= 0; i--) {
                if (s_wled_effects[i].id < current_id) return s_wled_effects[i].id;
            }
        } else if (delta > 0) {
            for (size_t i = 0; i < wled_effect_catalog_count(); i++) {
                if (s_wled_effects[i].id > current_id) return s_wled_effects[i].id;
            }
        }
        return current_id;
    }

    if (delta < 0 && index > 0) index--;
    else if (delta > 0 && index < (int)wled_effect_catalog_count() - 1) index++;
    return s_wled_effects[index].id;
}

const char *wled_effect_catalog_options(void)
{
    if (s_wled_effect_options_ready) return s_wled_effect_options;
    if (!s_wled_effect_options) {
        s_wled_effect_options = heap_caps_malloc(WLED_EFFECT_OPTIONS_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_wled_effect_options) return s_wled_effect_options_fallback;
    }

    size_t offset = 0;
    for (size_t i = 0; i < wled_effect_catalog_count(); i++) {
        const char *suffix = (i + 1 < wled_effect_catalog_count()) ? "\n" : "";
        int written = snprintf(s_wled_effect_options + offset,
                               WLED_EFFECT_OPTIONS_MAX - offset,
                               "%s%s", s_wled_effects[i].name, suffix);
        if (written < 0) break;
        size_t used = (size_t)written;
        if (used >= WLED_EFFECT_OPTIONS_MAX - offset) {
            offset = WLED_EFFECT_OPTIONS_MAX - 1;
            break;
        }
        offset += used;
    }

    s_wled_effect_options[WLED_EFFECT_OPTIONS_MAX - 1] = '\0';
    s_wled_effect_options_ready = true;
    return s_wled_effect_options;
}
