#include "e_mod_tiling.h"

/* types {{{ */

#define TILING_OVERLAY_TIMEOUT 5.0
#define TILING_RESIZE_STEP     5
#define TILING_WRAP_SPEED      0.1

typedef struct geom_t
{
   int x, y, w, h;
} geom_t;

typedef struct Client_Extra
{
   E_Client *client;
   geom_t    expected;
   struct
   {
      geom_t      geom;
      E_Maximize  maximized;
      const char *bordername;
   } orig;
   int       last_frame_adjustment; // FIXME: Hack for frame resize bug.
   Eina_Bool floating : 1;
   Eina_Bool tiled : 1;
} Client_Extra;

typedef struct _Instance
{
   E_Gadcon_Client  *gcc;
   Evas_Object      *gadget;
   Eina_Stringshare *gad_id;

   E_Menu           *lmenu;
} Instance;

struct tiling_g tiling_g = {
   .module = NULL,
   .config = NULL,
   .log_domain = -1,
};

static void             _add_client(E_Client *ec);
static void             _remove_client(E_Client *ec);
static void             _client_apply_settings(E_Client *ec, Client_Extra *extra);
static void             _foreach_desk(void (*func)(E_Desk *desk));

/* Func Proto Requirements for Gadcon */
static E_Gadcon_Client *_gc_init(E_Gadcon *gc, const char *name, const char *id, const char *style);
static void             _gc_shutdown(E_Gadcon_Client *gcc);
static void             _gc_orient(E_Gadcon_Client *gcc, E_Gadcon_Orient orient);

static const char  *_gc_label(const E_Gadcon_Client_Class *client_class EINA_UNUSED);
static Evas_Object *_gc_icon(const E_Gadcon_Client_Class *client_class EINA_UNUSED, Evas *evas);
static const char  *_gc_id_new(const E_Gadcon_Client_Class *client_class EINA_UNUSED);

static void         _gadget_icon_set(Instance *inst);

/* }}} */
/* Globals {{{ */

static struct tiling_mod_main_g
{
   char                 edj_path[PATH_MAX];
   E_Config_DD         *config_edd, *vdesk_edd;
   Ecore_Event_Handler *handler_client_resize, *handler_client_move,
                       *handler_client_add, *handler_client_remove, *handler_client_iconify,
                       *handler_client_uniconify, *handler_client_property,
                       *handler_desk_set, *handler_compositor_resize;
   E_Client_Hook       *handler_client_resize_begin;
   E_Client_Menu_Hook  *client_menu_hook;

   Tiling_Info         *tinfo;
   Eina_Hash           *info_hash;
   Eina_Hash           *client_extras;

   E_Action            *act_togglefloat, *act_move_up, *act_move_down, *act_move_left,
                       *act_move_right, *act_toggle_split_mode, *act_swap_window;

   Tiling_Split_Type    split_type;
} _G =
{
   .split_type = TILING_SPLIT_HORIZONTAL,
};

/* Define the class and gadcon functions this module provides */
static const E_Gadcon_Client_Class _gc_class =
{
   GADCON_CLIENT_CLASS_VERSION, "tiling",
   { _gc_init, _gc_shutdown, _gc_orient, _gc_label, _gc_icon, _gc_id_new,
     NULL, NULL },
   E_GADCON_CLIENT_STYLE_PLAIN
};

/* }}} */
/* Utils {{{ */

/* I wonder why noone has implemented the following one yet? */
static E_Desk *
get_current_desk(void)
{
   E_Manager *m = e_manager_current_get();
   E_Comp *c = m->comp;
   E_Zone *z = e_zone_current_get(c);

   return e_desk_current_get(z);
}

static Tiling_Info *
_initialize_tinfo(const E_Desk *desk)
{
   Tiling_Info *tinfo;

   tinfo = E_NEW(Tiling_Info, 1);
   tinfo->desk = desk;
   eina_hash_direct_add(_G.info_hash, &tinfo->desk, tinfo);

   tinfo->conf =
     get_vdesk(tiling_g.config->vdesks, desk->x, desk->y, desk->zone->num);

   return tinfo;
}

static void
check_tinfo(const E_Desk *desk)
{
   if (!_G.tinfo || _G.tinfo->desk != desk)
     {
        _G.tinfo = eina_hash_find(_G.info_hash, &desk);
        if (!_G.tinfo)
          {
             /* lazy init */
             _G.tinfo = _initialize_tinfo(desk);
          }
        if (!_G.tinfo->conf)
          {
             _G.tinfo->conf =
               get_vdesk(tiling_g.config->vdesks, desk->x, desk->y,
                         desk->zone->num);
          }
     }
}

static Eina_Bool
desk_should_tile_check(const E_Desk *desk)
{
   check_tinfo(desk);
   return _G.tinfo && _G.tinfo->conf && _G.tinfo->conf->nb_stacks;
}

static int
is_ignored_window(const Client_Extra *extra)
{
   if (extra->client->sticky || extra->floating)
     return true;

   return false;
}

static int
is_tilable(const E_Client *ec)
{
   if (ec->icccm.min_h == ec->icccm.max_h && ec->icccm.max_h > 0)
     return false;

#ifndef WAYLAND_ONLY
   if (ec->icccm.gravity == ECORE_X_GRAVITY_STATIC)
     return false;
#endif

   if (ec->e.state.centered)
     return false;

   if (!tiling_g.config->tile_dialogs && ((ec->icccm.transient_for != 0) ||
                                          (ec->netwm.type == E_WINDOW_TYPE_DIALOG)))
     return false;

   if (ec->fullscreen)
     {
        return false;
     }

   if (ec->iconic)
      return false;

   if (e_client_util_ignored_get(ec))
     return false;

   return true;
}

static void
change_window_border(E_Client *ec, const char *bordername)
{
   eina_stringshare_replace(&ec->bordername, bordername);
   ec->border.changed = true;
   ec->changes.border = true;
   ec->changed = true;

   DBG("%p -> border %s", ec, bordername);
}

