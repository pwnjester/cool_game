#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <ctype.h>
#include "./constants.h"

// TODO:
// i want to add npcs and figure out how the the story will progress (dialog)
// i want to build the combat system and make some cards and inventory items
// i want to add lighting

int game_is_running = FALSE;
SDL_Window* window = NULL;
SDL_Renderer* renderer = NULL;

int last_frame_time = 0;

static char level_tiles[MAX_ROWS][MAX_COLS][TOKEN_SIZE];
static int level_rows = 0;
static int level_cols = 0;
// collision map: 1 = solid, 0 = walkable
static int collision_map[MAX_ROWS][MAX_COLS] = {{0}};
// pixel offsets to center the level on screen
static int level_offset_x = 0;
static int level_offset_y = 0;

struct player {
    float x;
    float y;
    float width;
    float height;
} player;

// --- Inventory / Items ---
#define CARD_SLOTS 5
#define OTHER_SLOTS 10

typedef enum { ITEM_CARD, ITEM_WEAPON, ITEM_CONSUMABLE } ItemType;

typedef struct {
    char id[8]; // short id like "C01", "HP1"
    ItemType type;
    int stack; // current stack count
    int max_stack; // max allowed
    SDL_Texture* tex; // cached texture for this item
} Item;

typedef struct {
    Item slots[CARD_SLOTS];
} CardInventory;

typedef struct {
    Item slots[OTHER_SLOTS];
} OtherInventory;

static CardInventory cardInv;
static OtherInventory otherInv;

// Player stats
static int player_level = 1;
static int player_max_hp = 100;
static int player_hp = 100;
static int player_defense_pct = 0; // 0-100 percent damage reduction

// rendering scale (zoom)
static float render_scale = 1.0f;
static int game_over = 0;

// small 3x5 bitmap font for 0-9 and a few letters (H,P,C,W)
// each entry is 5 rows of 3 bits (LSB is rightmost pixel)
static const uint8_t font_3x5_digits[16][5] = {
    // 0
    {0b111,0b101,0b101,0b101,0b111},
    // 1
    {0b010,0b110,0b010,0b010,0b111},
    // 2
    {0b111,0b001,0b111,0b100,0b111},
    // 3
    {0b111,0b001,0b111,0b001,0b111},
    // 4
    {0b101,0b101,0b111,0b001,0b001},
    // 5
    {0b111,0b100,0b111,0b001,0b111},
    // 6
    {0b111,0b100,0b111,0b101,0b111},
    // 7
    {0b111,0b001,0b010,0b100,0b100},
    // 8
    {0b111,0b101,0b111,0b101,0b111},
    // 9
    {0b111,0b101,0b111,0b001,0b111},
    // 10: H
    {0b101,0b101,0b111,0b101,0b101},
    // 11: P
    {0b111,0b101,0b111,0b100,0b100},
    // 12: C
    {0b111,0b100,0b100,0b100,0b111},
    // 13: W
    {0b101,0b101,0b101,0b111,0b101},
    // 14: ':'
    {0b000,0b010,0b000,0b010,0b000},
    // 15: '/'
    {0b001,0b001,0b010,0b100,0b100}
};

static int char_to_font_index(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch == 'H') return 10;
    if (ch == 'P') return 11;
    if (ch == 'C') return 12;
    if (ch == 'W') return 13;
    if (ch == ':') return 14;
    if (ch == '/') return 15;
    return -1;
}

static void draw_char_small(int x, int y, int scale, SDL_Color color, char ch) {
    int idx = char_to_font_index(ch);
    if (idx < 0) return;
    const uint8_t *glyph = font_3x5_digits[idx];
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    for (int row = 0; row < 5; ++row) {
        for (int col = 0; col < 3; ++col) {
            if (glyph[row] & (1 << (2-col))) {
                SDL_Rect r = { x + col*scale, y + row*scale, scale, scale };
                SDL_RenderFillRect(renderer, &r);
            }
        }
    }
}

static void draw_string_small(int x, int y, int scale, SDL_Color color, const char *s) {
    int ox = x;
    while (*s) {
        if (*s == ' ') { ox += (3+1)*scale; s++; continue; }
        draw_char_small(ox, y, scale, color, *s);
        ox += (3+1)*scale; s++;
    }
}

// helper: clear an Item slot
static void clear_item(Item *it) {
    it->id[0] = '\0';
    it->type = ITEM_CARD;
    it->stack = 0;
    it->max_stack = 0;
    it->tex = NULL;
}

