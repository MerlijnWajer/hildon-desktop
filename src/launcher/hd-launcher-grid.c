/*
 * This file is part of hildon-desktop
 *
 * Copyright (C) 2008 Nokia Corporation.
 *
 * Author:  Unknown
 *          Marc Ordinas i Llopis <marc.ordinasillopis@collabora.co.uk>
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

#include "hd-launcher-grid.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib-object.h>

#include <cogl/cogl.h>
#include <clutter/clutter.h>
#include <hildon/hildon-defines.h>

#include <tidy/tidy-adjustment.h>
#include <tidy/tidy-interval.h>
#include <tidy/tidy-scrollable.h>

#include <math.h>

#include "hd-launcher.h"
#include "hd-launcher-item.h"
#include "hd-comp-mgr.h"
#include "hd-util.h"
#include "hd-transition.h"

#define I_(str) (g_intern_static_string ((str)))

#define HD_PARAM_READWRITE      (G_PARAM_READWRITE | \
                                 G_PARAM_STATIC_NICK | \
                                 G_PARAM_STATIC_NAME | \
                                 G_PARAM_STATIC_BLURB)
#define HD_PARAM_READABLE       (G_PARAM_READABLE | \
                                 G_PARAM_STATIC_NICK | \
                                 G_PARAM_STATIC_NAME | \
                                 G_PARAM_STATIC_BLURB)
#define HD_PARAM_WRITABLE       (G_PARAM_WRITABLE | \
                                 G_PARAM_STATIC_NICK | \
                                 G_PARAM_STATIC_NAME | \
                                 G_PARAM_STATIC_BLURB)

#define HD_LAUNCHER_GRID_GET_PRIVATE(obj)       (G_TYPE_INSTANCE_GET_PRIVATE ((obj), HD_TYPE_LAUNCHER_GRID, HdLauncherGridPrivate))

struct _HdLauncherGridPrivate
{
  /* list of actors */
  GList *tiles;
  /* list of 'blocker' actors that block presses
   * on the empty rows of pixels between the icons */
  GList *blockers;

  guint h_spacing;
  guint v_spacing;

  TidyAdjustment *h_adjustment;
  TidyAdjustment *v_adjustment;

  /* How far the transition will move the icons */
  gint transition_depth;
  /* Do we move the icons all together or in sequence? for launcher_in transitions */
  gboolean transition_sequenced;
  /* List of keyframes used on transitions like _IN and _IN_SUB */
  HdKeyFrameList *transition_keyframes; // ramp for tile movement
  HdKeyFrameList *transition_keyframes_label; // ramp for label alpha values
  HdKeyFrameList *transition_keyframes_icon; // ramp for icon alpha values

  /* an internal status indicating how to relayout the grid (which usually is
   * the same of the real device orientation, but may not be in sync with it) */
  gboolean is_portrait;
};

enum
{
  PROP_0,

  PROP_H_ADJUSTMENT,
  PROP_V_ADJUSTMENT
};

/* FIXME: Do we need signals here?
enum
{
  LAST_SIGNAL
};

static guint task_signals[LAST_SIGNAL] = {};
*/

static void tidy_scrollable_iface_init   (TidyScrollableInterface *iface);

static gboolean _hd_launcher_grid_blocker_release_cb (ClutterActor *actor,
                                        ClutterButtonEvent *event,
                                        gpointer *data);

static gboolean      hd_launcher_grid_is_portrait (HdLauncherGrid *self);
#define HD_LAUNCHER_GRID_MAX_COLUMNS_LANDSCAPE 5
#define HD_LAUNCHER_GRID_MAX_COLUMNS_PORTRAIT 3

#define HD_LAUNCHER_GRID_LEFT_DISMISSAL_AREA_LANDSCAPE (HD_LAUNCHER_LEFT_MARGIN)
#define HD_LAUNCHER_GRID_RIGHT_DISMISSAL_AREA_LANDSCAPE (HD_LAUNCHER_RIGHT_MARGIN)
#define HD_LAUNCHER_GRID_LEFT_DISMISSAL_AREA_PORTRAIT (64)
#define HD_LAUNCHER_GRID_RIGHT_DISMISSAL_AREA_PORTRAIT (64)


G_DEFINE_TYPE_WITH_CODE (HdLauncherGrid,
                         hd_launcher_grid,
                         CLUTTER_TYPE_GROUP,
                         G_IMPLEMENT_INTERFACE (TIDY_TYPE_SCROLLABLE,
                                                tidy_scrollable_iface_init));

