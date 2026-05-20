

<div align="center">

# LibFPrint

*LibFPrint is part of the **[FPrint][Website]** project.*

<br/>

[![Button Website]][Website]
[![Button Documentation]][Documentation]

[![Button Supported]][Supported]
[![Button Unsupported]][Unsupported]

[![Button Contribute]][Contribute]
[![Button Contributors]][Contributors]

[![Button Chinese]][ChineseReadme]

</div>

## History

**LibFPrint** was originally developed as part of an
academic project at the **[University Of Manchester]**.

It aimed to hide the differences between consumer
fingerprint scanners and provide a single uniform
API to application developers.

## Goal

The ultimate goal of the **FPrint** project is to make
fingerprint scanners widely and easily usable under
common Linux environments.

## Branch Notes: ELAN 04f3:0c4c Shared-Storage Driver

This branch contains an experimental driver for the ELAN
`04f3:0c4c` Match-on-Chip sensor used in some dual-boot laptops.

The implementation here is intentionally different from a normal
"Linux owns the device storage" flow:

- Windows and Linux are expected to share the same device-side storage.
- Linux uses **adopt/link** semantics by default.
- `fprintd-enroll` is implemented as "verify an existing on-device template
  and link it to the current Linux user".
- Linux does **not** create new device-side templates by default.
- Linux does **not** delete device-side templates by default.

In practice, the safe usage model is:

1. Enroll the finger in Windows first.
2. Boot Linux.
3. Enroll once in Linux to adopt/link the existing template.
4. Use verify/unlock normally from Linux afterwards.

### What Is Implemented Here

- `open/close`
- `verify`
- `identify`
- `enroll-as-adopt` for existing Windows-created templates
- local adopted-link persistence under `/var/lib/fprint/...`
- local `list/delete` semantics for adopted links

### What Is Not Enabled By Default

- device-side Linux-owned enroll
- device-side template delete
- device-wide storage erase/reset

Those operations are unsafe for the shared-storage dual-boot target.

## Build

The branch was validated with a build-tree workflow like:

```bash
meson setup /root/libfprint-build --wipe \
  -Dudev_rules=disabled \
  -Dudev_hwdb=disabled \
  -Dintrospection=false
meson compile -C /root/libfprint-build
```

Useful smoke tests from the build tree:

```bash
export LD_LIBRARY_PATH=/root/libfprint-build/libfprint
/root/libfprint-build/examples/device-info
/root/libfprint-build/examples/open-close
/root/libfprint-build/tests/test-elan04moc-list-fixed
/root/libfprint-build/tests/test-elan04moc-verify-fixed
/root/libfprint-build/tests/test-elan04moc-enroll-fixed
```

## Replacing Distro Components

If you want to try this on a normal distribution install, replace the
runtime pieces as a matched set:

- `libfprint-2.so.2.0.0`
- `fprintd`
- `fprintd-enroll`
- `fprintd-verify`
- `fprintd-list`
- `fprintd-delete`

A practical sequence is:

1. Build `libfprint` from this branch.
2. Build `fprintd` against that build-tree `libfprint`.
3. Back up the distro binaries and libraries.
4. Install the custom builds over the distro versions.
5. Run `ldconfig`.
6. Restart `fprintd`.

Example replacement flow:

```bash
install -m 0755 /root/libfprint-build/libfprint/libfprint-2.so.2.0.0 \
  /usr/lib/x86_64-linux-gnu/libfprint-2.so.2.0.0
ln -sf /usr/lib/x86_64-linux-gnu/libfprint-2.so.2.0.0 \
  /usr/lib/x86_64-linux-gnu/libfprint-2.so.2

install -m 0755 /root/fprintd-build/src/fprintd /usr/libexec/fprintd
install -m 0755 /root/fprintd-build/utils/fprintd-enroll /usr/bin/fprintd-enroll
install -m 0755 /root/fprintd-build/utils/fprintd-verify /usr/bin/fprintd-verify
install -m 0755 /root/fprintd-build/utils/fprintd-list   /usr/bin/fprintd-list
install -m 0755 /root/fprintd-build/utils/fprintd-delete /usr/bin/fprintd-delete

ldconfig
systemctl daemon-reload
systemctl restart fprintd
```

Back up your original distro files before doing this.

## PAM / Desktop Configuration

If command-line `fprintd-verify` works but the desktop does not show a
fingerprint option, check PAM first.

On the test system, enabling PAM support was required:

```bash
pam-auth-update --enable fprintd
```

After that, `common-auth` contained a line like:

```pam
auth    [success=3 default=ignore]    pam_fprintd.so max-tries=3 timeout=10
```

Then restart or re-login to refresh the desktop session. For GNOME/GDM,
restarting GDM or logging out/in was needed before the fingerprint option
appeared in Settings.

## Expected Shared-Storage Semantics

- deleting a fingerprint in Linux removes the **local link**
- the Windows-owned on-device template remains
- re-enrolling in Linux after deletion re-adopts the same device template

This is by design.

## Known Pitfalls

### 1. `40 FF 12` may succeed with all-zero payload