static Eina_Bool
_info_hash_update(const Eina_Hash *hash EINA_UNUSED,
                  const void *key EINA_UNUSED, void *data, void *fdata EINA_UNUSED)
{
   Tiling_Info *tinfo = data;

   if (tinfo->desk)
     {
        tinfo->conf =
          get_vdesk(tiling_g.config->vdesks, tinfo->desk->x, tinfo->desk->y,
                    tinfo->desk->zone->num);
     }
   else
     {
        tinfo->conf = NULL;
     }

   return true;
}

void
e_tiling_update_conf(void)
{
   eina_hash_foreach(_G.info_hash, _info_hash_update, NULL);
}

static void
_e_client_move_resize(E_Client *ec, int x, int y, int w, int h)
{
   Client_Extra *extra;

   extra = eina_hash_find(_G.client_extras, &ec);
   if (!extra)
     {
        ERR("No extra for %p", ec);
        return;
     }

   extra->last_frame_adjustment =
     MAX(ec->h - ec->client.h, ec->w - ec->client.w);
   DBG("%p -> %dx%d+%d+%d", ec, w, h, x, y);
   evas_object_geometry_set(ec->frame, x, y, w, h);
}

static void
_e_client_unmaximize(E_Client *ec, E_Maximize max)
{
   DBG("%p -> %s", ec,
       (max & E_MAXIMIZE_DIRECTION) ==
       E_MAXIMIZE_NONE ? "NONE" : (max & E_MAXIMIZE_DIRECTION) ==
       E_MAXIMIZE_VERTICAL ? "VERTICAL" : (max & E_MAXIMIZE_DIRECTION) ==
       E_MAXIMIZE_HORIZONTAL ? "HORIZONTAL" : "BOTH");
   e_client_unmaximize(ec, max);
}

static void
_restore_client(E_Client *ec)
{
   Client_Extra *extra;

   extra = eina_hash_find(_G.client_extras, &ec);
   if (!extra)
     {
        ERR("No extra for %p", ec);
        return;
     }

   if (!extra->tiled)
     return;

   _e_client_move_resize(ec, extra->orig.geom.x, extra->orig.geom.y,
                         extra->orig.geom.w, extra->orig.geom.h);
   if (extra->orig.maximized != ec->maximized)
     {
        e_client_maximize(ec, extra->orig.maximized);
        ec->maximized = extra->orig.maximized;
     }

   DBG("Change window border back to %s for %p", extra->orig.bordername, ec);
   change_window_border(ec,
                        (extra->orig.bordername) ? extra->orig.bordername : "default");
}

static Client_Extra *
_get_or_create_client_extra(E_Client *ec)
{
   Client_Extra *extra;

   extra = eina_hash_find(_G.client_extras, &ec);
   if (!extra)
     {
        extra = E_NEW(Client_Extra, 1);
        *extra = (Client_Extra)
        {
           .client = ec, .expected =
           {
              .x = ec->x, .y = ec->y, .w = ec->w, .h = ec->h,
           }

           , .orig =
           {
              .geom =
              {
                 .x = ec->x, .y = ec->y, .w = ec->w, .h = ec->h,
              }

              , .maximized = ec->maximized, .bordername =
                eina_stringshare_add(ec->bordername),
           }

           ,
        };
        eina_hash_direct_add(_G.client_extras, &extra->client, extra);
     }
   else
     {
        extra->expected = (geom_t)
        {
           .x = ec->x, .y = ec->y, .w = ec->w, .h = ec->h,
        };
        extra->orig.geom = extra->expected;
        extra->orig.maximized = ec->maximized;
        eina_stringshare_replace(&extra->orig.bordername, ec->bordername);
     }

   return extra;
}

void
tiling_e_client_move_resize_extra(E_Client *ec, int x, int y, int w, int h)
{
   Client_Extra *extra = eina_hash_find(_G.client_extras, &ec);

   if (!extra)
     {
        ERR("No extra for %p", ec);
        return;
     }

   extra->expected = (geom_t)
   {
      .x = x, .y = y, .w = w, .h = h,
   };

   _e_client_move_resize(ec, x, y, w, h);
}

static Client_Extra *
tiling_entry_no_desk_func(E_Client *ec)
{
   if (!ec)
     return NULL;

   if (!is_tilable(ec))
     return NULL;

   Client_Extra *extra = eina_hash_find(_G.client_extras, &ec);

   if (!extra)
     ERR("No extra for %p", ec);

   return extra;
}

static Client_Extra *
tiling_entry_func(E_Client *ec)
{
   Client_Extra *extra = tiling_entry_no_desk_func(ec);

   if (!extra)
      return NULL;

   if (!desk_should_tile_check(ec->desk))
     return NULL;

   return extra;
}

/* }}} */
/* Reorganize Stacks {{{ */

static void
_reapply_tree(void)
{
   int zx, zy, zw, zh;

   if (_G.tinfo->tree)
     {
        e_zone_useful_geometry_get(_G.tinfo->desk->zone, &zx, &zy, &zw, &zh);

        tiling_window_tree_apply(_G.tinfo->tree, zx, zy, zw, zh,
                                 tiling_g.config->window_padding);
     }
}

void
_restore_free_client(void *_item)
{
   Window_Tree *item = _item;

   if (item->client)
     {
        _restore_client(item->client);

        Client_Extra *extra = eina_hash_find(_G.client_extras, &item->client);

        if (extra)
          {
             extra->tiled = EINA_FALSE;
          }
     }
   free(item);
}

