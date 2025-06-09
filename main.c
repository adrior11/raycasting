#include <SDL.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SGN(x) ((x) > 0 ? 1 : ((x) < 0 ? -1 : 0))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))

#define SCREEN_WIDTH 800
#define SCREEN_HEIGHT 600
#define STRIDE SCREEN_WIDTH * 4
#define CAMERA_RADIUS 0.1f
#define WALL_DIM_FACTOR 0xC0
#define MAP_FILE "map.txt"
#define MAP_MAX 1024

static const float BASE_MOVE_SPEED = 3.0f * 0.016;
static const float BASE_ROTATION_SPEED = 3.0f * 0.016;

static const unsigned int COLORS[] = {0x00000000, 0xFF0000FF, 0xFF00FF00,
                                      0xFFFF0000, 0xFFFF00FF};

struct Map {
  size_t width;
  size_t height;
  int *tiles;
};

typedef struct {
  float pos_x, pos_y;
  float dir_x, dir_y;
  float plane_x, plane_y;
} Camera;

static inline float inv_abs(float val) {
  if (fabsf(val) < 1e-20f) {
    return INFINITY;
  }
  return 1.0f / fabsf(val);
}

unsigned int dim_color(unsigned int color, unsigned int factor) {
  unsigned int br = ((color & 0xFF00FF) * factor) >> 8;
  unsigned int g = ((color & 0x00FF00) * factor) >> 8;
  return 0xFF000000 | (br & 0xFF00FF) | (g & 0x00FF00);
}

void clear_pixels(uint8_t *pixels) {
  memset(pixels, 0, SCREEN_WIDTH * SCREEN_HEIGHT * 4);
}

float *generate_camera_lut() {
  float *lut = (float *)malloc(SCREEN_WIDTH * sizeof(float));
  if (!lut) {
    return NULL; // Memory allocation failed
  }
  for (int i = 0; i < SCREEN_WIDTH; i++) {
    lut[i] = (2.0f * i / SCREEN_WIDTH) - 1.0;
  }
  return lut;
};

/**
 * Loads a map from a file.
 * The file format is expected to be:
 * <width> <height>
 * <tile values for each row (space-separated)>
 */
struct Map load_map(const char *filename) {
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
    return map; // Invalid dimensions
  }

  map.tiles = (int *)malloc(map.width * map.height * sizeof(int));
  if (!map.tiles) {
    fclose(file);
    return map; // Memory allocation failed
  }

  for (int y = 0; y < map.height; y++) {
    for (int x = 0; x < map.width; x++) {
      fscanf(file, "%d", &map.tiles[y * map.width + x]);
    }
  }

  fclose(file);
  return map;
}

int get_tile(struct Map map, int x, int y) {
  if (x < 0 || x >= map.width || y < 0 || y >= map.height) {
    return -1;
  }
  return map.tiles[y * map.width + x];
}

void rotate_camera(Camera *camera, float rad) {
  float cos_rad = cosf(rad);
  float sin_rad = sinf(rad);

  float new_dir_x = camera->dir_x * cos_rad - camera->dir_y * sin_rad;
  float new_dir_y = camera->dir_x * sin_rad + camera->dir_y * cos_rad;
  camera->dir_x = new_dir_x;
  camera->dir_y = new_dir_y;

  float new_plane_x = camera->plane_x * cos_rad - camera->plane_y * sin_rad;
  float new_plane_y = camera->plane_x * sin_rad + camera->plane_y * cos_rad;
  camera->plane_x = new_plane_x;
  camera->plane_y = new_plane_y;
}

