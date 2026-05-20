#include <glib.h>
#include <glib-unix.h>

#include <libfprint/fprint.h>
#include "fpi-device.h"
#include "fpi-print.h"

#define ELAN04_PRINT_BLOB_V2_SIZE    245
#define ELAN04_PRINT_FLAG_HAS_SECURE_ID_HASH  (1u << 1)
#define ELAN04_PRINT_FLAG_HAS_ADOPTED_KEY     (1u << 4)
#define ELAN04_PRINT_FLAG_PAYLOAD68_ALL_ZERO  (1u << 6)

enum elan04_print_blob_offsets {
  ELAN04_BLOB_OFF_VERSION = 0,
  ELAN04_BLOB_OFF_ORIGIN = 1,
  ELAN04_BLOB_OFF_SUBFACTOR = 2,
  ELAN04_BLOB_OFF_FLAGS = 3,
  ELAN04_BLOB_OFF_SLOT = 4,
  ELAN04_BLOB_OFF_DEVICE_COUNT = 5,
  ELAN04_BLOB_OFF_ADOPTED_KEY = 8,
  ELAN04_BLOB_OFF_SECURE_ID_HASH = 40,
};

static const guint8 k_known_blob_v2[ELAN04_PRINT_BLOB_V2_SIZE] = {
  [ELAN04_BLOB_OFF_VERSION] = 2,
  [ELAN04_BLOB_OFF_ORIGIN] = 1,
  [ELAN04_BLOB_OFF_SUBFACTOR] = 0,
  [ELAN04_BLOB_OFF_FLAGS] = ELAN04_PRINT_FLAG_HAS_SECURE_ID_HASH |
                            ELAN04_PRINT_FLAG_HAS_ADOPTED_KEY |
                            ELAN04_PRINT_FLAG_PAYLOAD68_ALL_ZERO,
  [ELAN04_BLOB_OFF_SLOT] = 0xff,
  [ELAN04_BLOB_OFF_DEVICE_COUNT] = 0xff,

  [ELAN04_BLOB_OFF_ADOPTED_KEY + 0] = 0x18, [ELAN04_BLOB_OFF_ADOPTED_KEY + 1] = 0xf9,
  [ELAN04_BLOB_OFF_ADOPTED_KEY + 2] = 0xe2, [ELAN04_BLOB_OFF_ADOPTED_KEY + 3] = 0x63,
  [ELAN04_BLOB_OFF_ADOPTED_KEY + 4] = 0x30, [ELAN04_BLOB_OFF_ADOPTED_KEY + 5] = 0x58,
  [ELAN04_BLOB_OFF_ADOPTED_KEY + 6] = 0x8a, [ELAN04_BLOB_OFF_ADOPTED_KEY + 7] = 0xce,
  [ELAN04_BLOB_OFF_ADOPTED_KEY + 8] = 0x86, [ELAN04_BLOB_OFF_ADOPTED_KEY + 9] = 0x88,
  [ELAN04_BLOB_OFF_ADOPTED_KEY + 10] = 0x69, [ELAN04_BLOB_OFF_ADOPTED_KEY + 11] = 0x5e,
  [ELAN04_BLOB_OFF_ADOPTED_KEY + 12] = 0x75, [ELAN04_BLOB_OFF_ADOPTED_KEY + 13] = 0xa8,
  [ELAN04_BLOB_OFF_ADOPTED_KEY + 14] = 0x06, [ELAN04_BLOB_OFF_ADOPTED_KEY + 15] = 0x2b,
  [ELAN04_BLOB_OFF_ADOPTED_KEY + 16] = 0x47, [ELAN04_BLOB_OFF_ADOPTED_KEY + 17] = 0xa2,
  [ELAN04_BLOB_OFF_ADOPTED_KEY + 18] = 0x3c, [ELAN04_BLOB_OFF_ADOPTED_KEY + 19] = 0x32,
  [ELAN04_BLOB_OFF_ADOPTED_KEY + 20] = 0x7a, [ELAN04_BLOB_OFF_ADOPTED_KEY + 21] = 0xa6,
  [ELAN04_BLOB_OFF_ADOPTED_KEY + 22] = 0xe6, [ELAN04_BLOB_OFF_ADOPTED_KEY + 23] = 0xfc,
  [ELAN04_BLOB_OFF_ADOPTED_KEY + 24] = 0xad, [ELAN04_BLOB_OFF_ADOPTED_KEY + 25] = 0x00,
  [ELAN04_BLOB_OFF_ADOPTED_KEY + 26] = 0x85, [ELAN04_BLOB_OFF_ADOPTED_KEY + 27] = 0xc2,
  [ELAN04_BLOB_OFF_ADOPTED_KEY + 28] = 0x83, [ELAN04_BLOB_OFF_ADOPTED_KEY + 29] = 0x0b,
  [ELAN04_BLOB_OFF_ADOPTED_KEY + 30] = 0x2c, [ELAN04_BLOB_OFF_ADOPTED_KEY + 31] = 0x80,

  [ELAN04_BLOB_OFF_SECURE_ID_HASH + 0] = 0x62, [ELAN04_BLOB_OFF_SECURE_ID_HASH + 1] = 0xbf,
  [ELAN04_BLOB_OFF_SECURE_ID_HASH + 2] = 0x58, [ELAN04_BLOB_OFF_SECURE_ID_HASH + 3] = 0xea,
  [ELAN04_BLOB_OFF_SECURE_ID_HASH + 4] = 0xce, [ELAN04_BLOB_OFF_SECURE_ID_HASH + 5] = 0x0e,
  [ELAN04_BLOB_OFF_SECURE_ID_HASH + 6] = 0xe0, [ELAN04_BLOB_OFF_SECURE_ID_HASH + 7] = 0xab,
  [ELAN04_BLOB_OFF_SECURE_ID_HASH + 8] = 0x4e, [ELAN04_BLOB_OFF_SECURE_ID_HASH + 9] = 0x23,
  [ELAN04_BLOB_OFF_SECURE_ID_HASH + 10] = 0xec, [ELAN04_BLOB_OFF_SECURE_ID_HASH + 11] = 0x3f,
  [ELAN04_BLOB_OFF_SECURE_ID_HASH + 12] = 0x9b, [ELAN04_BLOB_OFF_SECURE_ID_HASH + 13] = 0xec,
  [ELAN04_BLOB_OFF_SECURE_ID_HASH + 14] = 0x38, [ELAN04_BLOB_OFF_SECURE_ID_HASH + 15] = 0x5c,
  [ELAN04_BLOB_OFF_SECURE_ID_HASH + 16] = 0x23, [ELAN04_BLOB_OFF_SECURE_ID_HASH + 17] = 0xc0,
  [ELAN04_BLOB_OFF_SECURE_ID_HASH + 18] = 0xd7, [ELAN04_BLOB_OFF_SECURE_ID_HASH + 19] = 0x5d,
  [ELAN04_BLOB_OFF_SECURE_ID_HASH + 20] = 0xed, [ELAN04_BLOB_OFF_SECURE_ID_HASH + 21] = 0x89,
  [ELAN04_BLOB_OFF_SECURE_ID_HASH + 22] = 0xaa, [ELAN04_BLOB_OFF_SECURE_ID_HASH + 23] = 0x43,
  [ELAN04_BLOB_OFF_SECURE_ID_HASH + 24] = 0xc3, [ELAN04_BLOB_OFF_SECURE_ID_HASH + 25] = 0xba,
  [ELAN04_BLOB_OFF_SECURE_ID_HASH + 26] = 0xa5, [ELAN04_BLOB_OFF_SECURE_ID_HASH + 27] = 0x87,
  [ELAN04_BLOB_OFF_SECURE_ID_HASH + 28] = 0x5e, [ELAN04_BLOB_OFF_SECURE_ID_HASH + 29] = 0x29,
  [ELAN04_BLOB_OFF_SECURE_ID_HASH + 30] = 0x9e, [ELAN04_BLOB_OFF_SECURE_ID_HASH + 31] = 0x0f,
};