void
change_desk_conf(struct _Config_vdesk *newconf)
{
   E_Manager *m;
   E_Comp *c;
   E_Zone *z;
   E_Desk *d;
   int old_nb_stacks, new_nb_stacks = newconf->nb_stacks;

   m = e_manager_current_get();
   if (!m)
     return;
   c = m->comp;
   z = e_comp_zone_number_get(c, newconf->zone_num);
   if (!z)
     return;
   d = e_desk_at_xy_get(z, newconf->x, newconf->y);
   if (!d)
     return;

   check_tinfo(d);
   old_nb_stacks = _G.tinfo->conf->nb_stacks;

   _G.tinfo->conf = newconf;
   _G.tinfo->conf->nb_stacks = new_nb_stacks;

   if (new_nb_stacks == 0)
     {
        tiling_window_tree_walk(_G.tinfo->tree, _restore_free_client);
        _G.tinfo->tree = NULL;
        e_place_zone_region_smart_cleanup(z);
     }
   else if (new_nb_stacks == old_nb_stacks)
     {
        E_Client *ec;

        E_CLIENT_FOREACH(e_comp_get(NULL), ec)
          {
             _client_apply_settings(ec, NULL);
          }

        _reapply_tree();
     }
   else
     {
        /* Add all the existing windows. */
        E_Client *ec;

        E_CLIENT_FOREACH(e_comp_get(NULL), ec)
          {
             _add_client(ec);
          }
     }
}

/* }}} */
/* Reorganize windows {{{ */

static void
_client_apply_settings(E_Client *ec, Client_Extra *extra)
{
   if (!extra)
     {
        extra = tiling_entry_func(ec);
     }

   if (!extra)
      return;

   if (is_ignored_window(extra))
     return;

   if (!extra->tiled)
      return;

   if (ec->maximized)
     _e_client_unmaximize(ec, E_MAXIMIZE_BOTH);

   if (!tiling_g.config->show_titles && (!ec->bordername ||
                                         strcmp(ec->bordername, "pixel")))
     change_window_border(ec, "pixel");
   else if (tiling_g.config->show_titles && (ec->bordername &&
                                         !strcmp(ec->bordername, "pixel")))
     change_window_border(ec, (extra->orig.bordername) ? extra->orig.bordername : "default");

}

static void
_add_client(E_Client *ec)
{
   /* Should I need to check that the client is not already added? */
   if (!ec)
     {
        return;
     }
   if (!is_tilable(ec))
     {
        return;
     }

   Client_Extra *extra = _get_or_create_client_extra(ec);

   if (!desk_should_tile_check(ec->desk))
     return;

   if (is_ignored_window(extra))
     return;

   if (_G.split_type == TILING_SPLIT_FLOAT)
     {
        extra->floating = EINA_TRUE;
        return;
     }

   if (extra->tiled)
     return;

   extra->tiled = EINA_TRUE;

   DBG("adding %p", ec);

   _client_apply_settings(ec, extra);

   /* Window tree updating. */
   {
      E_Client *ec_focused = e_client_focused_get();

      /* If focused is NULL, it should return the root. */
      Window_Tree *parent = tiling_window_tree_client_find(_G.tinfo->tree,
                                                           ec_focused);

      if (!parent && (ec_focused != ec))
        {
           Client_Extra *extra_focused =
             eina_hash_find(_G.client_extras, &ec_focused);
           if (_G.tinfo->tree && extra_focused &&
               !is_ignored_window(extra_focused))
             {
                ERR("Couldn't find tree item for focused client %p. Using root..", e_client_focused_get());
             }
        }

      _G.tinfo->tree =
        tiling_window_tree_add(_G.tinfo->tree, parent, ec, _G.split_type);
   }

   _reapply_tree();
}

static Eina_Bool
_client_remove_no_apply(E_Client *ec)
{
   if (!ec)
      return EINA_FALSE;

   DBG("removing %p", ec);

   Client_Extra *extra = eina_hash_find(_G.client_extras, &ec);

   if (!extra)
     {
        if (is_tilable(ec))
          {
             ERR("No extra for %p", ec);
          }
        return EINA_FALSE;
     }

   if (!extra->tiled)
      return EINA_FALSE;

   extra->tiled = EINA_FALSE;

   /* Window tree updating. */
     {
        /* If focused is NULL, it should return the root. */
        Window_Tree *item = tiling_window_tree_client_find(_G.tinfo->tree, ec);

        if (!item)
          {
             ERR("Couldn't find tree item for client %p!", ec);
             return EINA_FALSE;
          }

        _G.tinfo->tree = tiling_window_tree_remove(_G.tinfo->tree, item);
     }

   return EINA_TRUE;
}

static void
_remove_client(E_Client *ec)
{
   if (_client_remove_no_apply(ec))
      _reapply_tree();
}

/* }}} */
/* Toggle Floating {{{ */

static void
toggle_floating(E_Client *ec)
{
   Client_Extra *extra = tiling_entry_no_desk_func(ec);

   if (!extra)
     {
        return;
     }

   extra->floating = !extra->floating;

   if (!desk_should_tile_check(ec->desk))
     return;

   /* This is the new state, act accordingly. */
   if (extra->floating)
     {
        _restore_client(ec);
        _remove_client(ec);
     }
   else
     {
        _add_client(ec);
     }
}

static void
_e_mod_action_toggle_floating_cb(E_Object *obj EINA_UNUSED,
                                 const char *params EINA_UNUSED)
{
   toggle_floating(e_client_focused_get());
}

static E_Client *_go_mouse_client = NULL;

static void
_e_mod_action_swap_window_go_mouse(E_Object *obj EINA_UNUSED,
                                   const char *params EINA_UNUSED,
                                   E_Binding_Event_Mouse_Button *ev EINA_UNUSED)
{
   E_Client *ec = e_client_under_pointer_get(get_current_desk(), NULL);

   Client_Extra *extra = tiling_entry_func(ec);

   if (!extra)
     return;

   if (is_ignored_window(extra))
     return;

   _go_mouse_client = ec;
}

