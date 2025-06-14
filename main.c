#include "SDL_pixels.h"
#include "SDL_surface.h"
#include <SDL.h>
#include <SDL_image.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEBUG 1

#define SCREEN_WIDTH 1200
#define SCREEN_HEIGHT 800
#define STRIDE SCREEN_WIDTH * 4

#define CAMERA_RADIUS 0.1f
#define FOV_FACTOR 0.66f

#define WALL_DIM_FACTOR 0xC0u
#define SKY_COLOR 0xFF202020u
#define GROUND_COLOR 0xFF505050u
#define FOG_FACTOR 0.03f

#define MAP_FILE "map.txt"
#define MAP_MAX_STEPS 1024

#define TILE_MANIFEST "tiles.txt"
#define TILE_BASE_SIZE 128
#define MAX_TILE_ID 0xFF

#define CEILING_TILE_ID 0x41

static const float MOVE_SPEED_SEC = 5.0f;
static const float ROT_SPEED_SEC = 5.0f;

static inline int sgnf(float x) { return (x > 0) - (x < 0); }
static inline int maxi(int a, int b) { return a > b ? a : b; }
static inline int mini(int a, int b) { return a < b ? a : b; }
static inline float clampf(float v, float lo, float hi) {
  return (v < lo) ? lo : (v > hi ? hi : v);
}
static inline float inv_abs(float v) { return 1.0f / (fabsf(v) + 1e-20f); }

typedef enum {
  TILE_TYPE_EMPTY,
  TILE_TYPE_FLOOR,
  TILE_TYPE_WALL,
  TILE_TYPE_DOOR,
  TILE_TYPE_DECOR,
} TileType;

typedef struct {
  unsigned id;
  int width;
  int height;
  uint32_t *pixels;
  TileType type;
} Tile;
static Tile **tile_registry = NULL;
static size_t tile_count = 0;
static Tile *id_lut[MAX_TILE_ID + 1] = {NULL}; // Ensures O(1) access

void load_tiles(const char *manifest_path) {
  FILE *f = fopen(manifest_path, "r");
  if (!f) {
    fprintf(stderr, "Failed to open tile manifest: %s\n", manifest_path);
    return;
  }

  unsigned id;
  char filename[256], typestr[16];
  while (1) {
    int r = fscanf(f, "%x %255s %15s", &id, filename, typestr);
    if (r != 3)
      break;
    tile_count++;
  }

  tile_registry = calloc(tile_count, sizeof(Tile *));
  if (!tile_registry) {
    fprintf(stderr, "Memory allocation failed for tile registry\n");
    fclose(f);
    exit(EXIT_FAILURE);
  }

  rewind(f);

  size_t idx = 0;
  while (fscanf(f, "%x %255s %15s", &id, filename, typestr) == 3) {
    SDL_Surface *s = IMG_Load(filename);
    if (!s) {
      fprintf(stderr, "IMG_Load(%s): %s\n", filename, IMG_GetError());
      SDL_FreeSurface(s);
      exit(EXIT_FAILURE);
    }

    if (s->format->format != SDL_PIXELFORMAT_ARGB8888) {
      SDL_Surface *conv =
          SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ARGB8888, 0);
      SDL_FreeSurface(s);
      s = conv;
    }

    Tile *t = malloc(sizeof(Tile));
    if (!t) {
      fprintf(stderr, "Memory allocation failed for tile %u\n", id);
      SDL_FreeSurface(s);
      exit(EXIT_FAILURE);
    }

    t->id = id;
    t->width = s->w;
    t->height = s->h;
    t->pixels = malloc(t->width * t->height * sizeof(uint32_t));
    if (!t->pixels) {
      fprintf(stderr, "Memory allocation failed for tile pixels %u\n", id);
      free(t);
      SDL_FreeSurface(s);
      exit(EXIT_FAILURE);
    }
    t->type = (strcmp(typestr, "floor") == 0)  ? TILE_TYPE_FLOOR
              : (strcmp(typestr, "wall") == 0) ? TILE_TYPE_WALL
              : (strcmp(typestr, "door") == 0) ? TILE_TYPE_DOOR
                                               : TILE_TYPE_DECOR;

    memcpy(t->pixels, s->pixels, t->width * t->height * sizeof(uint32_t));
    SDL_FreeSurface(s);

    tile_registry[idx++] = t;

    if (id < MAX_TILE_ID)
      id_lut[id] = t;
  }

