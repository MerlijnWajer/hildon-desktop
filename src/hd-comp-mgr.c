/*
 * This file is part of hildon-desktop
 *
 * Copyright (C) 2008 Nokia Corporation.
 *
 * Author:  Johan Bilien <johan.bilien@nokia.com>
 *          Tomas Frydrych <tf@o-hand.com>
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

#include "hd-comp-mgr.h"
#include "hd-switcher.h"
#include "hd-home.h"

#include <matchbox/core/mb-wm.h>
#include <matchbox/core/mb-window-manager.h>
#include <matchbox/core/mb-wm-client.h>

#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/shape.h>

#include <clutter/clutter-container.h>
#include <clutter/x11/clutter-x11.h>

#define HDCM_UNMAP_DURATION 200

static int hd_comp_mgr_init (MBWMObject *obj, va_list vap);
static void hd_comp_mgr_class_init (MBWMObjectClass *klass);
static void hd_comp_mgr_destroy (MBWMObject *obj);
static void hd_comp_mgr_unregister_client (MBWMCompMgr *mgr, MBWindowManagerClient *c);
static void hd_comp_mgr_map_notify (MBWMCompMgr *mgr, MBWindowManagerClient *c);
static void hd_comp_mgr_turn_on (MBWMCompMgr *mgr);
static void hd_comp_mgr_effect (MBWMCompMgr *mgr, MBWindowManagerClient *c, MBWMCompMgrClientEvent event);
static void hd_comp_mgr_restack (MBWMCompMgr * mgr);
static void hd_comp_mgr_home_clicked (HdCompMgr *hmgr, ClutterActor *actor);

struct HdCompMgrPrivate
{
  ClutterActor *switcher_group;
  ClutterActor *home;

  gboolean      showing_home : 1;
  gboolean      stack_sync   : 1;
};

int
hd_comp_mgr_class_type ()
{
  static int type = 0;

  if (type == 0)
    {
      static MBWMObjectClassInfo info = {
        sizeof (HdCompMgrClass),
        sizeof (HdCompMgr),
        hd_comp_mgr_init,
        hd_comp_mgr_destroy,
        hd_comp_mgr_class_init
      };

      type = mb_wm_object_register_class (&info,
					  MB_WM_TYPE_COMP_MGR_CLUTTER, 0);
    }

  return type;
}

static void
hd_comp_mgr_class_init (MBWMObjectClass *klass)
{
  MBWMCompMgrClass *cm_klass = MB_WM_COMP_MGR_CLASS (klass);

  cm_klass->unregister_client = hd_comp_mgr_unregister_client;
  cm_klass->client_event      = hd_comp_mgr_effect;
  cm_klass->turn_on           = hd_comp_mgr_turn_on;
  cm_klass->map_notify        = hd_comp_mgr_map_notify;
  cm_klass->restack           = hd_comp_mgr_restack;

#if MBWM_WANT_DEBUG
  klass->klass_name = "HDCompMgr";
#endif
}

static int
hd_comp_mgr_init (MBWMObject *obj, va_list vap)
{
  MBWMCompMgr          *cmgr = MB_WM_COMP_MGR (obj);
  MBWindowManager      *wm = cmgr->wm;
  HdCompMgr            *hmgr = HD_COMP_MGR (obj);
  HdCompMgrPrivate     *priv;
  ClutterActor         *stage, *switcher, *home;

  priv = hmgr->priv = g_new0 (HdCompMgrPrivate, 1);

  stage = clutter_stage_get_default ();

  priv->switcher_group = switcher = g_object_new (HD_TYPE_SWITCHER,
						  "comp-mgr", cmgr,
						  NULL);

  clutter_actor_set_size (switcher, wm->xdpy_width, wm->xdpy_height);
  clutter_actor_show (switcher);

  clutter_container_add_actor (CLUTTER_CONTAINER (stage), switcher);

  /*
   * Add the home group to stage and push it to the bottom of the actor
   * stack.
   */
  home = priv->home =
    g_object_new (HD_TYPE_HOME, "comp-mgr", cmgr, NULL);

  clutter_actor_set_reactive (home, TRUE);

  g_signal_connect_swapped (home, "button-release-event",
                            G_CALLBACK (hd_comp_mgr_home_clicked),
                            cmgr);

  clutter_actor_show (home);
  clutter_container_add_actor (CLUTTER_CONTAINER (stage), home);
  clutter_actor_lower_bottom (home);

  return 1;
}

static void
hd_comp_mgr_destroy (MBWMObject *obj)
{
}