static inline void
hd_launcher_grid_refresh_h_adjustment (HdLauncherGrid *grid)
{
  HdLauncherGridPrivate *priv = grid->priv;
  ClutterFixed width;
  ClutterUnit clip_x, clip_width;
  ClutterUnit page_width;
  width = 0;
  clip_x = 0;
  clip_width = 0;
  page_width = 0;

  if (!priv->h_adjustment)
    return;

  clutter_actor_get_sizeu (CLUTTER_ACTOR (grid), &width, NULL);
  clutter_actor_get_clipu (CLUTTER_ACTOR (grid),
                           &clip_x, NULL,
                           &clip_width, NULL);

  if (clip_width == 0)
    page_width = CLUTTER_UNITS_TO_FIXED (width);
  else
    page_width = MIN (CLUTTER_UNITS_TO_FIXED (width),
                      CLUTTER_UNITS_TO_FIXED (clip_width - clip_x));

  tidy_adjustment_set_valuesx (priv->h_adjustment,
                               tidy_adjustment_get_valuex (priv->h_adjustment),
                               0,
                               width,
                               CFX_ONE,
                               CFX_ONE * 20,
                               page_width);
}

static inline void
hd_launcher_grid_refresh_v_adjustment (HdLauncherGrid *grid)
{
  HdLauncherGridPrivate *priv = grid->priv;
  ClutterFixed grid_height;
  ClutterUnit clip_y, clip_height;
  ClutterUnit page_height;
  grid_height = 0;
  clip_y = 0;
  clip_height = 0;
  page_height = 0;
  guint screen_height = (hd_launcher_grid_is_portrait (grid) ?
      HD_COMP_MGR_PORTRAIT_HEIGHT : HD_COMP_MGR_LANDSCAPE_HEIGHT);

  if (!priv->v_adjustment)
    return;

  clutter_actor_get_sizeu (CLUTTER_ACTOR (grid), NULL, &grid_height);
  clutter_actor_get_clipu (CLUTTER_ACTOR (grid),
                           NULL, &clip_y,
                           NULL, &clip_height);
  if (grid_height >= CLUTTER_UNITS_FROM_INT (screen_height))
    {
      /* Padding at the bottom. */
      if (hd_launcher_grid_is_portrait (grid))
        /* right margin is the bottom margin when in portrait */
        grid_height += CLUTTER_UNITS_FROM_INT (HD_LAUNCHER_RIGHT_MARGIN
            - HD_LAUNCHER_GRID_ROW_SPACING_PORTRAIT);
      else
        grid_height += CLUTTER_UNITS_FROM_INT (HD_LAUNCHER_BOTTOM_MARGIN
            - HD_LAUNCHER_GRID_ROW_SPACING_LANDSCAPE);

      tidy_adjustment_set_skirtx (priv->v_adjustment,
                     clutter_qdivx (CFX_ONE, CLUTTER_INT_TO_FIXED (4)));
    }
  else
    tidy_adjustment_set_skirtx (priv->v_adjustment, 0);

  if (clip_height == 0)
    page_height = MIN (CLUTTER_UNITS_TO_FIXED (grid_height),
                       CLUTTER_UNITS_TO_FIXED (
                         CLUTTER_UNITS_FROM_DEVICE (screen_height)));
  else
    page_height = MIN (CLUTTER_UNITS_TO_FIXED (grid_height),
                       CLUTTER_UNITS_TO_FIXED (clip_height - clip_y));

  tidy_adjustment_set_valuesx (priv->v_adjustment,
                               tidy_adjustment_get_valuex (priv->v_adjustment),
                               0,
                               grid_height,
                               CFX_ONE,
                               CFX_ONE * 20,
                               page_height);
}

void
hd_launcher_grid_reset_v_adjustment (HdLauncherGrid *grid)
{
  HdLauncherGridPrivate *priv = HD_LAUNCHER_GRID_GET_PRIVATE (grid);

  tidy_adjustment_set_valuex (priv->v_adjustment, 0);
}

static void
adjustment_value_notify (GObject    *gobject,
                         GParamSpec *pspec,
                         gpointer    user_data)
{
  ClutterActor *grid = user_data;
  HdLauncherGridPrivate *priv = HD_LAUNCHER_GRID (grid)->priv;

  clutter_actor_set_anchor_point(grid,
                             0,
                             tidy_adjustment_get_value(priv->v_adjustment));
}

