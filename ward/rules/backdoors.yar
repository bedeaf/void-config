/*
 * Reverse shells, bots and remote-access tools.
 */

rule shell_reverse_script
{
    meta:
        description = "Reverse shell one-liner in a script or text file"
        severity = "high"
    strings:
        $r1 = /bash -i *>& *\/dev\/tcp\/[0-9a-zA-Z.\-]{3,64}\/[0-9]{1,5}/
        $r2 = /\/dev\/tcp\/[0-9a-zA-Z.\-]{3,64}\/[0-9]{1,5} *0>&1/
        $r3 = /nc(at)? [^\n]{0,40}-e \/bin\/(ba)?sh/
        $r4 = /mkfifo \/tmp\/[^\n]{0,60}\| *\/bin\/(ba)?sh -i/
        $r5 = /socat [^\n]{0,60}exec:['"]?\/bin\/(ba)?sh/ nocase
        $r6 = /pty\.spawn\(['"]\/bin\/(ba)?sh/
        $r7 = /s\.connect\(\(['"][0-9.]{7,15}['"], *[0-9]{1,5}\)\)[^\n]{0,200}os\.dup2/
        $r8 = /perl -e ['"]use Socket;[^\n]{0,200}exec\(['"]\/bin\/(ba)?sh/
        $r9 = /php -r ['"]\$sock=fsockopen\(/
        $ra = /rm -f \/tmp\/f;mkfifo \/tmp\/f/
    condition:
        filesize < 5MB and uint32(0) != 0x464c457f and any of them
}

rule shell_reverse_elf
{
    meta:
        description = "Small ELF that connects out and execs a shell over the socket"
        severity = "high"
    strings:
        $sh1 = "/bin/sh" ascii
        $sh2 = "/bin/bash" ascii
        $f1  = "connect" ascii
        $f2  = "dup2" ascii
        $f3  = "execve" ascii
        $f4  = "socket" ascii
        $i1  = "HISTFILE" ascii
        $i2  = "PS1=" ascii
    condition:
        uint32(0) == 0x464c457f and filesize < 200KB and
        1 of ($sh*) and all of ($f*) and 1 of ($i*)
}

rule bot_irc_tsunami
{
    meta:
        description = "Tsunami/Kaiten-style IRC bot"
        severity = "high"
    strings:
        $i1 = "PRIVMSG %s :" ascii
        $i2 = "NICK %s" ascii
        $i3 = "JOIN %s" ascii
        $c1 = "KILLATTK" ascii
        $c2 = "GETSPOOFS" ascii
        $c3 = "TSUNAMI" ascii
        $c4 = "PAN " ascii
        $c5 = "UDPFLOOD" ascii nocase
        $c6 = "SYNFLOOD" ascii nocase
    condition:
        uint32(0) == 0x464c457f and filesize < 5MB and 2 of ($i*) and 2 of ($c*)
}

rule bot_mirai_gafgyt
{
    meta:
        description = "Mirai / Gafgyt (BASHLITE) family"
        severity = "high"
    strings:
        $m1 = "/bin/busybox MIRAI" ascii
        $m2 = "TSource Engine Query" ascii
        $m3 = "/dev/watchdog" ascii
        $m4 = "/dev/misc/watchdog" ascii
        $m5 = "POST /cdn-cgi/" ascii
        $m6 = "dvrHelper" ascii
        $g1 = "LOLNOGTFO" ascii
        $g2 = "HTTPFLOOD" ascii
        $g3 = "JUNK Flooding" ascii nocase
        $g4 = "HOLD Flooding" ascii nocase
        $g5 = "gayfgt" ascii nocase
        $g6 = "BUILD %s" ascii
    condition:
        uint32(0) == 0x464c457f and filesize < 5MB and (3 of ($m*) or 2 of ($g*))
}

rule rat_known_linux
{
    meta:
        description = "Known Linux RAT / implant strings (Sliver, Merlin, Pupy, Rekoobe, BPFDoor, ...)"
        severity = "high"
    strings:
        $s1 = "sliverpb" ascii
        $s2 = "github.com/bishopfox/sliver" ascii
        $s3 = "github.com/Ne0nd0g/merlin" ascii
        $s4 = "pupylib" ascii
        $s5 = "/var/run/haldrund.pid" ascii    // BPFDoor
        $s6 = "/dev/shm/kdmtmpflush" ascii     // BPFDoor
        $s7 = "justtryit" ascii               // BPFDoor magic password
        $s8 = "github.com/Ne0nd0g/merlin-agent" ascii
        $sa = "github.com/MythicAgents" ascii
        $sb = "chashell" ascii
    condition:
        uint32(0) == 0x464c457f and filesize < 60MB and any of them
}

rule fileless_memfd_exec_script
{
    meta:
        description = "Script that writes a payload into memfd and executes it (fileless)"
        severity = "high"
    strings:
        $m1 = "memfd_create" ascii
        $e1 = "/proc/self/fd/" ascii
        $e2 = "fexecve" ascii
        $e3 = "os.execv" ascii
        $e4 = "execveat" ascii
        $d1 = "urlopen" ascii
        $d2 = "requests.get" ascii
        $d3 = "b64decode" ascii
        $d4 = "zlib.decompress" ascii
        $d5 = "curl" ascii
    condition:
        filesize < 2MB and uint32(0) != 0x464c457f and
        $m1 and 1 of ($e*) and 1 of ($d*)
}
