# ward

Host intrusion detection for one laptop, built from small parts that already exist on Void: YARA, inotify, nftables, xbps and runit. It covers the parts of Wazuh that matter on a single machine without a manager server, and uses Suricata only while the laptop is on mains power.

| Part | What it does | When it runs | Battery cost |
| --- | --- | --- | --- |
| `ward watch` (service `ward`) | YARA-scans every file written to `/tmp`, `/var/tmp`, `/dev/shm`, the download folders, `~/.local/bin` and `/usr/local/bin`; alerts at once when `/etc/ld.so.preload`, `passwd`/`shadow`/`doas.conf`, `authorized_keys` or a runit service directory changes | on each file event (inotify) | none while nothing is written |
| `ward check` | rootcheck: `ld.so.preload`, processes running from temp dirs, memfd or deleted files, `LD_PRELOAD` in root processes, kernel modules not on disk or hidden from `/proc/modules`, taint changes, extra uid-0 or passwordless accounts, promiscuous interfaces, new listening sockets, blocklist hits | hourly | a few `/proc` reads |
| `ward fim` | hash, mode and owner baseline of `/etc`, `/boot`, `/usr/local`, runit services, SSH keys, shell start-up files, `~/.local/bin`; new setuid/setgid files anywhere. A change is reported once, then becomes the baseline. Files a package owns and `xbps-pkgdb` still vouches for are not reported | daily, mains only | |
| `ward sweep` | YARA over places no package owns (about a minute) | daily, mains only | |
| `ward pkgs` | `xbps-pkgdb -a`: every package file against its recorded hash. This replaces YARA over `/usr`, which took 12 minutes and found only false positives | weekly, mains only | |
| `ward net` | Spamhaus DROP (v4/v6) and abuse.ch Feodo botnet C2 addresses as nftables interval sets in table `inet ward`: outbound packets are rejected, inbound dropped. Private and link-local ranges are filtered out before loading; a truncated download keeps the old sets | daily, mains only; loaded at boot from the last copy | zero (kernel set lookup) |
| `ward-ids` | Suricata with ET Open rules on the default-route interface; priority 1–2 alerts become notifications | started and stopped by acpid on mains changes; never on battery | none on battery |

Alerts go to `/var/log/ward/alerts.log` and, once a day per message, to the desktop through the running `dunst`'s D-Bus session. A high-severity YARA hit inside `DROP_ZONES` is moved to `/var/lib/ward/quarantine` with mode 000; `ward q` lists it and `ward restore ID` puts it back. Outside those zones ward only alerts.

## Why not the full tools

- **Wazuh** needs a manager (server plus indexer) to report to; the agent alone is the heavy half without the dashboard. The modules a laptop needs are syscheck, rootcheck and active response, and these are what `ward fim`, `ward check` and the quarantine do.
- **CrowdSec** is not packaged for Void and works by parsing logs of exposed services. This machine drops all unsolicited inbound traffic and runs no syslog, so the agent would have nothing to read. The useful part for a laptop is the IP reputation list; `ward net` gets that from Spamhaus and abuse.ch without an account or a daemon.
- **Suricata** inspects every packet. Left running, it keeps the CPU out of its deep idle states and costs battery all day, while most traffic it sees is TLS and encrypted DNS. On mains power that cost does not matter, so it runs only then.

## Install

```sh
doas sh ward/install.sh      # packages, rules, services, first baselines
ward status
```

Edit `/etc/ward/ward.conf` (watched folders, quarantine zones, exclusions) and `doas sv restart ward`. After editing rules in `/etc/ward/rules`, run `doas ward rules`. `doas ward selftest` proves that the compiled rules load and match. `uninstall.sh` removes the services, program and nft table and keeps config, baselines and quarantine.

## Rules

`rules/*.yar`: miners (XMRig binaries, pool configs, dropper scripts), LKM and LD_PRELOAD rootkits (known families and a generic shape: a shared library that defines `readdir`/`readdir64`, imports `dlsym` and mentions `/proc` and `ld.so.preload`), PAM backdoors, reverse shells, Tsunami/Mirai/Gafgyt bots, Sliver/Merlin/BPFDoor implants, memfd fileless loaders, download-and-persist droppers, anti-forensics, SSH key injection, browser credential stealers that post to Discord/Telegram, and UPX-packed ELFs (low). Every script rule needs two independent signals.

Tested on 28 November 2026 against `/usr`, `/opt`, `/etc` and the home directory: the remaining matches were penetration-testing tools (Metasploit modules), zapret's UPX-packed binaries and a crackme, all excluded or low severity by `EXCLUDE_RE`.

## Limits

