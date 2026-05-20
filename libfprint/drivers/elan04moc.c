/*
 * Experimental driver for ELAN 04f3:0c4c shared-storage Match-On-Chip sensors
 *
 * This implementation is based on reverse engineering of the Windows stack
 * shipped with driver package SP140939 and on live-device testing.
 *
 * Current scope:
 * - open / close
 * - verify / identify using 40 FF 03 -> 40 FF 0C
 * - enroll-as-adopt using the same identity source
 * - local unlink only for delete
 *
 * It intentionally does not implement Linux-owned enroll, device-side delete
 * or erase.
 */

#define FP_COMPONENT "elan04moc"

#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <libusb.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#include <string.h>

#include "drivers_api.h"
#include "fpi-byte-reader.h"

G_DECLARE_FINAL_TYPE (FpiDeviceElan04Moc, fpi_device_elan04moc, FPI, DEVICE_ELAN04MOC, FpDevice)

#define ELAN04_DRIVER_FULLNAME "Elan 04f3:0c4c Shared-Storage MOC"

#define ELAN04_EP_OUT          (0x01 | LIBUSB_ENDPOINT_OUT)
#define ELAN04_EP_IN           (0x83 | LIBUSB_ENDPOINT_IN)
#define ELAN04_EP_VERIFY_IN    (0x84 | LIBUSB_ENDPOINT_IN)

#define ELAN04_TIMEOUT_MS      5000
#define ELAN04_VERIFY_TIMEOUT  15000

#define ELAN04_LEN_HOST_RANDOM 32
#define ELAN04_LEN_HOST_PUB    65
#define ELAN04_LEN_CONNECT_RSP 0x4b2
#define ELAN04_LEN_APPKEY_RSP  0x42
#define ELAN04_LEN_VERIFY_RSP  2
#define ELAN04_LEN_IDENTIFY_RSP 0x42
#define ELAN04_LEN_SUBSID_RSP  0x46
#define ELAN04_LEN_COUNT_RSP   2

#define ELAN04_PRINT_BLOB_V2_VERSION 2
#define ELAN04_PRINT_BLOB_V2_SIZE    245

#define ELAN04_PRINT_FLAG_HAS_SLOT            (1u << 0)
#define ELAN04_PRINT_FLAG_HAS_SECURE_ID_HASH  (1u << 1)
#define ELAN04_PRINT_FLAG_HAS_MAC_D_HASH      (1u << 2)
#define ELAN04_PRINT_FLAG_HAS_IDENTITY_HASH   (1u << 3)
#define ELAN04_PRINT_FLAG_HAS_ADOPTED_KEY     (1u << 4)
#define ELAN04_PRINT_FLAG_HAS_RAW_IDENTITY    (1u << 5)
#define ELAN04_PRINT_FLAG_PAYLOAD68_ALL_ZERO  (1u << 6)

#define ELAN04_PRINT_ORIGIN_EXTERNAL_ADOPTED  1u

enum elan04_print_blob_offsets {
  ELAN04_BLOB_OFF_VERSION = 0,
  ELAN04_BLOB_OFF_ORIGIN = 1,
  ELAN04_BLOB_OFF_SUBFACTOR = 2,
  ELAN04_BLOB_OFF_FLAGS = 3,
  ELAN04_BLOB_OFF_SLOT = 4,
  ELAN04_BLOB_OFF_DEVICE_COUNT = 5,
  ELAN04_BLOB_OFF_RESERVED0 = 6,
  ELAN04_BLOB_OFF_ADOPTED_KEY = 8,
  ELAN04_BLOB_OFF_SECURE_ID_HASH = 40,
  ELAN04_BLOB_OFF_MAC_D_HASH = 72,
  ELAN04_BLOB_OFF_IDENTITY_HASH = 104,
  ELAN04_BLOB_OFF_IDENTITY_BLOB = 136,
  ELAN04_BLOB_OFF_RESERVED1 = 213,
};

typedef void (*Elan04CmdCb) (FpDevice *device,
                             guchar   *buffer_in,
                             gsize     length_in,
                             GError   *error);

typedef struct
{
  Elan04CmdCb callback;
} Elan04CommandData;

typedef struct _FpiDeviceElan04Moc
{
  FpDevice        parent;
  FpiSsm         *task_ssm;
  FpiSsm         *cmd_ssm;
  FpiUsbTransfer *cmd_transfer;
  gboolean        cmd_cancelable;
  guint8          cmd_read_ep;
  gsize           cmd_len_in;

  guint8          host_random[ELAN04_LEN_HOST_RANDOM];
  guint8          host_pub[ELAN04_LEN_HOST_PUB];
  EC_KEY         *host_key;
  guint8          device_id[32];
  guint8          mac_d[32];
  guint8          identity_payload[68];
  gboolean        have_device_id;
  gboolean        have_mac_d;
  gboolean        have_identity_payload;
  gboolean        payload68_all_zero;
  guint8          device_count_snapshot;
  gboolean        have_device_count_snapshot;
  guint8          match_slot;
  guint8          first_enroll_id[32];
  gboolean        have_first_enroll_id;
  gint            enroll_stage;
} FpiDeviceElan04Moc;

G_DEFINE_TYPE (FpiDeviceElan04Moc, fpi_device_elan04moc, FP_TYPE_DEVICE)

static const FpIdEntry id_table[] = {
  { .vid = 0x04f3, .pid = 0x0c4c, .driver_data = 0 },
  { .vid = 0, .pid = 0, .driver_data = 0 },
};

enum elan04_task_states {
  ELAN04_OPEN_SEND_READY = 0,
  ELAN04_OPEN_GET_DIM,
  ELAN04_OPEN_SEND_CONNECT,
  ELAN04_OPEN_GET_APPKEY,
  ELAN04_OPEN_NUM_STATES,
};

