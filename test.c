// topdown.c
// Single-file SDL2 top-down demo
// Build: gcc -O2 -g topdown.c -o topdown `pkg-config --cflags --libs sdl2 SDL2_image`

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---------------------------
   Config
   --------------------------- */
#define WIN_W 960
#define WIN_H 640
#define TILE_W 32
#define TILE_H 32
#define MAX_ENTITIES 256
#define MAX_INV 64

/* ---------------------------
   Types
   --------------------------- */

typedef enum { ENT_PLAYER, ENT_NPC, ENT_CHEST, ENT_LIGHT } EntityType;

typedef struct {
    char *items[MAX_INV];
    int count;
} Inventory;

typedef struct {
    char type[32]; // "orc", "shop", etc
    int hp;
    int aggressive; // 0 = talk, 1 = attack
    char *talk_text;
    Inventory *inv;
} NPC;

typedef struct {
    Inventory *inv;
    int opened;
} Chest;

typedef struct {
    int id;
    EntityType type;
    float x,y;
    int w,h;
    void* comp; // NPC* or Chest*
    SDL_Texture* tex; // optional override
    int collidable;
    int alive;
} Entity;

typedef struct {
    SDL_Renderer* ren;
} AssetManager;

typedef struct {
    SDL_Window* win;
    SDL_Renderer* ren;
    int ww, wh;
    AssetManager* assets;
    Entity entities[MAX_ENTITIES];
    int next_entity_id;
    Entity* player;
    int level_w, level_h;
    int *tiles; // level_w * level_h
} Game;

/* ---------------------------
   Asset manager (simple)
   --------------------------- */

static AssetManager* asset_manager_create(SDL_Renderer* ren) {
    IMG_Init(IMG_INIT_PNG|IMG_INIT_JPG);
    AssetManager* a = (AssetManager*)malloc(sizeof(AssetManager));
    a->ren = ren;
    return a;
}

static void asset_manager_destroy(AssetManager* a) {
    if(!a) return;
    IMG_Quit();
    free(a);
}

static SDL_Texture* create_fallback_tex(SDL_Renderer* ren, int w, int h, Uint32 color) {
    SDL_Surface* s = SDL_CreateRGBSurface(0, w, h, 32,
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
        0xff000000, 0x00ff0000, 0x0000ff00, 0x000000ff
#else
        0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000
#endif
    );
    if(!s) return NULL;
    SDL_FillRect(s, NULL, color);
    SDL_Texture* t = SDL_CreateTextureFromSurface(ren, s);
    SDL_FreeSurface(s);
    return t;
}

static SDL_Texture* asset_load_texture(AssetManager* a, const char* path, int fallback_w, int fallback_h, Uint32 fallback_color) {
    if(!a) return NULL;
    SDL_Surface* s = IMG_Load(path);
    if(s) {
        SDL_Texture* t = SDL_CreateTextureFromSurface(a->ren, s);
        SDL_FreeSurface(s);
        if(t) return t;
    }
    // fallback (unique-ish color)
    SDL_Texture* f = create_fallback_tex(a->ren, fallback_w, fallback_h, fallback_color);
    return f;
}

/* ---------------------------
   Entity & Game management
   --------------------------- */

