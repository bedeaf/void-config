# Kernel security snapshot

Observed on 28 November 2026 with the Void 6.18 LTS kernel `6.18.63_1`. The machine moved from the end-of-life 7.2 series to 6.18 LTS; ZFSBootMenu follows the newest 6.18 kernel that has a ZFS module.

## Runtime values

Values marked *added* come from `etc/sysctl.d/90-hardening.conf`; the others are Void or kernel defaults.

| Setting | Value | Notes |
| --- | --- | --- |
| `kernel.randomize_va_space` | `2` | full userspace ASLR |
| `kernel.kptr_restrict` | `2` | *added*: kernel pointers hidden from root too (was `1`) |
| `kernel.dmesg_restrict` | `1` | |
| `kernel.yama.ptrace_scope` | `1` | `2` was tried and reverted: it also blocks `PTRACE_TRACEME`, so `gdb ./prog` needs root |
| `kernel.unprivileged_bpf_disabled` | `1` | |
| `kernel.io_uring_disabled` | `2` | *added*: io_uring off for everyone |
| `net.core.bpf_jit_harden` | `2` | *added* |
| `dev.tty.ldisc_autoload` | `0` | *added* |
| `fs.protected_hardlinks`, `fs.protected_symlinks` | `1` | |
| `fs.protected_fifos`, `fs.protected_regular` | `2` | *added* |
| `vm.mmap_rnd_bits` / `vm.mmap_rnd_compat_bits` | `32` / `16` | *added*: maximum mmap randomisation |

`etc/modprobe.d/90-hardening.conf` blocks (`install X /bin/false`) modules this laptop does not use but an unprivileged user could get auto-loaded, mostly through user namespaces: PPP/PPPoE/L2TP, GTP, `xfrm_user`/`af_key`, `encrypted_keys`, vhost, and rare protocols and filesystems. `etc/modprobe.d/hardening.conf` is the older list (DCCP, AF_ALG, FireWire, Thunderbolt, USB audio, ...). Remove a line to use that module again; IPsec VPNs need `xfrm_user`.

The CPU vulnerability report for Spectre v2 reads:

```text
Mitigation: Enhanced / Automatic IBRS; IBPB: conditional; PBRSB-eIBRS: SW sequence; BHI: SW loop, KVM: SW loop
```

Meltdown reports `Not affected` on this processor.

## Boot parameters

The ZFSBootMenu command line (`org.zfsbootmenu:commandline` on the boot environment) carries `slab_nomerge init_on_free=1 page_alloc.shuffle=1 debugfs=off`. The fast path (`usr/local/sbin/fbt`) boots the kernel from its own UEFI entry and uses the `CMDLINE` in that script instead, so those flags apply only when the same words are in `CMDLINE` and `fbt install` has re-created the entry. Check with `cat /proc/cmdline`.

## Kernel build options

`/boot/config-6.18.63_1` has these options set to `y`:

```text
CONFIG_RANDOMIZE_BASE
CONFIG_CPU_MITIGATIONS
CONFIG_SECCOMP
CONFIG_SECCOMP_FILTER
CONFIG_STACKPROTECTOR_STRONG
CONFIG_STRICT_KERNEL_RWX
CONFIG_STRICT_MODULE_RWX
CONFIG_SLAB_FREELIST_RANDOM
CONFIG_SLAB_FREELIST_HARDENED
CONFIG_INIT_ON_ALLOC_DEFAULT_ON
CONFIG_FORTIFY_SOURCE
CONFIG_HARDENED_USERCOPY
CONFIG_SECURITY_DMESG_RESTRICT
CONFIG_X86_UMIP
CONFIG_X86_KERNEL_IBT
```

Build-time support is distinct from runtime enforcement: seccomp, for example, requires a process to install a filter. These come from the packaged kernel; there is no custom kernel build.

## Recheck

```sh
uname -r
sysctl kernel.randomize_va_space kernel.kptr_restrict kernel.dmesg_restrict \
  kernel.yama.ptrace_scope kernel.unprivileged_bpf_disabled kernel.io_uring_disabled \
  dev.tty.ldisc_autoload fs.protected_hardlinks fs.protected_symlinks \
  fs.protected_fifos fs.protected_regular
doas sysctl net.core.bpf_jit_harden vm.mmap_rnd_bits vm.mmap_rnd_compat_bits
cat /proc/cmdline
cat /sys/devices/system/cpu/vulnerabilities/spectre_v2
```

This snapshot excludes host identifiers.
