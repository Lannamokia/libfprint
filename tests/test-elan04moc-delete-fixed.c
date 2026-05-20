#include <glib.h>

#include <libfprint/fprint.h>

int
main (void)
{
  g_autoptr(FpContext) ctx = NULL;
  GPtrArray *devices;
  FpDevice *dev = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) prints = NULL;
  guint i;

  ctx = fp_context_new ();
  fp_context_enumerate (ctx);
  devices = fp_context_get_devices (ctx);

  if (!devices || !devices->len)
    return 3;

  for (i = 0; i < devices->len; ++i)
    {
      FpDevice *candidate = g_ptr_array_index (devices, i);
      if (g_strcmp0 (fp_device_get_driver (candidate), "elan04moc") == 0)
        {
          dev = candidate;
          break;
        }
    }

  if (!dev)
    return 4;

  if (!fp_device_open_sync (dev, NULL, &error))
    {
      g_warning ("open failed: %s", error->message);
      return 1;
    }

  prints = fp_device_list_prints_sync (dev, NULL, &error);
  if (!prints)
    {
      g_warning ("list failed: %s", error->message);
      fp_device_close_sync (dev, NULL, NULL);
      return 2;
    }

  if (prints->len == 0)
    {
      g_print ("nothing to delete\n");
      fp_device_close_sync (dev, NULL, NULL);
      return 0;
    }

  if (!fp_device_delete_print_sync (dev, g_ptr_array_index (prints, 0), NULL, &error))
    {
      g_warning ("delete failed: %s", error->message);
      fp_device_close_sync (dev, NULL, NULL);
      return 5;
    }

  g_print ("deleted-first-print\n");
  fp_device_close_sync (dev, NULL, NULL);
  return 0;
}
