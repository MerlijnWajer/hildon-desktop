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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "hd-home.h"
#include "hd-home-glue.h"
#include "hd-switcher.h"
#include "hd-home-view.h"
#include "hd-home-view-container.h"
#include "hd-comp-mgr.h"
#include "hd-util.h"
#include "hd-gtk-style.h"
#include "hd-gtk-utils.h"
#include "hd-home-applet.h"
#include "hd-render-manager.h"
#include "hd-clutter-cache.h"
#include "hd-theme.h"
#include "hd-wm.h"
#include "hd-launcher-app.h"
#include "hd-dbus.h"
#include "hd-title-bar.h"

#include <clutter/clutter.h>
#include <clutter/x11/clutter-x11.h>
#include <clutter/clutter-texture.h>

#include <matchbox/core/mb-wm.h>

#include <gconf/gconf-client.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk-pixbuf-xlib/gdk-pixbuf-xlib.h>

#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-bindings.h>

#include <strings.h>
#include <unistd.h>

#include <X11/XKBlib.h>
#include <gdk/gdkx.h>
#include <gdk/gdkkeysyms.h>
#include <libhildondesktop/hd-pvr-texture.h>

#define HDH_EDIT_BUTTON_DURATION 200
#define HDH_EDIT_BUTTON_TIMEOUT 3000

#define HDH_LAYOUT_TOP_SCALE 0.5
#define HDH_LAYOUT_Y_OFFSET 60

/* FIXME -- match spec */
#define HDH_PAN_THRESHOLD 20     // threshold for panning
#define HDH_CLICK_THRESHOLD 40   // threshold for detecting a click
#define PAN_NEXT_PREVIOUS_PERCENTAGE 0.25
/* Time in secs to look back when finding average velocity */
#define HDH_PAN_VELOCITY_HISTORY 0.125

#define HD_HOME_DBUS_NAME  "com.nokia.HildonDesktop.Home"
#define HD_HOME_DBUS_PATH  "/com/nokia/HildonDesktop/Home"

#define GCONF_KEY_SCROLL_VERTICAL    "/apps/osso/hildon-desktop/scroll_vertical"
#define GCONF_KEY_PORTRAIT_WALLPAPER "/apps/osso/hildon-desktop/portrait_wallpaper"

#define CALL_UI_GCONF_KEY "/apps/osso/hildon-desktop/callui_dbus_interface"
#define ADDRESSBOOK_GCONF_KEY \
           "/apps/osso/hildon-desktop/addressbook_dbus_interface"

#define INDICATION_WIDTH 50
#define HD_EDGE_INDICATION_COLOR "SelectionColor"

#define MAX_VIEWS 9

#define FN_KEY GDK_ISO_Level3_Shift
#define FN_MODIFIER Mod5Mask
#define HD_HOME_KEY_PRESS_TIMEOUT (3)

#define LONG_PRESS_DUR 1

/* for debugging dragging of home view backgrounds */
#define DRAG_DEBUG(...)
//#define DRAG_DEBUG g_debug

gboolean in_alt_tab;

enum
{
  PROP_COMP_MGR = 1,
  PROP_HDRM,
};

enum EdgeIndicationOpacity
{
  EDGE_INDICATION_OPACITY_INVISIBLE = 0x00,
  EDGE_INDICATION_OPACITY_WIDGET_MOVING = 0x7f,
  EDGE_INDICATION_OPACITY_WIDGET_OVER = 0xff,
};

typedef enum
{
  HD_HOME_GCONF_UPDATE_BOTH,
  HD_HOME_GCONF_UPDATE_ABOOK,
  HD_HOME_GCONF_UPDATE_CALLUI
} HdHomeGconfUpdateMode;

struct _HdHomePrivate
{
  MBWMCompMgrClutter    *comp_mgr;

  ClutterEffectTemplate *show_edit_button_template;
  ClutterEffectTemplate *hide_edit_button_template;

  ClutterActor          *edit_group; /* An overlay group for edit mode */
  /* TODO: Edit button should probably be handled by HdTitleBar */
  ClutterActor          *edit_button;

  ClutterActor          *operator;
  ClutterActor          *operator_applet;

  ClutterActor          *edge_indication_left;
  ClutterActor          *edge_indication_right;

  ClutterActor          *view_container;

  /* container that sits infront of blurred home */
  ClutterGroup          *front;

  gulong                 desktop_motion_cb;

  guint                  edit_button_cb;

  gboolean               vertical_scrolling;
  gboolean               portrait_wallpaper;

  /* Pan variables */
  gint                   last_x;
  gint                   last_y;
  gint                   initial_x;
  gint                   initial_y;
  gint                   cumulative_x;
  gint                   cumulative_y;
  gint                   velocity_x; /* movement in pixels per sec */
  gint                   velocity_y; /* movement in pixels per sec */
  GTimer                 *last_move_time; /* time of last movement event */
  GList                  *drag_list;
  /* List of HdHomeDrag - history of mouse events used to work out an
   * average velocity */

  gboolean               moved_over_threshold : 1;
  gboolean               long_press : 1;
  guint                  press_timeout;
  ClutterActor          *pressed_applet;

  Window                 desktop;

  /* DBus Proxy for the call to com.nokia.CallUI.ShowDialpad */
  DBusGProxy            *call_ui_proxy;
  DBusGProxy            *addressbook_proxy;

  /* For hd_home_desktop_key_press() */
  enum
  {
    FN_STATE_NONE,      // interpret the KeyPress as it is
    FN_STATE_NEXT,      // the next key is Fn-modified
    FN_STATE_LOCKED,    // until turned off all key press are Fn-modified
  } fn_state, shift_state;
  time_t last_fn_time;

  enum
  {
    KEY_SENT_NONE,        // No key has been sent yet.
    KEY_SENT_CALLUI,      // A key has been sent to CallUI.
    KEY_SENT_ADDRESSBOOK  // A key has been sent to the addressbook.
  } key_sent;
  time_t last_key_time;

  /* For hd_home_desktop_key_release():
   * Don't change @fn_state if it was wasn't pressed alone. */
  gboolean ignore_next_fn_release;
  gboolean ignore_next_shift_release;
  /* These are all reset when the HDRM state is changed. */

  /* addressbook and Call UI D-Bus interfaces (name, path, method) */
  gchar *abook_interface[3];
  gchar *callui_interface[3];
};

typedef struct {
  gint    x;
  gint    y;
  gdouble period;
} HdHomeDrag;

static void hd_home_class_init (HdHomeClass *klass);
static void hd_home_init       (HdHome *self);
static void hd_home_dispose    (GObject *object);
static void hd_home_finalize   (GObject *object);

static void hd_home_set_property (GObject      *object,
				  guint         prop_id,
				  const GValue *value,
				  GParamSpec   *pspec);

static void hd_home_get_property (GObject      *object,
				  guint         prop_id,
				  GValue       *value,
				  GParamSpec   *pspec);

static void hd_home_constructed (GObject *object);

static void hd_home_show_edit_button (HdHome *home);

static void hd_home_reset_fn_state (HdHome *home);

static void do_applet_release (HdHome             *home,
                               ClutterActor       *applet,
                               ClutterButtonEvent *event);
static gboolean do_home_applet_motion (HdHome       *home,
                                       ClutterActor *applet,
                                       int           x,
                                       int           y);
static void update_edge_indication_visibility (HdHome *home,
                                               guint8  left_opacity,
                                               guint8  right_opacity);
static void
update_dbus_interfaces_from_gconf (HdHome *home, HdHomeGconfUpdateMode what);

G_DEFINE_TYPE (HdHome, hd_home, CLUTTER_TYPE_GROUP);

static void
hd_home_class_init (HdHomeClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GParamSpec   *pspec;

  g_type_class_add_private (klass, sizeof (HdHomePrivate));

  object_class->dispose      = hd_home_dispose;
  object_class->finalize     = hd_home_finalize;
  object_class->set_property = hd_home_set_property;
  object_class->get_property = hd_home_get_property;
  object_class->constructed  = hd_home_constructed;

  pspec = g_param_spec_pointer ("comp-mgr",
				"Composite Manager",
				"MBWMCompMgrClutter Object",
				G_PARAM_READWRITE | G_PARAM_CONSTRUCT);
  g_object_class_install_property (object_class, PROP_COMP_MGR, pspec);

  pspec = g_param_spec_pointer ("hdrm",
				"HdRenderManager",
				"--""--",
				G_PARAM_WRITABLE);
  g_object_class_install_property (object_class, PROP_HDRM, pspec);
}

static gboolean
hd_home_edit_button_clicked (ClutterActor *button,
			     ClutterEvent *event,
			     HdHome       *home)
{
	if(STATE_IS_PORTRAIT (hd_render_manager_get_state()))
	  hd_render_manager_set_state(HDRM_STATE_HOME_EDIT_PORTRAIT);
	else
	  hd_render_manager_set_state(HDRM_STATE_HOME_EDIT);

  return TRUE;
}

static void
hd_home_live_bg_emit_button1_event (
		HdHome             *home,
		Window       		mywindow,
		int                 x,
		int                 y,
		int					event_type)
{
  HdHomePrivate      *priv = home->priv;
  MBWindowManager    *wm;
  XButtonEvent        xev;
  wm = MB_WM_COMP_MGR(priv->comp_mgr)->wm;
  /* Emitting an event. */
  xev.type = event_type;
  xev.send_event = False;
  xev.display = wm->xdpy;
  xev.window = mywindow;
  xev.root = wm->root_win->xwindow;
  xev.subwindow = None;
  xev.time = CurrentTime;
  xev.x = x;
  xev.y = y;
  xev.x_root = x;
  xev.y_root = y;
  xev.state = 0;
  xev.button = Button1;
  xev.same_screen = True;
  /* Get the actual coordinates inside the window. */
  while (mywindow)
    {
      xev.window = mywindow;
      XTranslateCoordinates (wm->xdpy,
                             xev.root, xev.window,
                             xev.x_root, xev.y_root,
                             &xev.x, &xev.y,
                             &mywindow);
    }
  XSendEvent(wm->xdpy, xev.window, True, 0, (XEvent *)&xev);
}

