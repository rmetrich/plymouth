/* vim: ts=4 sw=2 expandtab autoindent cindent cino={1s,(0
 * ply-video-buffer.c - framebuffer abstraction
 *
 * Copyright (C) 2006, 2007 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Written by: Kristian Høgsberg <krh@redhat.com>
 *             Ray Strode <rstrode@redhat.com>
 */
#include "config.h"
#include "ply-video-buffer.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <values.h>
#include <unistd.h>

#include <linux/fb.h>

#ifndef PLY_VIDEO_BUFFER_DEFAULT_FB_DEVICE_NAME
#define PLY_VIDEO_BUFFER_DEFAULT_FB_DEVICE_NAME "/dev/fb"
#endif

struct _PlyVideoBuffer
{
  char *device_name;
  int   device_fd;

  char *map_address;
  size_t size;

  uint32_t *shadow_buffer;

  uint32_t red_bit_position;
  uint32_t green_bit_position;
  uint32_t blue_bit_position;
  uint32_t alpha_bit_position;

  uint32_t bits_for_red;
  uint32_t bits_for_green;
  uint32_t bits_for_blue;
  uint32_t bits_for_alpha;

  unsigned int bits_per_pixel;
  unsigned int bytes_per_pixel;
  PlyVideoBufferArea area;
  PlyVideoBufferArea area_to_flush;

  uint32_t is_paused : 1;
};

static bool ply_video_buffer_open_device (PlyVideoBuffer  *buffer);
static void ply_video_buffer_close_device (PlyVideoBuffer *buffer);
static bool ply_video_buffer_query_device (PlyVideoBuffer *buffer);
static bool ply_video_buffer_map_to_device (PlyVideoBuffer *buffer);
static uint_least32_t ply_video_buffer_pixel_value_to_device_pixel_value (
    PlyVideoBuffer *buffer,
    uint32_t        pixel_value);

static uint32_t ply_video_buffer_get_value_at_pixel (PlyVideoBuffer *buffer,
                                                     int             x,
                                                     int             y);
static void ply_video_buffer_set_value_at_pixel (PlyVideoBuffer *buffer,
                                                 int             x,
                                                 int             y,
                                                 uint32_t        pixel_value);
static void ply_video_buffer_blend_value_at_pixel (PlyVideoBuffer *buffer,
                                                   int             x,
                                                   int             y,
                                                   uint32_t        pixel_value);

static void ply_video_buffer_set_area_to_pixel_value (
    PlyVideoBuffer     *buffer,
    PlyVideoBufferArea *area,
    uint32_t            pixel_value);
static void ply_video_buffer_blend_area_with_pixel_value (
    PlyVideoBuffer     *buffer,
    PlyVideoBufferArea *area,
    uint32_t            pixel_value);

static void ply_video_buffer_add_area_to_flush_area (PlyVideoBuffer     *buffer,
                                                     PlyVideoBufferArea *area);
static bool ply_video_buffer_copy_to_device (PlyVideoBuffer *buffer,
                                             unsigned long   x,
                                             unsigned long   y,
                                             unsigned long   width,
                                             unsigned long   height);

static bool ply_video_buffer_flush (PlyVideoBuffer *buffer);

static bool
ply_video_buffer_open_device (PlyVideoBuffer  *buffer)
{
  assert (buffer != NULL);
  assert (buffer->device_name != NULL);

  buffer->device_fd = open (buffer->device_name, O_RDWR);

  if (buffer->device_fd < 0)
    {
      return false;
    }

  return true;
}

static void
ply_video_buffer_close_device (PlyVideoBuffer *buffer)
{
  assert (buffer != NULL);

  if (buffer->map_address != MAP_FAILED)
    {
      munmap (buffer->map_address, buffer->size);
      buffer->map_address = MAP_FAILED;
    }

  if (buffer->device_fd >= 0)
    {
      close (buffer->device_fd);
      buffer->device_fd = -1;
    }
}