static void
_e_mod_action_swap_window_end_mouse(E_Object *obj EINA_UNUSED,
                                    const char *params EINA_UNUSED,
                                    E_Binding_Event_Mouse_Button *ev EINA_UNUSED)
{
   E_Client *ec = e_client_under_pointer_get(get_current_desk(), NULL);
   E_Client *first_ec = _go_mouse_client;

   _go_mouse_client = NULL;

   if (!first_ec)
     return;

   Client_Extra *extra = tiling_entry_func(ec);

   if (!extra)
     return;

   if (is_ignored_window(extra))
     return;

   /* XXX: Only support swap on the first desk for now. */
   if (ec->desk != first_ec->desk)
     return;

   Window_Tree *item, *first_item;

   item = tiling_window_tree_client_find(_G.tinfo->tree, ec);

   if (!item)
     return;

   first_item = tiling_window_tree_client_find(_G.tinfo->tree, first_ec);

   if (!first_item)
     return;

   item->client = first_ec;
   first_item->client = ec;

   _reapply_tree();
}

static void
_e_mod_menu_border_cb(void *data, E_Menu *m EINA_UNUSED,
                      E_Menu_Item *mi EINA_UNUSED)
{
   E_Client *ec = data;

   toggle_floating(ec);
}

/* }}} */
/* {{{ Move windows */

static void
_action_swap(int cross_edge)
{
   E_Desk *desk;
   E_Client *focused_ec;

   desk = get_current_desk();
   if (!desk)
     return;

   focused_ec = e_client_focused_get();
   if (!focused_ec || focused_ec->desk != desk)
     return;

   if (!desk_should_tile_check(desk))
     return;

   Window_Tree *item =
     tiling_window_tree_client_find(_G.tinfo->tree, focused_ec);

   if (item)
     {
        tiling_window_tree_node_move(item, cross_edge);

        _reapply_tree();
     }
}

static void
_e_mod_action_move_left_cb(E_Object *obj EINA_UNUSED,
                           const char *params EINA_UNUSED)
{
   _action_swap(TILING_WINDOW_TREE_EDGE_LEFT);
}

static void
_e_mod_action_move_right_cb(E_Object *obj EINA_UNUSED,
                            const char *params EINA_UNUSED)
{
   _action_swap(TILING_WINDOW_TREE_EDGE_RIGHT);
}

static void
_e_mod_action_move_up_cb(E_Object *obj EINA_UNUSED,
                         const char *params EINA_UNUSED)
{
   _action_swap(TILING_WINDOW_TREE_EDGE_TOP);
}

static void
_e_mod_action_move_down_cb(E_Object *obj EINA_UNUSED,
                           const char *params EINA_UNUSED)
{
   _action_swap(TILING_WINDOW_TREE_EDGE_BOTTOM);
}

/* }}} */
/* Toggle split mode {{{ */

static void
_tiling_split_type_next(void)
{
   Instance *inst;
   Eina_List *itr;
   _G.split_type = (_G.split_type + 1) % TILING_SPLIT_LAST;

   /* If we don't allow floating, skip it. */
   if (!tiling_g.config->have_floating_mode &&
       (_G.split_type == TILING_SPLIT_FLOAT))
     {
        _G.split_type = (_G.split_type + 1) % TILING_SPLIT_LAST;
     }

   EINA_LIST_FOREACH(tiling_g.gadget_instances, itr, inst)
     {
        _gadget_icon_set(inst);
     }
}

static void
_e_mod_action_toggle_split_mode(E_Object *obj EINA_UNUSED,
                                const char *params EINA_UNUSED)
{
   E_Desk *desk;

   desk = get_current_desk();
   if (!desk)
     return;

   if (!desk_should_tile_check(desk))
     return;

   _tiling_split_type_next();
}

/* }}} */
/* Hooks {{{ */

static void
_move_or_resize(E_Client *ec)
{
   Client_Extra *extra = tiling_entry_func(ec);

   if (!extra)
     {
        return;
     }

   if (is_ignored_window(extra))
     return;

   if ((ec->x == extra->expected.x) && (ec->y == extra->expected.y) &&
       (ec->w == extra->expected.w) && (ec->h == extra->expected.h))
     {
        return;
     }

   if (!extra->last_frame_adjustment)
     {
        printf
          ("This is probably because of the frame adjustment bug. Return\n");
        _reapply_tree();
        return;
     }

   Window_Tree *item = tiling_window_tree_client_find(_G.tinfo->tree, ec);

   if (!item)
     {
        ERR("Couldn't find tree item for resized client %p!", ec);
        return;
     }

   {
      int w_dir = 1, h_dir = 1;
      double w_diff = 1.0, h_diff = 1.0;

      if (abs(extra->expected.w - ec->w) >= 1)
        {
           w_diff = ((double)ec->w) / extra->expected.w;
        }
      if (abs(extra->expected.h - ec->h) >= 1)
        {
           h_diff = ((double)ec->h) / extra->expected.h;
        }
      switch (ec->resize_mode)
        {
         case E_POINTER_RESIZE_L:
         case E_POINTER_RESIZE_BL:
           w_dir = -1;
           break;

         case E_POINTER_RESIZE_T:
         case E_POINTER_RESIZE_TR:
           h_dir = -1;
           break;

         case E_POINTER_RESIZE_TL:
           w_dir = -1;
           h_dir = -1;
           break;

         default:
           break;
        }
      if ((w_diff != 1.0) || (h_diff != 1.0))
        {
           if (!tiling_window_tree_node_resize(item, w_dir, w_diff, h_dir,
                                               h_diff))
             {
                /* FIXME: Do something? */
             }
        }
   }

   _reapply_tree();
}