enum elan04_cmd_states {
  ELAN04_CMD_SEND = 0,
  ELAN04_CMD_GET,
  ELAN04_CMD_NUM_STATES,
};

enum elan04_enroll_states {
  ELAN04_ENROLL_SEND_VERIFY = 0,
  ELAN04_ENROLL_GET_IDENTIFY,
  ELAN04_ENROLL_GET_SUBSID,
  ELAN04_ENROLL_GET_COUNT,
  ELAN04_ENROLL_NUM_STATES,
};

static uint8_t *
elan04_compose_cmd (const guint8 *header,
                    gsize         header_len,
                    const guint8 *payload,
                    gsize         payload_len)
{
  guint8 *buf;
  buf = g_new0 (guint8, header_len + payload_len);
  memcpy (buf, header, header_len);
  if (payload && payload_len)
    memcpy (buf + header_len, payload, payload_len);
  return buf;
}

static gboolean
elan04_generate_host_material (FpiDeviceElan04Moc *self,
                               GError            **error)
{
  const EC_GROUP *group;
  const EC_POINT *pub;
  gsize out_len = 0;

  if (self->host_key)
    {
      EC_KEY_free (self->host_key);
      self->host_key = NULL;
    }

  if (RAND_bytes (self->host_random, ELAN04_LEN_HOST_RANDOM) != 1)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL,
                   "RAND_bytes failed generating host random");
      return FALSE;
    }

  self->host_key = EC_KEY_new_by_curve_name (NID_X9_62_prime256v1);
  if (!self->host_key)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL,
                   "EC_KEY_new_by_curve_name failed");
      return FALSE;
    }

  if (EC_KEY_generate_key (self->host_key) != 1)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL,
                   "EC_KEY_generate_key failed");
      return FALSE;
    }

  group = EC_KEY_get0_group (self->host_key);
  pub = EC_KEY_get0_public_key (self->host_key);
  if (!group || !pub)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL,
                   "EC key missing group/public key");
      return FALSE;
    }

  out_len = EC_POINT_point2oct (group,
                                pub,
                                POINT_CONVERSION_UNCOMPRESSED,
                                self->host_pub,
                                sizeof (self->host_pub),
                                NULL);
  if (out_len != ELAN04_LEN_HOST_PUB)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL,
                   "Unexpected host public key length");
      return FALSE;
    }

  return TRUE;
}

static void
elan04_cmd_ssm_data_free (Elan04CommandData *data)
{
  g_free (data);
}

static void
elan04_cmd_receive_cb (FpiUsbTransfer *transfer,
                       FpDevice       *device,
                       gpointer        userdata,
                       GError         *error)
{
  g_autofree guchar *buffer = NULL;
  Elan04CommandData *data = userdata;
  Elan04CmdCb callback;
  gssize actual_length;

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  if (!data)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
      return;
    }

  callback = data->callback;
  buffer = g_steal_pointer (&transfer->buffer);
  actual_length = transfer->actual_length;

  fpi_ssm_mark_completed (transfer->ssm);

  if (callback)
    callback (device, buffer, actual_length, NULL);
}

static void
elan04_cmd_run_state (FpiSsm   *ssm,
                      FpDevice *device)
{
  FpiDeviceElan04Moc *self = FPI_DEVICE_ELAN04MOC (device);
  FpiUsbTransfer *transfer;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ELAN04_CMD_SEND:
      if (self->cmd_transfer)
        {
          self->cmd_transfer->ssm = ssm;
          fpi_usb_transfer_submit (g_steal_pointer (&self->cmd_transfer),
                                   self->cmd_cancelable ? ELAN04_VERIFY_TIMEOUT : ELAN04_TIMEOUT_MS,
                                   NULL,
                                   fpi_ssm_usb_transfer_cb,
                                   NULL);
        }
      else
        {
          fpi_ssm_next_state (ssm);
        }
      break;

    case ELAN04_CMD_GET:
      if (self->cmd_len_in == 0)
        {
          Elan04CommandData *data = fpi_ssm_get_data (ssm);
          if (data && data->callback)
            data->callback (device, NULL, 0, NULL);
          fpi_ssm_mark_completed (ssm);
          return;
        }

      transfer = fpi_usb_transfer_new (device);
      transfer->ssm = ssm;
      transfer->short_is_error = FALSE;
      fpi_usb_transfer_fill_bulk (transfer, self->cmd_read_ep, self->cmd_len_in);
      fpi_usb_transfer_submit (transfer,
                               self->cmd_cancelable ? ELAN04_VERIFY_TIMEOUT : ELAN04_TIMEOUT_MS,
                               self->cmd_cancelable ? fpi_device_get_cancellable (device) : NULL,
                               elan04_cmd_receive_cb,
                               fpi_ssm_get_data (ssm));
      break;
    }
}

static void
elan04_cmd_ssm_done (FpiSsm   *ssm,
                     FpDevice *device,
                     GError   *error)
{
  FpiDeviceElan04Moc *self = FPI_DEVICE_ELAN04MOC (device);
  Elan04CommandData *data = fpi_ssm_get_data (ssm);

  self->cmd_ssm = NULL;

  if (error)
    {
      if (data && data->callback)
        data->callback (device, NULL, 0, error);
      else
        g_error_free (error);
    }
}

