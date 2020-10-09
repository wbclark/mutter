/*
 * Copyright (C) 2020 Red Hat
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Author: Carlos Garnacho <carlosg@gnome.org>
 */

#include "config.h"

#include "meta-wayland-activation.h"

#include <glib.h>
#include <wayland-server.h>

#include "wayland/meta-wayland-private.h"
#include "wayland/meta-wayland-versions.h"
#include "xdg-activation-unstable-v1-server-protocol.h"

static void
unbind_resource (struct wl_resource *resource)
{
  wl_list_remove (wl_resource_get_link (resource));
}

static void
activation_destroy (struct wl_client   *client,
                    struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static struct wl_resource *
meta_wayland_activation_token_create_new_resource (MetaWaylandActivation *activation,
                                                   struct wl_client      *client,
                                                   struct wl_resource    *activation_resource,
                                                   uint32_t               id)
{
  struct wl_resource *activation_token_resource;

  activation_token_resource =
    wl_resource_create (client, &zxdg_activation_token_provider_v1_interface,
                        wl_resource_get_version (activation_resource),
                        id);

  return activation_token_resource;
}

static void
activation_get_activation_token (struct wl_client   *client,
                                 struct wl_resource *resource,
                                 uint32_t            id,
                                 struct wl_resource *surface_resource,
                                 uint32_t            serial,
                                 struct wl_resource *seat_resource)
{
  MetaWaylandActivation *activation = wl_resource_get_user_data (resource);
  MetaWaylandSeat *seat = wl_resource_get_user_data (seat_resource);
  MetaWaylandSurface *surface = wl_resource_get_user_data (surface_resource);
  struct wl_resource *token_resource;
  gchar *token;

  token_resource =
    meta_wayland_activation_token_create_new_resource (activation,
                                                       client,
                                                       resource,
                                                       id);

  if (meta_wayland_seat_get_grab_info (seat, surface, serial, FALSE, NULL, NULL))
    {
      token = g_uuid_string_random ();
      zxdg_activation_token_provider_v1_send_done (token_resource, token);
      g_hash_table_insert (activation->token_resources, token, token_resource);
    }
  else
    {
      zxdg_activation_token_provider_v1_send_failed (token_resource);
    }
}

static void
sequence_complete_cb (MetaStartupSequence   *sequence,
                      MetaWaylandActivation *activation)
{
  struct wl_resource *token_resource;

  token_resource = g_hash_table_lookup (activation->sequences, sequence);
  if (token_resource)
    g_hash_table_remove (activation->sequences, sequence);
}

static void
activation_associate (struct wl_client   *client,
                      struct wl_resource *resource,
                      const char         *token,
                      const char         *app_id)
{
  MetaWaylandActivation *activation = wl_resource_get_user_data (resource);
  MetaDisplay *display = meta_get_display ();
  MetaStartupSequence *sequence;
  struct wl_resource *token_resource;
  uint32_t timestamp;

  token_resource = g_hash_table_lookup (activation->token_resources, token);
  g_hash_table_steal (activation->token_resources, token);
  if (!token_resource)
    return;

  timestamp = meta_display_get_current_time_roundtrip (display);
  sequence = g_object_new (META_TYPE_STARTUP_SEQUENCE,
                           "id", token,
                           "application-id", app_id,
                           "timestamp", timestamp,
                           NULL);

  g_signal_connect (sequence,
                    "complete",
                    G_CALLBACK (sequence_complete_cb),
                    activation);

  meta_startup_notification_add_sequence (display->startup_notification,
                                          sequence);

  g_hash_table_insert (activation->sequences, sequence, token_resource);
  wl_resource_set_user_data (token_resource, sequence);
}

static void
activation_set_activation_token (struct wl_client   *client,
                                 struct wl_resource *resource,
                                 const char         *token)
{
  MetaWaylandActivation *activation = wl_resource_get_user_data (resource);
  MetaDisplay *display = meta_get_display ();
  MetaStartupSequence *sequence;

  sequence = meta_startup_notification_lookup_sequence (display->startup_notification,
                                                        token);
  if (sequence)
    {
      meta_startup_sequence_complete (sequence);
      g_hash_table_insert (activation->ack_clients, client, sequence);
    }
}

static void
activation_activate (struct wl_client   *client,
                     struct wl_resource *resource,
                     struct wl_resource *surface_resource)
{
  MetaWaylandActivation *activation = wl_resource_get_user_data (resource);
  MetaWaylandSurface *surface = wl_resource_get_user_data (surface_resource);
  MetaDisplay *display = meta_get_display ();
  MetaStartupSequence *sequence = NULL;
  MetaWindow *window;

  window = meta_wayland_surface_get_window (surface);
  if (!window)
    return;

  sequence = g_hash_table_lookup (activation->ack_clients, client);
  g_hash_table_steal (activation->ack_clients, client);

  if (sequence)
    {
      uint32_t timestamp;
      int32_t workspace_idx;

      workspace_idx = meta_startup_sequence_get_workspace (sequence);
      timestamp = meta_startup_sequence_get_timestamp (sequence);

      meta_startup_sequence_complete (sequence);
      meta_startup_notification_remove_sequence (display->startup_notification,
                                                 sequence);
      if (workspace_idx >= 0)
        meta_window_change_workspace_by_index (window, workspace_idx, TRUE);

      meta_window_activate_full (window, timestamp,
                                 META_CLIENT_TYPE_APPLICATION, NULL);
    }
  else
    {
      meta_window_set_demands_attention (window);
    }
}

static const struct zxdg_activation_v1_interface activation_interface = {
  activation_destroy,
  activation_get_activation_token,
  activation_associate,
  activation_set_activation_token,
  activation_activate,
};

static void
bind_activation (struct wl_client *client,
                 void             *data,
                 uint32_t          version,
                 uint32_t          id)
{
  MetaWaylandCompositor *compositor = data;
  MetaWaylandActivation *activation = compositor->activation;
  struct wl_resource *resource;

  resource = wl_resource_create (client, &zxdg_activation_v1_interface,
                                 MIN (version, META_ZXDG_ACTIVATION_V1_VERSION),
                                 id);
  wl_resource_set_implementation (resource, &activation_interface,
                                  activation, unbind_resource);
  wl_resource_set_user_data (resource, activation);
  wl_list_insert (&activation->resource_list,
                  wl_resource_get_link (resource));
}

void
meta_wayland_activation_init (MetaWaylandCompositor *compositor)
{
  MetaWaylandActivation *activation;

  activation = g_new0 (MetaWaylandActivation, 1);
  activation->compositor = compositor;
  activation->wl_display = compositor->wayland_display;
  wl_list_init (&activation->resource_list);

  activation->sequences = g_hash_table_new_full (NULL, NULL,
                                                 g_object_unref,
                                                 NULL);
  activation->token_resources = g_hash_table_new_full (g_str_hash,
                                                       g_str_equal,
                                                       g_free,
                                                       NULL);
  activation->ack_clients = g_hash_table_new (NULL, NULL);

  wl_global_create (activation->wl_display,
                    &zxdg_activation_v1_interface, 1,
                    compositor, bind_activation);

  compositor->activation = activation;
}
