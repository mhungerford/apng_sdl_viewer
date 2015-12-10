#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <SDL/SDL.h>

#include <upng.h>

#ifndef MIN
  #define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

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
}

void sdl_draw(void) {
  SDL_BlitSurface(img_surface, NULL, cpy_surface, NULL);
  SDL_SoftStretch(cpy_surface, 0, sdl_surface, 0);
  SDL_Flip(sdl_surface);

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
  png_buffer_size = file_to_buffer("images/sequence.png", &png_buffer);

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

  apng_dispose_ops last_dispose_op = APNG_DISPOSE_OP_BACKGROUND;
  uint32_t *buffer_previous = NULL;
  uint32_t previous_xoffset = 0;
  uint32_t previous_yoffset = 0;
  uint32_t previous_width = 0;
  uint32_t previous_height = 0;


  while (1) {
    upng_error error = upng_decode_image(upng); //decode the next image
    if (error != UPNG_EOK) {
      SDL_Delay(100);
      sdl_draw();
      continue;
    }

    apng_fctl fctl;
    upng_get_apng_fctl(upng, &fctl);
    raw_buffer = upng_get_buffer(upng);
    SDL_Delay((fctl.delay_num * 1000) / fctl.delay_den);

    printf("fctl seq:%d dispose:%s blend:%s\n", 
        fctl.sequence_number,
        (fctl.dispose_op == APNG_DISPOSE_OP_PREVIOUS) ? "PREVIOUS" : 
        ((fctl.dispose_op == APNG_DISPOSE_OP_BACKGROUND) ? "BACKGROUND" : "NONE"),
        (fctl.blend_op == APNG_BLEND_OP_SOURCE) ? "SOURCE" : "OVER");

    if (last_dispose_op == APNG_DISPOSE_OP_PREVIOUS) {
      //copy row by row
      for (int y = 0; y < previous_height; y++) {
        memcpy(&screenbuffer[(previous_yoffset + y) * width + previous_xoffset], 
            &buffer_previous[y * previous_width], 
            previous_width * sizeof(rgba32));
      }
    } else if (last_dispose_op == APNG_DISPOSE_OP_BACKGROUND) {
      for (int y = 0; y < previous_height; y++) {
        memset(&screenbuffer[(previous_yoffset + y) * width + previous_xoffset], 
            0, previous_width * sizeof(rgba32));
      }
    }

    previous_xoffset = fctl.x_offset;
    previous_yoffset = fctl.y_offset;
    previous_width = fctl.width;
    previous_height = fctl.height;

    if (fctl.dispose_op == APNG_DISPOSE_OP_PREVIOUS) {
      if (buffer_previous) {
        free(buffer_previous);
        buffer_previous = NULL;
      }
      buffer_previous = malloc(previous_width * previous_height * sizeof(rgba32));
      //copy row by row
      for (int y = 0; y < previous_height; y++) {
        memcpy(&buffer_previous[y * previous_width], 
            &screenbuffer[(previous_yoffset + y) * width + previous_xoffset], 
            previous_width * sizeof(rgba32));
      }
    } 
    
    for (int y = 0; y < fctl.height; y++) {
      for (int x = 0; x < fctl.width; x++) {
        uint8_t palette_index = ((uint8_t*)raw_buffer)[y * fctl.width + x];

        if (fctl.blend_op == APNG_BLEND_OP_OVER) {
          rgba32 src = rgba_palette[palette_index];
          rgba32 *dst = (rgba32*)&screenbuffer[(fctl.y_offset + y) * width + (fctl.x_offset + x)];
          dst->r = MIN(0xFF, (src.r * (src.a / 255) + (dst->r * ((255 - src.a) / 255))));
          dst->g = MIN(0xFF, (src.g * (src.a / 255) + (dst->g * ((255 - src.a) / 255))));
          dst->b = MIN(0xFF, (src.b * (src.a / 255) + (dst->b * ((255 - src.a) / 255))));
        } else {
          screenbuffer[(fctl.y_offset + y) * width + (fctl.x_offset + x)] = rgba_palette[palette_index].rgba;
        }
      }
    }

    last_dispose_op = fctl.dispose_op;

    //color_demo(screenbuffer);
    sdl_draw();
  }

  return 0;
}