typedef struct
{
  GMainLoop    *loop;
  GCancellable *cancellable;
  int           ret;
} VerifyFixedData;

static void
on_closed (FpDevice *dev, GAsyncResult *res, gpointer user_data)
{
  VerifyFixedData *data = user_data;
  g_autoptr(GError) error = NULL;

  if (!fp_device_close_finish (dev, res, &error))
    g_warning ("close failed: %s", error->message);

  g_main_loop_quit (data->loop);
}

static FpPrint *
create_known_print (FpDevice *dev)
{
  FpPrint *print;
  GVariant *blob;
  GVariant *data;

  print = fp_print_new (dev);
  blob = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, k_known_blob_v2, sizeof (k_known_blob_v2), 1);
  data = g_variant_new ("(@ay)", blob);

  fp_print_set_finger (print, FP_FINGER_RIGHT_INDEX);
  fp_print_set_username (print, g_get_user_name ());
  fp_print_set_description (print, "known-id32");
  fpi_print_set_type (print, FPI_PRINT_RAW);
  fpi_print_set_device_stored (print, TRUE);
  g_object_set (print, "fpi-data", data, NULL);

  return print;
}

static void
verify_fixed_quit (FpDevice *dev, VerifyFixedData *data)
{
  if (!fp_device_is_open (dev))
    {
      g_main_loop_quit (data->loop);
      return;
    }

  fp_device_close (dev, NULL, (GAsyncReadyCallback) on_closed, data);
}