// helper: add item to inventory (simple first-fit stacking)
static int add_item_to_inventory(Item item) {
    // choose inventory by type
    if (item.type == ITEM_CARD) {
        for (int i = 0; i < CARD_SLOTS; ++i) {
            Item *s = &cardInv.slots[i];
            if (s->id[0] == '\0') {
                *s = item; return 1;
            }
            if (strcmp(s->id, item.id) == 0 && s->stack < s->max_stack) {
                int can = s->max_stack - s->stack;
                int move = item.stack < can ? item.stack : can;
                s->stack += move; item.stack -= move;
                if (item.stack <= 0) return 1;
            }
        }
        return 0; // full
    } else {
        for (int i = 0; i < OTHER_SLOTS; ++i) {
            Item *s = &otherInv.slots[i];
            if (s->id[0] == '\0') {
                *s = item; return 1;
            }
            if (strcmp(s->id, item.id) == 0 && s->stack < s->max_stack) {
                int can = s->max_stack - s->stack;
                int move = item.stack < can ? item.stack : can;
                s->stack += move; item.stack -= move;
                if (item.stack <= 0) return 1;
            }
        }
        return 0;
    }
}

// initialize inventories
static void init_inventories() {
    for (int i = 0; i < CARD_SLOTS; ++i) clear_item(&cardInv.slots[i]);
    for (int i = 0; i < OTHER_SLOTS; ++i) clear_item(&otherInv.slots[i]);
}

typedef struct {
    char key[TOKEN_SIZE];
    SDL_Texture* tex;
} TextureCacheEntry;

static TextureCacheEntry texture_cache[MAX_TEXTURE_CACHE];
static int texture_cache_count = 0;
static SDL_Texture* player_tex = NULL;
static SDL_Texture* player_tex_up = NULL;
static SDL_Texture* player_tex_right = NULL;
static SDL_Texture* player_tex_left = NULL;

typedef enum { DIR_DOWN = 0, DIR_UP = 1, DIR_LEFT = 2, DIR_RIGHT = 3 } Direction;
static Direction player_dir = DIR_DOWN;

// fallback textures created at runtime when file is missing
static SDL_Texture* fallback_tile = NULL;
static SDL_Texture* fallback_entity = NULL;
static SDL_Texture* fallback_player = NULL;
static SDL_Texture* ui_slot_tex = NULL;
static SDL_Texture* ui_card_placeholder = NULL;
static SDL_Texture* ui_item_placeholder = NULL;
static TTF_Font* ui_font = NULL;

// simple NPC representation
typedef struct {
    char id; /* letter */
    float x, y;
    float width, height;
    SDL_Texture* tex;
} NPC;

static NPC npcs[128];
static int npc_count = 0;

// helper: create a solid-color texture for a token and cache it
static SDL_Texture* create_colored_texture_for_token(const char* token, int w, int h) {
    // use simple hashing to derive a color from token
    unsigned int hash = 0;
    for (const char* p = token; *p; ++p) hash = (hash * 131) + (unsigned char)(*p);
    Uint8 r = 80 + (hash & 0x7F);
    Uint8 g = 40 + ((hash >> 8) & 0x7F);
    Uint8 b = 120 + ((hash >> 16) & 0x7F);

    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!s) return NULL;
    SDL_FillRect(s, NULL, SDL_MapRGBA(s->format, r, g, b, 255));
    SDL_Texture* t = SDL_CreateTextureFromSurface(renderer, s);
    SDL_FreeSurface(s);
    return t;
}

int initialize_window(void) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "Error initializing SDL: %s\n", SDL_GetError());
        return FALSE;
    }

    int img_flags = IMG_INIT_PNG;
    if (!(IMG_Init(img_flags) & img_flags)) {
        fprintf(stderr, "Error initializing SDL_image: %s\n", SDL_GetError());
        return FALSE;
    }

    window = SDL_CreateWindow(
        "me_and_manas",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        WINDOW_WIDTH,
        WINDOW_HEIGHT,
        0
    );
    if (!window) {
        fprintf(stderr, "Error creating SDL Window: %s\n", SDL_GetError());
        return FALSE;
    }
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        fprintf(stderr, "Error creating SDL Renderer: %s\n", SDL_GetError());
        return FALSE;
    }
    
    last_frame_time = SDL_GetTicks();

    return TRUE;
}

static SDL_Texture* cache_lookup(const char* key) {
    for (int i = 0; i < texture_cache_count; ++i) {
        if (strcmp(texture_cache[i].key, key) == 0) return texture_cache[i].tex;
    }
    return NULL;
}

static SDL_Texture* cache_insert(const char* key, SDL_Texture* tex) {
    if (texture_cache_count >= MAX_TEXTURE_CACHE) return tex;
    strncpy(texture_cache[texture_cache_count].key, key, TOKEN_SIZE - 1);
    texture_cache[texture_cache_count].key[TOKEN_SIZE - 1] = '\0';
    texture_cache[texture_cache_count].tex = tex;
    texture_cache_count++;
    return tex;
}

