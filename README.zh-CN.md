<div align="center">

# LibFPrint

*LibFPrint 是 **[FPrint][Website]** 项目的一部分。*

<br/>

[![Button Website]][Website]
[![Button Documentation]][Documentation]

[![Button Supported]][Supported]
[![Button Unsupported]][Unsupported]

[![Button Contribute]][Contribute]
[![Button Contributors]][Contributors]

[![Button English]][EnglishReadme]

</div>

## 分支说明：ELAN 04f3:0c4c Shared-Storage 驱动

这个分支包含了针对 ELAN `04f3:0c4c` Match-on-Chip 指纹传感器的实验性驱动。

它的设计目标不是传统的“Linux 独占设备存储”，而是面向 **Windows / Linux 双系统共用同一份模组安全存储** 的场景：

- Windows 和 Linux 共享模组里的模板存储
- Linux 默认使用 **adopt/link** 语义
- `fprintd-enroll` 的实际含义是：
  - 验证一个已经存在于设备里的模板
  - 再把它链接给当前 Linux 用户
- Linux 默认**不创建新的设备侧模板**
- Linux 默认**不删除设备侧模板**

安全的使用方式是：

1. 先在 Windows 里录入指纹
2. 再进入 Linux
3. 在 Linux 里执行一次 enroll，把这枚已有模板 adopt 到当前用户
4. 后续正常使用 verify / unlock

## 当前已实现

- `open/close`
- `verify`
- `identify`
- 面向 Windows 已有模板的 `enroll-as-adopt`
- adopted-link 本地持久化
- 本地 `list/delete`

## 默认不启用的能力

- Linux 自己往模组里新建模板
- 删除设备侧模板
- 清空整个设备存储

这些操作对于双系统 shared-storage 目标是不安全的，所以默认关闭。

## 编译

本分支验证通过的 build-tree 编译方式如下：

```bash
meson setup /root/libfprint-build --wipe \
  -Dudev_rules=disabled \
  -Dudev_hwdb=disabled \
  -Dintrospection=false
meson compile -C /root/libfprint-build
```

常用自测命令：

```bash
export LD_LIBRARY_PATH=/root/libfprint-build/libfprint
/root/libfprint-build/examples/device-info
/root/libfprint-build/examples/open-close
/root/libfprint-build/tests/test-elan04moc-list-fixed
/root/libfprint-build/tests/test-elan04moc-verify-fixed
/root/libfprint-build/tests/test-elan04moc-enroll-fixed
```

## 替换发行版组件

如果你想直接在发行版系统里试用这套驱动，建议把运行时组件作为一组替换：

- `libfprint-2.so.2.0.0`
- `fprintd`
- `fprintd-enroll`
- `fprintd-verify`
- `fprintd-list`
- `fprintd-delete`

推荐步骤：

1. 编译这个分支的 `libfprint`
2. 用这份 `libfprint` 再编译 `fprintd`
3. 先备份发行版原有文件
4. 用自定义产物覆盖系统文件
5. 执行 `ldconfig`
6. 重启 `fprintd`

示例：

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

务必先备份系统原件。

## PAM / 桌面配置

如果命令行 `fprintd-verify` 已经能工作，但桌面设置里看不到指纹选项，优先检查 PAM。

测试机上需要显式启用：

```bash
pam-auth-update --enable fprintd
```

启用后，`/etc/pam.d/common-auth` 中应出现类似：

```pam
auth    [success=3 default=ignore]    pam_fprintd.so max-tries=3 timeout=10
```

然后重新登录桌面会话，或者重启 GDM / 显示管理器，让桌面环境重新读取认证栈。

## Shared-Storage 语义

- 在 Linux 里删除指纹，删除的是**本地 link**
- Windows 录入的设备模板仍然保留在模组里
- Linux 删除后重新 enroll，本质上是重新 adopt 同一枚设备模板

这是设计行为，不是 bug。

## 当前 print blob v2 行为

驱动现在把本地 `FpPrint` 落成固定长度的 blob v2。

当前已写入的字段包括：

- `version = 2`
- `origin = external_adopted`
- `slot`
  - 已知时写真实 `0..9`
  - 未知时写 `0xff`
- `device_count_snapshot`
  - 在 `40 FF 12` 后追加一次 `40 FF 04`
  - 成功时写真实 count
  - 未知时写 `0xff`
- `flags`
  - `has_secure_id_hash`
  - `has_mac_d_observed_hash`
  - `has_adopted_key`
  - `payload68_all_zero`
  - `has_identity_hash`
- `secure_id_hash = SHA256(id32)`
- `mac_d_observed_hash = SHA256(mac_d32)`
- `adopted_key = SHA256("elan-04f3-0c4c-adopt-v1" || secure_id_hash || optional identity_hash)`

当前还没有写入 `identity_blob[69]` 原文。

## 匹配语义

不要把整个 raw blob 当成“必须逐字节相等”的认证条件。