static void
_resize_begin_hook(void *data EINA_UNUSED, E_Client *ec)
{
   Client_Extra *extra = tiling_entry_func(ec);

   if (!extra)
     {
        return;
     }

   if (is_ignored_window(extra))
     return;

   Window_Tree *item = tiling_window_tree_client_find(_G.tinfo->tree, ec);

   if (!item)
     {
        ERR("Couldn't find tree item for resized client %p!", ec);
        return;
     }

   int edges = tiling_window_tree_edges_get(item);

   if (edges & TILING_WINDOW_TREE_EDGE_LEFT)
     {
        switch (ec->resize_mode)
          {
           case E_POINTER_RESIZE_L:
             ec->resize_mode = E_POINTER_RESIZE_NONE;
             break;

           case E_POINTER_RESIZE_TL:
             ec->resize_mode = E_POINTER_RESIZE_T;
             break;

           case E_POINTER_RESIZE_BL:
             ec->resize_mode = E_POINTER_RESIZE_B;
             break;

           default:
             break;
          }
     }
   if (edges & TILING_WINDOW_TREE_EDGE_RIGHT)
     {
        switch (ec->resize_mode)
          {
           case E_POINTER_RESIZE_R:
             ec->resize_mode = E_POINTER_RESIZE_NONE;
             break;

           case E_POINTER_RESIZE_TR:
             ec->resize_mode = E_POINTER_RESIZE_T;
             break;

           case E_POINTER_RESIZE_BR:
             ec->resize_mode = E_POINTER_RESIZE_B;
             break;

           default:
             break;
          }
     }
   if (edges & TILING_WINDOW_TREE_EDGE_TOP)
     {
        switch (ec->resize_mode)
          {
           case E_POINTER_RESIZE_T:
             ec->resize_mode = E_POINTER_RESIZE_NONE;
             break;

           case E_POINTER_RESIZE_TL:
             ec->resize_mode = E_POINTER_RESIZE_L;
             break;

           case E_POINTER_RESIZE_TR:
             ec->resize_mode = E_POINTER_RESIZE_R;
             break;

           default:
             break;
          }
     }
   if (edges & TILING_WINDOW_TREE_EDGE_BOTTOM)
     {
        switch (ec->resize_mode)
          {
           case E_POINTER_RESIZE_B:
             ec->resize_mode = E_POINTER_RESIZE_NONE;
             break;

           case E_POINTER_RESIZE_BL:
             ec->resize_mode = E_POINTER_RESIZE_L;
             break;

           case E_POINTER_RESIZE_BR:
             ec->resize_mode = E_POINTER_RESIZE_R;
             break;

           default:
             break;
          }
     }

   if (!e_client_util_resizing_get(ec))
     e_client_resize_cancel();
}

static Eina_Bool
_resize_hook(void *data EINA_UNUSED, int type EINA_UNUSED,
             E_Event_Client *event)
{
   E_Client *ec = event->ec;

   _move_or_resize(ec);

   return true;
}

static Eina_Bool
_move_hook(void *data EINA_UNUSED, int type EINA_UNUSED, E_Event_Client *event)
{
   E_Client *ec = event->ec;
   Client_Extra *extra = tiling_entry_func(ec);

   if (!extra)
     {
        return true;
     }

   if (is_ignored_window(extra))
     return true;

   e_client_act_move_end(event->ec, NULL);

   _reapply_tree();

   return true;
}

static Eina_Bool
_add_hook(void *data EINA_UNUSED, int type EINA_UNUSED, E_Event_Client *event)
{
   E_Client *ec = event->ec;

   _add_client(ec);

   return true;
}

static void
_frame_del_cb(void *data EINA_UNUSED, Evas *evas EINA_UNUSED,
      Evas_Object *obj EINA_UNUSED, void *event_info EINA_UNUSED)
{
   _reapply_tree();
}

static Eina_Bool
_remove_hook(void *data EINA_UNUSED, int type EINA_UNUSED,
             E_Event_Client *event)
{
   E_Client *ec = event->ec;

   if (e_client_util_ignored_get(ec))
     return ECORE_CALLBACK_RENEW;

   if (desk_should_tile_check(ec->desk))
     {
        _client_remove_no_apply(ec);
        evas_object_event_callback_add(ec->frame, EVAS_CALLBACK_DEL, _frame_del_cb, NULL);
     }

   eina_hash_del(_G.client_extras, &ec, NULL);

   return true;
}

static bool
_iconify_hook(void *data EINA_UNUSED, int type EINA_UNUSED,
              E_Event_Client *event)
{
   E_Client *ec = event->ec;

   DBG("iconify hook: %p", ec);

   if (ec->deskshow)
     return true;

   _remove_client(ec);

   return true;
}

static bool
_uniconify_hook(void *data EINA_UNUSED, int type EINA_UNUSED,
                E_Event_Client *event)
{
   E_Client *ec = event->ec;

   if (ec->deskshow)
     return true;

   _add_client(ec);

   return true;
}

static void
toggle_sticky(E_Client *ec)
{
   Client_Extra *extra = tiling_entry_func(ec);

   if (!extra)
     {
        return;
     }

   /* This is the new state, act accordingly. */
   if (ec->sticky)
     {
        _restore_client(ec);
        _remove_client(ec);
     }
   else
     {
        _add_client(ec);
     }
}

static Eina_Bool
_property_hook(void *data EINA_UNUSED, int type EINA_UNUSED,
            E_Event_Client_Property *event)
{
   if (event->property & E_CLIENT_PROPERTY_STICKY)
     {
        toggle_sticky(event->ec);
     }
   return true;
}

static bool
_desk_set_hook(void *data EINA_UNUSED, int type EINA_UNUSED,
               E_Event_Client_Desk_Set *ev)
{
   DBG("%p: from (%d,%d) to (%d,%d)", ev->ec, ev->desk->x, ev->desk->y,
       ev->ec->desk->x, ev->ec->desk->y);

   if (desk_should_tile_check(ev->desk))
     {
        if (tiling_window_tree_client_find(_G.tinfo->tree, ev->ec))
          {
             _restore_client(ev->ec);
             _remove_client(ev->ec);
          }
     }

   if (!desk_should_tile_check(ev->ec->desk))
     return true;

   _add_client(ev->ec);

   return true;
}

static void
_compositor_resize_hook_desk_reapply(E_Desk *desk)
{
   check_tinfo(desk);
   if (!desk_should_tile_check(desk))
     return;

   _reapply_tree();
}

