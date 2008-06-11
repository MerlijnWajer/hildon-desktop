/*
 * This file is part of hildon-desktop
 *
 * Copyright (C) 2008 Nokia Corporation.
 *
 * Author:  Tomas Frydrych <tf@o-hand.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#ifndef _HAVE_HD_APP_H
#define _HAVE_HD_APP_H

#include <matchbox/core/mb-wm.h>
#include <matchbox/client-types/mb-wm-client-app.h>

typedef struct HdApp      HdApp;
typedef struct HdAppClass HdAppClass;

#define HD_APP(c)       ((HdApp*)(c))
#define HD_APP_CLASS(c) ((HdAppClass*)(c))
#define HD_TYPE_APP     (hd_app_class_type ())
#define HD_IS_APP(c)    (MB_WM_OBJECT_TYPE(c)==HD_TYPE_APP)

struct HdApp
{
  MBWMClientApp    parent;

  gboolean         secondary_window;
};

struct HdAppClass
{
  MBWMClientAppClass parent;
};

MBWindowManagerClient*
hd_app_new (MBWindowManager *wm, MBWMClientWindow *win);

int
hd_app_class_type (void);

#endif
