/* SPDX-License-Identifier: GPL-2.0-only */
/* This file is licensed under the GNU General Public License, version 2 (see LICENSES/GPL-2.0.txt),
 * NOT under the MIT license of the rest of this project: it re-implements the behaviour of sbc09's
 * src/vdisk.c (Shinji Kono, GPL; (c) 1994-2014 L.C. Benschop and the sbc09 team) and is a derived work.
 * A program linked with this file (build/rtlsim) is therefore distributed under the GPL v2 as a whole.
 *
 * vdisk.h - host-side "virtual RBF" (the /v0 device): maps a host directory into
 * OS-9.  Behavioural model (not RTL) driven by command writes to the disk
 * controller register block. */
#ifndef VDISK_H
#define VDISK_H

#include <stdint.h>
#include "../soc/devices.h"

typedef struct Vdisk Vdisk;

Vdisk *vdisk_new(const char *root);
void   vdisk_free(Vdisk *v);
void   vdisk_command(Vdisk *v, DiskCtl *dc, uint8_t cmd);

#endif