On the validated hardware, `40 FF 12` returned success but the
`payload68` body was all zeroes. The driver therefore:

- uses stable `id32` material for the main adopted key path
- stores `payload68` state only as a diagnostic signal
- does not require non-zero `40 FF 12` payload to make verify/enroll work

### 2. `mac_d` is not a stable template key

`mac_d` changes with the identify challenge and must not be used as the
main equality key for matching. This branch stores `mac_d` only as a
diagnostic hash.

### 3. Do not compare the full raw blob for authentication

The on-disk print blob contains both stable fields and diagnostic fields.
Using a full raw-byte equality check causes false `no-match` results.

This branch matches using stable fields only:

- first `adopted_key`
- then `secure_id_hash`

and intentionally ignores:

- `mac_d_observed_hash`
- `device_count_snapshot`
- `slot`

for authentication equivalence.

### 4. Duplicate enroll after delete can fail if shared-storage delete is treated as device delete

For this sensor, delete must behave like local unlink for adopted prints.
Trying to garbage-collect an on-device "duplicate" by treating it as a
device-owned Linux template causes the next enroll to fail.

This branch handles delete as a no-op on device storage for adopted prints.

### 5. A failed fingerprint attempt may look like "the reader stopped working"

If your PAM stack is configured with `pam_fprintd.so max-tries=1`, one real
`verify-no-match` can end the whole PAM transaction and push the UI back to
password auth immediately.

If you want retry behavior at the lock screen, increase it, for example:

```pam
auth    [success=3 default=ignore]    pam_fprintd.so max-tries=3 timeout=10
```

### 6. `fprintd` may warn about `/usr/local/etc/fprintd.conf`

When using a custom build/install prefix, `fprintd` may log that it cannot
open `/usr/local/etc/fprintd.conf`. On the validated setup this warning was
harmless and did not block enumeration, enroll, list, delete, or verify.

### 7. Root-owned adopted prints can interfere with user enroll

If you previously adopted a template as `root`, delete that local link
before adopting the same finger for a normal user. Otherwise duplicate
detection may trip on the old local record.

## License

`Section 6` of the license states that for compiled works that use
this library, such works must include **LibFPrint** copyright notices
alongside the copyright notices for the other parts of the work.

**LibFPrint** includes code from **NIST's** **[NBIS]** software distribution.

We include **Bozorth3** from the **[US Export Controlled]**
distribution, which we have determined to be fine
being shipped in an open source project.

## Get in *touch*

 - [IRC] - `#fprint` @ `irc.oftc.net`
 - [Matrix] - `#fprint:matrix.org` bridged to the IRC channel
 - [MailingList] - low traffic, not much used these days

<br/>

<div align="right">

[![Badge License]][License]

</div>


<!----------------------------------------------------------------------------->

[Documentation]: https://fprint.freedesktop.org/libfprint-dev/
[Contributors]: https://gitlab.freedesktop.org/libfprint/libfprint/-/graphs/master
[Unsupported]: https://gitlab.freedesktop.org/libfprint/wiki/-/wikis/Unsupported-Devices
[Supported]: https://fprint.freedesktop.org/supported-devices.html
[Website]: https://fprint.freedesktop.org/
[MailingList]: https://lists.freedesktop.org/mailman/listinfo/fprint
[IRC]: ircs://irc.oftc.net:6697/#fprint
[Matrix]: https://matrix.to/#/#fprint:matrix.org

[Contribute]: ./HACKING.md
[ChineseReadme]: ./README.zh-CN.md
[License]: ./COPYING

[University Of Manchester]: https://www.manchester.ac.uk/
[US Export Controlled]: https://fprint.freedesktop.org/us-export-control.html
[NBIS]: http://fingerprint.nist.gov/NBIS/index.html


<!---------------------------------[ Badges ]---------------------------------->

[Badge License]: https://img.shields.io/badge/License-LGPL2.1-015d93.svg?style=for-the-badge&labelColor=blue


<!---------------------------------[ Buttons ]--------------------------------->

[Button Documentation]: https://img.shields.io/badge/Documentation-04ACE6?style=for-the-badge&logoColor=white&logo=BookStack
[Button Chinese]: https://img.shields.io/badge/%E4%B8%AD%E6%96%87_Readme-0A84FF?style=for-the-badge&logoColor=white&logo=ReadMe
[Button Contributors]: https://img.shields.io/badge/Contributors-FF4F8B?style=for-the-badge&logoColor=white&logo=ActiGraph
[Button Unsupported]: https://img.shields.io/badge/Unsupported_Devices-EF2D5E?style=for-the-badge&logoColor=white&logo=AdBlock
[Button Contribute]: https://img.shields.io/badge/Contribute-66459B?style=for-the-badge&logoColor=white&logo=Git
[Button Supported]: https://img.shields.io/badge/Supported_Devices-428813?style=for-the-badge&logoColor=white&logo=AdGuard
[Button Website]: https://img.shields.io/badge/Homepage-3B80AE?style=for-the-badge&logoColor=white&logo=freedesktopDotOrg