static void
hd_home_desktop_do_motion (HdHome *home,
                           int     x,
                           int     y)
{
  HdHomePrivate   *priv = home->priv;
  HdHomeDrag *drag_item;
  gdouble time;
  gint drag_distance;
  GList  *list;
  MBWindowManagerClient *live_bg;

  /* If the callback is 0, we're getting events after we got a do_release.
   * This is because clutter has stored up the events, and we're called from
   * hd_home_applet_motion instead. In this case just ignore, or we end up
   * in the wrong position (bug 127320) */
  if (!priv->desktop_motion_cb)
    return;

  /* MotionNotify is forwarded to any live background */
  live_bg = hd_home_view_container_get_live_bg (
                  HD_HOME_VIEW_CONTAINER (priv->view_container));
  if (!live_bg)
    live_bg = hd_home_view_get_live_bg (HD_HOME_VIEW(
                                             hd_home_get_current_view (home)));
  if (live_bg)
    hd_home_live_bg_emit_button1_event (home, live_bg->window->xwindow,
                                        x, y, MotionNotify);

  drag_item = g_malloc(sizeof(HdHomeDrag));
  drag_item->period = g_timer_elapsed(priv->last_move_time, NULL);
  g_timer_reset(priv->last_move_time);

  if(!STATE_IS_PORTRAIT(hd_render_manager_get_state ())
        || !priv->vertical_scrolling )
    { /* Scrolling in landscape mode */
      drag_item->x = x - priv->last_x;
      priv->cumulative_x += drag_item->x;

      DRAG_DEBUG("drag motion %dms, %dx%d -> %d", (int)(drag_item->period*1000),
                 x, y, drag_item->x);

      /* Remove any items older than a certain age */
      time = 0;
      drag_distance = 0;
      list = priv->drag_list;
      while (list)
        {
          GList *next = list->next;
          /* '&& next' ensures that we always average at least one previous
           * movement event to get our velocity, even if it is too old. */
          if (time > HDH_PAN_VELOCITY_HISTORY && next) {
            g_free (list->data);
            priv->drag_list = g_list_delete_link(priv->drag_list, list);
          }
          else
            {
              HdHomeDrag *drag = (HdHomeDrag*)list->data;
              time += drag->period;
              drag_distance += ABS(drag->x);
            }
          list = next;
        }
      /* Add new item to drag list */
      priv->drag_list = g_list_prepend(priv->drag_list, drag_item);
      /* Set velocity */
      time += drag_item->period;
      drag_distance += ABS(drag_item->x);
      if (time > 0)
        priv->velocity_x = drag_distance / time;
      else
        priv->velocity_x = 0;
      if (priv->cumulative_x < 0)
        priv->velocity_x = -priv->velocity_x;

      if (!priv->moved_over_threshold &&
          ABS (priv->cumulative_x) > HDH_PAN_THRESHOLD)
        {
          priv->moved_over_threshold = TRUE;
          if (priv->press_timeout)
            priv->press_timeout = (g_source_remove (priv->press_timeout), 0);
  
          /* Remove initial jump caused by the threshold */
          if (priv->cumulative_x > 0)
            priv->cumulative_x -= HDH_PAN_THRESHOLD;
          else
            priv->cumulative_x += HDH_PAN_THRESHOLD;
        }

      if (priv->moved_over_threshold)
        {
          /* unfocus any applet in case we start panning */
          mb_wm_client_focus (MB_WM_COMP_MGR (priv->comp_mgr)->wm->desktop);
          hd_home_view_container_set_offset (
                          HD_HOME_VIEW_CONTAINER (priv->view_container),
                          CLUTTER_UNITS_FROM_DEVICE (priv->cumulative_x));
        }
 
      priv->last_x = x;
    }
  else
    { /* Scrolling in portrait mode */
      drag_item->y = y - priv->last_y;
      priv->cumulative_y += drag_item->y;
  
      DRAG_DEBUG("drag motion %dms, %dx%d -> %d", (int)(drag_item->period*1000),
                 x, y, drag_item->x);

      /* Remove any items older than a certain age */
      time = 0;
      drag_distance = 0;
      list = priv->drag_list;
      while (list)
        {
          GList *next = list->next;
          /* '&& next' ensures that we always average at least one previous
           * movement event to get our velocity, even if it is too old. */
          if (time > HDH_PAN_VELOCITY_HISTORY && next) {
            g_free (list->data);
            priv->drag_list = g_list_delete_link(priv->drag_list, list);
          }
          else
            {
              HdHomeDrag *drag = (HdHomeDrag*)list->data;
              time += drag->period;
              drag_distance += ABS(drag->y);
            }
          list = next;
        }
      /* Add new item to drag list */
      priv->drag_list = g_list_prepend(priv->drag_list, drag_item);
      /* Set velocity */
      time += drag_item->period;
      drag_distance += ABS(drag_item->y);
      if (time > 0)
        priv->velocity_y = drag_distance / time;
      else
        priv->velocity_y = 0;
      if (priv->cumulative_y < 0)
        priv->velocity_y = -priv->velocity_y;
 
      if (!priv->moved_over_threshold &&
          ABS (priv->cumulative_y) > HDH_PAN_THRESHOLD)
        {
          priv->moved_over_threshold = TRUE;
          if (priv->press_timeout)
            priv->press_timeout = (g_source_remove (priv->press_timeout), 0);
  
          /* Remove initial jump caused by the threshold */
          if (priv->cumulative_y > 0)
            priv->cumulative_y -= HDH_PAN_THRESHOLD;
          else
            priv->cumulative_y += HDH_PAN_THRESHOLD;
        }

      if (priv->moved_over_threshold)
        {
          /* unfocus any applet in case we start panning */
          mb_wm_client_focus (MB_WM_COMP_MGR (priv->comp_mgr)->wm->desktop);
          hd_home_view_container_set_offset (
                          HD_HOME_VIEW_CONTAINER (priv->view_container),
                          CLUTTER_UNITS_FROM_DEVICE (priv->cumulative_y));
        }

      priv->last_y = y;
    }
}

static void
hd_home_desktop_motion (XButtonEvent *xev, void *userdata)
{
  HdHome *home = userdata;

  g_debug ("%s. (x, y) = (%d, %d)", __FUNCTION__, xev->x, xev->y);

  hd_home_desktop_do_motion (home, xev->x, xev->y);
}

static void
hd_home_desktop_do_release (HdHome *home)
{
  HdHomePrivate *priv = home->priv;
  MBWindowManager *wm = MB_WM_COMP_MGR (priv->comp_mgr)->wm;
  int cumulative_value;
  int velocity_value;

  /*g_debug("%s:", __FUNCTION__);*/
  DRAG_DEBUG("drag release");

  if (priv->press_timeout)
    priv->press_timeout = (g_source_remove (priv->press_timeout), 0);

  if (priv->desktop_motion_cb)
    mb_wm_main_context_x_event_handler_remove (wm->main_ctx,
					       MotionNotify,
					       priv->desktop_motion_cb);

  priv->desktop_motion_cb = 0;
  /* Free drag history */
  g_list_foreach(priv->drag_list, (GFunc)g_free, 0);
  g_list_free(priv->drag_list);
  priv->drag_list = 0;

  if(STATE_IS_PORTRAIT(hd_render_manager_get_state ())
      && priv->vertical_scrolling)
    {
      cumulative_value = priv->cumulative_y;
      velocity_value = priv->velocity_y;
    }
  else
    {
      cumulative_value = priv->cumulative_x;
      velocity_value = priv->velocity_x;
    }

  if (!priv->long_press && priv->moved_over_threshold)
    {
      if (ABS (cumulative_value) >= PAN_NEXT_PREVIOUS_PERCENTAGE * HD_COMP_MGR_LANDSCAPE_WIDTH) /* */
        {
          if (cumulative_value > 0)
            {
              hd_home_view_container_scroll_to_previous (HD_HOME_VIEW_CONTAINER (priv->view_container), velocity_value);
              DRAG_DEBUG("drag to_previous, vel=%d", velocity_value);
            }
          else
            {
              hd_home_view_container_scroll_to_next (HD_HOME_VIEW_CONTAINER (priv->view_container), velocity_value);
              DRAG_DEBUG("drag to_next, vel=%d", velocity_value);
            }
        }
      else
        {
          hd_home_view_container_scroll_back (HD_HOME_VIEW_CONTAINER (priv->view_container), velocity_value);
          DRAG_DEBUG("drag scroll_back, vel=%d", velocity_value);
        }
    }
  else if (!priv->long_press && STATE_IS_HOME(hd_render_manager_get_state()) &&
           priv->initial_x == -1 &&
           priv->initial_y == -1)
    {
      /*
       * If the button was not pressed over an applet we start up the edit
       * button.
       */
      hd_home_show_edit_button (home);
    }

  priv->cumulative_x = 0;
  priv->cumulative_y = 0;
  priv->moved_over_threshold = FALSE;
  priv->long_press = FALSE;
}

static gboolean
press_timeout_cb (gpointer data)
{
  HdHome *home = data;
  HdHomePrivate *priv = home->priv;

  if (priv->press_timeout)
    priv->press_timeout = 0;

  priv->long_press = TRUE;

	if(STATE_IS_PORTRAIT (hd_render_manager_get_state()))
	  hd_render_manager_set_state(HDRM_STATE_HOME_EDIT_PORTRAIT);
	else
	  hd_render_manager_set_state(HDRM_STATE_HOME_EDIT);

  if (priv->pressed_applet)
    do_applet_release (home,
                       priv->pressed_applet,
                       NULL);
  else
    hd_home_desktop_do_release (home);

  return FALSE;
}

static Bool
hd_home_desktop_do_press (HdHome *home,
                          int     x,
                          int     y)
{
  HdHomePrivate *priv = home->priv;
  MBWindowManager *wm = MB_WM_COMP_MGR (priv->comp_mgr)->wm;
  GSList *applets, *a;
  gboolean focus_applet_hit = FALSE, applet_hit = FALSE;

  DRAG_DEBUG("drag press %dx%d", x, y);

  if (priv->desktop_motion_cb)
    {
      mb_wm_main_context_x_event_handler_remove (wm->main_ctx,
						 MotionNotify,
						 priv->desktop_motion_cb);

      priv->desktop_motion_cb = 0;
    }

  /* if the press landed outside all focus-wanting applets, set focus to the
   * desktop window, unfocusing any applet */
  applets = hd_home_view_get_all_applets (
                  HD_HOME_VIEW (hd_home_get_current_view (home)));
  for (a = applets; a; a = a->next)
    {
      MBWMCompMgrClient *cc = a->data;
      MBWindowManagerClient *c;
      if (cc && (c = cc->wm_client) &&
          c->frame_geometry.x <= x &&
          c->frame_geometry.y <= y &&
          c->frame_geometry.x + c->frame_geometry.width >= x &&
          c->frame_geometry.y + c->frame_geometry.height >= y)
        {
          applet_hit = TRUE;
          if (mb_wm_client_want_focus (c))
            {
              focus_applet_hit = TRUE;
              break;
            }
        }
    }
  g_slist_free (applets);
  if (!focus_applet_hit)
    {
      /* g_printerr ("%s: focus the desktop\n", __func__); */
      mb_wm_client_focus (wm->desktop);
    }

  if (!applet_hit)
    {
      MBWindowManagerClient *live_bg;
      live_bg = hd_home_view_container_get_live_bg (
                    HD_HOME_VIEW_CONTAINER (priv->view_container));
      if (!live_bg)
        live_bg = hd_home_view_get_live_bg (HD_HOME_VIEW(
                                             hd_home_get_current_view (home)));
      if (live_bg)
        hd_home_live_bg_emit_button1_event (home, live_bg->window->xwindow,
                                            x, y, ButtonPress);
    }

  priv->long_press = FALSE;
  if (priv->press_timeout)
    priv->press_timeout = (g_source_remove (priv->press_timeout), 0);
  priv->press_timeout = g_timeout_add_seconds (LONG_PRESS_DUR,
                                               press_timeout_cb,
                                               home);

  priv->last_x = x;
  priv->cumulative_x = 0;
  priv->velocity_x = 0;
  priv->last_y = y;
  priv->cumulative_y = 0;
  priv->velocity_y = 0;
  g_timer_reset(priv->last_move_time);
  /* Make sure drag history is clear */
  g_list_foreach(priv->drag_list, (GFunc)g_free, 0);
  g_list_free(priv->drag_list);
  priv->drag_list = 0;

  priv->desktop_motion_cb =
    mb_wm_main_context_x_event_handler_add (wm->main_ctx,
					    priv->desktop,
					    MotionNotify,
					    (MBWMXEventFunc)
					    hd_home_desktop_motion,
					    home);
  return True;
}

