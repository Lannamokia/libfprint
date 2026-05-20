# AGENTS.md

This file is for coding agents working on the `feature/elan04moc-shared-storage`
branch.

## Scope

This branch contains an experimental driver for the ELAN `04f3:0c4c`
Match-on-Chip sensor in a **Windows/Linux shared-storage** configuration.

The main rule is:

- **Do not treat this device like normal Linux-owned storage.**

Default branch policy is:

- Linux adopts existing device templates
- Linux does not create device-side templates by default
- Linux does not delete device-side templates by default
- Linux does not erase/reset device storage

If you change that policy, you are changing the safety model.

## Repository Roles

This repository (`libfprint`) contains:

- the `elan04moc` driver implementation
- build-tree examples
- build-tree tests for verify/enroll/list/delete
- user-facing docs in `README.md` and `README.zh-CN.md`

This branch was validated together with a separately built `fprintd`.

## Build: libfprint

Validated build flow:

```bash
meson setup /root/libfprint-build --wipe \
  -Dudev_rules=disabled \
  -Dudev_hwdb=disabled \
  -Dintrospection=false
meson compile -C /root/libfprint-build
```

Build-tree runtime:

```bash
export LD_LIBRARY_PATH=/root/libfprint-build/libfprint
```

Useful local smoke tests:

```bash
/root/libfprint-build/examples/device-info
/root/libfprint-build/examples/open-close
/root/libfprint-build/tests/test-elan04moc-list-fixed
/root/libfprint-build/tests/test-elan04moc-delete-fixed
/root/libfprint-build/tests/test-elan04moc-verify-fixed
/root/libfprint-build/tests/test-elan04moc-enroll-fixed
```

## Build: fprintd

`fprintd` must be built against the custom `libfprint` build tree.

Validated flow:

```bash
cd /path/to/fprintd
PKG_CONFIG_PATH=/root/libfprint-build/meson-uninstalled \
  meson setup /root/fprintd-build --wipe \
    -Dpam=false \
    -Dsystemd=false \
    -Dman=false \
    -Dgtk_doc=false \
    -Dtests=false

meson compile -C /root/fprintd-build
```

Important:

- `libfprint-2-uninstalled.pc` under `/root/libfprint-build/meson-uninstalled`
  is the intended pkg-config entry point.

## Replacing System Components

Validated deployment requires replacing a matched runtime set:

- `/usr/lib/x86_64-linux-gnu/libfprint-2.so.2.0.0`
- `/usr/libexec/fprintd`
- `/usr/bin/fprintd-enroll`
- `/usr/bin/fprintd-verify`
- `/usr/bin/fprintd-list`
- `/usr/bin/fprintd-delete`

Suggested sequence:

```bash
BACKUP=/root/fprintd-system-backup-$(date +%Y%m%d-%H%M%S)
mkdir -p "$BACKUP"/usr/lib/x86_64-linux-gnu "$BACKUP"/usr/libexec "$BACKUP"/usr/bin

cp -a /usr/lib/x86_64-linux-gnu/libfprint-2.so.2.0.0 "$BACKUP"/usr/lib/x86_64-linux-gnu/
cp -a /usr/libexec/fprintd "$BACKUP"/usr/libexec/
cp -a /usr/bin/fprintd-enroll /usr/bin/fprintd-verify /usr/bin/fprintd-list /usr/bin/fprintd-delete "$BACKUP"/usr/bin/

install -m 0755 /root/libfprint-build/libfprint/libfprint-2.so.2.0.0 /usr/lib/x86_64-linux-gnu/libfprint-2.so.2.0.0
ln -sf /usr/lib/x86_64-linux-gnu/libfprint-2.so.2.0.0 /usr/lib/x86_64-linux-gnu/libfprint-2.so.2

install -m 0755 /root/fprintd-build/src/fprintd /usr/libexec/fprintd
install -m 0755 /root/fprintd-build/utils/fprintd-enroll /usr/bin/fprintd-enroll
install -m 0755 /root/fprintd-build/utils/fprintd-verify /usr/bin/fprintd-verify
install -m 0755 /root/fprintd-build/utils/fprintd-list /usr/bin/fprintd-list
install -m 0755 /root/fprintd-build/utils/fprintd-delete /usr/bin/fprintd-delete

ldconfig
systemctl daemon-reload
systemctl restart fprintd
```