void move_camera(Camera *camera, struct Map *map, float move_dir_x,
                 float move_dir_y, float move_speed) {
  float new_x = camera->pos_x + move_dir_x * move_speed;
  float new_y = camera->pos_y + move_dir_y * move_speed;

  // tiles that lie within 1 unit of the tentative center
  int min_tx = (int)fmaxf(floorf(new_x - 1.0f), 0.0f);
  int max_tx = (int)fminf(ceilf(new_x + 1.0f), (float)map->width - 1.0f);
  int min_ty = (int)fmaxf(floorf(new_y - 1.0f), 0.0f);
  int max_ty = (int)fminf(ceilf(new_y + 1.0f), (float)map->height - 1.0f);

  for (int ty = min_ty; ty <= max_ty; ty++) {
    for (int tx = min_tx; tx <= max_tx; tx++) {
      if (get_tile(*map, tx, ty) == 0)
        continue; // Empty tile – skip

      const float tile_l = (float)tx;
      const float tile_r = tile_l + 1.0f;
      const float tile_t = (float)ty;
      const float tile_b = tile_t + 1.0f;

      // closest point in the tile's AABB
      const float closest_x = CLAMP(new_x, tile_l, tile_r);
      const float closest_y = CLAMP(new_y, tile_t, tile_b);

      // vector from that point to circle centre
      float dx = new_x - closest_x;
      float dy = new_y - closest_y;
      float dist_sq = dx * dx + dy * dy;

      if (dist_sq < CAMERA_RADIUS * CAMERA_RADIUS) {
        float dist = sqrtf(dist_sq);
        if (dist > 0.0f) {
          // penetration depth and normal
          float penetration = CAMERA_RADIUS - dist;
          dx /= dist;
          dy /= dist;

          // Push the centre out of the wall
          new_x += dx * penetration;
          new_y += dy * penetration;
        }
      }
    }
  }

  camera->pos_x = new_x;
  camera->pos_y = new_y;
}

void vertical_line(uint8_t *pixels, int x, int y_start, int y_end,
                   unsigned int color) {
  // Decompose the color (0xAARRGGBB) into separate bytes
  unsigned int a = (color >> 24) & 0xFF;
  unsigned int r = (color >> 16) & 0xFF;
  unsigned int g = (color >> 8) & 0xFF;
  unsigned int b = color & 0xFF;

  y_start = MAX(0, y_start);
  y_end = MIN(SCREEN_HEIGHT - 1, y_end);

  // Convert pixel index to a byte offset in the buffer:
  // each pixel is represented by 4 bytes (ARGB)
  unsigned int offset = (y_start * SCREEN_WIDTH + x) * 4;

  for (int y = y_start; y <= y_end; y++) {
    pixels[offset] = r;
    pixels[offset + 1] = g;
    pixels[offset + 2] = b;
    pixels[offset + 3] = a;

    offset += STRIDE; // Move to the next pixel
  }
}

void render_raycast(float *camera_lut, uint8_t *restrict pixels, Camera camera,
                    struct Map map) {
  for (int x = 0; x < SCREEN_WIDTH; x++) {
    float camera_x = camera_lut[x];

    float ray_dir_x = camera.dir_x + camera.plane_x * camera_x;
    float ray_dir_y = camera.dir_y + camera.plane_y * camera_x;

    float ray_pos_x = camera.pos_x;
    float ray_pos_y = camera.pos_y;

    int ipos_x = (int)ray_pos_x;
    int ipos_y = (int)ray_pos_y;

    float delta_dist_x = inv_abs(ray_dir_x);
    float delta_dist_y = inv_abs(ray_dir_y);

    int step_x = SGN(ray_dir_x);
    int step_y = SGN(ray_dir_y);

    float side_dist_x = (ray_dir_x < 0)
                            ? (ray_pos_x - ipos_x) * delta_dist_x
                            : (ipos_x + 1.0f - ray_pos_x) * delta_dist_x;
    float side_dist_y = (ray_dir_y < 0)
                            ? (ray_pos_y - ipos_y) * delta_dist_y
                            : (ipos_y + 1.0f - ray_pos_y) * delta_dist_y;

    int hit_val = 0;
    int hit_side = 0;
    int steps = 0;

    // Perform DDA
    while (hit_val == 0) {
      if (steps >= MAP_MAX) {
        fprintf(stderr, "Raycast exceeded maximum steps at (%d, %d)\n", ipos_x,
                ipos_y);
        break;
      }

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
      if (hit_val == -1) {
        // TODO: error handling
        fprintf(stderr, "Raycast hit out of bounds at (%d, %d)\n", ipos_x,
                ipos_y);
      }

      steps++;
    }

    unsigned int color = COLORS[hit_val]; // NOTE: could fail

    if (hit_side == 1) {
      color = dim_color(color, WALL_DIM_FACTOR);
    }

    float perp_wall_dist = (hit_side == 0) ? side_dist_x - delta_dist_x
                                           : side_dist_y - delta_dist_y;
    int line_height = (int)(SCREEN_HEIGHT / perp_wall_dist);
    int draw_start = MAX(0, (SCREEN_HEIGHT / 2) - (line_height / 2));
    int draw_end =
        MIN(SCREEN_HEIGHT - 1, (SCREEN_HEIGHT / 2) + (line_height / 2));

    vertical_line(pixels, x, 0, draw_start, 0xFF202020);
    vertical_line(pixels, x, draw_end, SCREEN_HEIGHT, 0xFF505050);
    vertical_line(pixels, x, draw_start, draw_end, color);
  }
}

