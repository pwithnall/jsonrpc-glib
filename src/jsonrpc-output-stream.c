/* jsonrpc-output-stream.c
 *
 * Copyright (C) 2016 Christian Hergert <chergert@redhat.com>
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version.
 *
 * This file is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define G_LOG_DOMAIN "jsonrpc-output-stream"

#include <string.h>

#include "jsonrpc-output-stream.h"
#include "jsonrpc-version.h"

G_DEFINE_TYPE (JsonrpcOutputStream, jsonrpc_output_stream, G_TYPE_DATA_OUTPUT_STREAM)

static gboolean jsonrpc_output_stream_debug;

static void
jsonrpc_output_stream_class_init (JsonrpcOutputStreamClass *klass)
{
  jsonrpc_output_stream_debug = !!g_getenv ("JSONRPC_DEBUG");
}

static void
jsonrpc_output_stream_init (JsonrpcOutputStream *self)
{
}

static GBytes *
jsonrpc_output_stream_create_bytes (JsonrpcOutputStream  *self,
                                    JsonNode             *node,
                                    GError              **error)
{
  g_autofree gchar *str = NULL;
  GString *message;
  gsize len;

  g_assert (JSONRPC_IS_OUTPUT_STREAM (self));
  g_assert (node != NULL);

  if (!JSON_NODE_HOLDS_OBJECT (node) && !JSON_NODE_HOLDS_ARRAY (node))
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_INVAL,
                   "node must be an array or object");
      return FALSE;
    }

  str = json_to_string (node, FALSE);
  len = strlen (str);

  if G_UNLIKELY (jsonrpc_output_stream_debug)
    g_message (">>> %s", str);

  /*
   * Try to allocate our buffer in a single shot. Sadly we can't serialize
   * JsonNode directly into a GString or we could remove the double
   * allocation going on here.
   */
  message = g_string_sized_new (len + 32);

  g_string_append_printf (message, "Content-Length: %"G_GSIZE_FORMAT"\r\n\r\n", len);
  g_string_append_len (message, str, len);

  len = message->len;

  return g_bytes_new_take (g_string_free (message, FALSE), len);
}

JsonrpcOutputStream *
jsonrpc_output_stream_new (GOutputStream *base_stream)
{
  return g_object_new (JSONRPC_TYPE_OUTPUT_STREAM,
                       "base-stream", base_stream,
                       NULL);
}

static void
jsonrpc_output_stream_flush_cb (GObject      *object,
                                GAsyncResult *result,
                                gpointer      user_data)
{
  GOutputStream *stream = (GOutputStream *)object;
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = user_data;

  g_assert (G_IS_OUTPUT_STREAM (stream));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (G_IS_TASK (task));

  if (!g_output_stream_flush_finish (stream, result, &error))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  g_task_return_boolean (task, TRUE);
}

static void
jsonrpc_output_stream_write_message_async_cb (GObject      *object,
                                              GAsyncResult *result,
                                              gpointer      user_data)
{
  GOutputStream *stream = (GOutputStream *)object;
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = user_data;
  GCancellable *cancellable;
  GBytes *bytes;
  gsize n_written;

  g_assert (G_IS_OUTPUT_STREAM (stream));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (G_IS_TASK (task));

  if (!g_output_stream_write_all_finish (stream, result, &n_written, &error))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  bytes = g_task_get_task_data (task);

  if (g_bytes_get_size (bytes) != n_written)
    {
      g_task_return_new_error (task,
                               G_IO_ERROR,
                               G_IO_ERROR_CLOSED,
                               "Failed to write all bytes to peer");
      return;
    }

  cancellable = g_task_get_cancellable (task);

  g_output_stream_flush_async (stream,
                               G_PRIORITY_DEFAULT,
                               cancellable,
                               jsonrpc_output_stream_flush_cb,
                               g_steal_pointer (&task));
}

void
jsonrpc_output_stream_write_message_async (JsonrpcOutputStream *self,
                                           JsonNode            *node,
                                           GCancellable        *cancellable,
                                           GAsyncReadyCallback  callback,
                                           gpointer             user_data)
{
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GTask) task = NULL;
  g_autoptr(GError) error = NULL;
  const guint8 *data;
  gsize len;

  g_return_if_fail (JSONRPC_IS_OUTPUT_STREAM (self));
  g_return_if_fail (node != NULL);
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, jsonrpc_output_stream_write_message_async);

  if (NULL == (bytes = jsonrpc_output_stream_create_bytes (self, node, &error)))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  g_task_set_task_data (task, g_bytes_ref (bytes), (GDestroyNotify)g_bytes_unref);

  data = g_bytes_get_data (bytes, &len);

  g_output_stream_write_all_async (G_OUTPUT_STREAM (self),
                                   data,
                                   len,
                                   G_PRIORITY_DEFAULT,
                                   cancellable,
                                   jsonrpc_output_stream_write_message_async_cb,
                                   g_steal_pointer (&task));
}

gboolean
jsonrpc_output_stream_write_message_finish (JsonrpcOutputStream  *self,
                                            GAsyncResult         *result,
                                            GError              **error)
{
  g_return_val_if_fail (JSONRPC_IS_OUTPUT_STREAM (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
jsonrpc_output_stream_write_message_sync_cb (GObject      *object,
                                             GAsyncResult *result,
                                             gpointer      user_data)
{
  JsonrpcOutputStream *self = (JsonrpcOutputStream *)object;
  GTask *task = user_data;
  g_autoptr(GError) error = NULL;

  g_assert (JSONRPC_IS_OUTPUT_STREAM (self));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (G_IS_TASK (task));

  if (!jsonrpc_output_stream_write_message_finish (self, result, &error))
    g_task_return_error (task, g_steal_pointer (&error));
  else
    g_task_return_boolean (task, TRUE);
}

gboolean
jsonrpc_output_stream_write_message (JsonrpcOutputStream  *self,
                                     JsonNode             *node,
                                     GCancellable         *cancellable,
                                     GError              **error)
{
  g_autoptr(GTask) task = NULL;
  g_autoptr(GMainContext) main_context = NULL;

  g_return_val_if_fail (JSONRPC_IS_OUTPUT_STREAM (self), FALSE);
  g_return_val_if_fail (node != NULL, FALSE);
  g_return_val_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable), FALSE);

  main_context = g_main_context_ref_thread_default ();

  task = g_task_new (NULL, NULL, NULL, NULL);
  g_task_set_source_tag (task, jsonrpc_output_stream_write_message);

  jsonrpc_output_stream_write_message_async (self,
                                             node,
                                             cancellable,
                                             jsonrpc_output_stream_write_message_sync_cb,
                                             task);

  while (!g_task_get_completed (task))
    g_main_context_iteration (main_context, TRUE);

  return g_task_propagate_boolean (task, error);
}