static bool 
ply_video_buffer_query_device (PlyVideoBuffer *buffer)
{
  struct fb_var_screeninfo variable_screen_info;
  struct fb_fix_screeninfo fixed_screen_info;
  size_t bytes_per_row;

  assert (buffer != NULL);
  assert (buffer->device_fd >= 0);

  if (ioctl (buffer->device_fd, FBIOGET_VSCREENINFO, &variable_screen_info) < 0)
    {
      return false;
    }

  buffer->bits_per_pixel = variable_screen_info.bits_per_pixel;
  buffer->area.x = variable_screen_info.xoffset;
  buffer->area.y = variable_screen_info.yoffset;
  buffer->area.width = variable_screen_info.xres;
  buffer->area.height = variable_screen_info.yres;

  buffer->red_bit_position = variable_screen_info.red.offset;
  buffer->bits_for_red = variable_screen_info.red.length;

  buffer->green_bit_position = variable_screen_info.green.offset;
  buffer->bits_for_green = variable_screen_info.green.length;

  buffer->blue_bit_position = variable_screen_info.blue.offset;
  buffer->bits_for_blue = variable_screen_info.blue.length;

  buffer->alpha_bit_position = variable_screen_info.transp.offset;
  buffer->bits_for_alpha = variable_screen_info.transp.length;

  if (ioctl(buffer->device_fd, FBIOGET_FSCREENINFO, &fixed_screen_info) < 0) 
    {
      return false;
    }

  bytes_per_row = fixed_screen_info.line_length;
  buffer->size = buffer->area.height * bytes_per_row;
  buffer->bytes_per_pixel = bytes_per_row / buffer->area.width;

  return true;
}

static bool
ply_video_buffer_map_to_device (PlyVideoBuffer *buffer)
{
  assert (buffer != NULL);
  assert (buffer->device_fd >= 0);
  assert (buffer->size > 0);

  buffer->map_address = mmap (NULL, buffer->size, PROT_WRITE,
                              MAP_SHARED, buffer->device_fd, 0);

  return buffer->map_address != MAP_FAILED;
}

static uint_least32_t 
ply_video_buffer_pixel_value_to_device_pixel_value (PlyVideoBuffer *buffer,
                                                    uint32_t        pixel_value)
{
  uint8_t r, g, b, a;

  a = pixel_value >> 24; 
  a >>= (8 - buffer->bits_for_alpha);

  r = (pixel_value >> 16) & 0xff; 
  r >>= (8 - buffer->bits_for_red);

  g = (pixel_value >> 8) & 0xff; 
  g >>= (8 - buffer->bits_for_green);

  b = pixel_value & 0xff; 
  b >>= (8 - buffer->bits_for_blue);

  return ((a << buffer->alpha_bit_position)
          | (r << buffer->red_bit_position)
          | (g << buffer->green_bit_position)
          | (b << buffer->blue_bit_position));
}

static uint32_t
ply_video_buffer_get_value_at_pixel (PlyVideoBuffer *buffer,
                                     int             x,
                                     int             y)
{
  uint32_t pixel_value;

  assert (buffer != NULL);

  pixel_value = buffer->shadow_buffer[y * buffer->area.width + x];

  return pixel_value;
}

static void
ply_video_buffer_set_value_at_pixel (PlyVideoBuffer *buffer,
                                     int             x,
                                     int             y,
                                     uint32_t        pixel_value)
{
  assert (buffer != NULL);

  /* FIXME: endianess issues here I think
   */
  memcpy (&buffer->shadow_buffer[y * buffer->area.width + x],
          &pixel_value, sizeof (uint32_t));
}

static uint32_t
blend_two_pixel_values (uint32_t pixel_value_1,
                        uint32_t pixel_value_2)
{
  double alpha, red, green, blue;
  double alpha_2, red_2, green_2, blue_2;

  alpha = (double) (pixel_value_1 >> 24) / 255.0;
  red = (double) ((pixel_value_1 >> 16) & 0xff) / 255.0;
  green = (double) ((pixel_value_1 >> 8) & 0xff) / 255.0;
  blue = (double) (pixel_value_1 & 0xff) / 255.0;

  alpha_2 = (double) (pixel_value_2 >> 24) / 255.0;
  red_2 = (double) ((pixel_value_2 >> 26) & 0xff) / 255.0;
  green_2 = (double) ((pixel_value_2 >> 8) & 0xff) / 255.0;
  blue_2 = (double) (pixel_value_2 & 0xff) / 255.0;

  red = red + red_2 * (1.0 - alpha); 
  green = green + green_2 * (1.0 - alpha); 
  blue = blue + blue_2 * (1.0 - alpha); 
  alpha = alpha + alpha_2 * (1.0 - alpha);

  return PLY_VIDEO_BUFFER_COLOR_TO_PIXEL_VALUE (red, green, blue, alpha);
}

