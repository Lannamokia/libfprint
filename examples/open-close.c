/*
 * Minimal open/close validation tool for build-tree runtime testing.
 */

#define FP_COMPONENT "example-open-close"

#include <glib.h>
#include <libfprint/fprint.h>

#include "utilities.h"

typedef struct
{
  GMainLoop *loop;
  int ret;
} OpenCloseData;

static void
on_closed (FpDevice *dev, GAsyncResult *res, gpointer user_data)
{
  OpenCloseData *data = user_data;
  g_autoptr(GError) error = NULL;

  if (!fp_device_close_finish (dev, res, &error))
    {
      g_warning ("close failed: %s", error->message);
      data->ret = 2;
    }

  g_main_loop_quit (data->loop);
}

static void
on_opened (FpDevice *dev, GAsyncResult *res, gpointer user_data)
{
  OpenCloseData *data = user_data;
  g_autoptr(GError) error = NULL;

  if (!fp_device_open_finish (dev, res, &error))
    {
      g_warning ("open failed: %s", error->message);
      data->ret = 1;
      g_main_loop_quit (data->loop);
      return;
    }

  g_print ("opened driver=%s id=%s name=%s\n",
           fp_device_get_driver (dev),
           fp_device_get_device_id (dev),
           fp_device_get_name (dev));

  fp_device_close (dev, NULL, (GAsyncReadyCallback) on_closed, data);
}

int
main (void)
{
  g_autoptr(FpContext) ctx = NULL;
  GPtrArray *devices;
  FpDevice *dev;
  OpenCloseData data = { 0 };

  setenv ("G_MESSAGES_DEBUG", "all", 0);
  setenv ("LIBUSB_DEBUG", "3", 0);

  data.loop = g_main_loop_new (NULL, FALSE);
  data.ret = 0;

  ctx = fp_context_new ();
  devices = fp_context_get_devices (ctx);
  if (!devices)
    {
      g_warning ("failed to enumerate devices");
      return 3;
    }

  dev = discover_device (devices);
  if (!dev)
    {
      g_warning ("no matching device");
      return 4;
    }

  fp_device_open (dev, NULL, (GAsyncReadyCallback) on_opened, &data);
  g_main_loop_run (data.loop);
  g_main_loop_unref (data.loop);

  return data.ret;
}