static void
hd_launcher_grid_set_adjustments (TidyScrollable *scrollable,
                                  TidyAdjustment *h_adj,
                                  TidyAdjustment *v_adj)
{
  HdLauncherGridPrivate *priv = HD_LAUNCHER_GRID (scrollable)->priv;

  if (h_adj != priv->h_adjustment)
    {
      if (priv->h_adjustment)
        {
          g_signal_handlers_disconnect_by_func (priv->h_adjustment,
                                                adjustment_value_notify,
                                                scrollable);
          g_object_unref (priv->h_adjustment);
          priv->h_adjustment = NULL;
        }

      if (h_adj)
        {
          priv->h_adjustment = g_object_ref (h_adj);
          g_signal_connect (priv->h_adjustment, "notify::value",
                            G_CALLBACK (adjustment_value_notify),
                            scrollable);
        }
    }

  if (v_adj != priv->v_adjustment)
    {
      if (priv->v_adjustment)
        {
          g_signal_handlers_disconnect_by_func (priv->v_adjustment,
                                                adjustment_value_notify,
                                                scrollable);
          g_object_unref (priv->v_adjustment);
          priv->v_adjustment = NULL;
        }

      if (v_adj)
        {
          priv->v_adjustment = g_object_ref (v_adj);
          g_signal_connect (priv->v_adjustment, "notify::value",
                            G_CALLBACK (adjustment_value_notify),
                            scrollable);
        }
    }
}

static void
hd_launcher_grid_get_adjustments (TidyScrollable  *scrollable,
                                  TidyAdjustment **h_adj,
                                  TidyAdjustment **v_adj)
{
  HdLauncherGridPrivate *priv = HD_LAUNCHER_GRID (scrollable)->priv;

  if (h_adj)
    {
      if (priv->h_adjustment)
        *h_adj = priv->h_adjustment;
      else
        {
          TidyAdjustment *adjustment;

          adjustment = tidy_adjustment_newx (0, 0, 0, 0, 0, 0);
          hd_launcher_grid_set_adjustments (scrollable,
                                            adjustment,
                                            priv->v_adjustment);
          g_object_unref (adjustment);
          hd_launcher_grid_refresh_h_adjustment (HD_LAUNCHER_GRID (scrollable));

          *h_adj = adjustment;
        }
    }

  if (v_adj)
    {
      if (priv->v_adjustment)
        *v_adj = priv->v_adjustment;
      else
        {
          TidyAdjustment *adjustment;

          adjustment = tidy_adjustment_newx (0, 0, 0, 0, 0, 0);
          hd_launcher_grid_set_adjustments (scrollable,
                                            priv->h_adjustment,
                                            adjustment);
          g_object_unref (adjustment);
          hd_launcher_grid_refresh_v_adjustment (HD_LAUNCHER_GRID (scrollable));

          *v_adj = adjustment;
        }
    }
}

static void
tidy_scrollable_iface_init (TidyScrollableInterface *iface)
{
  iface->set_adjustments = hd_launcher_grid_set_adjustments;
  iface->get_adjustments = hd_launcher_grid_get_adjustments;
}

static void
hd_launcher_grid_actor_added (ClutterContainer *container,
                              ClutterActor     *actor)
{
  HdLauncherGridPrivate *priv = HD_LAUNCHER_GRID (container)->priv;

  g_object_ref (actor);

  if (HD_IS_LAUNCHER_TILE(actor))
    {
      priv->tiles = g_list_append (priv->tiles, g_object_ref(actor));

      /* relayout moved to the traversal code */
    }

  g_object_unref (actor);
}

static void
hd_launcher_grid_actor_removed (ClutterContainer *container,
                                ClutterActor     *actor)
{
  HdLauncherGridPrivate *priv = HD_LAUNCHER_GRID (container)->priv;

  g_object_ref (actor);

  if (HD_IS_LAUNCHER_TILE(actor))
    {
      priv->tiles = g_list_remove (priv->tiles, actor);
      g_object_unref(actor);

      /* relayout moved to the traversal code */
    }

  g_object_unref (actor);
}