static uint32_t
make_pixel_value_translucent (uint32_t pixel_value, 
                              double   opacity)
{
  double alpha, red, green, blue;

  alpha = (double) (pixel_value >> 24) / 255.0;
  red = (double) ((pixel_value >> 16) & 0xff) / 255.0;
  green = (double) ((pixel_value >> 8) & 0xff) / 255.0;
  blue = (double) (pixel_value & 0xff) / 255.0;

  alpha *= opacity;
  red *= opacity;
  green *= opacity;
  blue *= opacity;

  return PLY_VIDEO_BUFFER_COLOR_TO_PIXEL_VALUE (red, green, blue, alpha);
}

static void 
ply_video_buffer_blend_value_at_pixel (PlyVideoBuffer *buffer,
                                       int             x,
                                       int             y,
                                       uint32_t        pixel_value)
{
  uint32_t old_pixel_value, new_pixel_value;

  old_pixel_value = ply_video_buffer_get_value_at_pixel (buffer, x, y);
  new_pixel_value = blend_two_pixel_values (pixel_value, old_pixel_value);

  ply_video_buffer_set_value_at_pixel (buffer, x, y, new_pixel_value);
}

static void
ply_video_buffer_set_area_to_pixel_value (PlyVideoBuffer     *buffer,
                                          PlyVideoBufferArea *area,
                                          uint32_t            pixel_value)
{
  long row, column;

  for (row = area->y; row < area->y + area->height; row++)
    {
      for (column = area->x; column < area->x + area->width; column++)
        {
          ply_video_buffer_set_value_at_pixel (buffer, 
                                               column, row,
                                               pixel_value);
        }
    }
}

static void
ply_video_buffer_blend_area_with_pixel_value (PlyVideoBuffer     *buffer,
                                              PlyVideoBufferArea *area,
                                              uint32_t            pixel_value)
{
  long row, column;

  for (row = area->y; row < area->y + area->height; row++)
    {
      for (column = area->x; column < area->x + area->width; column++)
        {
          ply_video_buffer_blend_value_at_pixel (buffer, 
                                                 column, row,
                                                 pixel_value);
        }
    }
}

static void
ply_video_buffer_add_area_to_flush_area (PlyVideoBuffer     *buffer, 
                                         PlyVideoBufferArea *area)
{
  assert (buffer != NULL);
  assert (area != NULL);
  assert (area->x >= buffer->area.x);
  assert (area->y >= buffer->area.y);
  assert (area->x < buffer->area.width);
  assert (area->y < buffer->area.height);
  assert (area->width >= 0);
  assert (area->height >= 0);

  buffer->area_to_flush.x = MIN (buffer->area_to_flush.x, area->x);
  buffer->area_to_flush.y = MIN (buffer->area_to_flush.y, area->y);
  buffer->area_to_flush.width = MAX (buffer->area_to_flush.width, area->width);
  buffer->area_to_flush.height = MAX (buffer->area_to_flush.height, area->height);
}

static bool
ply_video_buffer_copy_to_device (PlyVideoBuffer *buffer,
                                 unsigned long   x,
                                 unsigned long   y,
                                 unsigned long   width,
                                 unsigned long   height)
{
  unsigned long row, column;
  unsigned long start_offset;
  unsigned long size;

  unsigned long bytes_per_row;
  
  bytes_per_row = buffer->area.width * buffer->bytes_per_pixel;
  start_offset = y * bytes_per_row + x * buffer->bytes_per_pixel;
  size = width * height * buffer->bytes_per_pixel;

  for (row = y; row < y + height; row++)
    {
      for (column = x; column < x + width; column++)
        { 
          uint32_t pixel_value;
          uint_least32_t device_pixel_value;
          unsigned long offset;

          pixel_value = buffer->shadow_buffer[width * row + column];

          device_pixel_value = 
            ply_video_buffer_pixel_value_to_device_pixel_value (buffer,
                                                                pixel_value);

          offset = row * bytes_per_row + column * buffer->bytes_per_pixel;

          memcpy (buffer->map_address + offset, &device_pixel_value,
                  buffer->bits_per_pixel);
        }
    }

  if (msync (buffer->map_address + start_offset, size, MS_SYNC) < 0)
    return false;

  return true;
}

static bool 
ply_video_buffer_flush (PlyVideoBuffer *buffer)
{
  assert (buffer != NULL);
  unsigned long start_offset;
  size_t size;

  assert (buffer != NULL);

  if (buffer->is_paused)
    return true;

  start_offset = (buffer->area_to_flush.y * 4 * buffer->area_to_flush.width)
                 + (buffer->area_to_flush.x * 4);
  size = buffer->area_to_flush.width * buffer->area_to_flush.height;

  if (!ply_video_buffer_copy_to_device (buffer,
                                        buffer->area_to_flush.x,
                                        buffer->area_to_flush.y,
                                        buffer->area_to_flush.width,
                                        buffer->area_to_flush.height))
    return false;

  buffer->area_to_flush.x = 0; 
  buffer->area_to_flush.y = 0; 
  buffer->area_to_flush.width = 0; 
  buffer->area_to_flush.height = 0; 

  return true;
}