int main(int argc, char *argv[]) {
  SDL_Window *window = NULL;
  SDL_Renderer *renderer = NULL;
  SDL_Texture *texture = NULL;
  uint8_t *pixels = NULL;
  float *camera_lut = NULL;

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "SDL_Init Error: %s\n", SDL_GetError());
    return 1;
  }

  window = SDL_CreateWindow("Test", SDL_WINDOWPOS_CENTERED_DISPLAY(0),
                            SDL_WINDOWPOS_CENTERED_DISPLAY(0), SCREEN_WIDTH,
                            SCREEN_HEIGHT, 0);
  if (!window) {
    fprintf(stderr, "SDL_CreateWindow Error: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  renderer = SDL_CreateRenderer(
      window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer) {
    fprintf(stderr, "SDL_CreateRenderer Error: %s\n", SDL_GetError());
    goto failed_startup;
  }

  texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, SCREEN_WIDTH,
                              SCREEN_HEIGHT);
  if (!texture) {
    fprintf(stderr, "SDL_CreateTexture Error: %s\n", SDL_GetError());
    goto failed_startup;
  }

  pixels = malloc(SCREEN_WIDTH * SCREEN_HEIGHT * 4);
  if (!pixels) {
    fprintf(stderr, "Memory allocation failed for pixels\n");
    goto failed_startup;
  }

  camera_lut = generate_camera_lut();
  if (!camera_lut) {
    fprintf(stderr, "Memory allocation failed for camera LUT\n");
    goto failed_startup;
  }

  struct Map map = load_map(MAP_FILE);
  if (!map.tiles) {
    fprintf(stderr, "Memory allocation failed for map tiles\n");
    goto failed_startup;
  }

  float dir_x = -1.0f;
  float dir_y = 0.1f;
  float normalized_length = sqrtf(dir_x * dir_x + dir_y * dir_y);
  Camera camera = {
      .pos_x = 2.0f,
      .pos_y = 2.0f,
      .dir_x = dir_x / normalized_length,
      .dir_y = dir_y / normalized_length,
      .plane_x = 0.0f,
      .plane_y = 0.66f,
  };

  SDL_Event e;
  int running = 1;
  while (running) {
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) {
        running = 0;
        break;
      }
    }

    const Uint8 *kb = SDL_GetKeyboardState(NULL);

    if (kb[SDL_SCANCODE_ESCAPE]) {
      running = 0;
      continue;
    }

    if (kb[SDL_SCANCODE_LEFT])
      rotate_camera(&camera, BASE_ROTATION_SPEED);
    if (kb[SDL_SCANCODE_RIGHT])
      rotate_camera(&camera, -BASE_ROTATION_SPEED);
    if (kb[SDL_SCANCODE_W])
      move_camera(&camera, &map, camera.dir_x, camera.dir_y, BASE_MOVE_SPEED);
    if (kb[SDL_SCANCODE_S])
      move_camera(&camera, &map, camera.dir_x, camera.dir_y, -BASE_MOVE_SPEED);
    if (kb[SDL_SCANCODE_A])
      move_camera(&camera, &map, camera.dir_y, -camera.dir_x, -BASE_MOVE_SPEED);
    if (kb[SDL_SCANCODE_D])
      move_camera(&camera, &map, camera.dir_y, -camera.dir_x, BASE_MOVE_SPEED);

    clear_pixels(pixels);

    render_raycast(camera_lut, pixels, camera, map);

    SDL_UpdateTexture(texture, NULL, pixels, SCREEN_WIDTH * 4);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
  }

  SDL_DestroyRenderer(renderer);
  SDL_DestroyTexture(texture);
  SDL_DestroyWindow(window);
  SDL_Quit();

  free(pixels);
  free(camera_lut);
  free(map.tiles);

  return 0;

failed_startup:
  if (pixels)
    free(pixels);
  if (camera_lut)
    free(camera_lut);
  if (map.tiles)
    free(map.tiles);
  if (texture)
    SDL_DestroyTexture(texture);
  if (renderer)
    SDL_DestroyRenderer(renderer);
  if (window)
    SDL_DestroyWindow(window);
  SDL_Quit();
  return 1;
}