static void
elan04_get_cmd (FpDevice     *device,
                guint8       *buffer_out,
                gsize         length_out,
                guint8        read_ep,
                gsize         length_in,
                gboolean      cancelable,
                Elan04CmdCb   callback)
{
  FpiDeviceElan04Moc *self = FPI_DEVICE_ELAN04MOC (device);
  g_autoptr(FpiUsbTransfer) transfer = NULL;
  Elan04CommandData *data = g_new0 (Elan04CommandData, 1);

  transfer = fpi_usb_transfer_new (device);
  transfer->short_is_error = TRUE;
  fpi_usb_transfer_fill_bulk_full (transfer, ELAN04_EP_OUT, buffer_out, length_out, g_free);
  data->callback = callback;

  self->cmd_transfer = g_steal_pointer (&transfer);
  self->cmd_len_in = length_in;
  self->cmd_cancelable = cancelable;
  self->cmd_read_ep = read_ep;

  self->cmd_ssm = fpi_ssm_new (device, elan04_cmd_run_state, ELAN04_CMD_NUM_STATES);
  fpi_ssm_set_data (self->cmd_ssm, data, (GDestroyNotify) elan04_cmd_ssm_data_free);
  fpi_ssm_start (self->cmd_ssm, elan04_cmd_ssm_done);
}

static FpPrint *
elan04_create_print_from_data (FpDevice     *device,
                               const guint8 *data,
                               gsize         data_len)
{
  FpPrint *print;
  GVariant *var_data;
  GVariant *fpi_data;

  print = fp_print_new (device);
  var_data = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, data, data_len, 1);
  fpi_data = g_variant_new ("(@ay)", var_data);

  fpi_print_set_type (print, FPI_PRINT_RAW);
  fpi_print_set_device_stored (print, TRUE);
  g_object_set (print, "fpi-data", fpi_data, NULL);
  g_object_set (print, "description", "ELAN adopted template", NULL);

  return print;
}

static void
elan04_copy_print_metadata (FpPrint *dst,
                            FpPrint *src)
{
  const gchar *username;
  const gchar *description;
  const GDate *date;

  if (!dst || !src)
    return;

  fp_print_set_finger (dst, fp_print_get_finger (src));

  username = fp_print_get_username (src);
  if (username)
    fp_print_set_username (dst, username);

  description = fp_print_get_description (src);
  if (description)
    fp_print_set_description (dst, description);

  date = fp_print_get_enroll_date (src);
  if (date)
    fp_print_set_enroll_date (dst, date);
}

static const gchar *
elan04_storage_root (void)
{
  const gchar *state_directory = g_getenv ("STATE_DIRECTORY");

  if (state_directory && *state_directory)
    return state_directory;

  return "/var/lib/fprint";
}

static gchar *
elan04_storage_print_path (FpPrint *print)
{
  const gchar *username = fp_print_get_username (print);
  gchar finger_name[2];

  if (!username || !*username)
    return NULL;

  g_snprintf (finger_name, sizeof (finger_name), "%x", fp_print_get_finger (print));
  return g_build_filename (elan04_storage_root (),
                           username,
                           fp_print_get_driver (print),
                           fp_print_get_device_id (print),
                           finger_name,
                           NULL);
}

static gboolean
elan04_save_local_print (FpPrint  *print,
                         GError  **error)
{
  g_autofree gchar *path = NULL;
  g_autofree gchar *dirpath = NULL;
  g_autofree guchar *serialized = NULL;
  gsize serialized_len = 0;

  path = elan04_storage_print_path (print);
  if (!path)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID,
                   "Print metadata is missing username");
      return FALSE;
    }

  if (!fp_print_serialize (print, &serialized, &serialized_len, error))
    return FALSE;

  dirpath = g_path_get_dirname (path);
  if (g_mkdir_with_parents (dirpath, 0700) < 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "Failed to create storage directory '%s': %s",
                   dirpath, g_strerror (errno));
      return FALSE;
    }

  if (!g_file_set_contents (path, (const gchar *) serialized, serialized_len, error))
    return FALSE;

  return TRUE;
}

static gboolean
elan04_delete_local_print (FpPrint  *print,
                           GError  **error)
{
  g_autofree gchar *path = NULL;
  const gchar *username;

  username = fp_print_get_username (print);
  if (!username || !*username)
    {
      g_debug ("shared-storage delete received print without username; treating as no-op");
      return TRUE;
    }

  path = elan04_storage_print_path (print);
  if (!path)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID,
                   "Print metadata is missing username");
      return FALSE;
    }

  if (!g_file_test (path, G_FILE_TEST_EXISTS))
    return TRUE;

  if (g_unlink (path) < 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "Failed to delete local adopted print '%s': %s",
                   path, g_strerror (errno));
      return FALSE;
    }

  return TRUE;
}