static void
_hd_launcher_grid_count_children_and_rows (HdLauncherGrid *grid,
                                           guint *children,
                                           guint *rows)
{
  HdLauncherGridPrivate *priv = HD_LAUNCHER_GRID_GET_PRIVATE (grid);
  GList *l;

  *children = 0;
  for (l = priv->tiles; l != NULL; l = l->next)
    {
      ClutterActor *child = l->data;

      if (CLUTTER_ACTOR_IS_VISIBLE (child))
        {
          ++(*children);
        }
    }

  if (*children > 0)
    {
      if (hd_launcher_grid_is_portrait (grid))
        *rows = (*children / HD_LAUNCHER_GRID_MAX_COLUMNS_PORTRAIT) +
          (*children % HD_LAUNCHER_GRID_MAX_COLUMNS_PORTRAIT ? 1 : 0);
      else
        *rows = (*children / HD_LAUNCHER_GRID_MAX_COLUMNS_LANDSCAPE) +
          (*children % HD_LAUNCHER_GRID_MAX_COLUMNS_LANDSCAPE ? 1 : 0);
    }
  else
    {
      *rows = 0;
    }
}


/**
 * Allocates a number of tiles in a row, starting at cur_y.
 * Returns the remaining children list (ie still to allocate).
 *
 * @grid: the HdLauncherGrid instance
 * @l: list of tiles still to be inserted in the layout
 * @remaining is a memory address containing a pointer to the number of
 * children still to insert into the grid.
 * @cur_y: the current y position of the grid, from where to begin placing
 * tiles
 * @h_spacing is the icon horizonal spacing.
 */
static GList *
_hd_launcher_grid_layout_row   (HdLauncherGrid *grid,
                                GList *l,
                                guint *remaining,
                                guint cur_y,
                                guint h_spacing)
{
  ClutterActor *child;
  guint allocated;
  guint cur_x;

  /* Figure out the starting X position needed to centre the icons */
  if (hd_launcher_grid_is_portrait (grid))
    {
      guint icons_width = HD_LAUNCHER_TILE_WIDTH * HD_LAUNCHER_GRID_MAX_COLUMNS_PORTRAIT +
                            h_spacing * (HD_LAUNCHER_GRID_MAX_COLUMNS_PORTRAIT-1);

      cur_x = (HD_LAUNCHER_PAGE_HEIGHT - icons_width) / 2;
      allocated = MIN (HD_LAUNCHER_GRID_MAX_COLUMNS_PORTRAIT, *remaining);
    }
  else
    {
      guint icons_width = HD_LAUNCHER_TILE_WIDTH * HD_LAUNCHER_GRID_MAX_COLUMNS_LANDSCAPE +
                            h_spacing * (HD_LAUNCHER_GRID_MAX_COLUMNS_LANDSCAPE-1);

      cur_x = (HD_LAUNCHER_PAGE_WIDTH - icons_width) / 2;
      allocated = MIN (HD_LAUNCHER_GRID_MAX_COLUMNS_LANDSCAPE, *remaining);
    }

  /* for each icon in the row... */
  for (int i = 0; i < allocated; i++)
    {
      child = l->data;

      clutter_actor_set_position(child, cur_x, cur_y);
      cur_x += HD_LAUNCHER_TILE_WIDTH + h_spacing;

      l = l->next;
    }
  *remaining -= allocated;
  return l;
}

/* hd_launcher_grid_layout:
 * @grid: launcher's grid
 *
 * (re-)layouts @grid according to its internal orientation state and tiles.
 *
 * This method should be called everytime a screen orientation changes and
 * the TL is/will be visible.
 *
 * To change the grid orientation one has to call
 * %hd_launcher_grid_set_portrait() before re-layouting.

 * The screen orientation should be set with the orietation matching
 * %hd_launcher_grid_is_portrait() before calling %hd_launcher_grid_layout().
 */