static Bool
hd_home_desktop_release (XButtonEvent *xev, void *userdata)
{
  HdHome *home = userdata;
  HdHomePrivate *priv = home->priv;
  MBWindowManagerClient *live_bg;

  g_debug ("%s. (x, y) = (%d, %d)", __FUNCTION__, xev->x, xev->y);

  hd_home_desktop_do_motion (home, xev->x, xev->y);
  hd_home_desktop_do_release (home);

  live_bg = hd_home_view_container_get_live_bg (
                    HD_HOME_VIEW_CONTAINER (priv->view_container));
  if (!live_bg)
    live_bg = hd_home_view_get_live_bg (HD_HOME_VIEW(
                                          hd_home_get_current_view (home)));
  if (live_bg)
    hd_home_live_bg_emit_button1_event (home, live_bg->window->xwindow,
                                        xev->x, xev->y, ButtonRelease);

  return True;
}

static Bool
hd_home_desktop_press (XButtonEvent *xev, void *userdata)
{
  HdHome *home = userdata;

  g_debug ("%s. (x, y) = (%d, %d)", __FUNCTION__, xev->x, xev->y);

  return hd_home_desktop_do_press (home, xev->x, xev->y);
}

static gboolean
is_russian_cyrillic_keyboard (void)
{
  GConfClient *client = gconf_client_get_default ();
  char *kb_layout;
  gboolean kb_level_shifted;
  gboolean russian_cyrillic;

  kb_layout = gconf_client_get_string (client,
                                       "/apps/osso/inputmethod/int_kb_layout",
                                       NULL);
  kb_level_shifted = gconf_client_get_bool (client,
                                            "/apps/osso/inputmethod/int_kb_level_shifted",
                                            NULL);

  russian_cyrillic = (g_strcmp0 (kb_layout, "ru") == 0) && kb_level_shifted;

  g_object_unref (client);
  g_free (kb_layout);

  return russian_cyrillic;
}

static guint
get_keyval_for_russian_cyrillic_keyboard (GdkKeymap       *keymap,
                                          guint            keycode,
                                          GdkModifierType  state)
{
  GdkKeymapKey keymap_key;

  gdk_keymap_translate_keyboard_state (keymap,
                                       keycode,
                                       state,
                                       0,
                                       NULL,
                                       &keymap_key.group,
                                       &keymap_key.level,
                                       NULL);

  keymap_key.keycode = keycode;
  keymap_key.level += 4;

  return gdk_keymap_lookup_key (keymap,
                                &keymap_key);
}

static void
hd_home_desktop_key_press (XKeyEvent *xev, void *userdata)
{
  static gboolean proxies_initialised = FALSE;
  HdHome *home = userdata;
  HdHomePrivate *priv = home->priv;
  GdkDisplay *display = gdk_display_get_default ();
  GdkKeymap *keymap = gdk_keymap_get_for_display (display);
  guint keyval;
  guint32 unicode;
  unsigned pretend_fn;
  time_t now;

  if (!proxies_initialised)
    {
      update_dbus_interfaces_from_gconf (home, HD_HOME_GCONF_UPDATE_BOTH);
      proxies_initialised = TRUE;
    }

  /* We don't care if Ctrl is pressed. */
  if (xev->state & ControlMask)
    {
      hd_home_reset_fn_state (home);
      return;
    }

  /* g_debug ("%s, display: %p, keymap: %p", __FUNCTION__, display, keymap); */

  if (hd_render_manager_get_state () == HDRM_STATE_LAUNCHER)
    {
      int keycode = xev->keycode;
      int delta = 0;

      if (keycode > 23 && keycode < 29)
        delta = 24;
      else if (keycode > 37 && keycode < 43)
        delta = 33;
      else if (keycode > 51 && keycode < 57)
        delta = 42;
      else if (keycode == 22)  /* Backspace sends -1, which means up one level */
        delta = 23;

      if (delta && conf_enable_launcher_navigator_accel)
        {
          hd_launcher_activate (keycode - delta);
        }
      else if (conf_enable_dbus_launcher_navigator)
        {
          char s[16];
          gdk_keymap_translate_keyboard_state (keymap,
                                               keycode,
                                               xev->state,
                                               0,
                                               &keyval,
                                               NULL, NULL, NULL);
          sprintf (s, "%i", keyval + 1024);
          hd_dbus_send_event (s);
        }
    }

  if (STATE_IS_TASK_NAV (hd_render_manager_get_state ()))
    {
      int keycode = xev->keycode;
      int delta = 0;
      int row = 0;

      if (keycode > 23 && keycode < 29)
        {
          delta = 24;
        }
      else if (keycode > 37 && keycode < 43)
        {
          delta = 38;
          row = 1;
        }
      else if (keycode > 51 && keycode < 57)
        {
          delta = 52;
          row = 2;
        }

      if (delta && conf_enable_launcher_navigator_accel)
        {
          time (&now);

          if (xev->state & (FN_MODIFIER | ShiftMask) ||
              (difftime (now, priv->last_fn_time) < 4 &&
               (priv->fn_state || priv->shift_state)))
            {
              hd_task_navigator_activate (keycode - delta, row, 1);
              hd_home_reset_fn_state (home);
            }
          else
            {
              hd_task_navigator_activate (keycode - delta, row, 0);
            }
        }
      else if (conf_enable_dbus_launcher_navigator)
        {
          char s[16];

          gdk_keymap_translate_keyboard_state (keymap,
                                               keycode,
                                               xev->state,
                                               0,
                                               &keyval,
                                               NULL, NULL, NULL);
          sprintf (s, "%i", keyval + 2048);
          hd_dbus_send_event (s);
        }
    }

  if (STATE_IS_HOME (hd_render_manager_get_state ()))
    {
      gdk_keymap_translate_keyboard_state (keymap,
                                           xev->keycode,
                                           xev->state,
                                           0,
                                           &keyval,
                                           NULL, NULL, NULL);
      g_warning ("kv=%i   %i %i", keyval, GDK_Left, GDK_Right);

      if (keyval == GDK_Left)
        {
          if (!hd_home_view_container_is_scrolling (HD_HOME_VIEW_CONTAINER (priv->view_container)))
            hd_home_view_container_scroll_to_previous (HD_HOME_VIEW_CONTAINER (priv->view_container), 20000);
          return;
        }

      if (keyval == GDK_Right)
        {
          if (!hd_home_view_container_is_scrolling (HD_HOME_VIEW_CONTAINER (priv->view_container)))
            hd_home_view_container_scroll_to_next (HD_HOME_VIEW_CONTAINER (priv->view_container), -20000);
          return;
        }

      if (conf_enable_home_contacts_phone)
        {
          /* First check how long has it been since last key press.
           * If more than n sec, reset. */
          time (&now);
          if (difftime (now, priv->last_key_time) >= HD_HOME_KEY_PRESS_TIMEOUT)
            priv->key_sent = KEY_SENT_NONE;

          priv->last_key_time = now;

          /* If we're not at home, check if we should send keys anyway
           * because we've already sent some to Contacts or CallUI */
          if (!STATE_ALLOW_CALL_FROM_HOME (hd_render_manager_get_state ()) &&
              (priv->key_sent == KEY_SENT_NONE))
            {
              goto handle_fn_shift;
            }

          if (priv->key_sent == KEY_SENT_CALLUI)
            pretend_fn = FN_MODIFIER;
          else
            pretend_fn = priv->fn_state != FN_STATE_NONE ? FN_MODIFIER : 0;

          if (is_russian_cyrillic_keyboard () && xev->keycode != 37)
            {
              keyval = get_keyval_for_russian_cyrillic_keyboard (keymap,
                                                                 xev->keycode,
                                                                 xev->state | pretend_fn);
            }
          else
            {
              gdk_keymap_translate_keyboard_state (keymap,
                                                   xev->keycode,
                                                   xev->state | pretend_fn,
                                                   0,
                                                   &keyval,
                                                   NULL,
                                                   NULL,
                                                   NULL);
            }

          g_debug ("%s. keycode: %u, state: %u, keyval: %u", __FUNCTION__,
                   xev->keycode, xev->state, keyval);

          unicode = gdk_keyval_to_unicode (keyval);
          if (priv->key_sent == KEY_SENT_NONE)
            {
              if (g_unichar_isdigit (unicode) ||
                  unicode == '#' || unicode == '*' || unicode == '+')
                {
                  priv->key_sent = KEY_SENT_CALLUI;
                }
              else if (g_unichar_isalpha (unicode))
                {
                  priv->key_sent = KEY_SENT_ADDRESSBOOK;
                }
            }

          if (priv->key_sent == KEY_SENT_CALLUI)
            {
              char buffer[8] = {0,};

              g_unichar_to_utf8 (unicode, buffer);

              g_debug ("%s, digit: keyval: %u, unicode: %u, buffer: %s",
                       __FUNCTION__, keyval, unicode, buffer);

              if (priv->call_ui_proxy)
                {
                  dbus_g_proxy_call_no_reply (priv->call_ui_proxy,
                                              priv->callui_interface[2],
                                              G_TYPE_STRING, buffer, G_TYPE_INVALID);
                }
            }
          else if (priv->key_sent == KEY_SENT_ADDRESSBOOK)
            {
              char buffer[8] = {0,};

              g_unichar_to_utf8 (unicode, buffer);

              g_debug ("%s, letter: keyval: %u, unicode: %u, buffer: %s",
                       __FUNCTION__, keyval, unicode, buffer);

              if (priv->addressbook_proxy)
                {
                  dbus_g_proxy_call_no_reply (priv->addressbook_proxy,
                                              priv->abook_interface[2],
                                              G_TYPE_STRING, buffer, G_TYPE_INVALID);
                }
            }
        }
      else if (conf_enable_dbus_launcher_navigator)
        {
          char s[16];
          gdk_keymap_translate_keyboard_state (keymap,
                                               xev->keycode,
                                               xev->state,
                                               0,
                                               &keyval,
                                               NULL, NULL, NULL);
          sprintf (s, "%i", keyval + 4096);
          hd_dbus_send_event (s);
        }
    }

handle_fn_shift:
  if (xev->state & FN_MODIFIER)
    {
      priv->fn_state = FN_STATE_NONE;
      priv->ignore_next_fn_release = TRUE;
    }
  else if (priv->fn_state == FN_STATE_NEXT)
    {
      priv->fn_state = FN_STATE_NONE;
      g_debug ("%s, FN state: %d", __FUNCTION__, priv->fn_state);
    }

  if (xev->state & ShiftMask)
    {
      priv->shift_state = FN_STATE_NONE;
      priv->ignore_next_shift_release = TRUE;
    }
  else if (priv->shift_state == FN_STATE_NEXT)
    {
      priv->shift_state = FN_STATE_NONE;
      g_debug ("%s, SHIFT state: %d", __FUNCTION__, priv->shift_state);
    }

  if (XkbKeycodeToKeysym (clutter_x11_get_default_display (), xev->keycode, 0, 0) == FN_KEY)
    priv->ignore_next_fn_release = FALSE;
  else if (XkbKeycodeToKeysym (clutter_x11_get_default_display (), xev->keycode, 0, 0) == GDK_Shift_L)
    priv->ignore_next_shift_release = FALSE;
}

