/*
 * Droppers, persistence and anti-forensics in scripts.
 * Every rule needs two independent signals so ordinary install scripts stay quiet.
 */

rule dropper_pipe_to_shell_persist
{
    meta:
        description = "Downloads and runs code, then installs persistence"
        severity = "high"
    strings:
        $d1 = /(curl|wget)[^\n|]{0,200}\| *(ba|z|da)?sh/
        $d2 = /(curl|wget)[^\n]{0,200}-O-? *[^\n]{0,80}&& *chmod \+x/
        $d3 = /base64 -d[^\n|]{0,40}\| *(ba)?sh/
        $p1 = "crontab -" ascii
        $p2 = "/etc/cron" ascii
        $p3 = ".ssh/authorized_keys" ascii
        $p4 = "/etc/ld.so.preload" ascii
        $p5 = "/etc/rc.local" ascii
        $p6 = "/etc/sv/" ascii
        $p7 = "/var/service/" ascii
        $p8 = ".config/autostart" ascii
        $p9 = ".xinitrc" ascii
        $pa = "systemctl enable" ascii
        $pb = "/etc/profile.d/" ascii
    condition:
        filesize < 2MB and uint32(0) != 0x464c457f and 1 of ($d*) and 1 of ($p*)
}

rule antiforensics_script
{
    meta:
        description = "Script that erases traces and locks files (TeamTNT/Kinsing style)"
        severity = "high"
    strings:
        $a1 = "unset HISTFILE" ascii
        $a2 = "history -c" ascii
        $a3 = "export HISTFILE=/dev/null" ascii
        $a4 = /rm -rf? \/var\/log\/(wtmp|btmp|lastlog|auth)/
        $a5 = "echo > /var/log/" ascii
        $a6 = "chattr +i" ascii
        $a7 = "chattr -i" ascii
        $a8 = /iptables -[AI] (INPUT|OUTPUT)[^\n]{0,60}-j (DROP|ACCEPT)/
        $a9 = "ufw disable" ascii
        $aa = "setenforce 0" ascii
        $ab = "/proc/sys/kernel/nmi_watchdog" ascii
        $ac = "vm.nr_hugepages" ascii
    condition:
        filesize < 2MB and uint32(0) != 0x464c457f and 3 of them
}

rule persistence_ssh_key_inject
{
    meta:
        description = "Script appends a key to authorized_keys and hides it"
        severity = "high"
    strings:
        $k  = /ssh-(rsa|ed25519) AAAA[0-9A-Za-z+\/]{40,}/
        $a  = />> *[^\n]{0,40}authorized_keys/
        $h1 = "chattr +i" ascii
        $h2 = "chmod 600" ascii
        $h3 = "touch -r" ascii
    condition:
        filesize < 1MB and uint32(0) != 0x464c457f and $k and $a and 1 of ($h*)
}

rule obfuscated_shell_payload
{
    meta:
        description = "Long base64/hex blob executed through eval or a shell"
        severity = "medium"
    strings:
        $e1 = /eval *\( *(base64_decode|gzinflate|str_rot13)\(/
        $e2 = /eval "\$\(echo [A-Za-z0-9+\/=]{200,} *\| *base64 -d/
        $e3 = /exec\((zlib\.decompress\(|marshal\.loads\()?(base64\.)?b64decode\(/
        $e4 = /echo [A-Za-z0-9+\/=]{400,} *\| *base64 -d *\| *(ba)?sh/
        $e5 = /python3? -c ['"]import base64[^\n]{0,40}exec\(/
    condition:
        filesize < 5MB and any of them
}

rule stealer_browser_exfil
{
    meta:
        description = "Reads browser credential stores and sends them to a chat webhook"
        severity = "high"
    strings:
        $b1 = "logins.json" ascii wide
        $b2 = "key4.db" ascii wide
        $b3 = "Login Data" ascii wide
        $b4 = "Local State" ascii wide
        $b5 = "cookies.sqlite" ascii wide
        $b6 = "Cookies" ascii wide fullword
        $b7 = "wallet.dat" ascii wide
        $b8 = "Local Storage/leveldb" ascii wide
        $x1 = "discord.com/api/webhooks" ascii wide
        $x2 = "discordapp.com/api/webhooks" ascii wide
        $x3 = "api.telegram.org/bot" ascii wide
        $x4 = "transfer.sh" ascii wide
        $x5 = "pastebin.com/api" ascii wide
        $x6 = "gofile.io" ascii wide
    condition:
        filesize < 60MB and 2 of ($b*) and 1 of ($x*)
}

rule packed_upx_elf
{
    meta:
        description = "UPX-packed ELF (common for dropped malware; legit tools are rarely packed)"
        severity = "low"
    strings:
        $u1 = "UPX!" ascii
        $u2 = "$Info: This file is packed with the UPX" ascii
    condition:
        uint32(0) == 0x464c457f and filesize < 30MB and ($u1 in (0..1024) or $u2)
}
