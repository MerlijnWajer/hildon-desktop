
#ifndef __HD_ATOM_H__
#define __HD_ATOM_H__

#include <X11/Xlib.h>
#include <X11/Xatom.h>          /* for XA_ATOM etc */

typedef enum HdAtoms
{
  HD_ATOM_HILDON_APP_KILLABLE = 0,
  HD_ATOM_HILDON_ABLE_TO_HIBERNATE,

  HD_ATOM_HILDON_HOME_VIEW,
  HD_ATOM_HILDON_STACKABLE_WINDOW,

  HD_ATOM_HILDON_WM_WINDOW_TYPE_HOME_APPLET,
  HD_ATOM_HILDON_WM_WINDOW_TYPE_APP_MENU,
  HD_ATOM_HILDON_WM_WINDOW_TYPE_STATUS_AREA,
  HD_ATOM_HILDON_WM_WINDOW_TYPE_STATUS_MENU,

  HD_ATOM_HILDON_NOTIFICATION_TYPE,
  HD_ATOM_HILDON_NOTIFICATION_TYPE_BANNER,
  HD_ATOM_HILDON_NOTIFICATION_TYPE_INFO,
  HD_ATOM_HILDON_NOTIFICATION_TYPE_CONFIRMATION,

  HD_ATOM_HILDON_CLIENT_MESSAGE_PAN,
  HD_ATOM_HILDON_CLIENT_MESSAGE_SHOW_SETTINGS,

  HD_ATOM_HILDON_APPLET_ID,

  HD_ATOM_WM_WINDOW_ROLE,

  HD_ATOM_UTF8_STRING,

  _HD_ATOM_LAST
} HdAtoms;

void
hd_atoms_init (Display * xdpy, Atom * atoms);

#endif
