#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <ctype.h>
#include "./constants.h"

// TODO: i need to decide what the game loop will be
// how the cards work
// and how combat is going to happen
// also learn how to use sdl

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

typedef struct {
    char key[TOKEN_SIZE];
    SDL_Texture* tex;
} TextureCacheEntry;

static TextureCacheEntry texture_cache[MAX_TEXTURE_CACHE];
static int texture_cache_count = 0;
static SDL_Texture* player_tex = NULL;

// fallback textures created at runtime when file is missing
static SDL_Texture* fallback_tile = NULL;
static SDL_Texture* fallback_entity = NULL;
static SDL_Texture* fallback_player = NULL;

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
                    n->width = TILE_SIZE;
                    n->height = TILE_SIZE;
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
    // Ensure NPC and player spawn tiles are set to 00 (floor) to avoid visual mismatches
    for (int i = 0; i < npc_count; ++i) {
        int tr = (int)(npcs[i].y) / TILE_SIZE;
        int tc = (int)(npcs[i].x) / TILE_SIZE;
        if (tr >= 0 && tr < MAX_ROWS && tc >= 0 && tc < MAX_COLS) {
            strncpy(level_tiles[tr][tc], "00", TOKEN_SIZE - 1);
            level_tiles[tr][tc][TOKEN_SIZE - 1] = '\0';
            fprintf(stdout, "Set NPC %c underlying tile at %d,%d to 00\n", npcs[i].id, tr, tc);
        }
    }
    // player
    int ptr = (int)(player.y) / TILE_SIZE;
    int ptc = (int)(player.x) / TILE_SIZE;
    if (ptr >= 0 && ptr < MAX_ROWS && ptc >= 0 && ptc < MAX_COLS) {
        strncpy(level_tiles[ptr][ptc], "00", TOKEN_SIZE - 1);
        level_tiles[ptr][ptc][TOKEN_SIZE - 1] = '\0';
        fprintf(stdout, "Set player underlying tile at %d,%d to 00\n", ptr, ptc);
    }

    // Debug: dump level tokens to stdout for inspection
    fprintf(stdout, "Level dump (%d x %d):\n", level_rows, level_cols);
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
    // Debug: print tokens under NPCs and player
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
    player.height = 32;
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

    load_level("levels/level1.txt");
}

void update() {
    // get a delta time factor for updating object position
    Uint32 now = SDL_GetTicks();
    float delta_time = (now - last_frame_time) / 1000.0f;
    last_frame_time = now;

    const uint8_t *keystate = SDL_GetKeyboardState(NULL);
    float speed = 100.0f;
    float dx = 0.0f, dy = 0.0f;
    if (keystate[SDL_SCANCODE_LEFT] || keystate[SDL_SCANCODE_A]) dx -= speed * delta_time;
    if (keystate[SDL_SCANCODE_RIGHT] || keystate[SDL_SCANCODE_D]) dx += speed * delta_time;
    if (keystate[SDL_SCANCODE_UP] || keystate[SDL_SCANCODE_W]) dy -= speed * delta_time;
    if (keystate[SDL_SCANCODE_DOWN] || keystate[SDL_SCANCODE_S]) dy += speed * delta_time;

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
}

void render() {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

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

    if (player_tex) {
        SDL_RenderCopy(renderer, player_tex, NULL, &dst);
    } else {
        SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);
        SDL_RenderFillRect(renderer, &dst);
    }

    SDL_RenderPresent(renderer);
}

void destroy_window() {
    // destroy cached textures
    for (int i = 0; i < texture_cache_count; ++i) {
        if (texture_cache[i].tex) SDL_DestroyTexture(texture_cache[i].tex);
    }
    if (player_tex && player_tex != fallback_player) SDL_DestroyTexture(player_tex);
    if (fallback_tile) SDL_DestroyTexture(fallback_tile);
    if (fallback_entity) SDL_DestroyTexture(fallback_entity);
    if (fallback_player) SDL_DestroyTexture(fallback_player);
    // destroy NPC textures if they are unique and cached ones already destroyed
    for (int i = 0; i < npc_count; ++i) {
        if (npcs[i].tex) {
            SDL_DestroyTexture(npcs[i].tex);
            npcs[i].tex = NULL;
        }
    }
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