static SDL_Texture* load_texture_for_token(const char* token) {
    SDL_Texture* cached = cache_lookup(token);
    if (cached) return cached;

    char path[512];
    if (isalpha((unsigned char)token[0])) {
        snprintf(path, sizeof(path), "assets/entities/%c.png", token[0]);
    } else {
        snprintf(path, sizeof(path), "assets/tiles/%s.png", token);
    }

    SDL_Texture* tex = IMG_LoadTexture(renderer, path);
    if (!tex) {
        fprintf(stderr, "Failed to load texture '%s': %s\n", path, IMG_GetError());
        // generate a per-token colored fallback and cache it
        int w = TILE_SIZE;
        int h = TILE_SIZE;
        if (isalpha((unsigned char)token[0])) {
            // entity sized fallback
            tex = create_colored_texture_for_token(token, w, h);
        } else {
            tex = create_colored_texture_for_token(token, w, h);
        }
        if (tex) cache_insert(token, tex);
    } else {
        cache_insert(token, tex);
    }
    return tex;
}

int load_level(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Failed to open level file '%s'\n", path);
        return FALSE;
    }
    char line[1024];
    int r = 0;
    int max_cols = 0;
    // reset npc list and collision map
    npc_count = 0;
    memset(collision_map, 0, sizeof(collision_map));

    while (r < MAX_ROWS && fgets(line, sizeof(line), f)) {
        char* p = strchr(line, '\n');
        if (p) *p = '\0';
        int c = 0;
        char* token = strtok(line, " \t");
        while (token && c < MAX_COLS) {
            // trim trailing control characters (e.g. '\r') from token
            size_t tlen = strlen(token);
            while (tlen > 0 && (unsigned char)token[tlen-1] < 32) {
                token[--tlen] = '\0';
            }

            // support syntax like A(00) or P(00): core token before '(' and underlying tile inside
            char core[64];
            char under[TOKEN_SIZE] = "";
            strncpy(core, token, sizeof(core)-1);
            core[sizeof(core)-1] = '\0';
            char *lp = strchr(core, '(');
            if (lp) {
                char *rp = strchr(lp, ')');
                if (rp && rp > lp+1) {
                    size_t ilen = (size_t)(rp - lp - 1);
                    char inner[16];
                    if (ilen >= sizeof(inner)) ilen = sizeof(inner)-1;
                    strncpy(inner, lp+1, ilen);
                    inner[ilen] = '\0';
                    // trim inner
                    size_t inlen = strlen(inner);
                    while (inlen > 0 && (unsigned char)inner[inlen-1] < 32) inner[--inlen] = '\0';
                    if (inlen > 0 && isdigit((unsigned char)inner[0])) {
                        int v = atoi(inner);
                        snprintf(under, TOKEN_SIZE, "%02d", v);
                    } else {
                        strncpy(under, inner, TOKEN_SIZE-1);
                        under[TOKEN_SIZE-1] = '\0';
                    }
                    *lp = '\0';
                }
            }

            // trim core trailing control
            size_t clen = strlen(core);
            while (clen > 0 && (unsigned char)core[clen-1] < 32) core[--clen] = '\0';

            // normalize main/core token
            char norm_main[TOKEN_SIZE];
            if (clen > 0 && isdigit((unsigned char)core[0])) {
                int v = atoi(core);
                snprintf(norm_main, TOKEN_SIZE, "%02d", v);
            } else {
                strncpy(norm_main, core, TOKEN_SIZE-1);
                norm_main[TOKEN_SIZE-1] = '\0';
            }

            // determine the effective tile shown under this cell: use 'under' if provided, else if main is numeric use norm_main, otherwise default to "00"
            char floor_token[TOKEN_SIZE];
            if (under[0] != '\0') {
                strncpy(floor_token, under, TOKEN_SIZE-1);
            } else if (isdigit((unsigned char)norm_main[0])) {
                strncpy(floor_token, norm_main, TOKEN_SIZE-1);
            } else {
                strncpy(floor_token, "00", TOKEN_SIZE-1);
            }
            floor_token[TOKEN_SIZE-1] = '\0';

            // store floor token into level map
            strncpy(level_tiles[r][c], floor_token, TOKEN_SIZE - 1);
            level_tiles[r][c][TOKEN_SIZE - 1] = '\0';

            // handle player spawn if core indicates P
            if ((core[0] == 'P' || core[0] == 'p')) {
                player.x = c * TILE_SIZE + (TILE_SIZE - player.width) / 2.0f;
                player.y = r * TILE_SIZE + (TILE_SIZE - player.height) / 2.0f;
            }

            // detect NPC (letter) based on core, but skip 'P' which is player
            if (isalpha((unsigned char)core[0]) && !(core[0] == 'P' || core[0] == 'p')) {
                if (npc_count < (int)(sizeof(npcs)/sizeof(npcs[0]))) {
                    NPC *n = &npcs[npc_count++];
                    n->id = core[0];
                    n->x = c * TILE_SIZE;
                    n->y = r * TILE_SIZE;
                    n->width = 24;
                    n->height = 31;
                    // use a clean single-char key when loading entity texture
                    char et[2] = { core[0], '\0' };
                    n->tex = load_texture_for_token(et);
                }
            }

            // collision: check floor_token (normalized)
            if (isdigit((unsigned char)floor_token[0])) {
                if (strcmp(floor_token, "01") == 0 || strcmp(floor_token, "02") == 0) {
                    collision_map[r][c] = 1;
                }
            }

            token = strtok(NULL, " \t");
            c++;
        }
        if (c > max_cols) max_cols = c;
        r++;
    }
    fclose(f);
    level_rows = r;
    level_cols = max_cols;
    for (int i = 0; i < npc_count; ++i) {
        int tr = (int)(npcs[i].y) / TILE_SIZE;
        int tc = (int)(npcs[i].x) / TILE_SIZE;
        if (tr >= 0 && tr < MAX_ROWS && tc >= 0 && tc < MAX_COLS) {
            strncpy(level_tiles[tr][tc], "00", TOKEN_SIZE - 1);
            level_tiles[tr][tc][TOKEN_SIZE - 1] = '\0';
        }
    }
    // player
    int ptr = (int)(player.y) / TILE_SIZE;
    int ptc = (int)(player.x) / TILE_SIZE;
    if (ptr >= 0 && ptr < MAX_ROWS && ptc >= 0 && ptc < MAX_COLS) {
        strncpy(level_tiles[ptr][ptc], "00", TOKEN_SIZE - 1);
        level_tiles[ptr][ptc][TOKEN_SIZE - 1] = '\0';
    }

    for (int rr = 0; rr < level_rows; ++rr) {
        for (int cc = 0; cc < level_cols; ++cc) {
            char tok[TOKEN_SIZE];
            strncpy(tok, level_tiles[rr][cc], TOKEN_SIZE-1);
            tok[TOKEN_SIZE-1] = '\0';
            fprintf(stdout, "%s", tok);
            if (cc < level_cols-1) fprintf(stdout, " ");
        }
        fprintf(stdout, "\n");
    }
    for (int i = 0; i < npc_count; ++i) {
        int tr = (int)(npcs[i].y) / TILE_SIZE;
        int tc = (int)(npcs[i].x) / TILE_SIZE;
        if (tr >= 0 && tr < level_rows && tc >= 0 && tc < level_cols) {
            fprintf(stdout, "NPC %c at %d,%d token=%s\n", npcs[i].id, tr, tc, level_tiles[tr][tc]);
        }
    }
    if (ptr >= 0 && ptr < level_rows && ptc >= 0 && ptc < level_cols) {
        fprintf(stdout, "Player at %d,%d token=%s\n", ptr, ptc, level_tiles[ptr][ptc]);
    }
    // compute pixel size and offsets to center
    int map_w = level_cols * TILE_SIZE;
    int map_h = level_rows * TILE_SIZE;
    level_offset_x = (WINDOW_WIDTH - map_w) / 2;
    level_offset_y = (WINDOW_HEIGHT - map_h) / 2;
    return TRUE;
}

