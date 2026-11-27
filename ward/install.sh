#!/bin/sh
# Install ward. Run as root:  doas sh ward/install.sh
# Re-running updates the program, rules and services; an edited ward.conf is kept.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
[ "$(id -u)" = 0 ] || { echo "run as root: doas sh $0"; exit 1; }
step() { printf '\n==> %s\n' "$*"; }

step "packages"
xbps-install -Sy yara inotify-tools suricata

step "program, rules, config"
install -Dm755 "$HERE/ward" /usr/local/sbin/ward
install -d -m700 /etc/ward /etc/ward/rules /var/lib/ward /var/log/ward
install -m600 "$HERE"/rules/*.yar /etc/ward/rules/
if [ -f /etc/ward/ward.conf ] && ! cmp -s "$HERE/ward.conf" /etc/ward/ward.conf; then
    install -m600 "$HERE/ward.conf" /etc/ward/ward.conf.new
    echo "kept your /etc/ward/ward.conf; the new default is ward.conf.new"
else
    install -m600 "$HERE/ward.conf" /etc/ward/ward.conf
fi
ward rules
ward selftest

step "runit services and the mains-power hook"
for s in ward ward-tick ward-ids; do
    install -d "/etc/sv/$s"
    install -m755 "$HERE/sv/$s/run" "/etc/sv/$s/run"
done
install -m755 "$HERE/sv/ward-ids/finish" /etc/sv/ward-ids/finish
touch /etc/sv/ward-ids/down            # never auto-starts; `ward power` decides
install -Dm644 "$HERE/acpi/ward-ac" /etc/acpi/events/ward-ac
sv hup acpid 2>/dev/null || true

step "first baselines (no alerts on the first run)"
ward net || echo "blocklist download failed; ward-tick retries daily on mains"
ward check
ward fim
if grep -q '^IDS=1' /etc/ward/ward.conf; then ward ids-rules || echo "ET rules failed; retried weekly"; fi

step "start"
for s in ward ward-tick ward-ids; do ln -sfn "/etc/sv/$s" "/var/service/$s"; done
sleep 6
ward power
ward status