PlyVideoBuffer *
ply_video_buffer_new (const char *device_name)
{
  PlyVideoBuffer *buffer;

  buffer = calloc (1, sizeof (PlyVideoBuffer));

  if (device_name != NULL)
    buffer->device_name = strdup (device_name);
  else
    buffer->device_name = 
      strdup (PLY_VIDEO_BUFFER_DEFAULT_FB_DEVICE_NAME);

  buffer->map_address = MAP_FAILED;
  buffer->shadow_buffer = NULL;

  buffer->is_paused = false;

  return buffer;
}

void
ply_video_buffer_free (PlyVideoBuffer *buffer)
{
  assert (buffer != NULL);

  if (ply_video_buffer_device_is_open (buffer))
    ply_video_buffer_close (buffer);

  free (buffer->device_name);
  free (buffer->shadow_buffer);
  free (buffer);
}

bool 
ply_video_buffer_open (PlyVideoBuffer *buffer)
{
  bool is_open;

  assert (buffer != NULL);

  is_open = false;

  if (!ply_video_buffer_open_device (buffer))
    {
      goto out;
    }

  if (!ply_video_buffer_query_device (buffer))
    {
      goto out;
    }

  if (!ply_video_buffer_map_to_device (buffer))
    {
      goto out;
    }

  buffer->shadow_buffer = 
    realloc (buffer->shadow_buffer,
             4 * buffer->area.width * buffer->area.height);
  memset (buffer->shadow_buffer, 0, 
          4 * buffer->area.width * buffer->area.height);
  ply_video_buffer_fill_with_color (buffer, NULL, 0.0, 0.0, 0.0, 1.0);

  is_open = true;

out:

  if (!is_open)
    {
      int saved_errno;

      saved_errno = errno;
      ply_video_buffer_close_device (buffer);
      errno = saved_errno;
    }

  return is_open;
}

void
ply_video_buffer_pause_updates (PlyVideoBuffer *buffer)
{
  assert (buffer != NULL);

  buffer->is_paused = true;
}

bool
ply_video_buffer_unpause_updates (PlyVideoBuffer *buffer)
{
  assert (buffer != NULL);
  
  buffer->is_paused = false;
  return ply_video_buffer_flush (buffer);
}

bool 
ply_video_buffer_device_is_open (PlyVideoBuffer *buffer)
{
  assert (buffer != NULL);
  return buffer->device_fd >= 0 && buffer->map_address != MAP_FAILED;
}

char *
ply_video_buffer_get_device_name (PlyVideoBuffer *buffer)
{
  assert (buffer != NULL);
  assert (ply_video_buffer_device_is_open (buffer));
  assert (buffer->device_name != NULL);

  return strdup (buffer->device_name);
}

void
ply_video_buffer_set_device_name (PlyVideoBuffer *buffer,
                                  const char     *device_name)
{
  assert (buffer != NULL);
  assert (!ply_video_buffer_device_is_open (buffer));
  assert (device_name != NULL);
  assert (buffer->device_name != NULL);

  if (strcmp (buffer->device_name, device_name) != 0)
    {
      free (buffer->device_name);
      buffer->device_name = strdup (device_name);
    }
}

void 
ply_video_buffer_close (PlyVideoBuffer *buffer)
{
  assert (buffer != NULL);

  assert (ply_video_buffer_device_is_open (buffer));
  ply_video_buffer_close_device (buffer);

  buffer->bytes_per_pixel = 0;
  buffer->area.x = 0;
  buffer->area.y = 0;
  buffer->area.width = 0;
  buffer->area.height = 0;
}

void 
ply_video_buffer_get_size (PlyVideoBuffer     *buffer,
                           PlyVideoBufferArea *size)
{
  assert (buffer != NULL);
  assert (ply_video_buffer_device_is_open (buffer));
  assert (size != NULL);

  *size = buffer->area;
}