static void
hd_home_desktop_key_release (XKeyEvent *xev, void *userdata)
{
  HdHome *home = userdata;
  HdHomePrivate *priv = home->priv;


  /* Ignore keys if not at home or launching an app that wants them. */
  if (!STATE_ALLOW_CALL_FROM_HOME (hd_render_manager_get_state ()) &&
      (priv->key_sent == KEY_SENT_NONE) && (!STATE_IS_TASK_NAV(hd_render_manager_get_state())))
      return;

  if((XkbKeycodeToKeysym(clutter_x11_get_default_display(), xev->keycode, 0, 0) == GDK_Control_L) &&
	(STATE_IS_TASK_NAV(hd_render_manager_get_state())) &&
	(conf_ctrl_backspace_in_tasknav==5) && in_alt_tab) {
	  in_alt_tab=FALSE;
	  hd_task_navigator_activate(0, 0, 0);
  }

  if(xev->state & ControlMask) {
	  priv->fn_state = FN_STATE_NONE;
	  priv->shift_state = FN_STATE_NONE;
	  priv->ignore_next_fn_release = TRUE;
	  priv->ignore_next_shift_release = TRUE;
      return;
  }

  if(XkbKeycodeToKeysym(clutter_x11_get_default_display(), xev->keycode, 0, 0) == FN_KEY) {
  if (priv->ignore_next_fn_release)
    priv->ignore_next_fn_release = FALSE;
  else if (priv->fn_state == FN_STATE_NONE)
    priv->fn_state = FN_STATE_NEXT;
  else if (priv->fn_state == FN_STATE_NEXT)
    priv->fn_state = FN_STATE_LOCKED;
  else
    priv->fn_state = FN_STATE_NONE;
	  if(priv->fn_state) {
		 if (STATE_IS_TASK_NAV(hd_render_manager_get_state())) {
		 	hd_home_show_edit_button(home);
		 	time(&priv->last_fn_time);
	  	} else {
			hd_home_reset_fn_state(home);
		}
	  }
  }
  if(XkbKeycodeToKeysym(clutter_x11_get_default_display(), xev->keycode, 0, 0) == GDK_Shift_L) {
	  if (priv->ignore_next_shift_release)
	    priv->ignore_next_shift_release = FALSE;
	  else if (priv->shift_state == FN_STATE_NONE)
	    priv->shift_state = FN_STATE_NEXT;
	  else if (priv->shift_state == FN_STATE_NEXT)
	    priv->shift_state = FN_STATE_LOCKED;
	  else
	    priv->shift_state = FN_STATE_NONE;
	  if(priv->shift_state) {
		 if (STATE_IS_TASK_NAV(hd_render_manager_get_state())) {
		 	hd_home_show_edit_button(home);
		 	time(&priv->last_fn_time);
	  	} else {
			hd_home_reset_fn_state(home);
		}
  	  }
  }
  home->priv->ignore_next_fn_release = TRUE;
}

static void
hd_home_reset_fn_state (HdHome *home)
{
  home->priv->fn_state = FN_STATE_NONE;
  home->priv->ignore_next_fn_release = TRUE;
  home->priv->shift_state = FN_STATE_NONE;
  home->priv->ignore_next_shift_release = FALSE;
}

/*
 * Create the loading screenshot of the application of @xwin which will
 * be put up the the application is started next or remove it.  If the
 * application already has a screenshot it's retained and we don't create
 * a new one.  If @take was requested returns whether a new screenshot
 * was taken, otherwise whether the screenshot was removed successfully.
 * Does nothing if @xwin doesn't have an application we know about.
 */
static gboolean
take_screenshot (MBWindowManager *wm, Window xwin, gboolean take)
{
  MBWindowManagerClient *client;
  HdLauncherApp *launcher_app;
  const char *service_name;
  char *filename;
  gboolean isok;

  client = mb_wm_managed_client_from_xwindow (wm, xwin);
  if (!client || !client->window)
    return FALSE;

  launcher_app = hd_comp_mgr_client_get_launcher (
                                HD_COMP_MGR_CLIENT (client->cm_client));
  if (!launcher_app)
    {
      g_warning ("Window 0x%lx did not have an application associated"
                 " with it", client->window->xwindow);
      return FALSE;
    }

  service_name = hd_launcher_app_get_service (launcher_app);
  if (!service_name || strchr (service_name, '/') || service_name[0] == '.')
    {
      g_warning ("Window 0x%lx has no sane service name",
                 client->window->xwindow);
      return FALSE; /* daft service name, don't get a loading pic */
    }

  filename = g_strdup_printf ("%s/.cache/launch", getenv("HOME"));
  g_mkdir_with_parents (filename, 0770);
  g_free (filename);

  isok = FALSE;

  if (STATE_IS_PORTRAIT(hd_render_manager_get_state()))
    filename = g_strdup_printf ("%s/.cache/launch/%s_portrait.pvr",
                                getenv("HOME"), service_name);
  else
    filename = g_strdup_printf ("%s/.cache/launch/%s.pvr",
                                getenv("HOME"), service_name);

  if (take)
  {
    Pixmap                          pixmap;
    GdkPixbuf                      *pixbuf;
    guint                           depth;
    guint                           width, height;
    ClutterActor                   *actor, *texture;

    if (g_file_test (filename, G_FILE_TEST_EXISTS))
      {
        g_debug ("%s: not creating '%s', already exists",
                 __func__, filename);
        g_free (filename);
        return FALSE;
      }

    actor = mb_wm_comp_mgr_clutter_client_get_actor (
                     MB_WM_COMP_MGR_CLUTTER_CLIENT (client->cm_client));
    texture = clutter_group_get_nth_child (CLUTTER_GROUP (actor), 0);
    g_object_get (texture,
                  "pixmap", &pixmap,
                  "pixmap-depth", &depth,
                  "pixmap-width", &width,
                  "pixmap-height", &height,
                  NULL);

    /* We could call mb_wm_theme_get_decor_dimensions() here and take out
     * the titlebar, etc, but in practice these aren't drawn on the loading
     * image so we have to keep them on. */
    pixbuf = gdk_pixbuf_xlib_get_from_drawable (NULL, pixmap,
                             xlib_rgb_get_cmap(), xlib_rgb_get_visual(),
                             0, 0, 0, 0, width, height);
    isok = hd_pvr_texture_save (filename, pixbuf, NULL);
    g_object_unref (pixbuf);
  } else
    isok = unlink (filename) == 0;

  g_free (filename);
  return isok;
}

void
hd_home_set_live_background (HdHome *home, MBWindowManagerClient *client)
{
  HdHomePrivate *priv = home->priv;
  hd_home_view_container_set_live_bg (HD_HOME_VIEW_CONTAINER (
                                           priv->view_container), client);
}

/* Called when a client message is sent to the root window. */
static void
root_window_client_message (XClientMessageEvent *event, HdHome *home)
{
  HdHomePrivate *priv = home->priv;
  MBWindowManager *wm = MB_WM_COMP_MGR (priv->comp_mgr)->wm;
  HdCompMgr     *hmgr = HD_COMP_MGR (home->priv->comp_mgr);

#if 0 //  FIXME should we really support NET_CURRENT_DESKTOP?
  if (event->message_type == wm->atoms[MBWM_ATOM_NET_CURRENT_DESKTOP])
    {
      hd_home_view_container_set_current_view (
                          HD_HOME_VIEW_CONTAINER (priv->view_container),
                          event->data.l[0]);
      return;
    }
#endif

  if (event->message_type == hd_comp_mgr_get_atom (hmgr,
                                     HD_ATOM_HILDON_LOADING_SCREENSHOT))
    {
      XEvent reply;

      /* Tell the client when the operation is complete. */
      reply.xclient.type = ClientMessage;
      reply.xclient.window = event->data.l[1];
      reply.xclient.message_type = hd_comp_mgr_get_atom (hmgr,
                                   HD_ATOM_HILDON_LOADING_SCREENSHOT);
      reply.xclient.format = 32;
      reply.xclient.data.l[0] = event->serial;
      reply.xclient.data.l[1] = take_screenshot (wm, event->data.l[1],
                                                 event->data.l[0] != 1);

      mb_wm_util_async_trap_x_errors (wm->xdpy);
      XSendEvent (wm->xdpy, reply.xclient.window, False,
                  NoEventMask, &reply);
      XFlush (wm->xdpy);
      mb_wm_util_async_untrap_x_errors ();
    }
}

static ClutterActor *
hd_home_create_edit_button (void)
{
  ClutterActor *edit_button;
  ClutterActor *bg_left, *bg_center, *bg_right, *icon;
  ClutterGeometry geom = { 0, };

  edit_button = clutter_group_new ();

  /* Load textures */
  bg_left = hd_clutter_cache_get_texture (HD_THEME_IMG_BUTTON_LEFT_HALF, TRUE);
  bg_right = hd_clutter_cache_get_texture (HD_THEME_IMG_BUTTON_RIGHT_HALF, TRUE);
  icon = hd_clutter_cache_get_texture (HD_THEME_IMG_EDIT_ICON, TRUE);

  /* Cut out the half of the texture */
  geom.width = clutter_actor_get_width (icon) / 4;
  geom.height = clutter_actor_get_height (icon);
  bg_center = hd_clutter_cache_get_sub_texture (HD_THEME_IMG_LEFT_ATTACHED, TRUE, &geom);

  /* Add textures to edit button */
  clutter_container_add (CLUTTER_CONTAINER (edit_button),
                         bg_left, bg_center, bg_right, icon, NULL);

  /* Layout textures */
  clutter_actor_set_position (bg_left, 0, 0);
  clutter_actor_set_position (bg_center, clutter_actor_get_width (bg_left), 0);
  clutter_actor_set_position (bg_right, clutter_actor_get_width (bg_left) + clutter_actor_get_width (bg_center), 0);

  /* Icon is centered on top the button */
  clutter_actor_set_position (icon,
                              (clutter_actor_get_width (edit_button) - clutter_actor_get_width (icon)) / 2,
                              0);

  return edit_button;
}

static ClutterActor *
create_edge_indicator (HdHome           *home,
                       ClutterContainer *edit_group,
                       const gchar      *name,
                       gint              x)
{
  ClutterActor *edge_indicator;

  edge_indicator = clutter_rectangle_new ();
  clutter_actor_set_name (edge_indicator, name);

  clutter_actor_set_size (edge_indicator,
                          HD_EDGE_INDICATION_WIDTH, HD_COMP_MGR_LANDSCAPE_HEIGHT);
  clutter_actor_set_position (edge_indicator, x, 0);

  clutter_container_add_actor (CLUTTER_CONTAINER (edit_group),
                               edge_indicator);
  clutter_actor_show (edge_indicator);

  clutter_actor_set_reactive (edge_indicator, FALSE);

  return edge_indicator;
}

