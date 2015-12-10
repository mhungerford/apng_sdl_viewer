#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <SDL/SDL.h>

#include <upng.h>

// Non-public but extremely useful stretch
// Both buffers must be same color depth
extern int SDL_SoftStretch(SDL_Surface *src, SDL_Rect *srcrect,
    SDL_Surface *dst, SDL_Rect *dstrect);

#define SCALE_WINDOW 2

#define FRAMEBUFFER_WIDTH 144
#define FRAMEBUFFER_HEIGHT 168

SDL_Surface *sdl_surface; // 32-bit sdl window surface
SDL_Surface *cpy_surface; // 32-bit surface for converting/scaling
SDL_Surface *img_surface; // 8-bit surface

// RGBA framebuffer 32-bit
uint32_t screenbuffer[FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT];

void sdl_setup(void) {
  SDL_Init(SDL_INIT_VIDEO);// | SDL_INIT_EVENTTHREAD);

#ifdef __APPLE__
  //Has to be hard-coded otherwise ABGR with 3-channel colors
  //with software buffer misses the RED channel (white is yellow)
  int sdl_bpp = 24; 
#else
  int sdl_bpp = SDL_GetVideoInfo()->vfmt->BitsPerPixel;
#endif

  sdl_surface = SDL_SetVideoMode(
    FRAMEBUFFER_WIDTH * SCALE_WINDOW, 
    FRAMEBUFFER_HEIGHT * SCALE_WINDOW, 
    sdl_bpp, //can't use 0 (autodetect) as next surface must match 
    SDL_HWSURFACE | SDL_DOUBLEBUF);
  if (!sdl_surface) {
    printf("SDL_SetVideoMode failed: %s\n", SDL_GetError());
    exit(EXIT_FAILURE);
  }
  SDL_ShowCursor( 0 );

  //Used to convert 8-bit buffer to 32-bit buffer
  cpy_surface = SDL_CreateRGBSurface( 0,
    FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT, 
    sdl_bpp, //depth
    0, 0, 0, 0); 

  img_surface = SDL_CreateRGBSurfaceFrom( screenbuffer,
    FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT, 
    32, //depth
    FRAMEBUFFER_WIDTH * 4, //row_stride in bytes
    0, 0, 0, 0); 

  if (!img_surface) {
    printf("SDL_CreateRGBSurface failed: %s\n", SDL_GetError());
    exit(EXIT_FAILURE);
  }
  /*
  // Default palette required for 4bit and 8bit color
  SDL_Color colors[256];
  // Set a 3,3,2 color cube
  for ( int r=0; r<8; ++r ) {
    for ( int g=0; g<8; ++g ) {
      for ( int b=0; b<4; ++b ) {
        int i = ((r<<5)|(g<<2)|b);
        colors[i].r = r<<5;
        colors[i].g = g<<5;
        colors[i].b = b<<6;
      }
    }
  }
  SDL_SetPalette(img_surface, SDL_LOGPAL|SDL_PHYSPAL, colors, 0, 256);
  */
}

void sdl_draw(void) {
  SDL_BlitSurface(img_surface, NULL, cpy_surface, NULL);
  SDL_SoftStretch(cpy_surface, 0, sdl_surface, 0);
  SDL_Flip(sdl_surface);
}

void sdl_event(void) {
  SDL_Event event;
  while(SDL_PollEvent(&event)) {
    switch (event.type) {
    case SDL_QUIT: {
      SDL_Quit();
      exit(EXIT_SUCCESS);
    }
    case SDL_KEYUP: {
      switch (event.key.keysym.sym) {
      case SDLK_q: {
        SDL_Quit();
        exit(EXIT_SUCCESS);
      }
      case SDLK_LEFT: {
        break;
      }
      case SDLK_RIGHT: {
        break;
      }
      default:
        break;
      }
      break;
    }
    default:
      break;
    }
  }
}

static void color_demo(uint8_t *buffer) {
  static uint8_t color = 0xc0;
  color = 0x3 | ((color >> 2) + 1) << 2;
  for (int y = 0; y < FRAMEBUFFER_HEIGHT; y++) {
    for (int x = 0; x < FRAMEBUFFER_WIDTH; x++) {
      buffer[y * FRAMEBUFFER_WIDTH + x] = color;
    }
  }
}

static size_t file_to_buffer(char *filename, char **output_buffer) {
  FILE *fd;
  fd = fopen(filename, "rb");
  fseek(fd, 0, SEEK_END);
  int filesize = ftell(fd);
  fseek(fd,  0, SEEK_SET);
  *output_buffer = (char*)malloc(filesize);
  size_t bytes_read = fread(*output_buffer, 1, filesize, fd);
  fclose(fd);
  return bytes_read;
}

int main(int argc, char* argv[]){
  sdl_setup();
 
  char *png_buffer = NULL;
  size_t png_buffer_size = 0;
  png_buffer_size = file_to_buffer("images/globe.png", &png_buffer);

  upng_t* upng = upng_new_from_bytes(png_buffer, png_buffer_size);

  upng_load(upng);

  uint32_t width = upng_get_width(upng);
  uint32_t height = upng_get_height(upng);

  //rgb palette
  rgb *palette = NULL;
  int palette_entries = upng_get_palette(upng, &palette);

  uint8_t *alpha_palette = NULL;
  int alpha_palette_entries = upng_get_alpha_palette(upng, &alpha_palette);

  typedef union rgba32 {
    uint32_t rgba;
    struct {
      unsigned char b;
      unsigned char g;
      unsigned char r;
      unsigned char a;
    };
  } rgba32;

  rgba32 *rgba_palette = (rgba32*)malloc(palette_entries * sizeof(rgba32));
  for (int i = 0; i < palette_entries; i++) {
    rgba_palette[i].r = palette[i].r;
    rgba_palette[i].g = palette[i].g;
    rgba_palette[i].b = palette[i].b;
    rgba_palette[i].a = (i < alpha_palette_entries) ? alpha_palette[i] : 0xFF;
  }

  upng_decode_image(upng); //decode the initial image

  const char* raw_buffer = upng_get_buffer(upng);
  for (int i = 0; i < width * height; i++) {
    uint8_t palette_index = ((uint8_t*)raw_buffer)[i];
    // Looks like ARGB on this
    screenbuffer[i] = rgba_palette[palette_index].rgba;
  }
  sdl_draw();

  while (1) {
    SDL_Delay(1000);
    sdl_event();
  }

  return 0;
}