static GPtrArray *
elan04_load_local_prints (FpDevice *device,
                          GError  **error)
{
  g_autoptr(GPtrArray) prints = NULL;
  g_autofree gchar *root_path = NULL;
  GDir *users_dir = NULL;
  const gchar *username = NULL;
  g_autoptr(GError) local_error = NULL;

  prints = g_ptr_array_new_with_free_func (g_object_unref);
  root_path = g_strdup (elan04_storage_root ());

  users_dir = g_dir_open (root_path, 0, &local_error);
  if (!users_dir)
    {
      if (g_error_matches (local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        return g_steal_pointer (&prints);

      g_propagate_error (error, g_steal_pointer (&local_error));
      return NULL;
    }

  while ((username = g_dir_read_name (users_dir)) != NULL)
    {
      g_autofree gchar *device_dir = NULL;
      GDir *finger_dir = NULL;
      const gchar *finger_name = NULL;

      device_dir = g_build_filename (root_path,
                                     username,
                                     fp_device_get_driver (device),
                                     fp_device_get_device_id (device),
                                     NULL);
      finger_dir = g_dir_open (device_dir, 0, NULL);
      if (!finger_dir)
        continue;

      while ((finger_name = g_dir_read_name (finger_dir)) != NULL)
        {
          g_autofree gchar *path = NULL;
          g_autofree gchar *contents = NULL;
          g_autoptr(FpPrint) print = NULL;
          gsize contents_len = 0;

          path = g_build_filename (device_dir, finger_name, NULL);
          if (!g_file_get_contents (path, &contents, &contents_len, NULL))
            continue;

          print = fp_print_deserialize ((const guchar *) contents, contents_len, NULL);
          if (!print)
            continue;

          if (!fp_print_compatible (print, device))
            continue;

          g_ptr_array_add (prints, g_steal_pointer (&print));
        }

      g_dir_close (finger_dir);
    }

  g_dir_close (users_dir);
  return g_steal_pointer (&prints);
}

static gboolean
elan04_sha256_bytes (const guint8 *data,
                     gsize         data_len,
                     guint8        out[32])
{
  g_autoptr(GChecksum) checksum = NULL;
  gsize digest_len = 32;

  checksum = g_checksum_new (G_CHECKSUM_SHA256);
  if (!checksum)
    return FALSE;

  g_checksum_update (checksum, data, data_len);
  g_checksum_get_digest (checksum, out, &digest_len);

  return digest_len == 32;
}

static gboolean
elan04_build_adopted_key (const guint8 secure_id_hash[32],
                          const guint8 *identity_hash,
                          guint8        out[32])
{
  static const guint8 domain[] = "elan-04f3-0c4c-adopt-v1";
  g_autoptr(GChecksum) checksum = NULL;
  gsize digest_len = 32;

  checksum = g_checksum_new (G_CHECKSUM_SHA256);
  if (!checksum)
    return FALSE;

  g_checksum_update (checksum, domain, sizeof (domain) - 1);
  g_checksum_update (checksum, secure_id_hash, 32);
  if (identity_hash)
    g_checksum_update (checksum, identity_hash, 32);

  g_checksum_get_digest (checksum, out, &digest_len);
  return digest_len == 32;
}

static gboolean
elan04_get_blob_bytes (FpPrint         *print,
                       const guint8   **data,
                       gsize           *data_len)
{
  g_autoptr(GVariant) fpi_data = NULL;
  GVariant *bytes = NULL;

  if (!print || !data || !data_len)
    return FALSE;

  g_object_get (print, "fpi-data", &fpi_data, NULL);
  if (!fpi_data || !g_variant_is_of_type (fpi_data, G_VARIANT_TYPE_TUPLE))
    return FALSE;

  g_variant_get (fpi_data, "(@ay)", &bytes);
  if (!bytes)
    return FALSE;

  *data = g_variant_get_fixed_array (bytes, data_len, 1);
  g_variant_unref (bytes);

  return *data != NULL;
}

static gboolean
elan04_print_matches_scan (FpPrint            *stored_print,
                           FpPrint            *scan_print)
{
  const guint8 *stored_data = NULL;
  const guint8 *scan_data = NULL;
  gsize stored_len = 0;
  gsize scan_len = 0;
  guint8 stored_flags;
  guint8 scan_flags;

  if (!elan04_get_blob_bytes (stored_print, &stored_data, &stored_len) ||
      !elan04_get_blob_bytes (scan_print, &scan_data, &scan_len))
    return fp_print_equal (stored_print, scan_print);

  if (stored_len < ELAN04_PRINT_BLOB_V2_SIZE ||
      scan_len < ELAN04_PRINT_BLOB_V2_SIZE)
    return fp_print_equal (stored_print, scan_print);

  if (stored_data[ELAN04_BLOB_OFF_VERSION] != ELAN04_PRINT_BLOB_V2_VERSION ||
      scan_data[ELAN04_BLOB_OFF_VERSION] != ELAN04_PRINT_BLOB_V2_VERSION)
    return fp_print_equal (stored_print, scan_print);

  stored_flags = stored_data[ELAN04_BLOB_OFF_FLAGS];
  scan_flags = scan_data[ELAN04_BLOB_OFF_FLAGS];

  if ((stored_flags & ELAN04_PRINT_FLAG_HAS_ADOPTED_KEY) &&
      (scan_flags & ELAN04_PRINT_FLAG_HAS_ADOPTED_KEY))
    {
      return memcmp (stored_data + ELAN04_BLOB_OFF_ADOPTED_KEY,
                     scan_data + ELAN04_BLOB_OFF_ADOPTED_KEY,
                     32) == 0;
    }

  if ((stored_flags & ELAN04_PRINT_FLAG_HAS_SECURE_ID_HASH) &&
      (scan_flags & ELAN04_PRINT_FLAG_HAS_SECURE_ID_HASH))
    {
      return memcmp (stored_data + ELAN04_BLOB_OFF_SECURE_ID_HASH,
                     scan_data + ELAN04_BLOB_OFF_SECURE_ID_HASH,
                     32) == 0;
    }

  return fp_print_equal (stored_print, scan_print);
}

static FpPrint *
elan04_create_print_from_match (FpDevice             *device,
                                FpiDeviceElan04Moc   *self)
{
  guint8 blob[ELAN04_PRINT_BLOB_V2_SIZE];
  guint8 secure_id_hash[32];
  guint8 identity_hash[32];
  const guint8 *identity_hash_ptr = NULL;

  memset (blob, 0, sizeof (blob));

  blob[ELAN04_BLOB_OFF_VERSION] = ELAN04_PRINT_BLOB_V2_VERSION;
  blob[ELAN04_BLOB_OFF_ORIGIN] = ELAN04_PRINT_ORIGIN_EXTERNAL_ADOPTED;
  blob[ELAN04_BLOB_OFF_SUBFACTOR] = 0;
  blob[ELAN04_BLOB_OFF_SLOT] = 0xff;
  blob[ELAN04_BLOB_OFF_DEVICE_COUNT] = 0xff;
  blob[ELAN04_BLOB_OFF_FLAGS] = ELAN04_PRINT_FLAG_HAS_SECURE_ID_HASH |
                                ELAN04_PRINT_FLAG_HAS_ADOPTED_KEY;

  if (!elan04_sha256_bytes (self->device_id, sizeof (self->device_id), secure_id_hash))
    return NULL;

  memcpy (blob + ELAN04_BLOB_OFF_SECURE_ID_HASH, secure_id_hash, sizeof (secure_id_hash));

  if (self->match_slot <= 9)
    {
      blob[ELAN04_BLOB_OFF_FLAGS] |= ELAN04_PRINT_FLAG_HAS_SLOT;
      blob[ELAN04_BLOB_OFF_SLOT] = self->match_slot;
    }

  if (self->have_device_count_snapshot)
    blob[ELAN04_BLOB_OFF_DEVICE_COUNT] = self->device_count_snapshot;

  if (self->have_mac_d &&
      elan04_sha256_bytes (self->mac_d, sizeof (self->mac_d), blob + ELAN04_BLOB_OFF_MAC_D_HASH))
    {
      blob[ELAN04_BLOB_OFF_FLAGS] |= ELAN04_PRINT_FLAG_HAS_MAC_D_HASH;
    }

  if (self->have_identity_payload)
    {
      if (self->payload68_all_zero)
        {
          blob[ELAN04_BLOB_OFF_FLAGS] |= ELAN04_PRINT_FLAG_PAYLOAD68_ALL_ZERO;
        }
      else if (elan04_sha256_bytes (self->identity_payload, sizeof (self->identity_payload), identity_hash))
        {
          blob[ELAN04_BLOB_OFF_FLAGS] |= ELAN04_PRINT_FLAG_HAS_IDENTITY_HASH;
          memcpy (blob + ELAN04_BLOB_OFF_IDENTITY_HASH, identity_hash, sizeof (identity_hash));
          identity_hash_ptr = identity_hash;
        }
    }

  if (!elan04_build_adopted_key (secure_id_hash, identity_hash_ptr, blob + ELAN04_BLOB_OFF_ADOPTED_KEY))
    return NULL;

  return elan04_create_print_from_data (device, blob, sizeof (blob));
}

static void
elan04_reset_scan_state (FpiDeviceElan04Moc *self)
{
  memset (self->device_id, 0, sizeof (self->device_id));
  memset (self->mac_d, 0, sizeof (self->mac_d));
  memset (self->identity_payload, 0, sizeof (self->identity_payload));
  self->have_device_id = FALSE;
  self->have_mac_d = FALSE;
  self->have_identity_payload = FALSE;
  self->payload68_all_zero = FALSE;
  self->device_count_snapshot = 0xff;
  self->have_device_count_snapshot = FALSE;
  self->match_slot = 0xff;
}

static void
elan04_reset_enroll_state (FpiDeviceElan04Moc *self)
{
  memset (self->first_enroll_id, 0, sizeof (self->first_enroll_id));
  self->have_first_enroll_id = FALSE;
  self->enroll_stage = 0;
}

static void
elan04_task_ssm_done (FpiSsm   *ssm,
                      FpDevice *device,
                      GError   *error)
{
  FpiDeviceElan04Moc *self = FPI_DEVICE_ELAN04MOC (device);
  self->task_ssm = NULL;

  if (error)
    {
      fpi_device_action_error (device, error);
      return;
    }
}

static void
elan04_finalize_scan (FpDevice *device)
{
  FpiDeviceElan04Moc *self = FPI_DEVICE_ELAN04MOC (device);
  g_autoptr(FpPrint) new_scan = NULL;

  if (!self->have_device_id)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Missing adopted id32"));
      return;
    }

  new_scan = elan04_create_print_from_match (device, self);
  if (!new_scan)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                     "Failed to build adopted print blob"));
      return;
    }

  if (fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_ENROLL)
    {
      FpPrint *template_print = NULL;

      fpi_device_get_enroll_data (device, &template_print);
      elan04_copy_print_metadata (new_scan, template_print);

      if (!self->have_first_enroll_id)
        {
          memcpy (self->first_enroll_id, self->device_id, sizeof (self->first_enroll_id));
          self->have_first_enroll_id = TRUE;
          self->enroll_stage = 1;
          fpi_device_enroll_progress (device, 1, g_object_ref (new_scan), NULL);
          elan04_reset_scan_state (self);
          fpi_ssm_jump_to_state (self->task_ssm, ELAN04_ENROLL_SEND_VERIFY);
          return;
        }

      if (memcmp (self->first_enroll_id, self->device_id, sizeof (self->first_enroll_id)) != 0)
        {
          elan04_reset_scan_state (self);
          elan04_reset_enroll_state (self);
          fpi_device_enroll_progress (device, 0, NULL,
                                      fpi_device_retry_new_msg (FP_DEVICE_RETRY_GENERAL,
                                                                "Adopted template changed between scans"));
          fpi_ssm_jump_to_state (self->task_ssm, ELAN04_ENROLL_SEND_VERIFY);
          return;
        }

      self->enroll_stage = 2;
      fpi_device_enroll_progress (device, 2, g_object_ref (new_scan), NULL);
      {
        g_autoptr(GError) save_error = NULL;

        if (!elan04_save_local_print (new_scan, &save_error))
          {
            fpi_ssm_mark_failed (self->task_ssm, g_steal_pointer (&save_error));
            return;
          }
      }
      fpi_device_enroll_complete (device, g_object_ref (new_scan), NULL);
      fpi_ssm_mark_completed (self->task_ssm);
      return;
    }

  if (fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_IDENTIFY)
    {
      GPtrArray *prints;
      guint i;

      fpi_device_get_identify_data (device, &prints);
      for (i = 0; prints && i < prints->len; ++i)
        {
          if (elan04_print_matches_scan (g_ptr_array_index (prints, i), new_scan))
            {
              fpi_device_identify_report (device, g_ptr_array_index (prints, i), g_object_ref (new_scan), NULL);
              fpi_device_identify_complete (device, NULL);
              fpi_ssm_mark_completed (self->task_ssm);
              return;
            }
        }

      fpi_device_identify_report (device, NULL, g_object_ref (new_scan), NULL);
      fpi_device_identify_complete (device, NULL);
      fpi_ssm_mark_completed (self->task_ssm);
      return;
    }

  {
    FpPrint *verify_print;

    fpi_device_get_verify_data (device, &verify_print);
    if (verify_print && elan04_print_matches_scan (verify_print, new_scan))
      fpi_device_verify_report (device, FPI_MATCH_SUCCESS, g_object_ref (new_scan), NULL);
    else
      fpi_device_verify_report (device, FPI_MATCH_FAIL, g_object_ref (new_scan), NULL);
    fpi_device_verify_complete (device, NULL);
    fpi_ssm_mark_completed (self->task_ssm);
  }
}