#ifdef DEBUG
  if (idx != tile_count) {
    fprintf(stderr, "Warning: Expected %zu tiles, but loaded %zu\n", tile_count,
            idx);
  }
#endif

  fclose(f);
}

static inline Tile *get_tile_by_id(unsigned id) {
  return (id <= MAX_TILE_ID) ? id_lut[id] : NULL;
}

void free_tile_registry() {
  for (size_t i = 0; i < tile_count; i++) {
    free(tile_registry[i]->pixels);
    free(tile_registry[i]);
  }
  free(tile_registry);
  tile_registry = NULL;
  tile_count = 0;
}

struct Map {
  size_t width;
  size_t height;
  Tile **tiles;
};

static struct Map load_map(const char *filename) {
  struct Map map = {0, 0, NULL};

  FILE *file = fopen(filename, "r");
  if (!file) {
    fprintf(stderr, "Failed to open map file: %s\n", filename);
    return map;
  }

  if (fscanf(file, "%zu %zu", &map.width, &map.height) != 2 || map.width <= 0 ||
      map.height <= 0) {
    fprintf(stderr, "Invalid map dimensions in file: %s\n", filename);
    fclose(file);
    return map;
  }

  map.tiles = calloc(map.width * map.height, sizeof *map.tiles);
  if (!map.tiles) {
    fclose(file);
    map.width = map.height = 0;
    return map;
  }

  for (size_t y = 0; y < map.height; y++) {
    for (size_t x = 0; x < map.width; x++) {
      unsigned hex;
      if (fscanf(file, "%x", &hex) != 1) {
        fprintf(stderr, "Premature end of map data at (%zu, %zu)\n", x, y);
        free(map.tiles);
        map.tiles = NULL;
        map.width = map.height = 0;
        fclose(file);
        return map;
      }
      map.tiles[y * map.width + x] = get_tile_by_id(hex);
    }
  }

  fclose(file);

#ifdef DEBUG
  if (map.tiles == NULL)
    fprintf(stderr, "Failed to load map tiles from file: %s\n", filename);
#endif

  return map;
}

static inline Tile *get_tile(const struct Map *m, int x, int y) {
  if ((unsigned)x >= m->width || (unsigned)y >= m->height)
    return NULL;
  return m->tiles[y * m->width + x];
}

static inline int is_solid(const struct Map *map, int x, int y) {
  Tile *t = get_tile(map, x, y);
  return !t || t->type == TILE_TYPE_WALL;
}

typedef struct {
  float pos_x, pos_y;
  float dir_x, dir_y;
  float plane_x, plane_y;
} Camera;

static void rotate_camera(Camera *camera, float rad) {
  float cos_rad = cosf(rad);
  float sin_rad = sinf(rad);

  float new_dir_x = camera->dir_x * cos_rad - camera->dir_y * sin_rad;
  float new_dir_y = camera->dir_x * sin_rad + camera->dir_y * cos_rad;
  float new_plane_x = camera->plane_x * cos_rad - camera->plane_y * sin_rad;
  float new_plane_y = camera->plane_x * sin_rad + camera->plane_y * cos_rad;

  camera->dir_x = new_dir_x;
  camera->dir_y = new_dir_y;
  camera->plane_x = new_plane_x;
  camera->plane_y = new_plane_y;
}

#define MAX_STEP (CAMERA_RADIUS * 0.5f)

static void move_camera(Camera *cam, const struct Map *map, float dir_x,
                        float dir_y, float speed) {
  float remaining = fabsf(speed);
  if (speed < 0.0f) {
    dir_x = -dir_x;
    dir_y = -dir_y;
  }

  // normalize direction vector
  float len = hypotf(dir_x, dir_y);
  if (len == 0.0f)
    return;
  dir_x /= len;
  dir_y /= len;

  // micro-step to avoid tunneling through walls
  int max_iter = 64;
  while (remaining > 1e-6f && max_iter--) {
    float step = (remaining > MAX_STEP) ? MAX_STEP : remaining;
    remaining -= step;

    float nx = cam->pos_x + dir_x * step;
    if (!is_solid(map, (int)floorf(nx + sgnf(dir_x) * CAMERA_RADIUS),
                  (int)floorf(cam->pos_y)))
      cam->pos_x = nx;

    float ny = cam->pos_y + dir_y * step;
    if (!is_solid(map, (int)floorf(cam->pos_x),
                  (int)floorf(ny + sgnf(dir_y) * CAMERA_RADIUS)))
      cam->pos_y = ny;
  }

#ifdef DEBUG
  if (is_solid(map, (int)cam->pos_x, (int)cam->pos_y))
    fprintf(stderr, "inside wall @ (%.2f, %.2f)\n", cam->pos_x, cam->pos_y);
#endif
}