static bool
_compositor_resize_hook(void *data EINA_UNUSED, int type EINA_UNUSED,
                        E_Event_Compositor_Resize *ev EINA_UNUSED)
{
   _foreach_desk(_compositor_resize_hook_desk_reapply);

   return true;
}

static void
_bd_hook(void *d EINA_UNUSED, E_Client *ec)
{
   E_Menu_Item *mi;
   E_Menu *m;
   Eina_List *l;

   if (!ec->border_menu)
     return;
   m = ec->border_menu;

   Client_Extra *extra = eina_hash_find(_G.client_extras, &ec);

   if (!extra)
     {
        return;
     }

   /* position menu item just before the last separator */
   EINA_LIST_REVERSE_FOREACH(m->items, l, mi)
     if (mi->separator)
       break;
   if ((!mi) || (!mi->separator))
     return;
   l = eina_list_prev(l);
   mi = eina_list_data_get(l);
   if (!mi)
     return;

   mi = e_menu_item_new_relative(m, mi);
   e_menu_item_label_set(mi, _("Floating"));
   e_menu_item_check_set(mi, true);
   e_menu_item_toggle_set(mi, (extra->floating) ? true : false);
   e_menu_item_callback_set(mi, _e_mod_menu_border_cb, ec);
}

/* }}} */
/* Module setup {{{ */

static void
_clear_info_hash(void *data)
{
   Tiling_Info *ti = data;

   tiling_window_tree_free(ti->tree);
   ti->tree = NULL;
   E_FREE(ti);
}

static void
_clear_border_extras(void *data)
{
   Client_Extra *extra = data;

   eina_stringshare_del(extra->orig.bordername);

   E_FREE(extra);
}

EAPI E_Module_Api e_modapi = {
   E_MODULE_API_VERSION,
   "Tiling"
};

EAPI void *
e_modapi_init(E_Module *m)
{
   E_Desk *desk;
   Eina_List *l;

   tiling_g.module = m;

   if (tiling_g.log_domain < 0)
     {
        tiling_g.log_domain = eina_log_domain_register("tiling", NULL);
        if (tiling_g.log_domain < 0)
          {
             EINA_LOG_CRIT("could not register log domain 'tiling'");
          }
     }

   _G.info_hash = eina_hash_pointer_new(_clear_info_hash);
   _G.client_extras = eina_hash_pointer_new(_clear_border_extras);

#define HANDLER(_h, _e, _f)                                \
  _h = ecore_event_handler_add(E_EVENT_##_e,               \
                               (Ecore_Event_Handler_Cb)_f, \
                               NULL);

   _G.handler_client_resize_begin =
      e_client_hook_add(E_CLIENT_HOOK_RESIZE_BEGIN, _resize_begin_hook, NULL);
   HANDLER(_G.handler_client_resize, CLIENT_RESIZE, _resize_hook);
   HANDLER(_G.handler_client_move, CLIENT_MOVE, _move_hook);
   HANDLER(_G.handler_client_add, CLIENT_ADD, _add_hook);
   HANDLER(_G.handler_client_remove, CLIENT_REMOVE, _remove_hook);

   HANDLER(_G.handler_client_iconify, CLIENT_ICONIFY, _iconify_hook);
   HANDLER(_G.handler_client_uniconify, CLIENT_UNICONIFY, _uniconify_hook);
   HANDLER(_G.handler_client_property, CLIENT_PROPERTY, _property_hook);

   HANDLER(_G.handler_desk_set, CLIENT_DESK_SET, _desk_set_hook);
   HANDLER(_G.handler_compositor_resize, COMPOSITOR_RESIZE,
           _compositor_resize_hook);
#undef HANDLER

#define ACTION_ADD(_action, _cb, _title, _value, _params, _example, _editable) \
  {                                                                            \
     const char *_name = _value;                                               \
     if ((_action = e_action_add(_name))) {                                    \
          _action->func.go = _cb;                                              \
          e_action_predef_name_set(N_("Tiling"), _title, _name,                \
                                   _params, _example, _editable);              \
       }                                                                       \
  }

   /* Module's actions */
   ACTION_ADD(_G.act_togglefloat, _e_mod_action_toggle_floating_cb,
              N_("Toggle floating"), "toggle_floating", NULL, NULL, 0);

   ACTION_ADD(_G.act_move_up, _e_mod_action_move_up_cb,
              N_("Move the focused window up"), "move_up", NULL, NULL, 0);
   ACTION_ADD(_G.act_move_down, _e_mod_action_move_down_cb,
              N_("Move the focused window down"), "move_down", NULL, NULL, 0);
   ACTION_ADD(_G.act_move_left, _e_mod_action_move_left_cb,
              N_("Move the focused window left"), "move_left", NULL, NULL, 0);
   ACTION_ADD(_G.act_move_right, _e_mod_action_move_right_cb,
              N_("Move the focused window right"), "move_right", NULL, NULL, 0);

   ACTION_ADD(_G.act_toggle_split_mode, _e_mod_action_toggle_split_mode,
              N_("Toggle split mode"), "toggle_split_mode", NULL, NULL, 0);

   ACTION_ADD(_G.act_swap_window, NULL, N_("Swap window"), "swap_window", NULL,
              NULL, 0);
   _G.act_swap_window->func.go_mouse = _e_mod_action_swap_window_go_mouse;
   _G.act_swap_window->func.end_mouse = _e_mod_action_swap_window_end_mouse;

#undef ACTION_ADD

   /* Configuration entries */
   snprintf(_G.edj_path, sizeof(_G.edj_path), "%s/e-module-tiling.edj",
            e_module_dir_get(m));
   e_configure_registry_category_add("windows", 50, _("Windows"), NULL,
                                     "preferences-system-windows");
   e_configure_registry_item_add("windows/tiling", 150, _("Tiling"), NULL,
                                 _G.edj_path, e_int_config_tiling_module);

   /* Configuration itself */
   _G.config_edd = E_CONFIG_DD_NEW("Tiling_Config", Config);
   _G.vdesk_edd = E_CONFIG_DD_NEW("Tiling_Config_VDesk", struct _Config_vdesk);

   E_CONFIG_VAL(_G.config_edd, Config, tile_dialogs, INT);
   E_CONFIG_VAL(_G.config_edd, Config, show_titles, INT);
   E_CONFIG_VAL(_G.config_edd, Config, have_floating_mode, INT);
   E_CONFIG_VAL(_G.config_edd, Config, window_padding, INT);

   E_CONFIG_LIST(_G.config_edd, Config, vdesks, _G.vdesk_edd);
   E_CONFIG_VAL(_G.vdesk_edd, struct _Config_vdesk, x, INT);
   E_CONFIG_VAL(_G.vdesk_edd, struct _Config_vdesk, y, INT);
   E_CONFIG_VAL(_G.vdesk_edd, struct _Config_vdesk, zone_num, INT);
   E_CONFIG_VAL(_G.vdesk_edd, struct _Config_vdesk, nb_stacks, INT);

   tiling_g.config = e_config_domain_load("module.tiling", _G.config_edd);
   if (!tiling_g.config)
     {
        tiling_g.config = E_NEW(Config, 1);
        tiling_g.config->tile_dialogs = 1;
        tiling_g.config->show_titles = 1;
        tiling_g.config->have_floating_mode = 1;
        tiling_g.config->window_padding = 0;
     }

   E_CONFIG_LIMIT(tiling_g.config->tile_dialogs, 0, 1);
   E_CONFIG_LIMIT(tiling_g.config->show_titles, 0, 1);
   E_CONFIG_LIMIT(tiling_g.config->have_floating_mode, 0, 1);
   E_CONFIG_LIMIT(tiling_g.config->window_padding, 0, TILING_MAX_PADDING);

   for (l = tiling_g.config->vdesks; l; l = l->next)
     {
        struct _Config_vdesk *vd;

        vd = l->data;

        E_CONFIG_LIMIT(vd->nb_stacks, 0, 1);
     }

   _G.client_menu_hook = e_int_client_menu_hook_add(_bd_hook, NULL);

   desk = get_current_desk();
   _G.tinfo = _initialize_tinfo(desk);

   /* Add all the existing windows. */
   {
      E_Client *ec;

      E_CLIENT_FOREACH(e_comp_get(NULL), ec)
      {
         _add_client(ec);
      }
   }

   e_gadcon_provider_register(&_gc_class);

   return m;
}

