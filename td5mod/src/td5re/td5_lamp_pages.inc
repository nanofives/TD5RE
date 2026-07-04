/* td5_lamp_pages.inc -- content-classified street-lamp GLOW texture pages
 * per level. Generated offline by a bright-radial-blob detector over the
 * extracted texture pages (re/assets/levels/levelNNN/textures.src/pages).
 * level023 (Moscow) list is eyeball-verified. Consumed by
 * td5_light_lamp_page_is_halo(). */
typedef struct { int level; int count; const int *pages; } TD5_LampPageTable;
static const int k_lamp_pages_001[] = { 152, 153, 154, 378 };
static const int k_lamp_pages_006[] = { 91, 92, 93, 211 };
static const int k_lamp_pages_008[] = { 91, 116 };
static const int k_lamp_pages_009[] = { 309 };
static const int k_lamp_pages_011[] = { 167, 217 };
static const int k_lamp_pages_016[] = { 327 };
static const int k_lamp_pages_023[] = { 253, 254, 255, 256, 474 };
static const int k_lamp_pages_030[] = { 81 };
static const TD5_LampPageTable k_lamp_page_table[] = {
    { 1, 4, k_lamp_pages_001 },
    { 6, 4, k_lamp_pages_006 },
    { 8, 2, k_lamp_pages_008 },
    { 9, 1, k_lamp_pages_009 },
    { 11, 2, k_lamp_pages_011 },
    { 16, 1, k_lamp_pages_016 },
    { 23, 5, k_lamp_pages_023 },
    { 30, 1, k_lamp_pages_030 },
};