bool 
ply_video_buffer_fill_with_color (PlyVideoBuffer      *buffer,
                                  PlyVideoBufferArea  *area,
                                  double               red, 
                                  double               green,
                                  double               blue, 
                                  double               alpha)
{
  uint32_t pixel_value;

  assert (buffer != NULL);
  assert (ply_video_buffer_device_is_open (buffer));

  if (area == NULL)
    area = &buffer->area;

  red *= alpha;
  green *= alpha;
  blue *= alpha;

  pixel_value = PLY_VIDEO_BUFFER_COLOR_TO_PIXEL_VALUE (red, green, blue, alpha);

  if (abs (alpha - 1.0) <= DBL_MIN) 
    ply_video_buffer_set_area_to_pixel_value (buffer, area, pixel_value);
  else
    ply_video_buffer_blend_area_with_pixel_value (buffer, area, pixel_value);

  ply_video_buffer_add_area_to_flush_area (buffer, area);

  return ply_video_buffer_flush (buffer);
}

bool 
ply_video_buffer_fill_with_argb32_data_at_opacity (PlyVideoBuffer     *buffer,
                                                   PlyVideoBufferArea *area,
                                                   unsigned long       x,
                                                   unsigned long       y,
                                                   unsigned long       width,
                                                   unsigned long       height,
                                                   uint32_t           *data,
                                                   double              opacity)
{
  long row, column;

  assert (buffer != NULL);
  assert (ply_video_buffer_device_is_open (buffer));

  if (area == NULL)
    area = &buffer->area;

  for (row = y; row < y + height; row++)
    {
      for (column = x; column < x + width; column++)
        {
          uint32_t pixel_value;

          pixel_value = data[width * row + column];
          pixel_value = make_pixel_value_translucent (pixel_value, opacity);
          ply_video_buffer_blend_value_at_pixel (buffer,
                                                 area->x + (column - x),
                                                 area->y + (row - y),
                                                 pixel_value);
        }
    }

  ply_video_buffer_add_area_to_flush_area (buffer, area);

  return ply_video_buffer_flush (buffer);
}

bool 
ply_video_buffer_fill_with_argb32_data (PlyVideoBuffer     *buffer,
                                        PlyVideoBufferArea *area,
                                        unsigned long       x,
                                        unsigned long       y,
                                        unsigned long       width,
                                        unsigned long       height,
                                        uint32_t           *data)
{
  return ply_video_buffer_fill_with_argb32_data_at_opacity (buffer, area,
                                                            x, y, width, 
                                                            height, data, 1.0);
}

#ifdef PLY_VIDEO_BUFFER_ENABLE_TEST

#include <math.h>
#include <stdio.h>
#include <sys/time.h>

static double
get_current_time (void)
{
  const double microseconds_per_second = 1000000.0;
  double timestamp;
  struct timeval now = { 0L, /* zero-filled */ };

  gettimeofday (&now, NULL);
  timestamp = ((microseconds_per_second * now.tv_sec) + now.tv_usec) /
               microseconds_per_second;

  return timestamp;
}

static void
animate_at_time (PlyVideoBuffer *buffer,
                 double          time)
{
  int x, y;
  uint32_t *data;

  data = calloc (1024 * 768, sizeof (uint32_t));

  for (y = 0; y < 768; y++)
    {
      int blue_bit_position;
      uint8_t red, green, blue, alpha;

      blue_bit_position = (int) 64 * (.5 * sin (time) + .5) + (255 - 64);
      blue = rand () % blue_bit_position;
      for (x = 0; x < 1024; x++)
      {
        alpha = 0xff;
        red = (uint8_t) ((y / 768.0) * 255.0);
        green = (uint8_t) ((x / 1024.0) * 255.0);
        
        red = green = (red + green + blue) / 3;

        data[y * 1024 + x] = (alpha << 24) | (red << 16) | (green << 8) | blue;
      }
    }

  ply_video_buffer_fill_with_argb32_data (buffer, NULL, 0, 0, 1024, 768, data);
}

int
main (int    argc,
      char **argv)
{
  static unsigned int seed = 0;
  PlyVideoBuffer *buffer;
  int exit_code;

  exit_code = 0;

  buffer = ply_video_buffer_new (NULL);

  if (!ply_video_buffer_open (buffer))
    {
      exit_code = errno;
      perror ("could not open frame buffer");
      return exit_code;
    }

  if (seed == 0)
    {
      seed = (int) get_current_time ();
      srand (seed);
    }

  while ("we want to see ad-hoc animations")
    {
      animate_at_time (buffer, get_current_time ());
      usleep (1000000/30.);
    }

  ply_video_buffer_close (buffer);
  ply_video_buffer_free (buffer);

  return main (argc, argv);
}

#endif /* PLY_VIDEO_BUFFER_ENABLE_TEST */