static void
hd_comp_mgr_setup_input_viewport (HdCompMgr *hmgr, ClutterGeometry * geom)
{
  XserverRegion      region;
  Window             overlay;
  Window             clutter_window;
  XRectangle         rectangle;
  MBWMCompMgr       *mgr = MB_WM_COMP_MGR (hmgr);
  MBWindowManager   *wm = mgr->wm;
  Display           *xdpy = wm->xdpy;

  overlay = XCompositeGetOverlayWindow (xdpy, wm->root_win->xwindow);

  XSelectInput (xdpy,
                overlay,
                FocusChangeMask |
                ExposureMask |
                PropertyChangeMask |
                ButtonPressMask | ButtonReleaseMask |
                KeyPressMask | KeyReleaseMask);

  rectangle.x      = geom->x;
  rectangle.y      = geom->y;
  rectangle.width  = geom->width;
  rectangle.height = geom->height;

  region = XFixesCreateRegion (wm->xdpy, &rectangle, 1);

  XFixesSetWindowShapeRegion (xdpy,
                              overlay,
                              ShapeBounding,
                              0, 0,
                              None);

  XFixesSetWindowShapeRegion (xdpy,
                              overlay,
                              ShapeInput,
                              0, 0,
                              region);

  clutter_window =
    clutter_x11_get_stage_window (CLUTTER_STAGE (clutter_stage_get_default()));

  XSelectInput (xdpy,
                clutter_window,
                FocusChangeMask |
                ExposureMask |
                PropertyChangeMask |
                ButtonPressMask | ButtonReleaseMask |
                KeyPressMask | KeyReleaseMask);

  XFixesSetWindowShapeRegion (xdpy,
                              clutter_window,
                              ShapeBounding,
                              0, 0,
                              None);

  XFixesSetWindowShapeRegion (xdpy,
                              clutter_window,
                              ShapeInput,
                              0, 0,
                              region);

  XFixesDestroyRegion (xdpy, region);
}

static void
hd_comp_mgr_turn_on (MBWMCompMgr *mgr)
{
  ClutterGeometry    geom;
  HdCompMgrPrivate * priv = HD_COMP_MGR (mgr)->priv;
  MBWMCompMgrClass * parent_klass =
    MB_WM_COMP_MGR_CLASS (MB_WM_OBJECT_GET_PARENT_CLASS(MB_WM_OBJECT(mgr)));

  /*
   * The parent class turn_on method deals with setting up the input shape on
   * the overlay window; so we first call it, and then change the shape to
   * suit our custom needs.
   */
  if (parent_klass->turn_on)
    parent_klass->turn_on (mgr);

  hd_switcher_get_button_geometry (HD_SWITCHER (priv->switcher_group), &geom);
  hd_comp_mgr_setup_input_viewport (HD_COMP_MGR (mgr), &geom);
}

static void
hd_comp_mgr_unregister_client (MBWMCompMgr *mgr, MBWindowManagerClient *c)
{
  HdCompMgrPrivate         * priv = HD_COMP_MGR (mgr)->priv;
  MBWMCompMgrClass         * parent_klass =
    MB_WM_COMP_MGR_CLASS (MB_WM_OBJECT_GET_PARENT_CLASS(MB_WM_OBJECT(mgr)));

  /*
   * If the actor is an appliation, remove it also to the switcher
   *
   * FIXME: will need to do this for notifications as well.
   */
  if (MB_WM_CLIENT_CLIENT_TYPE (c) == MBWMClientTypeApp)
    {
      MBWMCompMgrClutterClient * cclient;
      ClutterActor             * actor;

      cclient = MB_WM_COMP_MGR_CLUTTER_CLIENT (c->cm_client);

      actor = mb_wm_comp_mgr_clutter_client_get_actor (cclient);

      hd_switcher_remove_window_actor (HD_SWITCHER (priv->switcher_group),
				       actor);

      g_object_set_data (G_OBJECT (actor),
			 "HD-MBWindowManagerClient", NULL);
    }

  if (parent_klass->unregister_client)
    parent_klass->unregister_client (mgr, c);
}

static void
hd_comp_mgr_map_notify (MBWMCompMgr *mgr, MBWindowManagerClient *c)
{
  ClutterActor             * actor;
  HdCompMgrPrivate         * priv = HD_COMP_MGR (mgr)->priv;
  MBWMCompMgrClutterClient * cclient;
  MBWMCompMgrClass         * parent_klass =
    MB_WM_COMP_MGR_CLASS (MB_WM_OBJECT_GET_PARENT_CLASS(MB_WM_OBJECT(mgr)));

  /*
   * Parent class map_notify creates the actor representing the client.
   */
  if (parent_klass->map_notify)
    parent_klass->map_notify (mgr, c);

  /*
   * If the actor is an appliation, add it also to the switcher
   *
   * FIXME: will need to do this for notifications as well.
   */
  if (MB_WM_CLIENT_CLIENT_TYPE (c) != MBWMClientTypeApp)
    return;

  cclient = MB_WM_COMP_MGR_CLUTTER_CLIENT (c->cm_client);

  actor = mb_wm_comp_mgr_clutter_client_get_actor (cclient);

  g_object_set_data (G_OBJECT (actor),
		     "HD-MBWindowManagerClient", c);

  hd_switcher_add_window_actor (HD_SWITCHER (priv->switcher_group), actor);
}