void process_input() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                game_is_running = FALSE;
                break;
            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_ESCAPE)
                    game_is_running = FALSE;
                break;
        }
    }
}

void setup() {
    player.width = 24;
    player.height = 31;
    player.x = (WINDOW_WIDTH - player.width) / 2.0f;
    player.y = (WINDOW_HEIGHT - player.height) / 2.0f;

    // create simple fallback textures (colored rectangles) for missing assets
    // fallback_tile: dark gray
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, TILE_SIZE, TILE_SIZE, 32, SDL_PIXELFORMAT_RGBA32);
    if (s) {
        SDL_FillRect(s, NULL, SDL_MapRGBA(s->format, 80, 80, 80, 255));
        fallback_tile = SDL_CreateTextureFromSurface(renderer, s);
        SDL_FreeSurface(s);
    }
    // fallback_entity: brown
    s = SDL_CreateRGBSurfaceWithFormat(0, TILE_SIZE, TILE_SIZE, 32, SDL_PIXELFORMAT_RGBA32);
    if (s) {
        SDL_FillRect(s, NULL, SDL_MapRGBA(s->format, 160, 100, 40, 255));
        fallback_entity = SDL_CreateTextureFromSurface(renderer, s);
        SDL_FreeSurface(s);
    }
    // fallback_player: blue rectangle
    s = SDL_CreateRGBSurfaceWithFormat(0, (int)player.width, (int)player.height, 32, SDL_PIXELFORMAT_RGBA32);
    if (s) {
        SDL_FillRect(s, NULL, SDL_MapRGBA(s->format, 0, 0, 255, 255));
        fallback_player = SDL_CreateTextureFromSurface(renderer, s);
        SDL_FreeSurface(s);
    }

    player_tex = IMG_LoadTexture(renderer, "assets/player.png");
    if (!player_tex) {
        fprintf(stderr, "Could not load player texture: %s\n", IMG_GetError());
        player_tex = fallback_player;
    }

    // load directional player sprites (optional)
    player_tex_up = IMG_LoadTexture(renderer, "assets/playeru.png");
    if (!player_tex_up) { player_tex_up = player_tex; }
    player_tex_right = IMG_LoadTexture(renderer, "assets/playerr.png");
    if (!player_tex_right) { player_tex_right = player_tex; }
    player_tex_left = IMG_LoadTexture(renderer, "assets/playerl.png");
    if (!player_tex_left) { player_tex_left = player_tex; }

    // default facing down
    player_dir = DIR_DOWN;

    load_level("levels/level1.txt");
    // init inventories and player stats
    init_inventories();
    player_level = 1;
    player_max_hp = 100 + (player_level - 1) * 20;
    player_hp = player_max_hp;
    player_defense_pct = 5; // start with 5% damage reduction
    // disable automatic zoom — use scale 1.0
    render_scale = 1.0f;
        // initialize TTF and UI textures
        if (TTF_Init() == -1) fprintf(stderr, "TTF_Init error: %s\n", TTF_GetError());
        ui_font = TTF_OpenFont("assets/DejaVuSans.ttf", 16);
        if (!ui_font) {
            fprintf(stderr, "Could not open font, falling back to bitmap font: %s\n", TTF_GetError());
        }
    // create simple placeholders
    s = SDL_CreateRGBSurfaceWithFormat(0, 48, 48, 32, SDL_PIXELFORMAT_RGBA32);
        if (s) {
            SDL_FillRect(s, NULL, SDL_MapRGBA(s->format, 60,60,60,255));
            ui_slot_tex = SDL_CreateTextureFromSurface(renderer, s);
            SDL_FillRect(s, NULL, SDL_MapRGBA(s->format, 160,120,40,255));
            ui_card_placeholder = SDL_CreateTextureFromSurface(renderer, s);
            SDL_FillRect(s, NULL, SDL_MapRGBA(s->format, 40,160,40,255));
            ui_item_placeholder = SDL_CreateTextureFromSurface(renderer, s);
            SDL_FreeSurface(s);
        }
}