static Game* game_create(int w, int h) {
    Game* g = (Game*)calloc(1, sizeof(Game));
    g->ww = w; g->wh = h;
    if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        free(g); return NULL;
    }
    g->win = SDL_CreateWindow("Topdown", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, 0);
    if(!g->win) {
        fprintf(stderr, "Window create failed: %s\n", SDL_GetError());
        SDL_Quit(); free(g); return NULL;
    }
    g->ren = SDL_CreateRenderer(g->win, -1, SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
    if(!g->ren) {
        fprintf(stderr, "Renderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(g->win); SDL_Quit(); free(g); return NULL;
    }
    g->assets = asset_manager_create(g->ren);
    g->next_entity_id = 1;
    g->player = NULL;
    return g;
}

static void game_destroy(Game* g) {
    if(!g) return;
    if(g->tiles) free(g->tiles);
    // free entity components
    for(int i=0;i<MAX_ENTITIES;i++){
        if(g->entities[i].alive){
            if(g->entities[i].comp){
                if(g->entities[i].type == ENT_NPC){
                    NPC* n = (NPC*)g->entities[i].comp;
                    if(n->talk_text) free(n->talk_text);
                    if(n->inv){
                        for(int j=0;j<n->inv->count;j++) if(n->inv->items[j]) free(n->inv->items[j]);
                        free(n->inv);
                    }
                    free(n);
                } else if(g->entities[i].type == ENT_CHEST){
                    Chest* c = (Chest*)g->entities[i].comp;
                    if(c->inv){
                        for(int j=0;j<c->inv->count;j++) if(c->inv->items[j]) free(c->inv->items[j]);
                        free(c->inv);
                    }
                    free(c);
                }
            }
            if(g->entities[i].tex) SDL_DestroyTexture(g->entities[i].tex);
        }
    }
    asset_manager_destroy(g->assets);
    SDL_DestroyRenderer(g->ren);
    SDL_DestroyWindow(g->win);
    SDL_Quit();
    free(g);
}

static int game_spawn_entity(Game* g, EntityType t, float x, float y, int w, int h, void* comp) {
    for(int i=0;i<MAX_ENTITIES;i++){
        if(!g->entities[i].alive){
            Entity* e = &g->entities[i];
            memset(e,0,sizeof(*e));
            e->alive = 1;
            e->id = g->next_entity_id++;
            e->type = t;
            e->x = x; e->y = y; e->w = w; e->h = h;
            e->comp = comp;
            e->collidable = (t!=ENT_LIGHT);
            return e->id;
        }
    }
    return -1;
}

static Entity* game_entity_by_id(Game* g, int id) {
    for(int i=0;i<MAX_ENTITIES;i++) if(g->entities[i].alive && g->entities[i].id == id) return &g->entities[i];
    return NULL;
}

/* ---------------------------
   Inventory helpers
   --------------------------- */

static Inventory* inventory_create() {
    Inventory* inv = (Inventory*)malloc(sizeof(Inventory));
    inv->count = 0;
    return inv;
}

static void inventory_add(Inventory* inv, const char* name) {
    if(!inv || inv->count >= MAX_INV) return;
    inv->items[inv->count++] = strdup(name);
}

/* ---------------------------
   NPC & chest creation helpers
   --------------------------- */

static NPC* npc_create_orc() {
    NPC* n = (NPC*)calloc(1,sizeof(NPC));
    strcpy(n->type, "orc");
    n->hp = 20;
    n->aggressive = 1;
    n->talk_text = strdup("grrrr orc says hello");
    n->inv = inventory_create();
    inventory_add(n->inv, "orc_tooth");
    return n;
}

static NPC* npc_create_shop() {
    NPC* n = (NPC*)calloc(1,sizeof(NPC));
    strcpy(n->type, "shop");
    n->hp = 100;
    n->aggressive = 0;
    n->talk_text = strdup("welcome, buy stuff");
    n->inv = inventory_create();
    inventory_add(n->inv, "potion");
    inventory_add(n->inv, "bread");
    return n;
}

static Chest* chest_create_with_item(const char* it) {
    Chest* c = (Chest*)calloc(1,sizeof(Chest));
    c->opened = 0;
    c->inv = inventory_create();
    if(it) inventory_add(c->inv, it);
    return c;
}

/* ---------------------------
   Level loader
   --------------------------- */

// Map format: csv rows (tile indices). tile 0 = grass, 1 = wall, 2 = lamp, 3 = chest spawn tile (optional)
static int load_level(Game* g, const char* map_path, const char* npc_path) {
    FILE* f = fopen(map_path, "r");
    if(!f) {
        fprintf(stderr, "Could not open map %s\n", map_path);
        return -1;
    }

    char line[8192];
    int rows = 0;
    int cols = 0;
    // first pass: count rows/cols
    int temp_cap = 0;
    int *temp_data = NULL;
    while(fgets(line, sizeof(line), f)){
        char *s = line;
        int c = 0;
        char *tok = strtok(s, ",\r\n");
        while(tok){
            int val = atoi(tok);
            if(rows* (cols?cols:1000) + c >= temp_cap){
                temp_cap = temp_cap ? temp_cap*2 : 1024;
                temp_data = (int*)realloc(temp_data, sizeof(int)*temp_cap);
            }
            temp_data[rows*1000 + c] = val; // temporary pack in wide stride
            c++;
            tok = strtok(NULL, ",\r\n");
        }
        if(c>0){
            if(rows==0) cols = c;
            rows++;
        }
    }
    fclose(f);
    if(rows==0 || cols==0){
        if(temp_data) free(temp_data);
        fprintf(stderr, "Empty or invalid map\n");
        return -1;
    }
    g->level_w = cols;
    g->level_h = rows;
    g->tiles = (int*)malloc(sizeof(int) * cols * rows);
    // re-read properly
    f = fopen(map_path, "r");
    int yi = 0;
    while(fgets(line, sizeof(line), f) && yi < rows){
        int xi = 0;
        char *tok = strtok(line, ",\r\n");
        while(tok && xi < cols){
            g->tiles[yi*cols + xi] = atoi(tok);
            xi++;
            tok = strtok(NULL, ",\r\n");
        }
        // fill rest
        while(xi < cols){ g->tiles[yi*cols + xi] = 0; xi++; }
        yi++;
    }
    fclose(f);

    // spawn default lights for tile type 2 (lamp) and chests for tile 3
    for(int y=0;y<g->level_h;y++){
        for(int x=0;x<g->level_w;x++){
            int t = g->tiles[y*g->level_w + x];
            if(t == 2) {
                game_spawn_entity(g, ENT_LIGHT, x*TILE_W + TILE_W/2 - 16, y*TILE_H + TILE_H/2 - 16, 160, 160, NULL);
            } else if(t == 3) {
                Chest* c = chest_create_with_item("gold_coin");
                game_spawn_entity(g, ENT_CHEST, x*TILE_W + 2, y*TILE_H + 2, 28, 28, c);
            }
        }
    }

    // NPC file parsing
    FILE* nf = fopen(npc_path, "r");
    if(nf){
        while(fgets(line, sizeof(line), nf)){
            if(line[0] == '#' || line[0] == '\n') continue;
            char type[64]; int tx, ty;
            char arg[256] = {0};
            int n = sscanf(line, "%63[^,],%d,%d,%255[^\n]", type, &tx, &ty, arg);
            if(n >= 3){
                if(strcmp(type, "orc")==0) {
                    NPC* npc = npc_create_orc();
                    if(n == 4 && arg[0]) { free(npc->talk_text); npc->talk_text = strdup(arg); }
                    game_spawn_entity(g, ENT_NPC, tx*TILE_W + 2, ty*TILE_H + 2, 28, 28, npc);
                } else if(strcmp(type, "shop")==0) {
                    NPC* npc = npc_create_shop();
                    if(n == 4 && arg[0]) { free(npc->talk_text); npc->talk_text = strdup(arg); }
                    game_spawn_entity(g, ENT_NPC, tx*TILE_W + 2, ty*TILE_H + 2, 28, 28, npc);
                } else if(strcmp(type, "chest")==0) {
                    Chest* c = chest_create_with_item(arg[0]?arg:"mysterious_gem");
                    game_spawn_entity(g, ENT_CHEST, tx*TILE_W + 2, ty*TILE_H + 2, 28, 28, c);
                } else {
                    // unknown type -> treat as neutral NPC with talk text
                    NPC* npc = (NPC*)calloc(1,sizeof(NPC));
                    strncpy(npc->type, type, sizeof(npc->type)-1);
                    npc->hp = 10; npc->aggressive = 0;
                    npc->talk_text = strdup(arg[0]?arg:"hello");
                    npc->inv = inventory_create();
                    game_spawn_entity(g, ENT_NPC, tx*TILE_W + 2, ty*TILE_H + 2, 28, 28, npc);
                }
            }
        }
        fclose(nf);
    }
    if(temp_data) free(temp_data);
    return 0;
}

/* ---------------------------
   Simple renderer + lighting
   --------------------------- */

static void render_lightmap(Game* g) {
    // create target texture
    SDL_Texture* lm = SDL_CreateTexture(g->ren, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, g->ww, g->wh);
    if(!lm) return;
    SDL_SetTextureBlendMode(lm, SDL_BLENDMODE_BLEND);
    SDL_SetRenderTarget(g->ren, lm);
    // ambient darkness
    SDL_SetRenderDrawColor(g->ren, 0,0,0,200);
    SDL_RenderClear(g->ren);
    // for each light entity: draw radial gradient by concentric rectangles/lines (cheap)
    for(int i=0;i<MAX_ENTITIES;i++){
        Entity* e = &g->entities[i];
        if(!e->alive) continue;
        if(e->type == ENT_LIGHT){
            int radius = e->w; // encoded radius
            int cx = (int)e->x + e->w/2;
            int cy = (int)e->y + e->h/2;
            // draw gradient: darker at edges -> lower alpha at center
            for(int r = radius; r > 0; r -= 6) {
                double rel = 1.0 - (double)r / (double)radius;
                Uint8 alpha = (Uint8)(200 * (1.0 - rel)); // center less dark
                SDL_SetRenderDrawColor(g->ren, 0,0,0, alpha);
                // draw filled circle naive by scanlines
                for(int dy = -r; dy <= r; dy++){
                    int dx = (int)floor(sqrt((double)r*r - dy*dy));
                    SDL_Rect rr = { cx - dx, cy + dy, dx*2, 1 };
                    SDL_RenderFillRect(g->ren, &rr);
                }
            }
        }
    }
    SDL_SetRenderTarget(g->ren, NULL);
    SDL_SetTextureBlendMode(lm, SDL_BLENDMODE_MOD); // modulate scene with lightmap
    SDL_RenderCopy(g->ren, lm, NULL, NULL);
    SDL_DestroyTexture(lm);
}

static void render_frame(Game* g) {
    // clear background
    SDL_SetRenderDrawColor(g->ren, 20, 20, 30, 255);
    SDL_RenderClear(g->ren);

    // draw tiles
    for(int y=0;y<g->level_h;y++){
        for(int x=0;x<g->level_w;x++){
            int idx = g->tiles[y*g->level_w + x];
            SDL_Rect r = { x*TILE_W, y*TILE_H, TILE_W, TILE_H };
            if(idx == 0) {
                SDL_SetRenderDrawColor(g->ren, 34, 139, 34, 255);
                SDL_RenderFillRect(g->ren, &r);
            } else if(idx == 1) {
                SDL_SetRenderDrawColor(g->ren, 80, 80, 90, 255);
                SDL_RenderFillRect(g->ren, &r);
            } else if(idx == 2) {
                SDL_SetRenderDrawColor(g->ren, 50, 50, 70, 255);
                SDL_RenderFillRect(g->ren, &r);
                // lamp visual small
                SDL_Rect l = { r.x + TILE_W/4, r.y + TILE_H/4, TILE_W/2, TILE_H/2 };
                SDL_SetRenderDrawColor(g->ren, 200, 200, 160, 255);
                SDL_RenderFillRect(g->ren, &l);
            } else {
                // default fallback tile
                SDL_SetRenderDrawColor(g->ren, 100, 140, 100, 255);
                SDL_RenderFillRect(g->ren, &r);
            }
        }
    }

    // draw entities
    for(int i=0;i<MAX_ENTITIES;i++){
        Entity* e = &g->entities[i];
        if(!e->alive) continue;
        SDL_Rect r = { (int)e->x, (int)e->y, e->w, e->h };
        if(e->tex) {
            SDL_RenderCopy(g->ren, e->tex, NULL, &r);
        } else {
            switch(e->type){
                case ENT_PLAYER:
                    SDL_SetRenderDrawColor(g->ren, 200, 180, 50, 255);
                    SDL_RenderFillRect(g->ren, &r);
                    break;
                case ENT_NPC:
                    SDL_SetRenderDrawColor(g->ren, 180, 80, 80, 255);
                    SDL_RenderFillRect(g->ren, &r);
                    break;
                case ENT_CHEST: {
                    Chest* c = (Chest*)e->comp;
                    if(c && c->opened) SDL_SetRenderDrawColor(g->ren, 120, 120, 120, 255);
                    else SDL_SetRenderDrawColor(g->ren, 170, 120, 60, 255);
                    SDL_RenderFillRect(g->ren, &r);
                    break;
                }
                case ENT_LIGHT:
                    // optionally draw a small marker
                    // skip to keep it purely lighting
                    break;
            }
        }
    }

    // apply lighting
    render_lightmap(g);

    SDL_RenderPresent(g->ren);
}

/* ---------------------------
   Utility: simple AABB collision
   --------------------------- */

static int aabb_overlap(float ax, float ay, int aw, int ah, float bx, float by, int bw, int bh) {
    return !(ax + aw <= bx || bx + bw <= ax || ay + ah <= by || by + bh <= ay);
}

/* ---------------------------
   Interaction: find nearby entity
   --------------------------- */

static Entity* find_entity_near(Game* g, Entity* src, float range, int filter_types) {
    for(int i=0;i<MAX_ENTITIES;i++){
        Entity* e = &g->entities[i];
        if(!e->alive || e == src) continue;
        if(! (filter_types & (1 << e->type)) ) continue;
        float dx = (e->x + e->w/2) - (src->x + src->w/2);
        float dy = (e->y + e->h/2) - (src->y + src->h/2);
        float dist2 = dx*dx + dy*dy;
        if(dist2 <= range*range) return e;
    }
    return NULL;
}

/* ---------------------------
   Main loop & input
   --------------------------- */

int main(int argc, char** argv) {
    const char* mapfile = "levels/level1.map";
    const char* npcfile = "levels/level1.npc";
    if(argc >= 3) { mapfile = argv[1]; npcfile = argv[2]; }
    else if(argc == 2) { mapfile = argv[1]; }

    Game* g = game_create(WIN_W, WIN_H);
    if(!g) return 1;

    // load a default small map if file missing (embedded)
    FILE* tf = fopen(mapfile, "r");
    if(!tf) {
        // create directories optionally not needed; instead create tiny default map in memory
        int dw = 20, dh = 15;
        g->level_w = dw; g->level_h = dh;
        g->tiles = (int*)malloc(sizeof(int)*dw*dh);
        for(int y=0;y<dh;y++){
            for(int x=0;x<dw;x++){
                if(x==0 || y==0 || x==dw-1 || y==dh-1) g->tiles[y*dw + x] = 1;
                else if((x==5 && y==5) || (x==10 && y==8)) g->tiles[y*dw + x] = 2;
                else g->tiles[y*dw + x] = 0;
            }
        }
    } else {
        fclose(tf);
        if(load_level(g, mapfile, npcfile) != 0) {
            fprintf(stderr, "Failed to load level, abort\n");
            game_destroy(g);
            return 1;
        }
    }

    // spawn a player near center
    int px = (g->level_w * TILE_W) / 2;
    int py = (g->level_h * TILE_H) / 2;
    int pid = game_spawn_entity(g, ENT_PLAYER, px, py, 28, 28, NULL);
    g->player = game_entity_by_id(g, pid);
    // spawn a sample NPC if none exist
    int found_npc = 0;
    for(int i=0;i<MAX_ENTITIES;i++) if(g->entities[i].alive && g->entities[i].type == ENT_NPC) { found_npc = 1; break; }
    if(!found_npc){
        NPC* s = npc_create_shop();
        game_spawn_entity(g, ENT_NPC, px + 60, py, 28, 28, s);
        NPC* o = npc_create_orc();
        game_spawn_entity(g, ENT_NPC, px - 80, py - 20, 28, 28, o);
    }

    // simple player inventory
    Inventory* player_inv = inventory_create();

    int running = 1;
    Uint32 last_tick = SDL_GetTicks();
    float speed = 120.0f; // px/s

    while(running) {
        Uint32 now = SDL_GetTicks();
        float dt = (now - last_tick) / 1000.0f;
        if(dt > 0.05f) dt = 0.05f;
        last_tick = now;

        SDL_Event ev;
        const Uint8* keys = SDL_GetKeyboardState(NULL);
        while(SDL_PollEvent(&ev)){
            if(ev.type == SDL_QUIT) running = 0;
            if(ev.type == SDL_KEYDOWN){
                if(ev.key.keysym.sym == SDLK_ESCAPE) running = 0;
                if(ev.key.keysym.sym == SDLK_e) {
                    // interact with nearest NPC or chest in range
                    Entity* target = find_entity_near(g, g->player, 48.0f, (1<<ENT_NPC) | (1<<ENT_CHEST));
                    if(target){
                        if(target->type == ENT_NPC){
                            NPC* n = (NPC*)target->comp;
                            if(n){
                                if(n->aggressive){
                                    printf("The %s growls angrily.\n", n->type);
                                } else {
                                    printf("NPC says: %s\n", n->talk_text ? n->talk_text : "(silence)");
                                    // show items
                                    if(n->inv && n->inv->count>0){
                                        printf("They have:\n");
                                        for(int i=0;i<n->inv->count;i++) printf(" - %s\n", n->inv->items[i]);
                                    }
                                }
                            }
                        } else if(target->type == ENT_CHEST){
                            Chest* c = (Chest*)target->comp;
                            if(c){
                                if(!c->opened){
                                    c->opened = 1;
                                    if(c->inv && c->inv->count>0){
                                        for(int i=0;i<c->inv->count;i++){
                                            char* it = c->inv->items[i];
                                            if(it) {
                                                printf("Picked up: %s\n", it);
                                                inventory_add(player_inv, it);
                                                free(it);
                                                c->inv->items[i] = NULL;
                                            }
                                        }
                                        c->inv->count = 0;
                                    } else {
                                        printf("Chest is empty.\n");
                                    }
                                } else {
                                    printf("Chest already opened.\n");
                                }
                            }
                        }
                    } else {
                        printf("Nothing nearby to interact with\n");
                    }
                }
                if(ev.key.keysym.sym == SDLK_SPACE) {
                    // attack nearest aggressive NPC within range
                    Entity* target = find_entity_near(g, g->player, 48.0f, (1<<ENT_NPC));
                    if(target){
                        NPC* n = (NPC*)target->comp;
                        if(n && n->aggressive){
                            n->hp -= 8;
                            printf("Hit %s, hp now %d\n", n->type, n->hp);
                            if(n->hp <= 0){
                                printf("%s dies\n", n->type);
                                // drop items to chest-like entity
                                if(n->inv && n->inv->count>0){
                                    Chest* chest = chest_create_with_item(NULL);
                                    // move items
                                    for(int i=0;i<n->inv->count;i++){
                                        if(n->inv->items[i]) inventory_add(chest->inv, n->inv->items[i]);
                                    }
                                    game_spawn_entity(g, ENT_CHEST, target->x, target->y, 28, 28, chest);
                                }
                                // remove NPC component
                                if(n->talk_text) free(n->talk_text);
                                if(n->inv){
                                    for(int j=0;j<n->inv->count;j++) if(n->inv->items[j]) free(n->inv->items[j]);
                                    free(n->inv);
                                }
                                free(n);
                                target->comp = NULL;
                                target->alive = 0;
                            }
                        } else {
                            printf("That one won't fight back\n");
                        }
                    }
                }
                if(ev.key.keysym.sym == SDLK_i) {
                    // list inventory
                    printf("Inventory (%d):\n", player_inv->count);
                    for(int i=0;i<player_inv->count;i++) printf(" - %s\n", player_inv->items[i]);
                }
            }
        }

        // movement
        float dx = 0, dy = 0;
        if(keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) dy -= 1;
        if(keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]) dy += 1;
        if(keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]) dx -= 1;
        if(keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) dx += 1;
        if(dx != 0 && dy != 0) { dx *= 0.70710678f; dy *= 0.70710678f; }
        g->player->x += dx * speed * dt;
        g->player->y += dy * speed * dt;

        // simple bounds
        if(g->player->x < 2) g->player->x = 2;
        if(g->player->y < 2) g->player->y = 2;
        if(g->player->x + g->player->w > g->level_w * TILE_W - 2) g->player->x = g->level_w*TILE_W - g->player->w - 2;
        if(g->player->y + g->player->h > g->level_h * TILE_H - 2) g->player->y = g->level_h*TILE_H - g->player->h - 2;

        // simple NPC AI: aggressive move toward player if in range
        for(int i=0;i<MAX_ENTITIES;i++){
            Entity* e = &g->entities[i];
            if(!e->alive) continue;
            if(e->type == ENT_NPC){
                NPC* n = (NPC*)e->comp;
                if(!n) continue;
                if(n->aggressive){
                    float px = g->player->x + g->player->w/2;
                    float py = g->player->y + g->player->h/2;
                    float ex = e->x + e->w/2;
                    float ey = e->y + e->h/2;
                    float vx = px - ex;
                    float vy = py - ey;
                    float dist2 = vx*vx + vy*vy;
                    if(dist2 < 300*300){
                        float mag = sqrtf(dist2);
                        if(mag > 1.0f) {
                            vx /= mag; vy /= mag;
                            e->x += vx * 50.0f * dt;
                            e->y += vy * 50.0f * dt;
                        }
                        // if close, deal damage
                        if(mag < 20.0f){
                            // damage player? we'll just print
                            // in full game you'd reduce player hp
                            // (left simple here)
                            //printf("%s hits you!\n", n->type);
                        }
                    } else {
                        // wander a bit (naive)
                        e->x += (sin(now/1000.0f + e->id) * 0.5f) * dt * 8.0f;
                    }
                } else {
                    // non-aggressive might bob or face player - omitted
                }
            }
        }

        // render
        render_frame(g);

        SDL_Delay(1);
    }

    // cleanup player inventory
    for(int i=0;i<player_inv->count;i++) if(player_inv->items[i]) free(player_inv->items[i]);
    free(player_inv);

    game_destroy(g);
    return 0;
}
