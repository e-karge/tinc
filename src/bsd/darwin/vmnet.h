/*
 *  vmnet - Tun device emulation for Darwin
 *  Copyright (C) 2024 Eric Karge
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef VMNET_H
#define VMNET_H

#include <sys/types.h>

int macos_vmnet_open(void);
int macos_vmnet_close(int fd);
ssize_t macos_vmnet_write(uint8_t *buffer, size_t buflen);

#endif