void update() {
    // get a delta time factor for updating object position
    Uint32 now = SDL_GetTicks();
    float delta_time = (now - last_frame_time) / 1000.0f;
    last_frame_time = now;

    const uint8_t *keystate = SDL_GetKeyboardState(NULL);
    if (game_over) {
        // restart on Enter
        if (keystate[SDL_SCANCODE_RETURN]) {
            game_over = 0; player_level = 1; player_max_hp = 100; player_hp = player_max_hp; player_defense_pct = 5; init_inventories();
        }
        return;
    }
    // quick debug keys to add items
    if (keystate[SDL_SCANCODE_1]) {
        Item it; clear_item(&it); strcpy(it.id, "C01"); it.type = ITEM_CARD; it.stack = 1; it.max_stack = 3; add_item_to_inventory(it);
    }
    if (keystate[SDL_SCANCODE_2]) {
        Item it; clear_item(&it); strcpy(it.id, "W01"); it.type = ITEM_WEAPON; it.stack = 1; it.max_stack = 1; add_item_to_inventory(it);
    }
    if (keystate[SDL_SCANCODE_3]) {
        Item it; clear_item(&it); strcpy(it.id, "H01"); it.type = ITEM_CONSUMABLE; it.stack = 1; it.max_stack = 5; add_item_to_inventory(it);
    }
    if (keystate[SDL_SCANCODE_KP_PLUS] || keystate[SDL_SCANCODE_EQUALS]) {
        player_hp += 1; if (player_hp > player_max_hp) player_hp = player_max_hp;
    }
    if (keystate[SDL_SCANCODE_KP_MINUS] || keystate[SDL_SCANCODE_MINUS]) {
        player_hp -= 1; if (player_hp < 0) player_hp = 0;
    }
    if (keystate[SDL_SCANCODE_L]) {
        player_level++; player_max_hp = 100 + (player_level - 1) * 20; if (player_hp > player_max_hp) player_hp = player_max_hp;
    }
    float speed = 100.0f;
    float dx = 0.0f, dy = 0.0f;
    if (keystate[SDL_SCANCODE_LEFT] || keystate[SDL_SCANCODE_A]) dx -= speed * delta_time;
    if (keystate[SDL_SCANCODE_RIGHT] || keystate[SDL_SCANCODE_D]) dx += speed * delta_time;
    if (keystate[SDL_SCANCODE_UP] || keystate[SDL_SCANCODE_W]) dy -= speed * delta_time;
    if (keystate[SDL_SCANCODE_DOWN] || keystate[SDL_SCANCODE_S]) dy += speed * delta_time;

    // determine facing direction from movement input
    if (dx > 0) player_dir = DIR_RIGHT;
    else if (dx < 0) player_dir = DIR_LEFT;
    else if (dy < 0) player_dir = DIR_UP;
    else if (dy > 0) player_dir = DIR_DOWN;

    // collision check: simple tile-based blocking
    float new_x = player.x + dx;
    float new_y = player.y + dy;
    int left = (int)(new_x) / TILE_SIZE;
    int right = (int)(new_x + player.width - 1) / TILE_SIZE;
    int top = (int)(player.y) / TILE_SIZE;
    int bottom = (int)(player.y + player.height - 1) / TILE_SIZE;
    int blocked_x = 0;
    if (left < 0 || right >= level_cols) blocked_x = 1;
    for (int rr = top; rr <= bottom && !blocked_x; ++rr) {
        if (rr < 0 || rr >= level_rows) continue;
        if (collision_map[rr][left] || collision_map[rr][right]) blocked_x = 1;
    }
    if (!blocked_x) player.x = new_x;

    left = (int)(player.x) / TILE_SIZE;
    right = (int)(player.x + player.width - 1) / TILE_SIZE;
    top = (int)(new_y) / TILE_SIZE;
    bottom = (int)(new_y + player.height - 1) / TILE_SIZE;
    int blocked_y = 0;
    if (top < 0 || bottom >= level_rows) blocked_y = 1;
    for (int cc = left; cc <= right && !blocked_y; ++cc) {
        if (cc < 0 || cc >= level_cols) continue;
        if (collision_map[top][cc] || collision_map[bottom][cc]) blocked_y = 1;
    }
    if (!blocked_y) player.y = new_y;

    // check game over
    if (player_hp <= 0) {
        game_over = 1;
    }
}