static void
elan04_ready_cb (FpDevice *device,
                 guchar   *buffer_in,
                 gsize     length_in,
                 GError   *error)
{
  FpiDeviceElan04Moc *self = FPI_DEVICE_ELAN04MOC (device);

  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  if (length_in < 2 || buffer_in[0] != 0x40 || buffer_in[1] != 0x03)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Unexpected bridge-ready response"));
      return;
    }
  fpi_ssm_next_state (self->task_ssm);
}

static void
elan04_dim_cb (FpDevice *device,
               guchar   *buffer_in,
               gsize     length_in,
               GError   *error)
{
  FpiDeviceElan04Moc *self = FPI_DEVICE_ELAN04MOC (device);
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }
  if (length_in < 4)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Unexpected sensor-trace response"));
      return;
    }
  fpi_ssm_next_state (self->task_ssm);
}

static void
elan04_connect_cb (FpDevice *device,
                   guchar   *buffer_in,
                   gsize     length_in,
                   GError   *error)
{
  FpiDeviceElan04Moc *self = FPI_DEVICE_ELAN04MOC (device);
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }
  if (length_in < ELAN04_LEN_CONNECT_RSP || buffer_in[0] != 0x40 || buffer_in[1] != 0x00)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Unexpected secure-connect response"));
      return;
    }
  fpi_ssm_next_state (self->task_ssm);
}