static inline unsigned int dim_color(unsigned int color, unsigned int factor) {
  unsigned int br = ((color & 0xFF00FFu) * factor) >> 8;
  unsigned int g = ((color & 0x00FF00u) * factor) >> 8;
  return 0xFF000000 | (br & 0xFF00FFu) | (g & 0x00FF00u);
}

// static inline unsigned int distance_fog(unsigned int color, float dist,
//                                         float fog_factor) {
//   if (dist < 0.0f || fog_factor <= 0.0f)
//     return color;
//   float factor = 1.0f / (1.0f + dist * fog_factor);
//   unsigned int a = (color >> 24) & 0xFF;
//   unsigned int r = (color >> 16) & 0xFF;
//   unsigned int g = (color >> 8) & 0xFF;
//   unsigned int b = color & 0xFF;
//   r = (unsigned int)(r * factor);
//   g = (unsigned int)(g * factor);
//   b = (unsigned int)(b * factor);
//   return (a << 24) | (r << 16) | (g << 8) | b;
// }

static void clear_pixels(uint8_t *pixels) {
  memset(pixels, 0, SCREEN_HEIGHT * STRIDE);
}

static float *generate_camera_lut() {
  float *lut = (float *)malloc(SCREEN_WIDTH * sizeof(float));
  if (!lut)
    return NULL;

  for (int i = 0; i < SCREEN_WIDTH; i++)
    lut[i] = (2.0f * i / SCREEN_WIDTH) - 1.0f;

  return lut;
}

static void vertical_line(uint8_t *pixels, int x, int y0, int y1,
                          unsigned int color) {
  unsigned int a = (color >> 24) & 0xFF;
  unsigned int r = (color >> 16) & 0xFF;
  unsigned int g = (color >> 8) & 0xFF;
  unsigned int b = color & 0xFF;

  y0 = maxi(0, y0);
  y1 = mini(SCREEN_HEIGHT - 1, y1);

  unsigned int offset = (y0 * SCREEN_WIDTH + x) * 4;

  uint8_t *row = pixels + (size_t)y0 * STRIDE + x * 4;
  for (int y = y0; y <= y1; y++) {
    row[0] = b;
    row[1] = g;
    row[2] = r;
    row[3] = a;
    row += STRIDE;
  }
}