## PAM / Desktop Enablement

If command-line tools work but the desktop does not show fingerprint options,
check PAM first.

Enable PAM support:

```bash
pam-auth-update --enable fprintd
```

Validated `common-auth` entry:

```pam
auth    [success=3 default=ignore]    pam_fprintd.so max-tries=3 timeout=10 # debug
```

Notes:

- `max-tries=1` makes lock-screen behavior look broken after one real
  `verify-no-match`
- after changing PAM, restart GDM or re-login the desktop session

## Storage / Matching Model

The driver stores adopted links locally in:

```text
/var/lib/fprint/<username>/<driver>/<device-id>/<finger-hex>
```

Do not assume:

- `list` enumerates physical device templates
- `delete` removes physical device templates

Current semantics:

- `list` enumerates local adopted links
- `delete` removes local adopted links only

### Print Matching

Do not use raw blob byte equality as the final authentication rule.

Stable comparison order:

1. `adopted_key`
2. fallback `secure_id_hash`

Do not treat these as final equality keys:

- `mac_d_observed_hash`
- `device_count_snapshot`
- `slot`

Those are diagnostic fields.

## What Was Validated

On the reference test system, the following have been validated:

- build-tree `verify`
- build-tree `enroll-as-adopt`
- build-tree local `list/delete`
- build-tree `fprintd-list`
- build-tree `fprintd-verify`
- system-replaced `fprintd-list`
- system-replaced `fprintd-verify`
- desktop login / unlock
- desktop delete / re-enroll

## Known Pitfalls

### 1. `40 FF 12` success with all-zero payload

Observed on the validated hardware. Do not require non-zero `payload68`
to make the driver usable.

### 2. `mac_d` is challenge-dependent

Never promote `mac_d` to the main authentication identity key.

### 3. Delete during duplicate cleanup may carry no username

`fprintd` duplicate cleanup can hand the driver a print without user metadata.
For this branch, that delete path must behave like a shared-storage no-op,
not a hard error.

### 4. Root-enrolled adopted link can block normal-user enroll

If a template was previously adopted by `root`, delete the root-side local
link before enrolling/adopting the same finger for a normal user.

## Debugging Checklist

When something regresses, check these in order:

1. `device-info` still sees `elan04moc`
2. `fprintd-list <user>` still lists the expected local adopted link
3. `fprintd-verify -f <finger> <user>` still returns `verify-match`
4. `journalctl -u fprintd.service -n 200 --no-pager`
5. `/var/log/auth.log`
6. PAM config in `/etc/pam.d/common-auth`

For lock-screen issues, also check whether the problem is:

- service/device state
- PAM retry policy
- GDM UI fallback behavior

## Rollback

If a deployment is bad, restore the saved backup set and restart `fprintd`.

Typical rollback:

```bash
cp -a "$BACKUP"/usr/lib/x86_64-linux-gnu/libfprint-2.so.2.0.0 /usr/lib/x86_64-linux-gnu/
cp -a "$BACKUP"/usr/libexec/fprintd /usr/libexec/
cp -a "$BACKUP"/usr/bin/fprintd-enroll "$BACKUP"/usr/bin/fprintd-verify "$BACKUP"/usr/bin/fprintd-list "$BACKUP"/usr/bin/fprintd-delete /usr/bin/
ldconfig
systemctl restart fprintd
```

## Change Discipline

Before making risky changes:

- keep the shared-storage safety model intact
- prefer additive diagnostics over semantic broadening
- test with build-tree tools first
- then test with `fprintd`
- then test in the real desktop/PAM path
