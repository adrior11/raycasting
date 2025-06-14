#include "SDL_pixels.h"
#include "SDL_surface.h"
#include <SDL.h>
#include <SDL_image.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_WIDTH 800
#define SCREEN_HEIGHT 600
#define STRIDE SCREEN_WIDTH * 4
#define CAMERA_RADIUS 0.1f
#define FOV_FACTOR 0.66f
#define MAP_FILE "map.txt"
#define MAP_MAX_STEPS 1024
#define WALL_DIM_FACTOR 0xC0u
#define SKY_COLOR 0xFF202020u
#define GROUND_COLOR 0xFF505050u
#define FOG_FACTOR 0.03f

#define TEX_W 16
#define TEX_H 16
#define TEX_COUNT 4
#define ATLAS_W (TEX_W * TEX_COUNT)
#define ATLAS_H TEX_H

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
  TILE_EMPTY = 0,
  TILE_BLUE,
  TILE_GREEN,
  TILE_RED,
  TILE_MAGENTA,
  TILE_MAX
} TileType;

struct Map {
  size_t width;
  size_t height;
  int *tiles;
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
      int t;
      if (fscanf(file, "%d", &t) != 1) {
        fprintf(stderr, "Premature end of map data at (%zu, %zu)\n", x, y);
        free(map.tiles);
        map.tiles = NULL;
        map.width = map.height = 0;
        fclose(file);
        return map;
      }
      map.tiles[y * map.width + x] = t;
    }
  }

  fclose(file);
  return map;
}