static void
elan04_appkey_cb (FpDevice *device,
                  guchar   *buffer_in,
                  gsize     length_in,
                  GError   *error)
{
  FpiDeviceElan04Moc *self = FPI_DEVICE_ELAN04MOC (device);
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }
  if (length_in < ELAN04_LEN_APPKEY_RSP || buffer_in[0] != 0x40 || buffer_in[1] != 0x00)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Unexpected app-key response"));
      return;
    }
  if (fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_OPEN)
    {
      fpi_device_open_complete (device, NULL);
      fpi_ssm_mark_completed (self->task_ssm);
    }
  else
    {
      fpi_ssm_next_state (self->task_ssm);
    }
}

static void
elan04_verify_cb (FpDevice *device,
                  guchar   *buffer_in,
                  gsize     length_in,
                  GError   *error)
{
  FpiDeviceElan04Moc *self = FPI_DEVICE_ELAN04MOC (device);

  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  if (length_in < 2 || buffer_in[0] != 0x40)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Unexpected verify response"));
      return;
    }

  if (buffer_in[1] <= 9)
    {
      self->match_slot = buffer_in[1];
      fpi_ssm_next_state (self->task_ssm);
      return;
    }

  if (buffer_in[1] == 0xfd)
    {
      if (fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_ENROLL)
        {
          elan04_reset_scan_state (self);
          fpi_device_enroll_progress (device, self->enroll_stage, NULL,
                                      fpi_device_retry_new_msg (FP_DEVICE_RETRY_GENERAL,
                                                                "No existing device template matched"));
          fpi_ssm_jump_to_state (self->task_ssm, ELAN04_ENROLL_SEND_VERIFY);
        }
      else if (fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_IDENTIFY)
        {
          fpi_device_identify_report (device, NULL, NULL, NULL);
          fpi_device_identify_complete (device, NULL);
          fpi_ssm_mark_completed (self->task_ssm);
        }
      else
        {
          fpi_device_verify_report (device, FPI_MATCH_FAIL, NULL, NULL);
          fpi_device_verify_complete (device, NULL);
          fpi_ssm_mark_completed (self->task_ssm);
        }
      return;
    }

  if (fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_ENROLL)
    {
      GError *retry = NULL;

      switch (buffer_in[1])
        {
        case 0x41:
        case 0x42:
        case 0x43:
        case 0x44:
          retry = fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER);
          break;
        default:
          retry = fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL);
          break;
        }

      elan04_reset_scan_state (self);
      fpi_device_enroll_progress (device, self->enroll_stage, NULL, retry);
      fpi_ssm_jump_to_state (self->task_ssm, ELAN04_ENROLL_SEND_VERIFY);
      return;
    }

  if (fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_IDENTIFY)
    {
      GError *retry = NULL;
      switch (buffer_in[1])
        {
        case 0x41:
        case 0x42:
        case 0x43:
        case 0x44:
          retry = fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER);
          break;
        default:
          retry = fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL);
          break;
        }
      fpi_device_identify_report (device, NULL, NULL, retry);
      fpi_device_identify_complete (device, NULL);
    }
  else
    {
      GError *retry = NULL;
      switch (buffer_in[1])
        {
        case 0x41:
        case 0x42:
        case 0x43:
        case 0x44:
          retry = fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER);
          break;
        default:
          retry = fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL);
          break;
        }
      fpi_device_verify_report (device, FPI_MATCH_ERROR, NULL, retry);
      fpi_device_verify_complete (device, NULL);
    }
  fpi_ssm_mark_completed (self->task_ssm);
}