static void
_disable_desk(E_Desk *desk)
{
   check_tinfo(desk);
   if (!_G.tinfo->conf)
     return;

   tiling_window_tree_walk(_G.tinfo->tree, _restore_free_client);
   _G.tinfo->tree = NULL;
}

static void
_disable_all_tiling(void)
{
   const Eina_List *l, *ll;
   E_Comp *comp;
   E_Zone *zone;

   _foreach_desk(_disable_desk);

   EINA_LIST_FOREACH(e_comp_list(), l, comp)
     {
        EINA_LIST_FOREACH(comp->zones, ll, zone)
          {
             e_place_zone_region_smart_cleanup(zone);
          }
     }
}

static void
_foreach_desk(void (*func)(E_Desk *desk))
{
   const Eina_List *l, *ll;
   E_Comp *comp;
   E_Zone *zone;
   E_Desk *desk;
   int x, y;

   EINA_LIST_FOREACH(e_comp_list(), l, comp)
     {
        EINA_LIST_FOREACH(comp->zones, ll, zone)
          {
             for (x = 0; x < zone->desk_x_count; x++)
               {
                  for (y = 0; y < zone->desk_y_count; y++)
                    {
                       desk = zone->desks[x + (y * zone->desk_x_count)];

                       func(desk);
                    }
               }
          }
     }
}

EAPI int
e_modapi_shutdown(E_Module *m EINA_UNUSED)
{
   e_gadcon_provider_unregister(&_gc_class);

   _disable_all_tiling();

   e_int_client_menu_hook_del(_G.client_menu_hook);

   if (tiling_g.log_domain >= 0)
     {
        eina_log_domain_unregister(tiling_g.log_domain);
        tiling_g.log_domain = -1;
     }
#define SAFE_FREE(x, freefunc) \
   if (x) \
     { \
        freefunc(x); \
        x = NULL; \
     }
#define FREE_HANDLER(x)            \
   SAFE_FREE(x, ecore_event_handler_del);

   FREE_HANDLER(_G.handler_client_resize);
   FREE_HANDLER(_G.handler_client_move);
   FREE_HANDLER(_G.handler_client_add);
   FREE_HANDLER(_G.handler_client_remove);

   FREE_HANDLER(_G.handler_client_iconify);
   FREE_HANDLER(_G.handler_client_uniconify);
   FREE_HANDLER(_G.handler_client_property);

   FREE_HANDLER(_G.handler_desk_set);

   SAFE_FREE(_G.handler_client_resize_begin, e_client_hook_del);
#undef FREE_HANDLER
#undef SAFE_FREE

#define ACTION_DEL(act, title, value)             \
  if (act) {                                      \
       e_action_predef_name_del("Tiling", title); \
       e_action_del(value);                       \
       act = NULL;                                \
    }
   ACTION_DEL(_G.act_togglefloat, "Toggle floating", "toggle_floating");
   ACTION_DEL(_G.act_move_up, "Move the focused window up", "move_up");
   ACTION_DEL(_G.act_move_down, "Move the focused window down", "move_down");
   ACTION_DEL(_G.act_move_left, "Move the focused window left", "move_left");
   ACTION_DEL(_G.act_move_right, "Move the focused window right", "move_right");

   ACTION_DEL(_G.act_toggle_split_mode, "Toggle split mode",
              "toggle_split_mode");
   ACTION_DEL(_G.act_swap_window, "Swap window", "swap_window");
#undef ACTION_DEL

   e_configure_registry_item_del("windows/tiling");
   e_configure_registry_category_del("windows");

   E_FREE(tiling_g.config);
   E_CONFIG_DD_FREE(_G.config_edd);
   E_CONFIG_DD_FREE(_G.vdesk_edd);

   tiling_g.module = NULL;

   eina_hash_free(_G.info_hash);
   _G.info_hash = NULL;

   eina_hash_free(_G.client_extras);
   _G.client_extras = NULL;

   _G.tinfo = NULL;

   return 1;
}