void hd_launcher_grid_layout (HdLauncherGrid *grid)
{
  HdLauncherGridPrivate *priv = grid->priv;
  GList *l;
  guint cur_height, n_visible_launchers, n_rows;

  /* Free our list of 'blocker' actors that we use to block mouse clicks.
   * TODO: just check we have 'nrows' worth */
  g_list_foreach(priv->blockers,
                 (GFunc)clutter_actor_destroy,
                 NULL);
  g_list_free(priv->blockers);
  priv->blockers = NULL;

  _hd_launcher_grid_count_children_and_rows (grid,
      &n_visible_launchers, &n_rows);

  if (hd_launcher_grid_is_portrait (grid))
    cur_height = HD_LAUNCHER_PAGE_XMARGIN;
  else
    cur_height = HD_LAUNCHER_PAGE_YMARGIN;

  l = priv->tiles;
  while (l) {
    /* Allocate all icons on this row */
    l = _hd_launcher_grid_layout_row(grid, l, &n_visible_launchers,
                                       cur_height, priv->h_spacing);
    if (l)
      {
        /* If there is another row, we must create an actor that
         * goes between the two rows that will grab the clicks that
         * would have gone between them and dismissed the launcher  */
        ClutterActor *blocker = clutter_group_new();
        clutter_actor_set_name(blocker, "HdLauncherGrid::blocker");
        clutter_actor_show(blocker);
        clutter_container_add_actor(CLUTTER_CONTAINER(grid), blocker);
        clutter_actor_set_reactive(blocker, TRUE);
        g_signal_connect (blocker, "button-release-event",
                          G_CALLBACK (_hd_launcher_grid_blocker_release_cb),
                          NULL);

        if (hd_launcher_grid_is_portrait (grid))
          {
            clutter_actor_set_position(blocker,
                HD_LAUNCHER_BOTTOM_MARGIN,
                cur_height + HD_LAUNCHER_TILE_HEIGHT);
            clutter_actor_set_size(blocker,
                HD_LAUNCHER_GRID_WIDTH_PORTRAIT -
                (HD_LAUNCHER_GRID_LEFT_DISMISSAL_AREA_PORTRAIT +
                 HD_LAUNCHER_GRID_RIGHT_DISMISSAL_AREA_PORTRAIT),
                priv->v_spacing);
          }
        else
          {
            clutter_actor_set_position(blocker,
                HD_LAUNCHER_LEFT_MARGIN,
                cur_height + HD_LAUNCHER_TILE_HEIGHT);
            clutter_actor_set_size(blocker,
                HD_LAUNCHER_GRID_WIDTH_LANDSCAPE -
                (HD_LAUNCHER_GRID_LEFT_DISMISSAL_AREA_LANDSCAPE +
                 HD_LAUNCHER_GRID_RIGHT_DISMISSAL_AREA_LANDSCAPE),
                priv->v_spacing);
          }
        priv->blockers = g_list_prepend(priv->blockers, blocker);
      }

    cur_height += HD_LAUNCHER_TILE_HEIGHT + priv->v_spacing;
  }

  if (hd_launcher_grid_is_portrait (grid))
    clutter_actor_set_size(CLUTTER_ACTOR(grid),
        HD_LAUNCHER_PAGE_HEIGHT,
        cur_height);
  else
    clutter_actor_set_size(CLUTTER_ACTOR(grid),
        HD_LAUNCHER_PAGE_WIDTH,
        cur_height);


  if (priv->h_adjustment)
    hd_launcher_grid_refresh_h_adjustment (grid);

  if (priv->v_adjustment)
    hd_launcher_grid_refresh_v_adjustment (grid);
}

static void
hd_launcher_grid_dispose (GObject *gobject)
{
  HdLauncherGridPrivate *priv = HD_LAUNCHER_GRID (gobject)->priv;

  g_list_foreach (priv->tiles,
                  (GFunc) clutter_actor_destroy,
                  NULL);
  g_list_free (priv->tiles);
  priv->tiles = NULL;

  g_list_free(priv->blockers);
  priv->blockers = NULL;

  G_OBJECT_CLASS (hd_launcher_grid_parent_class)->dispose (gobject);
}

static void
hd_launcher_grid_set_property (GObject      *gobject,
                               guint         prop_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
  HdLauncherGridPrivate *priv = HD_LAUNCHER_GRID (gobject)->priv;

  switch (prop_id)
    {
    case PROP_H_ADJUSTMENT:
      hd_launcher_grid_set_adjustments (TIDY_SCROLLABLE (gobject),
                                        g_value_get_object (value),
                                        priv->v_adjustment);
      break;

    case PROP_V_ADJUSTMENT:
      hd_launcher_grid_set_adjustments (TIDY_SCROLLABLE (gobject),
                                        priv->h_adjustment,
                                        g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
      break;
    }
}

static void
hd_launcher_grid_get_property (GObject    *gobject,
                               guint       prop_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
  switch (prop_id)
    {
    case PROP_H_ADJUSTMENT:
      {
        TidyAdjustment *adjustment = NULL;

        hd_launcher_grid_get_adjustments (TIDY_SCROLLABLE (gobject),
                                          &adjustment,
                                          NULL);
        g_value_set_object (value, adjustment);
      }
      break;

    case PROP_V_ADJUSTMENT:
      {
        TidyAdjustment *adjustment = NULL;

        hd_launcher_grid_get_adjustments (TIDY_SCROLLABLE (gobject),
                                          NULL,
                                          &adjustment);
        g_value_set_object (value, adjustment);
      }
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
      break;
    }
}

static void
hd_launcher_grid_class_init (HdLauncherGridClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (HdLauncherGridPrivate));

  gobject_class->set_property = hd_launcher_grid_set_property;
  gobject_class->get_property = hd_launcher_grid_get_property;
  gobject_class->dispose = hd_launcher_grid_dispose;

  g_object_class_override_property (gobject_class,
                                    PROP_H_ADJUSTMENT,
                                    "hadjustment");

  g_object_class_override_property (gobject_class,
                                    PROP_V_ADJUSTMENT,
                                    "vadjustment");
}