static void
elan04_identify_cb (FpDevice *device,
                    guchar   *buffer_in,
                    gsize     length_in,
                    GError   *error)
{
  FpiDeviceElan04Moc *self = FPI_DEVICE_ELAN04MOC (device);

  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  g_debug ("40 FF 0C resp len=%" G_GSIZE_FORMAT " b0=0x%02x b1=0x%02x",
           length_in,
           length_in > 0 ? buffer_in[0] : 0xff,
           length_in > 1 ? buffer_in[1] : 0xff);

  if (length_in < ELAN04_LEN_IDENTIFY_RSP || buffer_in[0] != 0x40 || buffer_in[1] != 0x00)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Unexpected identify response"));
      return;
    }

  memcpy (self->device_id, buffer_in + 2, 32);
  memcpy (self->mac_d, buffer_in + 2 + 32, 32);
  self->have_device_id = TRUE;
  self->have_mac_d = TRUE;
  fpi_ssm_next_state (self->task_ssm);
}

static void
elan04_subsid_cb (FpDevice *device,
                  guchar   *buffer_in,
                  gsize     length_in,
                  GError   *error)
{
  FpiDeviceElan04Moc *self = FPI_DEVICE_ELAN04MOC (device);
  guint i;

  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  if (length_in < ELAN04_LEN_SUBSID_RSP || buffer_in[0] != 0x40 || buffer_in[1] != 0x00)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Unexpected subsid response"));
      return;
    }

  memcpy (self->identity_payload, buffer_in + 2, sizeof (self->identity_payload));
  self->have_identity_payload = TRUE;
  self->payload68_all_zero = TRUE;

  for (i = 0; i < sizeof (self->identity_payload); ++i)
    {
      if (self->identity_payload[i] != 0x00)
        {
          self->payload68_all_zero = FALSE;
          break;
        }
    }

  g_debug ("40 FF 12 slot=%u payload68=%s",
           self->match_slot,
           self->payload68_all_zero ? "all-zero" : "nonzero");

  fpi_ssm_next_state (self->task_ssm);
}

static void
elan04_count_cb (FpDevice *device,
                 guchar   *buffer_in,
                 gsize     length_in,
                 GError   *error)
{
  FpiDeviceElan04Moc *self = FPI_DEVICE_ELAN04MOC (device);

  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  if (length_in < ELAN04_LEN_COUNT_RSP || buffer_in[0] != 0x40)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Unexpected device-count response"));
      return;
    }

  self->device_count_snapshot = buffer_in[1];
  self->have_device_count_snapshot = TRUE;
  g_debug ("40 FF 04 count=%u", self->device_count_snapshot);

  elan04_finalize_scan (device);
}

static void
elan04_run_state (FpiSsm   *ssm,
                  FpDevice *device)
{
  FpiDeviceElan04Moc *self = FPI_DEVICE_ELAN04MOC (device);
  guint8 *cmd_buf = NULL;
  static const guint8 ready_cmd[] = { 0x40, 0xff, 0x00 };
  static const guint8 dim_cmd[] = { 0x00, 0x0c };
  static const guint8 appkey_cmd[] = { 0x40, 0xff, 0x0d };

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ELAN04_OPEN_SEND_READY:
      cmd_buf = elan04_compose_cmd (ready_cmd, sizeof (ready_cmd), NULL, 0);
      elan04_get_cmd (device, cmd_buf, sizeof (ready_cmd), ELAN04_EP_IN, 2, FALSE, elan04_ready_cb);
      break;

    case ELAN04_OPEN_GET_DIM:
      cmd_buf = elan04_compose_cmd (dim_cmd, sizeof (dim_cmd), NULL, 0);
      elan04_get_cmd (device, cmd_buf, sizeof (dim_cmd), ELAN04_EP_IN, 4, FALSE, elan04_dim_cb);
      break;

    case ELAN04_OPEN_SEND_CONNECT:
      {
        guint8 payload[ELAN04_LEN_HOST_RANDOM + ELAN04_LEN_HOST_PUB];
        memset (payload, 0, sizeof (payload));
        memcpy (payload, self->host_random, ELAN04_LEN_HOST_RANDOM);
        memcpy (payload + ELAN04_LEN_HOST_RANDOM, self->host_pub, ELAN04_LEN_HOST_PUB);
        cmd_buf = elan04_compose_cmd ((const guint8[]){0x40, 0xff, 0x06}, 3, payload, sizeof (payload));
        elan04_get_cmd (device, cmd_buf, 3 + sizeof (payload), ELAN04_EP_IN, ELAN04_LEN_CONNECT_RSP, FALSE, elan04_connect_cb);
      }
      break;

    case ELAN04_OPEN_GET_APPKEY:
      cmd_buf = elan04_compose_cmd (appkey_cmd, sizeof (appkey_cmd), NULL, 0);
      elan04_get_cmd (device, cmd_buf, sizeof (appkey_cmd), ELAN04_EP_IN, ELAN04_LEN_APPKEY_RSP, FALSE, elan04_appkey_cb);
      break;

    default:
      g_assert_not_reached ();
    }
}

enum elan04_verify_states {
  ELAN04_VERIFY_SEND_VERIFY = 0,
  ELAN04_VERIFY_GET_IDENTIFY,
  ELAN04_VERIFY_GET_SUBSID,
  ELAN04_VERIFY_GET_COUNT,
  ELAN04_VERIFY_NUM_STATES,
};