EAPI int
e_modapi_save(E_Module *m EINA_UNUSED)
{
   e_config_domain_save("module.tiling", _G.config_edd, tiling_g.config);

   return true;
}

/* GADGET STUFF. */

/* Hack to properly save and free the gadget id. */
static Eina_Stringshare *_current_gad_id = NULL;

static void
_gadget_icon_set(Instance *inst)
{
   switch (_G.split_type)
     {
      case TILING_SPLIT_HORIZONTAL:
        edje_object_signal_emit(inst->gadget, "tiling,mode,horizontal", "e");
        break;

      case TILING_SPLIT_VERTICAL:
        edje_object_signal_emit(inst->gadget, "tiling,mode,vertical", "e");
        break;

      case TILING_SPLIT_FLOAT:
        edje_object_signal_emit(inst->gadget, "tiling,mode,floating", "e");
        break;

      default:
        ERR("Unknown split type.");
     }
}

static void
_tiling_cb_menu_configure(void *data EINA_UNUSED, E_Menu *m EINA_UNUSED, E_Menu_Item *mi EINA_UNUSED)
{
   // FIXME here need to be some checks and return ?
   e_int_config_tiling_module(NULL, NULL);
}

static void
_gadget_mouse_down_cb(void *data, Evas *e, Evas_Object *obj EINA_UNUSED, void *event_info)
{
   Evas_Event_Mouse_Down *ev = event_info;
   Instance *inst = data;

   if (ev->button == 1) /* Change on left-click. */
     {
        _tiling_split_type_next();
     }
   else if (ev->button == 3)
     {
        E_Zone *zone;
        E_Menu *m;
        E_Menu_Item *mi;
        int x, y;

        zone = e_util_zone_current_get(e_manager_current_get());

        m = e_menu_new();
        mi = e_menu_item_new(m);
        e_menu_item_label_set(mi, _("Settings"));
        e_util_menu_item_theme_icon_set(mi, "configure");
        e_menu_item_callback_set(mi, _tiling_cb_menu_configure, NULL);

        m = e_gadcon_client_util_menu_items_append(inst->gcc, m, 0);

        e_gadcon_canvas_zone_geometry_get(inst->gcc->gadcon, &x, &y, NULL, NULL);
        e_menu_activate_mouse(m, zone, x + ev->output.x, y + ev->output.y,
                              1, 1, E_MENU_POP_DIRECTION_AUTO, ev->timestamp);
        evas_event_feed_mouse_up(e, ev->button,
                                 EVAS_BUTTON_NONE, ev->timestamp, NULL);
     }
}

static E_Gadcon_Client *
_gc_init(E_Gadcon *gc, const char *name, const char *id, const char *style)
{
   Evas_Object *o;
   E_Gadcon_Client *gcc;
   Instance *inst;

   inst = E_NEW(Instance, 1);

   o = edje_object_add(gc->evas);
   if (!e_theme_edje_object_set(o, "base/theme/modules/tiling",
                                "modules/tiling/main"))
     edje_object_file_set(o, _G.edj_path, "modules/tiling/main");
   evas_object_show(o);

   gcc = e_gadcon_client_new(gc, name, id, style, o);
   gcc->data = inst;
   inst->gcc = gcc;
   inst->gad_id = _current_gad_id;
   _current_gad_id = NULL;

   evas_object_event_callback_add(o, EVAS_CALLBACK_MOUSE_DOWN,
                                  _gadget_mouse_down_cb, inst);

   inst->gadget = o;

   _gadget_icon_set(inst);

   tiling_g.gadget_instances = eina_list_append(tiling_g.gadget_instances, inst);

   return gcc;
}

static void
_gc_shutdown(E_Gadcon_Client *gcc)
{
   Instance *inst;
   Evas_Object *o;

   if (!(inst = gcc->data)) return;

   o = inst->gadget;

   evas_object_event_callback_del_full(o, EVAS_CALLBACK_MOUSE_DOWN,
                                       _gadget_mouse_down_cb, inst);

   if (inst->gadget)
     evas_object_del(inst->gadget);

   tiling_g.gadget_instances = eina_list_remove(tiling_g.gadget_instances, inst);

   eina_stringshare_del(inst->gad_id);

   E_FREE(inst);
}

static void
_gc_orient(E_Gadcon_Client *gcc, E_Gadcon_Orient orient EINA_UNUSED)
{
   e_gadcon_client_aspect_set(gcc, 16, 16);
   e_gadcon_client_min_size_set(gcc, 16, 16);
}

static const char *
_gc_label(const E_Gadcon_Client_Class *client_class EINA_UNUSED)
{
   return _("Tiling");
}

static Evas_Object *
_gc_icon(const E_Gadcon_Client_Class *client_class EINA_UNUSED, Evas *evas)
{
   Evas_Object *o;

   o = edje_object_add(evas);
   edje_object_file_set(o, _G.edj_path, "icon");
   return o;
}

static const char *
_gc_id_new(const E_Gadcon_Client_Class *client_class EINA_UNUSED)
{
   char buf[1024];

   snprintf(buf, sizeof(buf), "%s %d", _("Tiling"), tiling_g.gadget_number);

   tiling_g.gadget_number++;

   return _current_gad_id = eina_stringshare_add(buf);
}

/* }}} */
