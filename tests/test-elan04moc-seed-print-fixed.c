#include <glib.h>
#include <glib/gstdio.h>

#include <libfprint/fprint.h>
#include "fpi-print.h"

#define ELAN04_PRINT_BLOB_V2_SIZE    245
#define ELAN04_PRINT_FLAG_HAS_SECURE_ID_HASH  (1u << 1)
#define ELAN04_PRINT_FLAG_HAS_MAC_D_HASH      (1u << 2)
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
  ELAN04_BLOB_OFF_MAC_D_HASH = 72,
};

static const guint8 k_blob_v2[ELAN04_PRINT_BLOB_V2_SIZE] = {
  [ELAN04_BLOB_OFF_VERSION] = 2,
  [ELAN04_BLOB_OFF_ORIGIN] = 1,
  [ELAN04_BLOB_OFF_SUBFACTOR] = 0,
  [ELAN04_BLOB_OFF_FLAGS] = ELAN04_PRINT_FLAG_HAS_SECURE_ID_HASH |
                            ELAN04_PRINT_FLAG_HAS_MAC_D_HASH |
                            ELAN04_PRINT_FLAG_HAS_ADOPTED_KEY |
                            ELAN04_PRINT_FLAG_PAYLOAD68_ALL_ZERO,
  [ELAN04_BLOB_OFF_SLOT] = 0,
  [ELAN04_BLOB_OFF_DEVICE_COUNT] = 1,

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

  [ELAN04_BLOB_OFF_MAC_D_HASH + 0] = 0x42, [ELAN04_BLOB_OFF_MAC_D_HASH + 1] = 0xf2,
  [ELAN04_BLOB_OFF_MAC_D_HASH + 2] = 0xa6, [ELAN04_BLOB_OFF_MAC_D_HASH + 3] = 0x94,
  [ELAN04_BLOB_OFF_MAC_D_HASH + 4] = 0x0c, [ELAN04_BLOB_OFF_MAC_D_HASH + 5] = 0xc7,
  [ELAN04_BLOB_OFF_MAC_D_HASH + 6] = 0x0a, [ELAN04_BLOB_OFF_MAC_D_HASH + 7] = 0x18,
  [ELAN04_BLOB_OFF_MAC_D_HASH + 8] = 0x44, [ELAN04_BLOB_OFF_MAC_D_HASH + 9] = 0x3b,
  [ELAN04_BLOB_OFF_MAC_D_HASH + 10] = 0x93, [ELAN04_BLOB_OFF_MAC_D_HASH + 11] = 0x2e,
  [ELAN04_BLOB_OFF_MAC_D_HASH + 12] = 0x6d, [ELAN04_BLOB_OFF_MAC_D_HASH + 13] = 0xa6,
  [ELAN04_BLOB_OFF_MAC_D_HASH + 14] = 0x78, [ELAN04_BLOB_OFF_MAC_D_HASH + 15] = 0x79,
  [ELAN04_BLOB_OFF_MAC_D_HASH + 16] = 0x32, [ELAN04_BLOB_OFF_MAC_D_HASH + 17] = 0x3f,
  [ELAN04_BLOB_OFF_MAC_D_HASH + 18] = 0x5e, [ELAN04_BLOB_OFF_MAC_D_HASH + 19] = 0xa3,
  [ELAN04_BLOB_OFF_MAC_D_HASH + 20] = 0x4e, [ELAN04_BLOB_OFF_MAC_D_HASH + 21] = 0x0a,
  [ELAN04_BLOB_OFF_MAC_D_HASH + 22] = 0xe0, [ELAN04_BLOB_OFF_MAC_D_HASH + 23] = 0x07,
  [ELAN04_BLOB_OFF_MAC_D_HASH + 24] = 0x9f, [ELAN04_BLOB_OFF_MAC_D_HASH + 25] = 0xf3,
  [ELAN04_BLOB_OFF_MAC_D_HASH + 26] = 0x49, [ELAN04_BLOB_OFF_MAC_D_HASH + 27] = 0x88,
  [ELAN04_BLOB_OFF_MAC_D_HASH + 28] = 0x2c, [ELAN04_BLOB_OFF_MAC_D_HASH + 29] = 0x89,
  [ELAN04_BLOB_OFF_MAC_D_HASH + 30] = 0x96, [ELAN04_BLOB_OFF_MAC_D_HASH + 31] = 0x82,
};

int
main (void)
{
  g_autoptr(FpContext) ctx = NULL;
  GPtrArray *devices;
  FpDevice *dev = NULL;
  g_autoptr(FpPrint) print = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree guchar *serialized = NULL;
  g_autofree gchar *path = NULL;
  g_autofree gchar *dirpath = NULL;
  GVariant *blob;
  GVariant *data;
  gsize serialized_len = 0;
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

  print = fp_print_new (dev);
  fp_print_set_finger (print, FP_FINGER_RIGHT_INDEX);
  fp_print_set_username (print, g_get_user_name ());
  fp_print_set_description (print, "ELAN adopted template");
  fpi_print_set_type (print, FPI_PRINT_RAW);
  fpi_print_set_device_stored (print, TRUE);

  blob = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, k_blob_v2, sizeof (k_blob_v2), 1);
  data = g_variant_new ("(@ay)", blob);
  g_object_set (print, "fpi-data", data, NULL);

  if (!fp_print_serialize (print, &serialized, &serialized_len, &error))
    {
      g_warning ("serialize failed: %s", error->message);
      return 1;
    }

  path = g_build_filename ("/var/lib/fprint",
                           g_get_user_name (),
                           fp_print_get_driver (print),
                           fp_print_get_device_id (print),
                           "7",
                           NULL);
  dirpath = g_path_get_dirname (path);
  if (g_mkdir_with_parents (dirpath, 0700) < 0)
    return 2;

  if (!g_file_set_contents (path, (const gchar *) serialized, serialized_len, &error))
    {
      g_warning ("save failed: %s", error->message);
      return 5;
    }

  g_print ("seeded=%s\n", path);
  return 0;
}