static gboolean
hd_launcher_grid_is_portrait (HdLauncherGrid *grid)
{
  HdLauncherGridPrivate *priv;

  g_return_val_if_fail (HD_IS_LAUNCHER_GRID (grid), FALSE);

  priv = HD_LAUNCHER_GRID_GET_PRIVATE (grid);

  return priv->is_portrait;
}

/* hd_launcher_grid_set_portrait:
 * @grid: a launcher's grid
 * @portraited: new grid's orientation
 *
 * Sets the current @grid internal orientation and spacing values.
 *
 * The next time @grid is re-layouted, its orientation and tiles' spacing will
 * be showed according to @portraited.
 *
 * See also #hd_launcher_grid_layout().
 */
void
hd_launcher_grid_set_portrait (HdLauncherGrid *grid,
    gboolean portraited)
{
  HdLauncherGridPrivate *priv;

  g_return_if_fail (HD_IS_LAUNCHER_GRID (grid));

  priv = HD_LAUNCHER_GRID_GET_PRIVATE (grid);

  priv->is_portrait = portraited;

  /* update tiles' spacing */
  if (portraited)
    {
      priv->h_spacing = HD_LAUNCHER_GRID_ICON_MARGIN_PORTRAIT;
      priv->v_spacing = HD_LAUNCHER_GRID_ROW_SPACING_PORTRAIT;
    }
  else
    {
      priv->h_spacing = HD_LAUNCHER_GRID_ICON_MARGIN_LANDSCAPE;
      priv->v_spacing = HD_LAUNCHER_GRID_ROW_SPACING_LANDSCAPE;
    }

}

static void
hd_launcher_grid_init (HdLauncherGrid *launcher)
{
  HdLauncherGridPrivate *priv;

  launcher->priv = priv = HD_LAUNCHER_GRID_GET_PRIVATE (launcher);

  /* set grid's orientation and h/v_spacing values to landscape by default */
  hd_launcher_grid_set_portrait (launcher, FALSE);

  clutter_actor_set_reactive (CLUTTER_ACTOR (launcher), FALSE);

  g_signal_connect(
      launcher, "actor-added", G_CALLBACK(hd_launcher_grid_actor_added), 0);
  g_signal_connect(
      launcher, "actor-removed", G_CALLBACK(hd_launcher_grid_actor_removed), 0);
}

ClutterActor *
hd_launcher_grid_new (void)
{
  return g_object_new (HD_TYPE_LAUNCHER_GRID, NULL);
}

void
hd_launcher_grid_clear (HdLauncherGrid *grid)
{
  HdLauncherGridPrivate *priv;
  GList *l;

  g_return_if_fail (HD_IS_LAUNCHER_GRID (grid));

  priv = grid->priv;

  l = priv->tiles;
  while (l)
    {
      ClutterActor *child = l->data;

      l = l->next;

      clutter_container_remove_actor (CLUTTER_CONTAINER (grid), child);
    }
}

/* Reset the grid before it is shown */
void
hd_launcher_grid_reset(HdLauncherGrid *grid, gboolean hard)
{
  HdLauncherGridPrivate *priv;
  GList *l;

  g_return_if_fail (HD_IS_LAUNCHER_GRID (grid));

  priv = grid->priv;
  l = priv->tiles;

  while (l)
    {
      HdLauncherTile *tile = l->data;

      hd_launcher_tile_reset(tile, hard);
      l = l->next;
    }
}


