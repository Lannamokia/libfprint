/*
 * Minimal runtime probe tool for build-tree validation.
 */

#define FP_COMPONENT "example-device-info"

#include <glib.h>
#include <libfprint/fprint.h>

int
main (void)
{
  g_autoptr(FpContext) ctx = NULL;
  GPtrArray *devices;
  guint i;

  ctx = fp_context_new ();
  fp_context_enumerate (ctx);
  devices = fp_context_get_devices (ctx);

  g_print ("devices=%u\n", devices ? devices->len : 0);
  if (!devices)
    return 1;

  for (i = 0; i < devices->len; ++i)
    {
      FpDevice *dev = g_ptr_array_index (devices, i);
      g_print ("[%u] driver=%s id=%s name=%s features=0x%x scan=%d\n",
               i,
               fp_device_get_driver (dev),
               fp_device_get_device_id (dev),
               fp_device_get_name (dev),
               fp_device_get_features (dev),
               fp_device_get_scan_type (dev));
    }

  return 0;
}