typedef struct _HDEffectData
{
  MBWMCompMgrClutterClient * cclient;
} HDEffectData;

static void
hd_comp_mgr_effect_completed (ClutterActor * actor, HDEffectData *data)
{
  mb_wm_comp_mgr_clutter_client_unset_flags (data->cclient,
					MBWMCompMgrClutterClientDontUpdate |
                                        MBWMCompMgrClutterClientEffectRunning);

  mb_wm_object_unref (MB_WM_OBJECT (data->cclient));

  g_object_unref (actor);

  g_free (data);
};

static void
hd_comp_mgr_effect (MBWMCompMgr                *mgr,
                    MBWindowManagerClient      *c,
                    MBWMCompMgrClientEvent      event)
{
  MBWMClientType c_type = MB_WM_CLIENT_CLIENT_TYPE (c);

  if (c_type == MBWMClientTypeApp)
    {
      switch (event)
	{
	case MBWMCompMgrClientEventUnmap:
	  {
	    MBWMCompMgrClutterClient * cclient;
	    ClutterActor             * actor;
	    ClutterTimeline          * timeline;
	    ClutterEffectTemplate    * tmpl;
	    gdouble                    scale_x, scale_y;
	    HDEffectData             * data;

	    cclient = MB_WM_COMP_MGR_CLUTTER_CLIENT (c->cm_client);
	    actor = mb_wm_comp_mgr_clutter_client_get_actor (cclient);

	    tmpl =
	      clutter_effect_template_new_for_duration (HDCM_UNMAP_DURATION,
						   CLUTTER_ALPHA_RAMP_INC);

	    clutter_actor_get_scale (actor, &scale_x, &scale_y);

	    data = g_new0 (HDEffectData, 1);
	    data->cclient = mb_wm_object_ref (MB_WM_OBJECT (cclient));

	    clutter_actor_move_anchor_point_from_gravity (actor,
						     CLUTTER_GRAVITY_CENTER);

	    timeline = clutter_effect_scale (tmpl, actor,
					     scale_x, 0.1,
					     (ClutterEffectCompleteFunc)
					     hd_comp_mgr_effect_completed,
					     data);

	    mb_wm_comp_mgr_clutter_client_set_flags (cclient,
					MBWMCompMgrClutterClientDontUpdate |
                                        MBWMCompMgrClutterClientEffectRunning);

	    clutter_timeline_start (timeline);
	  }
	  break;

	default:
	  break;
	}
    }
}

static void
hd_comp_mgr_restack (MBWMCompMgr * mgr)
{
  HdCompMgrPrivate         * priv = HD_COMP_MGR (mgr)->priv;
  MBWMCompMgrClass         * parent_klass =
    MB_WM_COMP_MGR_CLASS (MB_WM_OBJECT_GET_PARENT_CLASS(MB_WM_OBJECT(mgr)));

  /*
   * We use the parent class restack() method to do the stacking, but as our
   * switcher shares actors with the CM, we cannot run this when the switcher
   * is showing; instead we set a flag, and let the switcher request stack
   * sync when it closes.
   */
  if (hd_switcher_showing_switcher (HD_SWITCHER (priv->switcher_group)))
    priv->stack_sync = TRUE;
  else if (parent_klass->restack)
    parent_klass->restack (mgr);
}

void
hd_comp_mgr_sync_stacking (HdCompMgr * hmgr)
{
  HdCompMgrPrivate * priv = hmgr->priv;

  /*
   * If the stack_sync flag is set, force restacking of the CM actors
   */
  if (priv->stack_sync)
    {
      priv->stack_sync = FALSE;
      hd_comp_mgr_restack (MB_WM_COMP_MGR (hmgr));
    }
}

void
hd_comp_mgr_raise_home_actor (HdCompMgr *hmgr)
{
  HdCompMgrPrivate * priv = hmgr->priv;

  clutter_actor_lower (priv->home, priv->switcher_group);
}

void
hd_comp_mgr_lower_home_actor (HdCompMgr *hmgr)
{
  HdCompMgrPrivate * priv = hmgr->priv;

  clutter_actor_lower_bottom (priv->home);
}

static void
hd_comp_mgr_home_clicked (HdCompMgr *hmgr, ClutterActor *actor)
{
  hd_comp_mgr_top_home (hmgr);
}

void
hd_comp_mgr_top_home (HdCompMgr *hmgr)
{
  /* TODO */
  g_print ("topping home\n");
}