void
hd_launcher_grid_transition_begin(HdLauncherGrid *grid,
                                  HdLauncherPageTransition trans_type)
{
  HdLauncherGridPrivate *priv = grid->priv;

  priv->transition_depth =
      hd_transition_get_int(
                hd_launcher_page_get_transition_string(trans_type),
                "depth",
                100 /* default value */);
  priv->transition_sequenced = 0;
  if (trans_type == HD_LAUNCHER_PAGE_TRANSITION_IN ||
      trans_type == HD_LAUNCHER_PAGE_TRANSITION_IN_SUB)
    {
      priv->transition_sequenced =
            hd_transition_get_int(
                      hd_launcher_page_get_transition_string(trans_type),
                      "sequenced",
                      0);
      if (priv->transition_sequenced)
        {
          grid->priv->transition_keyframes =
            hd_transition_get_keyframes(
                      hd_launcher_page_get_transition_string(trans_type),
                      "keyframes", "0,1");
          grid->priv->transition_keyframes_label =
            hd_transition_get_keyframes(
                      hd_launcher_page_get_transition_string(trans_type),
                      "keyframes_label", "0,1");
          grid->priv->transition_keyframes_icon =
                 hd_transition_get_keyframes(
                      hd_launcher_page_get_transition_string(trans_type),
                      "keyframes_icon", "0,1");
        }

      /* Reset adjustments so the view is always back to 0,0 */
      if (priv->h_adjustment)
        tidy_adjustment_set_valuex (priv->h_adjustment, 0);

      if (priv->v_adjustment)
        tidy_adjustment_set_valuex (priv->v_adjustment, 0);
    }
}

void
hd_launcher_grid_transition_end(HdLauncherGrid *grid)
{
  /* Free anything we may have allocated for the transition here */
  if (grid->priv->transition_keyframes)
    {
      hd_key_frame_list_free(grid->priv->transition_keyframes);
      grid->priv->transition_keyframes = 0;
    }
  if (grid->priv->transition_keyframes_label)
    {
      hd_key_frame_list_free(grid->priv->transition_keyframes_label);
      grid->priv->transition_keyframes_label = 0;
    }
  if (grid->priv->transition_keyframes_icon)
    {
      hd_key_frame_list_free(grid->priv->transition_keyframes_icon);
      grid->priv->transition_keyframes_icon = 0;
    }
}