static void render_raycast(float *camera_lut, uint8_t *pixels, Camera *camera,
                           const struct Map *map) {

  // FLOOR & CEILING RENDERING
  for (int y = SCREEN_HEIGHT / 2; y < SCREEN_HEIGHT; y++) {
    float p = (float)(y - SCREEN_HEIGHT / 2.0f);
    float camera_z = 0.5f * SCREEN_HEIGHT;
    float row_dist = camera_z / p;

    float ray_dir_x0 = camera->dir_x - camera->plane_x;
    float ray_dir_y0 = camera->dir_y - camera->plane_y;
    float ray_dir_x1 = camera->dir_x + camera->plane_x;
    float ray_dir_y1 = camera->dir_y + camera->plane_y;

    float step_x = row_dist * (ray_dir_x1 - ray_dir_x0) / SCREEN_WIDTH;
    float step_y = row_dist * (ray_dir_y1 - ray_dir_y0) / SCREEN_WIDTH;

    float floor_x = camera->pos_x + ray_dir_x0 * row_dist;
    float floor_y = camera->pos_y + ray_dir_y0 * row_dist;

    for (int x = 0; x < SCREEN_WIDTH; x++) {
      int map_x = (int)floorf(floor_x);
      int map_y = (int)floorf(floor_y);

      floor_x += step_x;
      floor_y += step_y;

      Tile *floor_tile = get_tile(map, map_x, map_y);
      if (!floor_tile || floor_tile->type != TILE_TYPE_FLOOR) {
        continue;
      }

      int tex_x = ((int)((floor_x - map_x) * floor_tile->width) &
                   (floor_tile->width - 1));
      int tex_y = ((int)((floor_y - map_y) * floor_tile->height) &
                   (floor_tile->height - 1));

      uint32_t floor_color =
          floor_tile->pixels[tex_y * floor_tile->width + tex_x];
      uint8_t *fp = pixels + (size_t)y * STRIDE + x * 4;
      fp[0] = floor_color & 0xFF;         // B
      fp[1] = (floor_color >> 8) & 0xFF;  // G
      fp[2] = (floor_color >> 16) & 0xFF; // R
      fp[3] = 0xFF;                       // A

      Tile *ceil_tile = get_tile_by_id(CEILING_TILE_ID);
      uint32_t ceil_color = ceil_tile->pixels[tex_y * ceil_tile->width + tex_x];
      ceil_color = (ceil_color >> 1) & 8355711; // make a bit darker
      uint8_t *cp = pixels + (size_t)(SCREEN_HEIGHT - y - 1) * STRIDE + x * 4;
      cp[0] = ceil_color & 0xFF;         // B
      cp[1] = (ceil_color >> 8) & 0xFF;  // G
      cp[2] = (ceil_color >> 16) & 0xFF; // R
      cp[3] = 0xFF;                      // A
    }
  }

  // WALL RENDERING
  for (int x = 0; x < SCREEN_WIDTH; x++) {
    float camera_x = camera_lut[x];

    float ray_dir_x = camera->dir_x + camera->plane_x * camera_x;
    float ray_dir_y = camera->dir_y + camera->plane_y * camera_x;

    float ray_pos_x = camera->pos_x;
    float ray_pos_y = camera->pos_y;

    int map_x = (int)ray_pos_x;
    int map_y = (int)ray_pos_y;

    // length of ray from one x or y-side to next x or y-side
    float delta_dist_x = inv_abs(ray_dir_x);
    float delta_dist_y = inv_abs(ray_dir_y);

    int step_x = sgnf(ray_dir_x);
    int step_y = sgnf(ray_dir_y);

    // length of ray from current position to next x or y-side
    float side_dist_x = (ray_dir_x < 0)
                            ? (ray_pos_x - map_x) * delta_dist_x
                            : (map_x + 1.0f - ray_pos_x) * delta_dist_x;
    float side_dist_y = (ray_dir_y < 0)
                            ? (ray_pos_y - map_y) * delta_dist_y
                            : (map_y + 1.0f - ray_pos_y) * delta_dist_y;

    Tile *hit_tile = NULL;
    int hit_side = 0;
    for (int steps = 0; steps < MAP_MAX_STEPS; steps++) {
      if (side_dist_x < side_dist_y) {
        side_dist_x += delta_dist_x;
        map_x += step_x;
        hit_side = 0;
      } else {
        side_dist_y += delta_dist_y;
        map_y += step_y;
        hit_side = 1;
      }

      hit_tile = get_tile(map, map_x, map_y);
      if (hit_tile && hit_tile->type == TILE_TYPE_WALL) {
        break;
      }

      // continue searching
      hit_tile = NULL;
    }

    if (!hit_tile) {
      vertical_line(pixels, x, 0, SCREEN_HEIGHT - 1, SKY_COLOR);
      continue;
    }

    // calculate the perpendicular distance to the wall
    float perp_wall_dist = (hit_side == 0) ? side_dist_x - delta_dist_x
                                           : side_dist_y - delta_dist_y;
    if (perp_wall_dist < 1e-6f)
      perp_wall_dist = 1e-6f;

    float wall_x = (hit_side == 0) ? (ray_pos_y + perp_wall_dist * ray_dir_y)
                                   : (ray_pos_x + perp_wall_dist * ray_dir_x);
    wall_x -= floorf(wall_x);

    int tex_x = (int)(wall_x * hit_tile->width) & (hit_tile->width - 1);
    if ((hit_side == 0 && ray_dir_x > 0) || (hit_side == 1 && ray_dir_y < 0)) {
      tex_x = hit_tile->width - 1 - tex_x;
    }

    int line_height = (int)(SCREEN_HEIGHT / perp_wall_dist);
    int draw_start = maxi(0, (SCREEN_HEIGHT - line_height) / 2);
    int draw_end = mini(SCREEN_HEIGHT - 1, (SCREEN_HEIGHT + line_height) / 2);

    for (int y = draw_start; y <= draw_end; y++) {
      int d = y * 256 - SCREEN_HEIGHT * 128 + line_height * 128;
      int tex_y = ((d * hit_tile->height) / line_height) / 256;

      uint32_t color = hit_tile->pixels[tex_y * hit_tile->width + tex_x];
      if (hit_side)
        color = dim_color(color, WALL_DIM_FACTOR);

      uint8_t *p = pixels + (size_t)y * STRIDE + x * 4;
      p[0] = color & 0xFF;         // B
      p[1] = (color >> 8) & 0xFF;  // G
      p[2] = (color >> 16) & 0xFF; // R
      p[3] = 0xFF;                 // A
    }
  }
}