static void
elan04_verify_run_state (FpiSsm   *ssm,
                         FpDevice *device)
{
  FpiDeviceElan04Moc *self = FPI_DEVICE_ELAN04MOC (device);
  guint8 *cmd_buf = NULL;
  static const guint8 verify_cmd[] = { 0x40, 0xff, 0x03 };

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ELAN04_VERIFY_SEND_VERIFY:
      cmd_buf = elan04_compose_cmd (verify_cmd, sizeof (verify_cmd), NULL, 0);
      elan04_get_cmd (device, cmd_buf, sizeof (verify_cmd), ELAN04_EP_VERIFY_IN, ELAN04_LEN_VERIFY_RSP, TRUE, elan04_verify_cb);
      break;

    case ELAN04_VERIFY_GET_IDENTIFY:
      {
        guint8 challenge[32] = { 0 };
        cmd_buf = elan04_compose_cmd ((const guint8[]){0x40, 0xff, 0x0c}, 3, challenge, sizeof (challenge));
        elan04_get_cmd (device, cmd_buf, 3 + sizeof (challenge), ELAN04_EP_IN, ELAN04_LEN_IDENTIFY_RSP, TRUE, elan04_identify_cb);
      }
      break;

    case ELAN04_VERIFY_GET_SUBSID:
      {
        guint8 slot_payload[1];

        slot_payload[0] = self->match_slot;
        cmd_buf = elan04_compose_cmd ((const guint8[]){0x40, 0xff, 0x12}, 3, slot_payload, sizeof (slot_payload));
        elan04_get_cmd (device, cmd_buf, 3 + sizeof (slot_payload), ELAN04_EP_IN, ELAN04_LEN_SUBSID_RSP, TRUE, elan04_subsid_cb);
      }
      break;

    case ELAN04_VERIFY_GET_COUNT:
      cmd_buf = elan04_compose_cmd ((const guint8[]){0x40, 0xff, 0x04}, 3, NULL, 0);
      elan04_get_cmd (device, cmd_buf, 3, ELAN04_EP_IN, ELAN04_LEN_COUNT_RSP, TRUE, elan04_count_cb);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
elan04_enroll_run_state (FpiSsm   *ssm,
                         FpDevice *device)
{
  elan04_verify_run_state (ssm, device);
}

static void
elan04_probe (FpDevice *device)
{
  fpi_device_probe_complete (device, NULL, NULL, NULL);
}

static void
elan04_open (FpDevice *device)
{
  FpiDeviceElan04Moc *self = FPI_DEVICE_ELAN04MOC (device);
  GError *error = NULL;
  GUsbDevice *usb_dev = fpi_device_get_usb_device (device);

  if (!g_usb_device_claim_interface (usb_dev, 0, 0, &error))
    {
      fpi_device_open_complete (device, error);
      return;
    }

  if (!elan04_generate_host_material (self, &error))
    {
      fpi_device_open_complete (device, error);
      return;
    }

  self->task_ssm = fpi_ssm_new (device, elan04_run_state, ELAN04_OPEN_NUM_STATES);
  fpi_ssm_start (self->task_ssm, elan04_task_ssm_done);
}

static void
elan04_close (FpDevice *device)
{
  FpiDeviceElan04Moc *self = FPI_DEVICE_ELAN04MOC (device);
  GError *error = NULL;

  if (self->host_key)
    {
      EC_KEY_free (self->host_key);
      self->host_key = NULL;
    }

  g_usb_device_release_interface (fpi_device_get_usb_device (device), 0, 0, &error);
  fpi_device_close_complete (device, error);
}

static void
elan04_verify_identify (FpDevice *device)
{
  FpiDeviceElan04Moc *self = FPI_DEVICE_ELAN04MOC (device);
  elan04_reset_scan_state (self);
  self->task_ssm = fpi_ssm_new (device, elan04_verify_run_state, ELAN04_VERIFY_NUM_STATES);
  fpi_ssm_start (self->task_ssm, elan04_task_ssm_done);
}

static void
elan04_enroll_as_adopt (FpDevice *device)
{
  FpiDeviceElan04Moc *self = FPI_DEVICE_ELAN04MOC (device);
  elan04_reset_scan_state (self);
  elan04_reset_enroll_state (self);
  self->task_ssm = fpi_ssm_new (device, elan04_enroll_run_state, ELAN04_ENROLL_NUM_STATES);
  fpi_ssm_start (self->task_ssm, elan04_task_ssm_done);
}

static void
elan04_delete_print (FpDevice *device)
{
  FpPrint *print = NULL;
  g_autoptr(GError) error = NULL;

  fpi_device_get_delete_data (device, &print);

  if (!elan04_delete_local_print (print, &error))
    {
      fpi_device_delete_complete (device, g_steal_pointer (&error));
      return;
    }

  fpi_device_delete_complete (device, NULL);
}

static void
elan04_list (FpDevice *device)
{
  g_autoptr(GError) error = NULL;
  GPtrArray *prints;

  prints = elan04_load_local_prints (device, &error);
  if (!prints)
    {
      fpi_device_list_complete (device, NULL, g_steal_pointer (&error));
      return;
    }

  fpi_device_list_complete (device, prints, NULL);
}

static void
fpi_device_elan04moc_init (FpiDeviceElan04Moc *self)
{
  G_DEBUG_HERE ();
}

static void
fpi_device_elan04moc_class_init (FpiDeviceElan04MocClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = ELAN04_DRIVER_FULLNAME;
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->id_table = id_table;
  dev_class->nr_enroll_stages = 2;
  dev_class->temp_hot_seconds = -1;

  dev_class->probe = elan04_probe;
  dev_class->open = elan04_open;
  dev_class->close = elan04_close;
  dev_class->verify = elan04_verify_identify;
  dev_class->identify = elan04_verify_identify;
  dev_class->enroll = elan04_enroll_as_adopt;
  dev_class->delete = elan04_delete_print;
  dev_class->list = elan04_list;

  fpi_device_class_auto_initialize_features (dev_class);
}
