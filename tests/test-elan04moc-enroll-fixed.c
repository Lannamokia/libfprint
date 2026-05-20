#include <glib.h>
#include <glib-unix.h>

#include <libfprint/fprint.h>
#include "fpi-device.h"
#include "fpi-print.h"

typedef struct
{
  GMainLoop    *loop;
  GCancellable *cancellable;
  int           ret;
} EnrollFixedData;

static void
on_closed (FpDevice *dev, GAsyncResult *res, gpointer user_data)
{
  EnrollFixedData *data = user_data;
  g_autoptr(GError) error = NULL;

  g_print ("on_closed entered\n");

  if (!fp_device_close_finish (dev, res, &error))
    g_warning ("close failed: %s", error->message);

  g_print ("on_closed quitting main loop\n");
  g_main_loop_quit (data->loop);
}

static void
enroll_fixed_quit (FpDevice *dev, EnrollFixedData *data)
{
  g_print ("enroll_fixed_quit entered is_open=%d\n", fp_device_is_open (dev));

  if (!fp_device_is_open (dev))
    {
      g_print ("device already closed, quitting main loop directly\n");
      g_main_loop_quit (data->loop);
      return;
    }

  g_print ("calling fp_device_close\n");
  fp_device_close (dev, NULL, (GAsyncReadyCallback) on_closed, data);
}

static void
on_enroll_progress (FpDevice *device,
                    gint      completed_stages,
                    FpPrint  *print,
                    gpointer  user_data,
                    GError   *error)
{
  if (error)
    g_warning ("enroll progress retry/error: %s", error->message);
  else
    g_print ("enroll progress stage=%d/%d\n",
             completed_stages,
             fp_device_get_nr_enroll_stages (device));

  if (print)
    g_print ("progress print description: %s\n", fp_print_get_description (print));
}

static void
on_enroll_completed (FpDevice *dev, GAsyncResult *res, gpointer user_data)
{
  EnrollFixedData *data = user_data;
  g_autoptr(FpPrint) print = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) fpi_data = NULL;

  g_print ("on_enroll_completed entered\n");

  print = fp_device_enroll_finish (dev, res, &error);
  if (!print)
    {
      g_warning ("enroll failed: %s", error->message);
      data->ret = 10;
      enroll_fixed_quit (dev, data);
      return;
    }

  g_object_get (print, "fpi-data", &fpi_data, NULL);
  g_print ("enroll complete description=%s device_stored=%d\n",
           fp_print_get_description (print),
           fp_print_get_device_stored (print));
  if (fpi_data)
    g_print ("enroll produced fpi-data type=%s\n", g_variant_get_type_string (fpi_data));

  data->ret = 0;
  g_print ("on_enroll_completed calling enroll_fixed_quit\n");
  enroll_fixed_quit (dev, data);
}

static void
on_opened (FpDevice *dev, GAsyncResult *res, gpointer user_data)
{
  EnrollFixedData *data = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(FpPrint) tmpl = NULL;

  if (!fp_device_open_finish (dev, res, &error))
    {
      g_warning ("open failed: %s", error->message);
      data->ret = 1;
      g_main_loop_quit (data->loop);
      return;
    }

  g_print ("opened driver=%s name=%s\n",
           fp_device_get_driver (dev),
           fp_device_get_name (dev));

  tmpl = fp_print_new (dev);
  fp_print_set_finger (tmpl, FP_FINGER_RIGHT_INDEX);
  fp_print_set_username (tmpl, g_get_user_name ());
  fp_print_set_description (tmpl, "elan04moc-adopt");

  g_print ("Ready for finger, enrolling as adopt...\n");
  fp_device_enroll (dev, g_steal_pointer (&tmpl), data->cancellable,
                    on_enroll_progress, data, NULL,
                    (GAsyncReadyCallback) on_enroll_completed,
                    data);
}

static gboolean
sigint_cb (gpointer user_data)
{
  EnrollFixedData *data = user_data;
  g_cancellable_cancel (data->cancellable);
  return G_SOURCE_CONTINUE;
}

int
main (void)
{
  g_autoptr(FpContext) ctx = NULL;
  GPtrArray *devices;
  FpDevice *dev = NULL;
  EnrollFixedData data = { 0 };
  guint i;

  setenv ("G_MESSAGES_DEBUG", "all", 0);
  setenv ("LIBUSB_DEBUG", "3", 0);

  data.loop = g_main_loop_new (NULL, FALSE);
  data.cancellable = g_cancellable_new ();
  data.ret = 2;
  g_unix_signal_add_full (G_PRIORITY_HIGH, SIGINT, sigint_cb, &data, NULL);

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

  fp_device_open (dev, data.cancellable, (GAsyncReadyCallback) on_opened, &data);
  g_main_loop_run (data.loop);
  g_main_loop_unref (data.loop);
  g_object_unref (data.cancellable);

  return data.ret;
}
