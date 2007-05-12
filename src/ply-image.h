/* vim: ts=4 sw=2 expandtab autoindent cindent 
 * ply-video-buffer.h - framebuffer abstraction
 *
 * Copyright (C) 2007 Red Hat, Inc.
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
 * Written By: Ray Strode <rstrode@redhat.com>
 */
#ifndef PLY_IMAGE_H
#define PLY_IMAGE_H

#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

typedef struct _ply_image ply_image_t;

#ifndef PLY_HIDE_FUNCTION_DECLARATIONS
ply_image_t *ply_image_new (const char *filename);
void ply_image_free (ply_image_t *image);
bool ply_image_load (ply_image_t *image);
uint32_t *ply_image_get_data (ply_image_t *image);
ssize_t ply_image_get_size (ply_image_t *image);
long ply_image_get_width (ply_image_t *image);
long ply_image_get_height (ply_image_t *image);

#endif

#endif /* PLY_IMAGE_H */