static void
hd_home_constructed (GObject *object)
{
  HdHomePrivate   *priv = HD_HOME (object)->priv;
  ClutterActor    *edit_group;
  MBWindowManager *wm = MB_WM_COMP_MGR (priv->comp_mgr)->wm;
  XSetWindowAttributes attr;
  XWMHints         wmhints;
  pid_t            our_pid = getpid ();
  char             buf[HOST_NAME_MAX+1];

  clutter_actor_set_name (CLUTTER_ACTOR(object), "HdHome");

  priv->front = CLUTTER_GROUP(clutter_group_new());
  clutter_actor_set_name (CLUTTER_ACTOR(priv->front), "HdHome:front");
  clutter_container_add_actor (CLUTTER_CONTAINER (object),
                               CLUTTER_ACTOR(priv->front));

  edit_group = priv->edit_group = clutter_group_new ();
  clutter_actor_set_name (edit_group, "HdHome:edit_group");
  clutter_container_add_actor (CLUTTER_CONTAINER (priv->front), edit_group);
  clutter_actor_hide (edit_group);

  GConfClient *client = gconf_client_get_default ();
  priv->vertical_scrolling = gconf_client_get_bool (client, GCONF_KEY_SCROLL_VERTICAL, NULL);
  priv->portrait_wallpaper = gconf_client_get_bool (client, GCONF_KEY_PORTRAIT_WALLPAPER, NULL);
  g_object_unref (client);

  priv->view_container = hd_home_view_container_new (
                                                HD_COMP_MGR (priv->comp_mgr),
                                                CLUTTER_ACTOR (object));
  clutter_actor_set_name (priv->view_container, "HdHome:view_container");
  clutter_container_add_actor (CLUTTER_CONTAINER (object), priv->view_container);
  clutter_actor_set_size (CLUTTER_ACTOR (priv->view_container),
                          HD_COMP_MGR_LANDSCAPE_WIDTH,
                          HD_COMP_MGR_LANDSCAPE_HEIGHT);
  /* Eliminates black rectangles between views in portrait mode. */
  /* Visible with horizontal scrolling. */
  /* It should be disabled when vertical scrolling is in use. */
  if (!priv->vertical_scrolling)
    g_signal_connect_swapped(clutter_stage_get_default(), "notify::allocation",
                            G_CALLBACK(hd_home_resize_view_container),
                            priv->view_container);
  clutter_actor_show (priv->view_container);

  priv->edit_button = hd_home_create_edit_button ();
  clutter_actor_hide (priv->edit_button);
  clutter_actor_set_reactive (priv->edit_button, TRUE);

  g_signal_connect (priv->edit_button, "button-release-event",
		    G_CALLBACK (hd_home_edit_button_clicked),
		    object);

  priv->operator = clutter_group_new ();
  clutter_actor_set_name(priv->operator, "HdHome:operator");
  clutter_actor_show (priv->operator);
  clutter_actor_reparent (priv->operator, CLUTTER_ACTOR (object));
  clutter_actor_raise (priv->operator, priv->view_container);

  priv->edge_indication_left = create_edge_indicator (HD_HOME (object),
                                                      CLUTTER_CONTAINER (edit_group),
                                                      "HdHome:left_switch",
                                                      0);

  priv->edge_indication_right = create_edge_indicator (HD_HOME (object),
                                                       CLUTTER_CONTAINER (edit_group),
                                                       "HdHome:right_switch",
                                                       HD_COMP_MGR_LANDSCAPE_WIDTH - HD_EDGE_INDICATION_WIDTH);

  /* Set color of edge indicators */
  hd_home_theme_changed (HD_HOME (object));

  clutter_actor_lower_bottom (priv->view_container);

  /*
   * Create an InputOnly desktop window; we have a custom desktop client that
   * that will automatically wrap it, ensuring it is located in the correct
   * place.  Make this window focusable ("Input" WM hint) so
   * hd_home_desktop_key_press() will actually work.
   */
  attr.event_mask = MBWMChildMask |
    ButtonPressMask | ButtonReleaseMask |
    PointerMotionMask | ExposureMask |
    KeyPressMask | KeyReleaseMask;
  wmhints.input = True;
  wmhints.flags = InputHint;

  priv->desktop = XCreateWindow (wm->xdpy,
				 wm->root_win->xwindow,
				 0, 0,
				 HD_COMP_MGR_LANDSCAPE_WIDTH,
				 HD_COMP_MGR_LANDSCAPE_WIDTH,
				 0,
				 CopyFromParent,
				 InputOnly,
				 CopyFromParent,
				 CWEventMask,
				 &attr);
  XSetWMHints (wm->xdpy, priv->desktop, &wmhints);
  mb_wm_rename_window (wm, priv->desktop, "desktop");

  XChangeProperty (wm->xdpy, priv->desktop,
		   wm->atoms[MBWM_ATOM_NET_WM_WINDOW_TYPE],
		   XA_ATOM, 32, PropModeReplace,
		   (unsigned char *)
		   &wm->atoms[MBWM_ATOM_NET_WM_WINDOW_TYPE_DESKTOP],
		   1);

  XChangeProperty(wm->xdpy, priv->desktop,
		  wm->atoms[MBWM_ATOM_NET_WM_PID],
		  XA_CARDINAL, 32, PropModeReplace,
		  (unsigned char *)&our_pid,
		  1);

  if (gethostname (buf, sizeof(buf)) < 0)
  {
          g_warning ("%s: gethostname() failed", __func__);
          return;
  }

  XChangeProperty(wm->xdpy, priv->desktop,
		  wm->atoms[MBWM_ATOM_WM_CLIENT_MACHINE],
		  XA_STRING, 8, PropModeReplace,
		  (unsigned char *)buf,
		  strlen(buf)+1);

  XMapWindow (wm->xdpy, priv->desktop);

  mb_wm_main_context_x_event_handler_add (wm->main_ctx,
					  priv->desktop,
					  ButtonPress,
					  (MBWMXEventFunc)
					  hd_home_desktop_press,
					  object);

  mb_wm_main_context_x_event_handler_add (wm->main_ctx,
					  priv->desktop,
					  ButtonRelease,
					  (MBWMXEventFunc)
					  hd_home_desktop_release,
					  object);

  mb_wm_main_context_x_event_handler_add (wm->main_ctx,
					  priv->desktop,
					  KeyPress,
					  (MBWMXEventFunc)
					  hd_home_desktop_key_press,
					  object);

  mb_wm_main_context_x_event_handler_add (wm->main_ctx,
					  priv->desktop,
					  KeyRelease,
					  (MBWMXEventFunc)
					  hd_home_desktop_key_release,
					  object);

  mb_wm_main_context_x_event_handler_add (wm->main_ctx,
					  wm->root_win->xwindow,
					  ClientMessage,
					  (MBWMXEventFunc)
					  root_window_client_message,
					  object);
}

static void
update_dbus_interfaces_from_gconf (HdHome *home, HdHomeGconfUpdateMode what)
{
  int i;
  GSList *l, *l_start;
  HdHomePrivate *priv = home->priv;
  GConfClient *gconf_client = gconf_client_get_default ();
  DBusGConnection *connection;
  GError *error = NULL;

  connection = dbus_g_bus_get (DBUS_BUS_SESSION, &error);
  if (error)
    {
      g_error_free (error);
      return;
    }

  if (what == HD_HOME_GCONF_UPDATE_BOTH || what == HD_HOME_GCONF_UPDATE_ABOOK)
    {
      l_start = l = gconf_client_get_list (gconf_client,
                                           ADDRESSBOOK_GCONF_KEY,
                                           GCONF_VALUE_STRING,
                                           NULL);
      for (i = 0; l && i < 3; ++i, l = l->next)
        {
          if (priv->abook_interface[i])
            g_free (priv->abook_interface[i]);
          priv->abook_interface[i] = g_strdup (l->data);
        }

      g_slist_free (l_start);

      if (priv->addressbook_proxy)
        g_object_unref (priv->addressbook_proxy);

      priv->addressbook_proxy = dbus_g_proxy_new_for_name (connection,
                                                   priv->abook_interface[0],
                                                   priv->abook_interface[1],
                                                   priv->abook_interface[0]);
    }

  if (what == HD_HOME_GCONF_UPDATE_BOTH || what == HD_HOME_GCONF_UPDATE_CALLUI)
    {
      l_start = l = gconf_client_get_list (gconf_client,
                                           CALL_UI_GCONF_KEY,
                                           GCONF_VALUE_STRING,
                                           NULL);
      for (i = 0; l && i < 3; ++i, l = l->next)
        {
          if (priv->callui_interface[i])
            g_free (priv->callui_interface[i]);
          priv->callui_interface[i] = g_strdup (l->data);
        }

      g_slist_free (l_start);

      if (priv->call_ui_proxy)
        g_object_unref (priv->call_ui_proxy);

      priv->call_ui_proxy = dbus_g_proxy_new_for_name (connection,
                                                   priv->callui_interface[0],
                                                   priv->callui_interface[1],
                                                   priv->callui_interface[0]);
    }
}

static void
dbus_interface_notify_func (GConfClient *client,
                            guint        cnxn_id,
                            GConfEntry  *entry,
                            gpointer     user_data)
{
  HdHome *home = user_data;
  const char *key = gconf_entry_get_key (entry);

  if (g_strcmp0 (key, CALL_UI_GCONF_KEY) == 0)
    update_dbus_interfaces_from_gconf (home, HD_HOME_GCONF_UPDATE_CALLUI);
  else
    update_dbus_interfaces_from_gconf (home, HD_HOME_GCONF_UPDATE_ABOOK);
}

static void
hd_home_init (HdHome *self)
{
  HdHomePrivate *priv;
  DBusGConnection *connection;
  DBusGProxy *bus_proxy = NULL;
  guint result;
  GError *error = NULL;
  GConfClient *gconf_client;

  priv = self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, HD_TYPE_HOME,
                                                   HdHomePrivate);

  priv->initial_x = -1;
  priv->initial_y = -1;
  priv->last_move_time = g_timer_new();

  priv->show_edit_button_template =
          clutter_effect_template_new_for_duration (HDH_EDIT_BUTTON_DURATION,
                                                    CLUTTER_ALPHA_SINE_INC);
  priv->hide_edit_button_template =
          clutter_effect_template_new_for_duration (HDH_EDIT_BUTTON_DURATION,
                                                    CLUTTER_ALPHA_SINE_INC);

  /* Listen to gconf notifications */
  gconf_client = gconf_client_get_default ();
  gconf_client_add_dir (gconf_client,
                        "/apps/osso/hildon-desktop",
			GCONF_CLIENT_PRELOAD_NONE,
			NULL);

  /* Register to D-Bus */
  connection = dbus_g_bus_get (DBUS_BUS_SESSION, &error);
  if (error != NULL)
    {
      g_warning ("Failed to open connection to session bus: %s", error->message);
      g_error_free (error);

      goto cleanup;
    }

  /* Request the well known name from the Bus */
  bus_proxy = dbus_g_proxy_new_for_name (connection,
                                         DBUS_SERVICE_DBUS,
                                         DBUS_PATH_DBUS,
                                         DBUS_INTERFACE_DBUS);

  if (!org_freedesktop_DBus_request_name (bus_proxy,
                                          HD_HOME_DBUS_NAME,
                                          DBUS_NAME_FLAG_DO_NOT_QUEUE,
                                          &result,
                                          &error))
    {
      g_warning ("Could not register name: %s", error->message);
      g_error_free (error);

      goto cleanup;
    }

  if (result == DBUS_REQUEST_NAME_REPLY_EXISTS)
    goto cleanup;

  dbus_g_object_type_install_info (HD_TYPE_HOME,
                                   &dbus_glib_hd_home_object_info);

  dbus_g_connection_register_g_object (connection,
                                       HD_HOME_DBUS_PATH,
                                       G_OBJECT (self));

  g_debug ("%s registered to session bus at %s",
           HD_HOME_DBUS_NAME,
           HD_HOME_DBUS_PATH);

  gconf_client_notify_add (gconf_client, ADDRESSBOOK_GCONF_KEY,
                           dbus_interface_notify_func, self, NULL, NULL);
  gconf_client_notify_add (gconf_client, CALL_UI_GCONF_KEY,
                           dbus_interface_notify_func, self, NULL, NULL);

cleanup:
  if (bus_proxy != NULL)
    g_object_unref (bus_proxy);
}

static void
hd_home_dispose (GObject *object)
{
  HdHomePrivate *priv = HD_HOME(object)->priv;

  if (priv->last_move_time)
    {
      g_timer_destroy(priv->last_move_time);
      priv->last_move_time = 0;
    }

  if (priv->press_timeout)
    priv->press_timeout = (g_source_remove (priv->press_timeout), 0);

  G_OBJECT_CLASS (hd_home_parent_class)->dispose (object);
}

static void
hd_home_finalize (GObject *object)
{
  G_OBJECT_CLASS (hd_home_parent_class)->finalize (object);
}