static inline int get_tile(const struct Map *map, int x, int y) {
  if (!map->tiles)
    return -1;
  if (x < 0 || y < 0)
    return -1;
  if ((size_t)x >= map->width || (size_t)y >= map->height)
    return -1;
  return map->tiles[y * map->width + x];
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

static void move_camera(Camera *camera, struct Map *map, float move_dir_x,
                        float move_dir_y, float move_speed) {
  float new_x = camera->pos_x + move_dir_x * move_speed;
  float new_y = camera->pos_y + move_dir_y * move_speed;

  int min_tx = (int)fmaxf(floorf(new_x - 1.0f), 0.0f);
  int max_tx = (int)fminf(ceilf(new_x + 1.0f), (float)map->width - 1.0f);
  int min_ty = (int)fmaxf(floorf(new_y - 1.0f), 0.0f);
  int max_ty = (int)fminf(ceilf(new_y + 1.0f), (float)map->height - 1.0f);

  for (int ty = min_ty; ty <= max_ty; ty++) {
    for (int tx = min_tx; tx <= max_tx; tx++) {
      if (get_tile(map, tx, ty) == TILE_EMPTY)
        continue;

      const float tile_l = (float)tx;
      const float tile_r = tile_l + 1.0f;
      const float tile_t = (float)ty;
      const float tile_b = tile_t + 1.0f;

      const float closest_x = clampf(new_x, tile_l, tile_r);
      const float closest_y = clampf(new_y, tile_t, tile_b);

      float dx = new_x - closest_x;
      float dy = new_y - closest_y;
      float dist_sq = dx * dx + dy * dy;

      if (dist_sq < CAMERA_RADIUS * CAMERA_RADIUS) {
        float dist = sqrtf(dist_sq);
        if (dist > 0.0f) {
          float penetration = CAMERA_RADIUS - dist;
          new_x += dx / dist * penetration;
          new_y += dy / dist * penetration;
        }
      }
    }
  }

  camera->pos_x = new_x;
  camera->pos_y = new_y;
}

static uint32_t *atlas_pixels = NULL;
static void load_atlas(const char *png) {
  IMG_Init(IMG_INIT_PNG);

  SDL_Surface *s = IMG_Load(png);
  if (!s) {
    fprintf(stderr, "Failed to load texture atlas: %s\n", IMG_GetError());
    exit(EXIT_FAILURE);
  }

  if (s->format->format != SDL_PIXELFORMAT_ARGB8888) {
    SDL_Surface *conv =
        SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(s);
    s = conv;
  }

  atlas_pixels = malloc(ATLAS_W * ATLAS_H * sizeof(uint32_t));
  memcpy(atlas_pixels, s->pixels, ATLAS_W * ATLAS_H * sizeof(uint32_t));
  SDL_FreeSurface(s);
}

static char *get_texture_path(const char *name) {
  static char path[256];
  snprintf(path, sizeof(path), "textures/%s.png", name);
  return path;
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
  for (int x = 0; x < SCREEN_WIDTH; x++) {
    float camera_x = camera_lut[x];

    float ray_dir_x = camera->dir_x + camera->plane_x * camera_x;
    float ray_dir_y = camera->dir_y + camera->plane_y * camera_x;

    float ray_pos_x = camera->pos_x;
    float ray_pos_y = camera->pos_y;

    int ipos_x = (int)ray_pos_x;
    int ipos_y = (int)ray_pos_y;

    float delta_dist_x = inv_abs(ray_dir_x);
    float delta_dist_y = inv_abs(ray_dir_y);

    int step_x = sgnf(ray_dir_x);
    int step_y = sgnf(ray_dir_y);

    float side_dist_x = (ray_dir_x < 0)
                            ? (ray_pos_x - ipos_x) * delta_dist_x
                            : (ipos_x + 1.0f - ray_pos_x) * delta_dist_x;
    float side_dist_y = (ray_dir_y < 0)
                            ? (ray_pos_y - ipos_y) * delta_dist_y
                            : (ipos_y + 1.0f - ray_pos_y) * delta_dist_y;

    int hit_val = TILE_EMPTY;
    int hit_side = 0;
    int steps = 0;

    while (hit_val == TILE_EMPTY && steps < MAP_MAX_STEPS) {
      if (side_dist_x < side_dist_y) {
        side_dist_x += delta_dist_x;
        ipos_x += step_x;
        hit_side = 0;
      } else {
        side_dist_y += delta_dist_y;
        ipos_y += step_y;
        hit_side = 1;
      }

      hit_val = get_tile(map, ipos_x, ipos_y);
      if (hit_val == -1)
        break;

      steps++;
    }

    if (hit_val < 0 || hit_val >= TILE_MAX)
      hit_val = TILE_EMPTY;

    float perp_wall_dist = (hit_side == 0) ? side_dist_x - delta_dist_x
                                           : side_dist_y - delta_dist_y;
    if (perp_wall_dist < 1e-6f)
      perp_wall_dist = 1e-6f;

    int tex_id = maxi(0, hit_val - 1);

    if (tex_id >= TEX_COUNT) {
      fprintf(stderr, "Invalid texture ID: %d\n", tex_id);
      tex_id = 0;
    }

    float wall_x = (hit_side == 0) ? (ray_pos_y + perp_wall_dist * ray_dir_y)
                                   : (ray_pos_x + perp_wall_dist * ray_dir_x);
    wall_x -= floorf(wall_x);

    int tex_x = (int)(wall_x * TEX_W);
    if ((hit_side == 0 && ray_dir_x > 0) || (hit_side == 1 && ray_dir_y < 0)) {
      tex_x = TEX_W - tex_x - 1;
    }

    // Horizontal offset of this tile inside the 64-pixel atlas row
    int atlas_x0 = tex_id * TEX_W;

    int line_height = (int)(SCREEN_HEIGHT / perp_wall_dist);
    int draw_start = maxi(0, (SCREEN_HEIGHT - line_height) / 2);
    int draw_end = mini(SCREEN_HEIGHT - 1, (SCREEN_HEIGHT + line_height) / 2);

    for (int y = draw_start; y <= draw_end; y++) {
      int d = y * 256 - SCREEN_HEIGHT * 128 + line_height * 128;
      int tex_y = ((d * TEX_H) / line_height) / 256; // 0..15

      uint32_t c = atlas_pixels[(tex_y * ATLAS_W) + atlas_x0 + tex_x];

      if (hit_side)
        c = dim_color(c, WALL_DIM_FACTOR);

      uint8_t *p = pixels + (size_t)y * STRIDE + x * 4;
      p[0] = c & 0xFF;         // B
      p[1] = (c >> 8) & 0xFF;  // G
      p[2] = (c >> 16) & 0xFF; // R
      p[3] = 0xFF;             // A
    }

    vertical_line(pixels, x, 0, draw_start, SKY_COLOR);
    vertical_line(pixels, x, draw_end, SCREEN_HEIGHT, GROUND_COLOR);
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

  struct Map map = load_map(MAP_FILE);
  if (!map.tiles) {
    fprintf(stderr, "Memory allocation failed for map tiles\n");
    free(camera_lut);
    free(pixels);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return EXIT_FAILURE;
  }

  char *atlas_path = get_texture_path("atlas");
  load_atlas(atlas_path);

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

  free(atlas_pixels);
  free(map.tiles);
  free(camera_lut);
  free(pixels);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyTexture(texture);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