void render() {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    // draw map (no zoom)
    for (int r = 0; r< level_rows; ++r) {
        for (int c = 0; c < level_cols; ++c) {
            char* tok = level_tiles[r][c];
            if (tok[0] == '\0') continue;
            SDL_Texture* tex = load_texture_for_token(tok);
            if (!tex) continue;
            SDL_Rect dst = { level_offset_x + c * TILE_SIZE, level_offset_y + r * TILE_SIZE, TILE_SIZE, TILE_SIZE };
            SDL_RenderCopy(renderer, tex, NULL, &dst);
        }
    }

    // render NPCs
    for (int i = 0; i < npc_count; ++i) {
    NPC *n = &npcs[i];
    SDL_Rect nd = { level_offset_x + (int)n->x, level_offset_y + (int)n->y, (int)n->width, (int)n->height };
        if (n->tex) SDL_RenderCopy(renderer, n->tex, NULL, &nd);
        else {
            // draw fallback colored rect per id
            char key[2] = { n->id, '\0' };
            SDL_Texture* ft = cache_lookup(key);
            if (!ft) {
                ft = create_colored_texture_for_token(key, (int)n->width, (int)n->height);
                if (ft) cache_insert(key, ft);
            }
            if (ft) SDL_RenderCopy(renderer, ft, NULL, &nd);
        }
    }

    SDL_Rect dst = { level_offset_x + (int)player.x, level_offset_y + (int)player.y, (int)player.width, (int)player.height };

    // choose texture by facing direction
    SDL_Texture* use_tex = NULL;
    switch (player_dir) {
        case DIR_UP: use_tex = player_tex_up; break;
        case DIR_LEFT: use_tex = player_tex_left; break;
        case DIR_RIGHT: use_tex = player_tex_right; break;
        case DIR_DOWN: default: use_tex = player_tex; break;
    }
    if (use_tex) {
        SDL_RenderCopy(renderer, use_tex, NULL, &dst);
    } else {
        SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);
        SDL_RenderFillRect(renderer, &dst);
    }

    int ui_x = WINDOW_WIDTH - 340;
    int ui_y = 20;
    SDL_Rect panel = { ui_x, ui_y, 320, 440 };
    SDL_SetRenderDrawColor(renderer, 24, 24, 28, 230);
    SDL_RenderFillRect(renderer, &panel);
    // outer border
    SDL_SetRenderDrawColor(renderer, 60, 60, 70, 255);
    SDL_Rect border = { ui_x, ui_y, panel.w, panel.h };
    SDL_RenderDrawRect(renderer, &border);

    // player portrait area (top-left of panel)
    SDL_Color white = { 230,230,230,255 };
    int pad = 12;
    int portrait_s = 80;
    SDL_Rect portrait = { ui_x + pad, ui_y + pad, portrait_s, portrait_s };
    SDL_SetRenderDrawColor(renderer, 40, 40, 48, 255);
    SDL_RenderFillRect(renderer, &portrait);
    // draw player texture inside portrait (scaled to fit)
    if (player_tex) {
        SDL_Rect src = { 0,0, (int)player.width, (int)player.height };
        SDL_RenderCopy(renderer, player_tex, NULL, &portrait);
    } else if (fallback_player) {
        SDL_RenderCopy(renderer, fallback_player, NULL, &portrait);
    }

    // big HP bar to the right of portrait
    int hp_x = ui_x + pad + portrait_s + 12;
    int hp_y = ui_y + pad + 8;
    int hp_w = panel.w - (hp_x - ui_x) - pad;
    int hp_h = 22;
    // background
    SDL_Rect hp_bg = { hp_x, hp_y, hp_w, hp_h };
    SDL_SetRenderDrawColor(renderer, 50, 50, 60, 255);
    SDL_RenderFillRect(renderer, &hp_bg);
    // fill based on hp percentage
    float hp_pct = (player_max_hp > 0) ? ((float)player_hp / (float)player_max_hp) : 0.0f;
    if (hp_pct < 0) hp_pct = 0; if (hp_pct > 1) hp_pct = 1;
    SDL_Rect hp_fill = { hp_x + 1, hp_y + 1, (int)((hp_w - 2) * hp_pct), hp_h - 2 };
    SDL_SetRenderDrawColor(renderer, 180, 40, 40, 255);
    SDL_RenderFillRect(renderer, &hp_fill);
    // HP numeric big
    char hpbuf[32]; snprintf(hpbuf, sizeof(hpbuf), "%d / %d", player_hp, player_max_hp);
    if (ui_font) {
        SDL_Color col = {255,255,255,255};
        SDL_Surface* surf = TTF_RenderText_Blended(ui_font, hpbuf, col);
        if (surf) {
            SDL_Texture* t = SDL_CreateTextureFromSurface(renderer, surf);
            SDL_FreeSurface(surf);
            if (t) {
                int tw = 0, th = 0; SDL_QueryTexture(t, NULL, NULL, &tw, &th);
                SDL_Rect tr = { hp_x + (hp_w - tw)/2, hp_y + (hp_h - th)/2, tw, th };
                SDL_RenderCopy(renderer, t, NULL, &tr);
                SDL_DestroyTexture(t);
            }
        }
    } else {
        draw_string_small(hp_x + 8, hp_y + 4, 3, white, hpbuf);
    }

    // defense and level under the HP bar
    char defbuf[32]; snprintf(defbuf, sizeof(defbuf), "DEF: %d%%", player_defense_pct);
    char lvbuf[32]; snprintf(lvbuf, sizeof(lvbuf), "LVL: %d", player_level);
    draw_string_small(hp_x, hp_y + hp_h + 8, 2, white, defbuf);
    draw_string_small(hp_x + 110, hp_y + hp_h + 8, 2, white, lvbuf);

    // separator line
    SDL_SetRenderDrawColor(renderer, 70,70,80,255);
    SDL_Rect sep = { ui_x + pad, ui_y + pad + portrait_s + 12, panel.w - pad*2, 2 };
    SDL_RenderFillRect(renderer, &sep);

    // CARDS: spread across a single centered row with large icons
    draw_string_small(ui_x + pad, sep.y + 12, 2, white, "CARDS");
    int cards_y = sep.y + 36;
    int card_w = 56;
    int card_gap = 12;
    int total_cards_w = CARD_SLOTS * card_w + (CARD_SLOTS - 1) * card_gap;
    int start_x = ui_x + (panel.w - total_cards_w) / 2;
    for (int i = 0; i < CARD_SLOTS; ++i) {
        int sx = start_x + i * (card_w + card_gap);
        int sy = cards_y;
        SDL_Rect slot = { sx, sy, card_w, card_w };
        if (ui_slot_tex) SDL_RenderCopy(renderer, ui_slot_tex, NULL, &slot);
        if (cardInv.slots[i].id[0] != '\0') {
            SDL_Texture* itex = cardInv.slots[i].tex ? cardInv.slots[i].tex : ui_card_placeholder;
            if (itex) SDL_RenderCopy(renderer, itex, NULL, &slot);
            // draw stack number small
            char sb[8]; snprintf(sb, sizeof(sb), "%d", cardInv.slots[i].stack);
            if (ui_font) {
                SDL_Color col = {255,255,255,255};
                SDL_Surface* surf = TTF_RenderText_Blended(ui_font, sb, col);
                if (surf) {
                    SDL_Texture* t = SDL_CreateTextureFromSurface(renderer, surf);
                    SDL_FreeSurface(surf);
                    if (t) { SDL_Rect tr = { sx + card_w - 18, sy + card_w - 18, 16, 16 }; SDL_RenderCopy(renderer, t, NULL, &tr); SDL_DestroyTexture(t); }
                }
            } else draw_string_small(sx + card_w - 18, sy + card_w - 18, 2, white, sb);
        }
    }

    // ITEMS: grid below cards
    int items_y = cards_y + card_w + 24;
    draw_string_small(ui_x + pad, items_y, 2, white, "ITEMS");
    int grid_y = items_y + 20;
    int item_w = 48; int item_gap = 10; int cols = 5;
    for (int i = 0; i < OTHER_SLOTS; ++i) {
        int row = i / cols;
        int col = i % cols;
        int sx = ui_x + pad + col * (item_w + item_gap);
        int sy = grid_y + row * (item_w + item_gap);
        SDL_Rect slot = { sx, sy, item_w, item_w };
        if (ui_slot_tex) SDL_RenderCopy(renderer, ui_slot_tex, NULL, &slot);
        if (otherInv.slots[i].id[0] != '\0') {
            SDL_Texture* itex = otherInv.slots[i].tex ? otherInv.slots[i].tex : ui_item_placeholder;
            if (itex) SDL_RenderCopy(renderer, itex, NULL, &slot);
            char sb[8]; snprintf(sb, sizeof(sb), "%d", otherInv.slots[i].stack);
            if (ui_font) {
                SDL_Color col = {255,255,255,255};
                SDL_Surface* surf = TTF_RenderText_Blended(ui_font, sb, col);
                if (surf) {
                    SDL_Texture* t = SDL_CreateTextureFromSurface(renderer, surf);
                    SDL_FreeSurface(surf);
                    if (t) { SDL_Rect tr = { sx + item_w - 18, sy + item_w - 18, 16, 16 }; SDL_RenderCopy(renderer, t, NULL, &tr); SDL_DestroyTexture(t); }
                }
            } else draw_string_small(sx + item_w - 18, sy + item_w - 18, 2, white, sb);
        }
    }

    // Game over overlay
    if (game_over) {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 200);
        SDL_Rect o = { 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT };
        SDL_RenderFillRect(renderer, &o);
        draw_string_small(WINDOW_WIDTH/2 - 20, WINDOW_HEIGHT/2 - 12, 4, (SDL_Color){255,50,50,255}, "GAME");
        draw_string_small(WINDOW_WIDTH/2 + 12, WINDOW_HEIGHT/2 - 12, 4, (SDL_Color){255,50,50,255}, "OVER");
        draw_string_small(WINDOW_WIDTH/2 - 24, WINDOW_HEIGHT/2 + 16, 2, white, "Press Enter to restart");
    }

    SDL_RenderPresent(renderer);
}