static void
hd_home_set_property (GObject       *object,
		      guint         prop_id,
		      const GValue *value,
		      GParamSpec   *pspec)
{
  HdHomePrivate *priv = HD_HOME (object)->priv;

  switch (prop_id)
    {
    case PROP_COMP_MGR:
      priv->comp_mgr = g_value_get_pointer (value);
      break;
    case PROP_HDRM:
      g_signal_connect_swapped (g_value_get_pointer (value),
                                "notify::state",
                                G_CALLBACK (hd_home_reset_fn_state),
                                object);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
hd_home_get_property (GObject      *object,
		      guint         prop_id,
		      GValue       *value,
		      GParamSpec   *pspec)
{
  HdHomePrivate *priv = HD_HOME (object)->priv;

  switch (prop_id)
    {
    case PROP_COMP_MGR:
      g_value_set_pointer (value, priv->comp_mgr);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

/* FOR HDRM_STATE_HOME */
static void
_hd_home_do_normal_layout (HdHome *home)
{
  HdHomePrivate *priv = home->priv;

  clutter_actor_hide (priv->edit_group);
}

/* FOR HDRM_STATE_HOME_EDIT */
static void
_hd_home_do_edit_layout (HdHome *home)
{
  HdHomePrivate *priv = home->priv;

  _hd_home_do_normal_layout (home);

  clutter_actor_set_position (priv->edit_group, 0, 0);
  clutter_actor_show (priv->edit_group);

  update_edge_indication_visibility (home,
                                     EDGE_INDICATION_OPACITY_INVISIBLE,
                                     EDGE_INDICATION_OPACITY_INVISIBLE);
}

void
hd_home_update_layout (HdHome * home)
{
  /* FIXME: ideally all this should be done by HdRenderManager */
  HdHomePrivate *priv;
  guint i;

  if (!HD_IS_HOME(home))
    return;
  priv = home->priv;

  switch (hd_render_manager_get_state())
    {
    case HDRM_STATE_HOME:
    case HDRM_STATE_HOME_PORTRAIT:
      _hd_home_do_normal_layout(home);
      break;
    case HDRM_STATE_HOME_EDIT:
    case HDRM_STATE_HOME_EDIT_PORTRAIT:
    case HDRM_STATE_HOME_EDIT_DLG:
    case HDRM_STATE_HOME_EDIT_DLG_PORTRAIT:
      _hd_home_do_edit_layout(home);
      break;
    default:
      g_warning("%s: should only be called for HDRM_STATE_HOME.*",
                __FUNCTION__);
    }

  for (i = 0; i < MAX_VIEWS; i++)
    {
      ClutterActor *view;

      view = hd_home_view_container_get_view (HD_HOME_VIEW_CONTAINER (priv->view_container),
                                              i);
      hd_home_view_update_state (HD_HOME_VIEW (view));
    }
}

void
hd_home_remove_status_area (HdHome *home, ClutterActor *sa)
{
  g_debug ("hd_home_remove_status_area, sa=%p\n", sa);

  hd_render_manager_set_status_area (NULL);
}

void
hd_home_add_status_menu (HdHome *home, ClutterActor *sa)
{
  g_debug ("hd_home_add_status_menu, sa=%p\n", sa);

  hd_render_manager_set_status_menu (sa);
}

void
hd_home_remove_status_menu (HdHome *home, ClutterActor *sa)
{
  g_debug ("hd_home_remove_status_menu, sa=%p\n", sa);

  hd_render_manager_set_status_menu (NULL);
}

void
hd_home_add_status_area (HdHome *home, ClutterActor *sa)
{
  g_debug ("hd_home_add_status_area, sa=%p\n", sa);

  hd_render_manager_set_status_area(sa);
}


static void
hd_home_applet_emit_button_release_event (
		HdHome             *home,
		ClutterActor       *applet,
		int                 x,
		int                 y)
{
  HdHomePrivate      *priv = home->priv;
  MBWindowManager    *wm;
  MBWMCompMgrClient  *cclient;
  XButtonEvent        xev;
  Window              mywindow;

  wm = MB_WM_COMP_MGR(priv->comp_mgr)->wm;
  cclient = g_object_get_data (G_OBJECT (applet),
                               "HD-MBWMCompMgrClutterClient");
  /*
   * Emitting a button release event.
   */
  xev.type = ButtonRelease;
  xev.display = wm->xdpy;
  xev.window = MB_WM_CLIENT_XWIN(cclient->wm_client);
  xev.root = wm->root_win->xwindow;
  xev.subwindow = None;
  xev.time = CurrentTime;
  xev.x = x;
  xev.y = y;
  xev.x_root = x;
  xev.y_root = y;
  xev.state = Button1Mask;
  xev.button = Button1;
  xev.same_screen = True;

  /* We need to find the window inside the plugin. */
  XTranslateCoordinates (wm->xdpy,
		  xev.root, xev.window,
		  xev.x_root, xev.y_root,
		  &xev.x, &xev.y,
		  &mywindow);
  while (mywindow) {
    xev.window = mywindow;
    XTranslateCoordinates (wm->xdpy,
		  xev.root, xev.window,
		  xev.x_root, xev.y_root,
		  &xev.x, &xev.y,
		  &mywindow);
  }

  XSendEvent(wm->xdpy, xev.window, True,
	     0, (XEvent *)&xev);
}

static void
hd_home_applet_emit_button_press_event (
		HdHome             *home,
		ClutterActor       *applet,
		int                 x,
		int                 y)
{
  HdHomePrivate      *priv = home->priv;
  MBWindowManager    *wm;
  MBWMCompMgrClient  *cclient;
  XButtonEvent        xev;
  Window              mywindow;

  wm = MB_WM_COMP_MGR(priv->comp_mgr)->wm;
  cclient = g_object_get_data (G_OBJECT (applet),
                               "HD-MBWMCompMgrClutterClient");
  /*
   * Emitting a button press event.
   */
  xev.type = ButtonPress;
  xev.send_event = False;
  xev.display = wm->xdpy;
  xev.window = MB_WM_CLIENT_XWIN(cclient->wm_client);
  xev.root = wm->root_win->xwindow;
  xev.subwindow = None;
  xev.time = CurrentTime;
  xev.x = x;
  xev.y = y;
  xev.x_root = x;
  xev.y_root = y;
  xev.state = 0;
  xev.button = Button1;
  xev.same_screen = True;

  /* We need to find the window inside the plugin. */
  XTranslateCoordinates (wm->xdpy,
		  xev.root, xev.window,
		  xev.x_root, xev.y_root,
		  &xev.x, &xev.y,
		  &mywindow);
  while (mywindow) {
    xev.window = mywindow;
    XTranslateCoordinates (wm->xdpy,
		  xev.root, xev.window,
		  xev.x_root, xev.y_root,
		  &xev.x, &xev.y,
		  &mywindow);
  }

  XSendEvent(wm->xdpy, xev.window, True,
  	     0, (XEvent *)&xev);
}

static void
hd_home_applet_emit_leave_event (
		HdHome             *home,
		ClutterActor       *applet,
		int                 x,
		int                 y)
{
  HdHomePrivate      *priv = home->priv;
  MBWindowManager    *wm;
  MBWMCompMgrClient  *cclient;
  XCrossingEvent      xev;
  Window              mywindow;

  wm = MB_WM_COMP_MGR(priv->comp_mgr)->wm;
  cclient = g_object_get_data (G_OBJECT (applet),
                               "HD-MBWMCompMgrClutterClient");
  /*
   * Emitting a leave event.
   */
  xev.type = LeaveNotify;
  xev.display = wm->xdpy;
  xev.window = MB_WM_CLIENT_XWIN(cclient->wm_client);
  xev.root = wm->root_win->xwindow;
  xev.subwindow = None;
  xev.time = CurrentTime;
  xev.x = x;
  xev.y = y;
  xev.x_root = x;
  xev.y_root = y;
  xev.mode = NotifyNormal;
  xev.focus = False;
  xev.same_screen = True;

  /* We need to find the window inside the plugin. */
  XTranslateCoordinates (wm->xdpy,
		  xev.root, xev.window,
		  xev.x_root, xev.y_root,
		  &xev.x, &xev.y,
		  &mywindow);

  if (mywindow)
      xev.window = mywindow;

  xev.x = -10;
  xev.y = -10;
  xev.x_root = -10;
  xev.y_root = -10;

  XSendEvent(wm->xdpy, xev.window, True,
	     0, (XEvent *)&xev);
}

static void
hd_home_applet_emit_enter_event (
		HdHome             *home,
		ClutterActor       *applet,
		int                 x,
		int                 y)
{
  HdHomePrivate      *priv = home->priv;
  MBWindowManager    *wm;
  MBWMCompMgrClient  *cclient;
  XCrossingEvent      xev;
  Window              mywindow;

  wm = MB_WM_COMP_MGR(priv->comp_mgr)->wm;
  cclient = g_object_get_data (G_OBJECT (applet),
                               "HD-MBWMCompMgrClutterClient");
  /*
   * Emitting a enter event.
   */
  xev.type = EnterNotify;
  xev.display = wm->xdpy;
  xev.window = MB_WM_CLIENT_XWIN(cclient->wm_client);
  xev.root = wm->root_win->xwindow;
  xev.subwindow = None;
  xev.time = CurrentTime;
  xev.x = x;
  xev.y = y;
  xev.x_root = x;
  xev.y_root = y;
  xev.mode = NotifyNormal;
  xev.focus = False;
  xev.same_screen = True;

  /* We need to find the window inside the plugin. */
  XTranslateCoordinates (wm->xdpy,
		  xev.root, xev.window,
		  xev.x_root, xev.y_root,
		  &xev.x, &xev.y,
		  &mywindow);

  if (mywindow)
      xev.window = mywindow;

  XSendEvent(wm->xdpy, xev.window, True,
	     0, (XEvent *)&xev);
}

static gboolean
hd_home_client_owns_or_child_xwindow (MBWindowManagerClient *client,
                                      Window xwindow)
{
  Window *children = NULL, parent, root;
  unsigned int nchildren = 0;
  Status s;
  int i;
  gboolean found = FALSE;

  if (mb_wm_client_owns_xwindow (client, xwindow))
    return TRUE;

  mb_wm_util_async_trap_x_errors (client->wmref->xdpy);
  s = XQueryTree (client->wmref->xdpy, client->window->xwindow,
                  &root, &parent, &children, &nchildren);
  mb_wm_util_async_untrap_x_errors ();

  if (!s) return FALSE;

  /* TODO: should probably check grandchildren as well... */
  for (i = 0; i < nchildren; ++i)
    if (children[i] == xwindow)
      {
        found = TRUE;
        break;
      }

  if (children)
    XFree (children);

  return found;
}

static gboolean
hd_home_applet_press (ClutterActor       *applet,
		      ClutterButtonEvent *event,
		      HdHome             *home)
{
  HdHomePrivate   *priv = home->priv;
  MBWMCompMgrClient *cclient;
  MBWindowManagerClient *client;
  gboolean focus_will_be_assigned_to_this_applet_on_release = FALSE;

  /*
   * If we are in edit mode the HdHomeView will have to deal with this event.
   */
  if (STATE_IN_EDIT_MODE (hd_render_manager_get_state ()))
    return FALSE;

  g_debug ("%s. (x, y) = (%d, %d)", __FUNCTION__, event->x, event->y);

  /*
   * We always emit a button press event to animate it on the screen. Later we
   * can abort the click with a LeaveNotify event.
   */
  hd_home_applet_emit_enter_event (home, applet, event->x, event->y);

  /* send the press event to the applet unless it's wanting the focus
   * and focus is not yet assigned to it */
  cclient = g_object_get_data (G_OBJECT (applet),
                               "HD-MBWMCompMgrClutterClient");
  if (cclient && (client = cclient->wm_client) &&
      mb_wm_client_want_focus (client))
    {
      Window focused = None;
      int revert_to;
      MBWindowManager *wm;
      wm = MB_WM_COMP_MGR (priv->comp_mgr)->wm;
      XGetInputFocus (wm->xdpy, &focused, &revert_to);
      if (focused == None ||
          !hd_home_client_owns_or_child_xwindow (client, focused))
        focus_will_be_assigned_to_this_applet_on_release = TRUE;
    }

  if (!focus_will_be_assigned_to_this_applet_on_release)
    hd_home_applet_emit_button_press_event (home, applet, event->x, event->y);

  /*
   * We store the coordinates where the screen was touched. These values are
   * set only when applet was clicked, so we will know at release time if the
   * button was pressed on an applet.
   */
  priv->initial_x = event->x;
  priv->initial_y = event->y;

  clutter_grab_pointer_without_pick (applet);
  hd_home_desktop_do_press (home, event->x, event->y);

  priv->pressed_applet = applet;

  return TRUE;
}

static void
do_applet_release (HdHome             *home,
                   ClutterActor       *applet,
                   ClutterButtonEvent *event)
{
  HdHomePrivate   *priv = home->priv;

  priv->pressed_applet = NULL;

  hd_home_desktop_do_release (home);
  clutter_ungrab_pointer ();

  /*
   * If we have the initial coordinates the pointer was not moved over the
   * treshold to pan the desktop.
   */
  if (priv->initial_x > -1 && priv->initial_y > -1) {
      /*
       * If we are still inside the applet (and did not get the event because of
       * the pointer grab) we send a click to the applet window.
       */
      if (event && event->source == applet)
        {
          MBWMCompMgrClient *cclient;
          MBWindowManagerClient *client = NULL;
          gboolean applet_has_focus = FALSE;

          cclient = g_object_get_data (G_OBJECT (applet),
                                       "HD-MBWMCompMgrClutterClient");

          if (cclient && (client = cclient->wm_client) &&
              mb_wm_client_want_focus (client))
            {
              MBWindowManager *wm;
              Window focused = None;
              int revert_to;
              wm = MB_WM_COMP_MGR (priv->comp_mgr)->wm;
              XGetInputFocus (wm->xdpy, &focused, &revert_to);
              if (focused != None &&
                  hd_home_client_owns_or_child_xwindow (client, focused))
                applet_has_focus = TRUE;
            }

          if (client && !applet_has_focus && mb_wm_client_want_focus (client))
            {
              /* g_printerr ("%s: set input focus for applet %p, client %p\n",
                          __func__, applet, client);*/
              mb_wm_client_focus (client);
            }
          else
            {
              /*g_printerr ("%s: tapped applet %p does not want focus or"
                          " already has it\n",
                          __func__, applet); */
              hd_home_applet_emit_button_release_event (home, applet,
                                                        priv->initial_x,
                                                        priv->initial_y);
            }
        }
      else
        hd_home_applet_emit_leave_event (home, applet,
                                         priv->initial_x,
                                         priv->initial_y);

      priv->initial_x = -1;
      priv->initial_y = -1;
  }
}

static gboolean
hd_home_applet_release (ClutterActor       *applet,
	  	        ClutterButtonEvent *event,
			HdHome             *home)
{
  /*
   * If we are in edit mode the HdHomeView will have to deal with this event.
   */
  if (STATE_IN_EDIT_MODE (hd_render_manager_get_state ()))
    return FALSE;

  g_debug ("%s. (x, y) = (%d, %d)", __FUNCTION__, event->x, event->y);

  do_home_applet_motion (home, applet, event->x, event->y);
  do_applet_release (home, applet, event);

  return TRUE;
}

static gboolean
do_home_applet_motion (HdHome       *home,
                       ClutterActor *applet,
                       int           x,
                       int           y)
{
  HdHomePrivate *priv = home->priv;
  gboolean moved_over_threshold;

  /*
   * If we are in edit mode the HdHomeView will have to deal with this event.
   */

  if (STATE_IN_EDIT_MODE (hd_render_manager_get_state ()))
    return FALSE;

  hd_home_desktop_do_motion (home, x, y);

  /*
   * If the pointer was moved over the threshold the initial_x and initial_y is
   * -1;
   */
  if (priv->initial_x == -1 &&
      priv->initial_x == -1)
    return TRUE;

  moved_over_threshold =
	  ABS (priv->initial_x - x) > HDH_CLICK_THRESHOLD ||
	  ABS (priv->initial_y - y) > HDH_CLICK_THRESHOLD;

  if (moved_over_threshold) {
      if (priv->press_timeout)
        priv->press_timeout = (g_source_remove (priv->press_timeout), 0);

      hd_home_applet_emit_leave_event (home, applet,
                                       priv->initial_x,
                                       priv->initial_y);
      priv->initial_x = -1;
      priv->initial_y = -1;
  }


  return TRUE;
}

static gboolean
hd_home_applet_motion (ClutterActor       *applet,
		       ClutterMotionEvent *event,
		       HdHome             *home)
{
  DRAG_DEBUG("%s. (x, y) = (%d, %d)", __FUNCTION__, event->x, event->y);

  if (!(event->modifier_state &
	(CLUTTER_BUTTON1_MASK | CLUTTER_BUTTON2_MASK | CLUTTER_BUTTON2_MASK)))
    return FALSE;

  return do_home_applet_motion (home, applet, event->x, event->y);
}


void
hd_home_add_applet (HdHome *home, ClutterActor *applet)
{
  HdHomePrivate *priv = home->priv;
  gint view_id;
  GConfClient *client = gconf_client_get_default ();
  gchar *view_key;
  MBWMCompMgrClient *cclient;
  HdHomeApplet *wm_applet;
  GError *error = NULL;

  cclient = g_object_get_data (G_OBJECT (applet),
                               "HD-MBWMCompMgrClutterClient");
  wm_applet = (HdHomeApplet *) cclient->wm_client;

  view_key = g_strdup_printf ("/apps/osso/hildon-desktop/applets/%s/view",
                              wm_applet->applet_id);

  gconf_client_clear_cache (client);

  /* Get view id and adjust to 0..3 */
  view_id = gconf_client_get_int (client,
                                  view_key,
                                  &error);
  view_id--;

  if (error)
    {
      g_warning ("%s. Could not get view for applet %s. %d. %s.",
                 __FUNCTION__,
                 wm_applet->applet_id,
                 error->code,
                 error->message);
      g_clear_error (&error);
    }

  /*
   * Here we connect to the pointer events of the applet actor. It is needed for
   * the panning initiated inside the applets.
   */
  g_signal_connect (applet, "button-press-event",
		  G_CALLBACK (hd_home_applet_press), home);
  g_signal_connect (applet, "button-release-event",
		  G_CALLBACK (hd_home_applet_release), home);
  g_signal_connect (applet, "motion-event",
                  G_CALLBACK (hd_home_applet_motion), home);

  if (view_id < 0 || view_id >= MAX_VIEWS)
    {
      GError *error = NULL;

      g_debug ("%s. Fix View %d for widget %s.",
               __FUNCTION__,
               view_id,
               wm_applet->applet_id);

      view_id = hd_home_get_current_view_id (home);

      /* from 0 to 3 */
      gconf_client_set_int (client, view_key, view_id + 1, &error);

      if (G_UNLIKELY (error))
        {
          g_warning ("%s. Could not set GConf key/value. %s",
                     __FUNCTION__,
                     error->message);
          g_clear_error (&error);
        }

      gconf_client_suggest_sync (client,
                                 &error);
      if (G_UNLIKELY (error))
        {
          g_warning ("%s. Could not sync GConf. %s",
                     __FUNCTION__,
                     error->message);
          g_clear_error (&error);
        }
    }
  else if (!hd_home_view_container_get_active (HD_HOME_VIEW_CONTAINER (priv->view_container),
                                               view_id))
    {
      view_id = hd_home_get_current_view_id (home);
    }

  wm_applet->view_id = view_id;

  g_object_set_data (G_OBJECT (applet),
                     "HD-view-id", GINT_TO_POINTER (view_id));

  g_object_unref (client);
  g_free (view_key);

  g_debug ("hd_home_add_applet (), view: %d", view_id);

  if (view_id >= 0)
    {
      ClutterActor *view;

      view = hd_home_view_container_get_view (
                      HD_HOME_VIEW_CONTAINER (priv->view_container), view_id);

      hd_home_view_add_applet (HD_HOME_VIEW (view), applet, FALSE);
    }
  else
    {
      g_debug ("%s FIXME: implement sticky applets", __FUNCTION__);
    }
}

static gboolean
hd_home_edit_button_timeout (gpointer data)
{
  HdHome *home = data;

  hd_home_hide_edit_button (home);

  hd_home_reset_fn_state(home);

  return FALSE;
}

static void
hd_home_show_edit_button (HdHome *home)
{
  HdHomePrivate   *priv = home->priv;
  guint            button_width, button_height;
  ClutterTimeline *timeline;
  gint             x;

  if (hd_render_manager_actor_is_visible(priv->edit_button))
    return;

  clutter_actor_get_size (priv->edit_button, &button_width, &button_height);

	if(STATE_IS_PORTRAIT(hd_render_manager_get_state()))
	  x = HD_COMP_MGR_PORTRAIT_WIDTH - button_width - HD_COMP_MGR_OPERATOR_PADDING;
	else
		x = HD_COMP_MGR_LANDSCAPE_WIDTH - button_width - HD_COMP_MGR_TOP_RIGHT_BTN_WIDTH;

  clutter_actor_set_position (priv->edit_button,
                              x,
                              0);
  /* we must set the final position first so that the X input
   * area can be set properly by HDRM */
  clutter_actor_show(priv->edit_button);
  hd_render_manager_set_input_viewport();


  clutter_actor_set_position (priv->edit_button,
                              x,
                              -button_height);

  /*g_debug ("moving edit button from %d, %d to %d, 0", x, -button_height, x);*/

  timeline = clutter_effect_move (priv->show_edit_button_template,
                                  CLUTTER_ACTOR (priv->edit_button),
                                  x, 0,
                                  NULL,
                                  NULL);

  priv->edit_button_cb =
    g_timeout_add (HDH_EDIT_BUTTON_TIMEOUT, hd_home_edit_button_timeout, home);

  clutter_timeline_start (timeline);
}

static void
hd_home_edit_button_move_completed (ClutterActor *actor, gpointer data)
{
  HdHome *home = HD_HOME(data);
  HdHomePrivate   *priv = home->priv;

  /* Hide the edit button, and alert hdrm it doesn't need to grab this
   * area any more */
  clutter_actor_hide(priv->edit_button);
  hd_render_manager_set_input_viewport();
}

void
hd_home_hide_edit_button (HdHome *home)
{
  HdHomePrivate   *priv = home->priv;
  guint            button_width, button_height;
  ClutterTimeline *timeline;
  gint             x;

  if (!hd_render_manager_actor_is_visible(priv->edit_button))
    return;


  if (priv->edit_button_cb)
    {
      g_source_remove (priv->edit_button_cb);
      priv->edit_button_cb = 0;
    }

  clutter_actor_get_size (priv->edit_button, &button_width, &button_height);

	if(STATE_IS_PORTRAIT(hd_render_manager_get_state()))
	  x = HD_COMP_MGR_PORTRAIT_WIDTH - button_width - HD_COMP_MGR_OPERATOR_PADDING;
	else
		x = HD_COMP_MGR_LANDSCAPE_WIDTH - button_width - HD_COMP_MGR_TOP_RIGHT_BTN_WIDTH;

  timeline = clutter_effect_move (priv->hide_edit_button_template,
                                  CLUTTER_ACTOR (priv->edit_button),
                                  x, -button_height,
                                  (ClutterEffectCompleteFunc) hd_home_edit_button_move_completed,
                                  home);

  clutter_timeline_start (timeline);
}

ClutterActor*
hd_home_get_edit_button (HdHome *home)
{
  HdHomePrivate *priv = home->priv;
  return priv->edit_button;
}

ClutterActor*
hd_home_get_operator (HdHome *home)
{
  HdHomePrivate *priv = home->priv;
  return priv->operator;
}

ClutterActor*
hd_home_get_front (HdHome *home)
{
  HdHomePrivate *priv = home->priv;
  return CLUTTER_ACTOR(priv->front);
}

guint
hd_home_get_current_view_id (HdHome *home)
{
  HdHomePrivate   *priv = home->priv;

  return hd_home_view_container_get_current_view (HD_HOME_VIEW_CONTAINER (priv->view_container));
}

ClutterActor *
hd_home_get_current_view (HdHome *home)
{
  HdHomePrivate   *priv = home->priv;

  return hd_home_view_container_get_view (HD_HOME_VIEW_CONTAINER (priv->view_container),
                                          hd_home_get_current_view_id (home));
}

GSList *
hd_home_get_not_visible_views (HdHome *home)
{
  HdHomePrivate *priv = home->priv;
  GSList *list = NULL;
  guint i, current;

  current = hd_home_get_current_view_id (home);

  for (i = 0; i < MAX_VIEWS; i++)
    {
      if (i != current)
        {
          ClutterActor *view;

          view = hd_home_view_container_get_view (HD_HOME_VIEW_CONTAINER (priv->view_container),
                                                  i);
          list = g_slist_prepend (list, view);
        }
    }

  return list;
}

void
hd_home_set_operator_applet (HdHome *home, ClutterActor *applet)
{
  HdHomePrivate *priv = home->priv;

  if (priv->operator_applet)
    {
      clutter_actor_unparent(priv->operator_applet);
      g_object_unref (priv->operator_applet);
    }

  priv->operator_applet = applet;

  if (applet)
    {
      g_object_ref(applet);
      clutter_actor_show (applet);
      clutter_actor_reparent (applet, priv->operator);
    }
}

static void
update_edge_indication_visibility (HdHome *home,
                                   guint8  left_opacity,
                                   guint8  right_opacity)
{
  HdHomePrivate *priv = home->priv;

  if (left_opacity &&
      hd_home_view_container_get_previous_view (HD_HOME_VIEW_CONTAINER (priv->view_container)))
    {
      clutter_actor_show (priv->edge_indication_left);
      clutter_actor_set_opacity (priv->edge_indication_left, left_opacity);
    }
  else
    {
      clutter_actor_hide (priv->edge_indication_left);
    }

  if (right_opacity &&
      hd_home_view_container_get_next_view (HD_HOME_VIEW_CONTAINER (priv->view_container)))
    {
      clutter_actor_show (priv->edge_indication_right);
      clutter_actor_set_opacity (priv->edge_indication_right, right_opacity);
    }
  else
    {
      clutter_actor_hide (priv->edge_indication_right);
    }
}

void
hd_home_show_edge_indication (HdHome *home)
{
  HdHomePrivate *priv = home->priv;

  if (STATE_IS_PORTRAIT (hd_render_manager_get_state ()))
    {
      if (priv->vertical_scrolling)
        {
          clutter_actor_set_size (priv->edge_indication_left, HD_COMP_MGR_PORTRAIT_HEIGHT,
                                  HD_EDGE_INDICATION_WIDTH);
          clutter_actor_set_position (priv->edge_indication_left, 0, HD_COMP_MGR_TOP_MARGIN);
          clutter_actor_set_size (priv->edge_indication_right, HD_COMP_MGR_PORTRAIT_HEIGHT,
                                  HD_EDGE_INDICATION_WIDTH);
          clutter_actor_set_position (priv->edge_indication_right, 0,
                                      HD_COMP_MGR_PORTRAIT_HEIGHT - HD_EDGE_INDICATION_WIDTH);
        }
      else
        {
          clutter_actor_set_size(priv->edge_indication_left, HD_EDGE_INDICATION_WIDTH, HD_COMP_MGR_PORTRAIT_HEIGHT);
          clutter_actor_set_position (priv->edge_indication_left, 0, 0);
          clutter_actor_set_size (priv->edge_indication_right, HD_EDGE_INDICATION_WIDTH, HD_COMP_MGR_PORTRAIT_HEIGHT);
          clutter_actor_set_position (priv->edge_indication_right, HD_COMP_MGR_PORTRAIT_WIDTH - HD_EDGE_INDICATION_WIDTH, 0);
        }
    }
  else
    {
      clutter_actor_set_size (priv->edge_indication_left, HD_EDGE_INDICATION_WIDTH,
                              HD_COMP_MGR_LANDSCAPE_HEIGHT);
      clutter_actor_set_position (priv->edge_indication_left, 0, 0);
      clutter_actor_set_size (priv->edge_indication_right, HD_EDGE_INDICATION_WIDTH,
                              HD_COMP_MGR_LANDSCAPE_HEIGHT);
      clutter_actor_set_position (priv->edge_indication_right,
                                  HD_COMP_MGR_LANDSCAPE_WIDTH - HD_EDGE_INDICATION_WIDTH, 0);
    }

  update_edge_indication_visibility (home,EDGE_INDICATION_OPACITY_WIDGET_MOVING,
                                     EDGE_INDICATION_OPACITY_WIDGET_MOVING);
}

void
hd_home_hide_edge_indication (HdHome *home)
{
  update_edge_indication_visibility (home,
                                     EDGE_INDICATION_OPACITY_INVISIBLE,
                                     EDGE_INDICATION_OPACITY_INVISIBLE);
}

void
hd_home_highlight_edge_indication (HdHome *home, gboolean left, gboolean right)
{
  update_edge_indication_visibility (home,
                                     left ? EDGE_INDICATION_OPACITY_WIDGET_OVER : EDGE_INDICATION_OPACITY_WIDGET_MOVING,
                                     right ? EDGE_INDICATION_OPACITY_WIDGET_OVER : EDGE_INDICATION_OPACITY_WIDGET_MOVING);
}

static gboolean
is_status_menu_dialog (MBWindowManagerClient *c)
{
  XClassHint xwinhint;
  gboolean status_menu_dialog = FALSE;

  if (XGetClassHint (c->window->wm->xdpy, c->window->xwindow, &xwinhint))
    {
      if (xwinhint.res_name)
        {
          status_menu_dialog = !strcmp (xwinhint.res_name,
                                        "hildon-status-menu");
          XFree (xwinhint.res_name);
        }

      if (xwinhint.res_class)
        XFree (xwinhint.res_class);
    }

  return status_menu_dialog;
}

gboolean
hd_is_hildon_home_dialog (MBWindowManagerClient  *c)
{
  if (HD_WM_CLIENT_CLIENT_TYPE(c) == HdWmClientTypeAppMenu
      && !strcmp(c->window->name, "hildon-home"))
    return TRUE;
  if (MB_WM_CLIENT_CLIENT_TYPE(c) != MBWMClientTypeDialog)
    return FALSE;

  /* Just as an extra safety precaution do not consider any dialogs to be
   * hildon-home dialogs over the stacking layer 0. This way we will not close
   * anything important like the device lock dialog. */
  if (c->stacking_layer > 0)
    return FALSE;

  /* Do not close if it is a hildon-status-menu dialog
   * like the class 0 SMS window. */
  if (is_status_menu_dialog (c))
    return FALSE;

  return c->transient_for ? hd_is_hildon_home_dialog (c->transient_for) : TRUE;
}

/* Remove any hildon-home dialogs that are showing */
void hd_home_remove_dialogs(HdHome *home)
{
  HdHomePrivate *priv = home->priv;
  MBWindowManager *wm;
  MBWindowManagerClient *c;

  if (!priv->comp_mgr)
    return;
  if (!(wm = MB_WM_COMP_MGR(priv->comp_mgr)->wm))
    return;

  for (c = wm->stack_top; c && c != wm->desktop; c = c->stacked_below)
    {
      /* Since we're leaving EDIT_DLG state all the interesting dialogs
       * are stacked above the desktop. */
      if (hd_is_hildon_home_dialog(c))
        {
          MBWMList *l, *l_iter;

          /* close its transients first */
          l = mb_wm_client_get_transients (c);
          for (l_iter = l; l_iter; l_iter = l_iter->next)
            {
              mb_wm_client_hide (MB_WM_CLIENT (l_iter->data));
              mb_wm_client_deliver_delete (MB_WM_CLIENT (l_iter->data));
            }
          if (l)
            mb_wm_util_list_free (l);

          mb_wm_client_hide (c);
          mb_wm_client_deliver_delete (c);
        }
    }
}

/* Called on theme change */
void
hd_home_theme_changed (HdHome *home)
{
  HdHomePrivate *priv = home->priv;
  ClutterColor col;

  /* Get color from theme */
  hd_gtk_style_resolve_logical_color(&col, HD_EDGE_INDICATION_COLOR);

  clutter_rectangle_set_color (CLUTTER_RECTANGLE (priv->edge_indication_left),
                               &col);
  clutter_rectangle_set_color (CLUTTER_RECTANGLE (priv->edge_indication_right),
                               &col);
}

void
hd_home_unregister_applet (HdHome       *home,
                           ClutterActor *applet)
{
  HdHomePrivate *priv = home->priv;
  HdHomeView *view;

  if (priv->pressed_applet == applet)
    priv->pressed_applet = NULL;

  view = g_object_get_data (G_OBJECT (applet),
                            "HD-HomeView");
  if (HD_IS_HOME_VIEW (view))
    hd_home_view_unregister_applet (view, applet);
}

HdHomeViewContainer *hd_home_get_view_container(HdHome *home) {
  return HD_HOME_VIEW_CONTAINER (home->priv->view_container);
}

void
hd_home_update_applets_position (HdHome *home)
{
  HdHomePrivate *priv = home->priv;
  int i;

  for (i = 0; i < MAX_VIEWS; i++)
    {
      ClutterActor *view = hd_home_view_container_get_view (HD_HOME_VIEW_CONTAINER (priv->view_container), i);
      hd_home_view_change_applets_position (HD_HOME_VIEW (view));
    }
}

void
hd_home_update_wallpaper(HdHome *home)
{
  HdHomePrivate *priv = home->priv;
  int i;

  for (i = 0; i < MAX_VIEWS; i++)
    {
      ClutterActor *view = hd_home_view_container_get_view (HD_HOME_VIEW_CONTAINER (priv->view_container), i);
      hd_home_view_change_wallpaper (HD_HOME_VIEW (view));
    }
}

gboolean
hd_home_is_portrait_capable (void)
{
  return hd_app_mgr_is_portrait () && !hd_app_mgr_slide_is_open ();
}

void
hd_home_resize_view_container (ClutterActor *actor, GParamSpec *unused,
                               ClutterActor *stage)
{
  clutter_actor_set_size (actor,
                          hd_comp_mgr_get_current_screen_width (),
                          hd_comp_mgr_get_current_screen_height ());
}

gboolean
hd_home_get_vertical_scrolling (HdHome *home)
{
  HdHomePrivate *priv = home->priv;

  return priv->vertical_scrolling;
}

gboolean
hd_home_is_portrait_wallpaper_enabled (HdHome *home)
{
  HdHomePrivate *priv = home->priv;

  return priv->portrait_wallpaper;
}

gboolean
hd_home_is_desktop_in_portrait_mode (void)
{
	return STATE_IS_PORTRAIT (hd_render_manager_get_state ());
}