static void
on_verify_match (FpDevice *dev,
                 FpPrint  *match,
                 FpPrint  *print,
                 gpointer  user_data,
                 GError   *error)
{
  VerifyFixedData *data = user_data;

  if (error)
    {
      g_warning ("verify callback error: %s", error->message);
      return;
    }

  if (match)
    {
      g_print ("MATCH report received\n");
      data->ret = 0;
    }
  else
    {
      g_print ("NO MATCH report received\n");
      data->ret = 5;
    }

  if (print)
    g_print ("Scanned print description: %s\n", fp_print_get_description (print));
}

static void
on_verify_completed (FpDevice *dev, GAsyncResult *res, gpointer user_data)
{
  VerifyFixedData *data = user_data;
  gboolean matched = FALSE;
  g_autoptr(FpPrint) print = NULL;
  g_autoptr(GError) error = NULL;

  if (!fp_device_verify_finish (dev, res, &matched, &print, &error))
    {
      g_warning ("verify failed: %s", error->message);
      data->ret = 6;
      verify_fixed_quit (dev, data);
      return;
    }

  g_print ("verify finished matched=%s\n", matched ? "true" : "false");
  data->ret = matched ? 0 : 7;
  verify_fixed_quit (dev, data);
}

static void
on_opened (FpDevice *dev, GAsyncResult *res, gpointer user_data)
{
  VerifyFixedData *data = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(FpPrint) known = NULL;

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

  known = create_known_print (dev);
  g_print ("Ready for finger, verifying against known id32...\n");
  fp_device_verify (dev, known, data->cancellable,
                    on_verify_match, data, NULL,
                    (GAsyncReadyCallback) on_verify_completed,
                    data);
}

static gboolean
sigint_cb (gpointer user_data)
{
  VerifyFixedData *data = user_data;
  g_cancellable_cancel (data->cancellable);
  return G_SOURCE_CONTINUE;
}

int
main (void)
{
  g_autoptr(FpContext) ctx = NULL;
  GPtrArray *devices;
  FpDevice *dev = NULL;
  VerifyFixedData data = { 0 };
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
