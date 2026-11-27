#!/bin/sh
# Remove ward. Quarantined files stay in /var/lib/ward/quarantine until you delete them.
set -u
[ "$(id -u)" = 0 ] || { echo "run as root: doas sh $0"; exit 1; }
for s in ward ward-tick ward-ids; do sv down "$s" 2>/dev/null; rm -f "/var/service/$s"; done
sleep 2
nft delete table inet ward 2>/dev/null
rm -rf /etc/sv/ward /etc/sv/ward-tick /etc/sv/ward-ids /etc/acpi/events/ward-ac /usr/local/sbin/ward
sv hup acpid 2>/dev/null
echo "removed. Left: /etc/ward (config, rules), /var/lib/ward (baselines, quarantine), /var/log/ward"
