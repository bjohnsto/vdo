/*
 * Copyright (c) 2018 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA. 
 *
 * $Id: //eng/uds-releases/homer/userLinux/uds/loggerDefs.h#2 $
 *
 */

#ifndef LINUX_USER_LOGGER_DEFS_H
#define LINUX_USER_LOGGER_DEFS_H

#include "minisyslog.h"

// For compatibility with hooks we need when compiling in kernel mode.
#define PRIptr "p"

/*
 * Apply a rate limiter to a log method call.
 *
 * @param logFunc  A method that does logging, which is always invoked because
 *                 we do not do ratelimiting in user mode.
 */
#define logRatelimit(logFunc, ...) logFunc(__VA_ARGS__)

#endif // LINUX_USER_LOGGER_DEFS_H