这个分支已经把认证匹配收敛为稳定字段比较：

1. 如果双方都有 `adopted_key`，优先比较 `adopted_key`
2. 否则如果双方都有 `secure_id_hash`，退化为比较 `secure_id_hash`

以下字段**不参与认证等价判断**：

- `mac_d_observed_hash`
- `device_count_snapshot`
- `slot`

这样可以保留诊断信息，同时避免把 challenge-dependent 或会话相关信息误当成模板主键。

## 本地持久化 / list / delete

当前驱动维护的是 `shared-storage adopted-link` 本地视图，不是设备真实模板枚举。

落盘位置与 fprintd 默认文件布局兼容：

```text
/var/lib/fprint/<username>/<driver>/<device-id>/<finger-hex>
```

语义如下：

- `enroll-as-adopt` 成功后，驱动直接把 adopted print 序列化写入上面这个路径
- `list` 返回这层本地 adopted-link
- `delete` 只删除本地 adopted-link 文件，不会向设备发送模板删除命令

## 已踩过的坑

### 1. `40 FF 12` 成功但 payload68 全零

在当前验证硬件上，`40 FF 12` 会返回 success，但 `payload68` 可能稳定为全零。

所以这个分支：

- 仍然记录 `payload68` 状态
- 但不会把“非零 payload68”当成 adopt/verify 的硬前提

### 2. `mac_d` 不是稳定模板键

`mac_d` 会跟 challenge 变化，不能拿来做最终匹配键。
这里只把它存成诊断 hash。

### 3. 不能用 raw blob 全量相等做 verify

如果把诊断字段也纳入 `fp_print_equal()` 的全量比较，真实匹配会被误判成 `no-match`。

### 4. adopted delete 必须是本地 unlink

对这个 shared-storage 设备来说，adopted print 的 delete 只能删本地 link，不能把它当成 Linux 自己的设备模板去删。

### 5. `pam_fprintd max-tries=1` 会让锁屏体验很差

如果 `pam_fprintd.so max-tries=1`，一次真实 `no-match` 就会结束整轮 PAM 认证，看起来像“第一次错了后面再按就没用了”。

建议至少设成：

```pam
auth    [success=3 default=ignore]    pam_fprintd.so max-tries=3 timeout=10
```

### 6. `fprintd` 提示 `/usr/local/etc/fprintd.conf` 不存在

在自定义前缀或手工替换环境下，`fprintd` 可能会提示找不到 `/usr/local/etc/fprintd.conf`。

在我们的验证里，这条 warning 不影响：

- 设备枚举
- enroll
- list
- delete
- verify

### 7. root 用户残留的 adopted print 会影响普通用户录入

如果你之前用 `root` 用户做过 adopt，再让普通用户录入同一枚手指时，可能会被 duplicate 检测撞到。

先删掉 `root` 的本地 link，再给普通用户录入。

### 8. 桌面列表为空，但重新录入提示重复

这通常不是设备模板没删，而是 `fprintd` 的 duplicate cleanup 路径拿到了一枚没有 `username` 元数据的 print。

本分支已经修正：

- 对这种 shared-storage duplicate cleanup delete
- 驱动按 no-op 成功处理
- 不再因为 `Print metadata is missing username` 中断重新录入

## 当前验证结果

当前分支已经在真实设备上验证通过：

- `libfprint` build-tree `verify`
- `libfprint` build-tree `enroll-as-adopt`
- 本地 adopted-link `list/delete`
- `fprintd-list`
- `fprintd-verify`
- 系统替换后的桌面登录 / 解锁 / 删除 / 重新录入

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
[EnglishReadme]: ./README.md

[University Of Manchester]: https://www.manchester.ac.uk/
[US Export Controlled]: https://fprint.freedesktop.org/us-export-control.html
[NBIS]: http://fingerprint.nist.gov/NBIS/index.html

<!---------------------------------[ Buttons ]--------------------------------->

[Button Documentation]: https://img.shields.io/badge/Documentation-04ACE6?style=for-the-badge&logoColor=white&logo=BookStack
[Button Contributors]: https://img.shields.io/badge/Contributors-FF4F8B?style=for-the-badge&logoColor=white&logo=ActiGraph
[Button Unsupported]: https://img.shields.io/badge/Unsupported_Devices-EF2D5E?style=for-the-badge&logoColor=white&logo=AdBlock
[Button Contribute]: https://img.shields.io/badge/Contribute-66459B?style=for-the-badge&logoColor=white&logo=Git
[Button Supported]: https://img.shields.io/badge/Supported_Devices-428813?style=for-the-badge&logoColor=white&logo=AdGuard
[Button Website]: https://img.shields.io/badge/Homepage-3B80AE?style=for-the-badge&logoColor=white&logo=freedesktopDotOrg
[Button English]: https://img.shields.io/badge/English_Readme-0A84FF?style=for-the-badge&logoColor=white&logo=ReadMe