void destroy_window() {
    // destroy cached textures
    for (int i = 0; i < texture_cache_count; ++i) {
        if (texture_cache[i].tex) SDL_DestroyTexture(texture_cache[i].tex);
    }
    if (player_tex && player_tex != fallback_player) SDL_DestroyTexture(player_tex);
    if (player_tex_up && player_tex_up != player_tex && player_tex_up != fallback_player) SDL_DestroyTexture(player_tex_up);
    if (player_tex_right && player_tex_right != player_tex && player_tex_right != fallback_player) SDL_DestroyTexture(player_tex_right);
    if (player_tex_left && player_tex_left != player_tex && player_tex_left != fallback_player) SDL_DestroyTexture(player_tex_left);
    if (fallback_tile) SDL_DestroyTexture(fallback_tile);
    if (fallback_entity) SDL_DestroyTexture(fallback_entity);
    if (fallback_player) SDL_DestroyTexture(fallback_player);
    if (ui_slot_tex) SDL_DestroyTexture(ui_slot_tex);
    if (ui_card_placeholder) SDL_DestroyTexture(ui_card_placeholder);
    if (ui_item_placeholder) SDL_DestroyTexture(ui_item_placeholder);
    if (ui_font) { TTF_CloseFont(ui_font); ui_font = NULL; }
    // destroy NPC textures if they are unique and cached ones already destroyed
    for (int i = 0; i < npc_count; ++i) {
        if (npcs[i].tex) {
            SDL_DestroyTexture(npcs[i].tex);
            npcs[i].tex = NULL;
        }
    }
    // destroy item textures
    for (int i = 0; i < CARD_SLOTS; ++i) if (cardInv.slots[i].tex) SDL_DestroyTexture(cardInv.slots[i].tex);
    for (int i = 0; i < OTHER_SLOTS; ++i) if (otherInv.slots[i].tex) SDL_DestroyTexture(otherInv.slots[i].tex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    IMG_Quit();
    SDL_Quit();
}

int main() {
    game_is_running = initialize_window();

    setup();

    while (game_is_running) {
        process_input();
        update();
        render();
    }

    destroy_window();

    return 0;
}