int main(int argc, char *argv[]) {

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "SDL_Init Error: %s\n", SDL_GetError());
    return 1;
  }

  SDL_Window *window = SDL_CreateWindow(
      "Test", SDL_WINDOWPOS_CENTERED_DISPLAY(0),
      SDL_WINDOWPOS_CENTERED_DISPLAY(0), SCREEN_WIDTH, SCREEN_HEIGHT, 0);
  if (!window) {
    fprintf(stderr, "SDL_CreateWindow Error: %s\n", SDL_GetError());
    SDL_Quit();
    return EXIT_FAILURE;
  }

  SDL_Renderer *renderer = SDL_CreateRenderer(
      window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer) {
    fprintf(stderr, "SDL_CreateRenderer Error: %s\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return EXIT_FAILURE;
  }

  SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                           SDL_TEXTUREACCESS_STREAMING,
                                           SCREEN_WIDTH, SCREEN_HEIGHT);
  if (!texture) {
    fprintf(stderr, "SDL_CreateTexture Error: %s\n", SDL_GetError());
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return EXIT_FAILURE;
  }

  uint8_t *pixels = malloc(SCREEN_WIDTH * SCREEN_HEIGHT * 4);
  if (!pixels) {
    fprintf(stderr, "Memory allocation failed for pixels\n");
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return EXIT_FAILURE;
  }

  float *camera_lut = generate_camera_lut();
  if (!camera_lut) {
    fprintf(stderr, "Memory allocation failed for camera LUT\n");
    free(pixels);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return EXIT_FAILURE;
  }

  load_tiles(TILE_MANIFEST);
  if (!tile_registry || tile_count == 0) {
    fprintf(stderr, "Failed to load tiles from manifest: %s\n", TILE_MANIFEST);
    free(camera_lut);
    free(pixels);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return EXIT_FAILURE;
  }

  struct Map map = load_map(MAP_FILE);
  if (!map.tiles) {
    fprintf(stderr, "Memory allocation failed for map tiles\n");
    free(tile_registry);
    free(camera_lut);
    free(pixels);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return EXIT_FAILURE;
  }

  Camera camera = {
      .pos_x = 2.0f,
      .pos_y = 2.0f,
      .dir_x = -1.0f,
      .dir_y = 0.0f,
      .plane_x = 0.0f,
      .plane_y = FOV_FACTOR,
  };

  float len = sqrtf(camera.dir_x * camera.dir_x + camera.dir_y * camera.dir_y);
  if (len > 0.f) {
    camera.dir_x /= len;
    camera.dir_y /= len;
  }

  Uint64 freq = SDL_GetPerformanceFrequency();
  Uint64 last = SDL_GetPerformanceCounter();

  int running = 1;
  SDL_Event e;

  while (running) {
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) {
        running = 0;
      }
    }

    Uint64 now = SDL_GetPerformanceCounter();
    float dt = (float)(now - last) / (float)freq;
    last = now;
    float move_speed = MOVE_SPEED_SEC * dt;
    float rot_speed = ROT_SPEED_SEC * dt;

    const Uint8 *kb = SDL_GetKeyboardState(NULL);
    if (kb[SDL_SCANCODE_ESCAPE])
      break;
    if (kb[SDL_SCANCODE_LEFT])
      rotate_camera(&camera, rot_speed);
    if (kb[SDL_SCANCODE_RIGHT])
      rotate_camera(&camera, -rot_speed);
    if (kb[SDL_SCANCODE_W])
      move_camera(&camera, &map, camera.dir_x, camera.dir_y, move_speed);
    if (kb[SDL_SCANCODE_S])
      move_camera(&camera, &map, camera.dir_x, camera.dir_y, -move_speed);
    if (kb[SDL_SCANCODE_A])
      move_camera(&camera, &map, camera.dir_y, -camera.dir_x, -move_speed);
    if (kb[SDL_SCANCODE_D])
      move_camera(&camera, &map, camera.dir_y, -camera.dir_x, move_speed);

    clear_pixels(pixels);

    render_raycast(camera_lut, pixels, &camera, &map);

    SDL_UpdateTexture(texture, NULL, pixels, STRIDE);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
  }

  free(map.tiles);
  free_tile_registry();
  free(camera_lut);
  free(pixels);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyTexture(texture);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