void
hd_launcher_grid_transition(HdLauncherGrid *grid,
                            HdLauncherPage *page,
                            HdLauncherPageTransition trans_type,
                            float amount)
{
  HdLauncherGridPrivate *priv;
  GList *l;
  ClutterVertex movement_centre = {0,0,0};

  switch (trans_type)
   {
     case HD_LAUNCHER_PAGE_TRANSITION_IN:
       movement_centre.x = 0;
       movement_centre.y = 0;
       break;
     case HD_LAUNCHER_PAGE_TRANSITION_LAUNCH:
       clutter_actor_get_sizeu(CLUTTER_ACTOR(page),
                               &movement_centre.x, &movement_centre.y);
       break;
     default:
       break;
   }

  g_return_if_fail (HD_IS_LAUNCHER_GRID (grid));

  priv = grid->priv;

  l = priv->tiles;
  while (l)
    {
      ClutterActor *child = l->data;
      l = l->next;
      if (HD_IS_LAUNCHER_TILE(child))
      {
        HdLauncherTile *tile = HD_LAUNCHER_TILE(child);
        ClutterActor *tile_icon = 0;
        ClutterActor *tile_label = 0;
        ClutterVertex pos = {0,0,0};
        float d, dx, dy;
        float order_diff;
        float order_amt; /* amount as if ordered */
        ClutterUnit depth;

        tile_icon = hd_launcher_tile_get_icon(tile);
        tile_label = hd_launcher_tile_get_label(tile);

        clutter_actor_get_positionu(CLUTTER_ACTOR(tile), &pos.x, &pos.y);
        dx = CLUTTER_UNITS_TO_FLOAT(pos.x - movement_centre.x);
        dy = CLUTTER_UNITS_TO_FLOAT(pos.y - movement_centre.y);
        /* We always want d to be 0 <=d <= 1 */
        d = sqrt(dx*dx + dy*dy) / 1000.0f;
        if (d>1) d=1;

        order_diff = (CLUTTER_UNITS_TO_FLOAT(pos.x) +
                     (CLUTTER_UNITS_TO_FLOAT(pos.y))) /
                       (HD_COMP_MGR_LANDSCAPE_WIDTH +
                        HD_COMP_MGR_LANDSCAPE_HEIGHT);
        if (order_diff>1) order_diff = 1;
        order_amt = amount*2 - order_diff;
        if (order_amt<0) order_amt = 0;
        if (order_amt>1) order_amt = 1;

        switch (trans_type)
          {
            case HD_LAUNCHER_PAGE_TRANSITION_IN:
            case HD_LAUNCHER_PAGE_TRANSITION_IN_SUB:
              {
                float label_amt, icon_amt;

                if (priv->transition_sequenced)
                  {
                    label_amt = hd_key_frame_interpolate(
                          priv->transition_keyframes_label, order_amt);
                    icon_amt = hd_key_frame_interpolate(
                          priv->transition_keyframes_icon, order_amt);

                    if (label_amt<0) label_amt=0;
                    if (label_amt>1) label_amt=1;
                    if (icon_amt<0) icon_amt = 0;
                    if (icon_amt>1) icon_amt = 1;
                    depth = CLUTTER_UNITS_FROM_FLOAT(
                       priv->transition_depth *
                       (1 - hd_key_frame_interpolate(priv->transition_keyframes,
                                                     order_amt)));
                  }
                else
                  {
                    depth = CLUTTER_UNITS_FROM_FLOAT(
                                        priv->transition_depth * (1 - amount));
                    label_amt = amount;
                    icon_amt = amount;
                  }

                clutter_actor_set_depthu(CLUTTER_ACTOR(tile), depth);
                clutter_actor_set_opacity(CLUTTER_ACTOR(tile), 255);
                if (tile_icon)
                  clutter_actor_set_opacity(tile_icon, (int)(icon_amt*255));
                if (tile_label)
                  clutter_actor_set_opacity(tile_label, (int)(label_amt*255));
                break;
              }
            case HD_LAUNCHER_PAGE_TRANSITION_OUT:
            case HD_LAUNCHER_PAGE_TRANSITION_OUT_SUB:
              {
                depth = CLUTTER_UNITS_FROM_FLOAT(priv->transition_depth*amount);
                clutter_actor_set_depthu(CLUTTER_ACTOR(tile), depth);
                clutter_actor_set_opacity(CLUTTER_ACTOR(tile), 255 - (int)(amount*255));
                break;
              }
            case HD_LAUNCHER_PAGE_TRANSITION_LAUNCH:
              {
                float tile_amt = amount*2 - d;
                if (tile_amt<0) tile_amt = 0;
                if (tile_amt>1) tile_amt = 1;
                depth = CLUTTER_UNITS_FROM_FLOAT(-priv->transition_depth*tile_amt);
                clutter_actor_set_depthu(CLUTTER_ACTOR(tile), depth);
                clutter_actor_set_opacity(CLUTTER_ACTOR(tile), 255 - (int)(amount*255));
                break;
              }
            /* We do't do anything for these now because we just use blur on
             * the whole group */
            case HD_LAUNCHER_PAGE_TRANSITION_BACK:
                depth = CLUTTER_UNITS_FROM_FLOAT(-priv->transition_depth*amount);
                clutter_actor_set_depthu(CLUTTER_ACTOR(tile), depth);
                clutter_actor_set_opacity(CLUTTER_ACTOR(tile), 255 - (int)(amount*255));
                break;
            case HD_LAUNCHER_PAGE_TRANSITION_FORWARD:
                depth = CLUTTER_UNITS_FROM_FLOAT(-priv->transition_depth*(1-amount));
                clutter_actor_set_depthu(CLUTTER_ACTOR(tile), depth);
                clutter_actor_set_opacity(CLUTTER_ACTOR(tile), (int)(amount*255));
                break;
            case HD_LAUNCHER_PAGE_TRANSITION_OUT_BACK:
                break;
          }
      }
    }
}

static gboolean
_hd_launcher_grid_blocker_release_cb (ClutterActor *actor,
                                      ClutterButtonEvent *event,
                                      gpointer *data)
{
  return TRUE;
}

void hd_launcher_grid_activate(ClutterActor *actor, int p)
{
  HdLauncherGridPrivate *priv = HD_LAUNCHER_GRID_GET_PRIVATE (actor);
  GList *l = priv->tiles;

  while ((p > 0) && l)
    {
      l=l->next;
      p--;
    }

  if (l && !p)
    hd_launcher_tile_activate (l->data);
}
