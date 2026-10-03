#!/usr/bin/env bash

# Verify that macOS vmnet and virtio-snd can operate in the same rv32emu
# process. vmnet requires privilege escalation, while virtio-snd uses
# PortAudio/CoreAudio from the same host process.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "${SCRIPT_DIR}/common.sh"

check_platform

if [[ "${OS_TYPE}" != "Darwin" ]]; then
    print_warning "Skipping vmnet + virtio-snd test on non-macOS host"
    exit 0
fi

if ! sudo -n true 2> /dev/null; then
    print_error "vmnet + virtio-snd test requires passwordless sudo"
    exit 1
fi

register_cleanup cleanup_emulator
cleanup

RET=0

MESSAGES=(
    "${COLOR_G}OK!"
    "${COLOR_R}Fail to boot"
    "${COLOR_R}Fail to identify vmnet bridge"
    "${COLOR_R}Fail to bind virtio-net driver"
    "${COLOR_R}Fail to bind virtio-snd driver"
    "${COLOR_R}Fail to configure guest network"
    "${COLOR_R}Fail to ping vmnet gateway"
    "${COLOR_R}Fail to enumerate ALSA card"
    "${COLOR_R}Fail to enumerate ALSA PCM device"
    "${COLOR_R}Fail to play PCM with speaker-test"
)

# Remember the bridge interfaces that exist before rv32emu starts. The vmnet
# shared backend creates another bridge whose address becomes the guest gateway.
VMNET_BRIDGES_BEFORE="$(
    ifconfig -l \
        | tr ' ' '\n' \
        | grep -E '^bridge[0-9]+$' \
        | sort \
        | tr '\n' ' ' || true
)"
export VMNET_BRIDGES_BEFORE

RUN_LINUX="sudo -E build/rv32emu ${OPTS_BASE} -x vnet:vmnet -x vsnd"

export RUN_LINUX
export TIMEOUT

printf "${COLOR_Y}===== Test option: ${OPTS_BASE} -x vnet:vmnet -x vsnd =====${COLOR_N}\n"

run_vmnet_vsnd_case()
{
    expect <<'DONE'
	set timeout $env(TIMEOUT)
	set guest_ip ""
	set gateway_ip ""

	spawn sh -c $env(RUN_LINUX)

	expect {
	    "buildroot login:" {
	        # Find the bridge created by this vmnet invocation.
	        if { [catch {
	            set vmnet_info [exec sh -c {
	                before=" ${VMNET_BRIDGES_BEFORE:-} "

	                for attempt in 1 2 3 4 5; do
	                    for bridge in $(ifconfig -l); do
	                        case "$bridge" in
	                            bridge[0-9]*)
	                                case "$before" in
	                                    *" $bridge "*)
	                                        continue
	                                        ;;
	                                esac

	                                ip=$(ifconfig "$bridge" 2>/dev/null |
	                                    awk '/inet / {print $2; exit}')

	                                if [ -n "$ip" ]; then
	                                    echo "$bridge $ip"
	                                    exit 0
	                                fi
	                                ;;
	                        esac
	                    done

	                    sleep 1
	                done

	                exit 1
	            }]
	        } vmnet_error] } {
	            puts stderr "failed to identify vmnet bridge: $vmnet_error"
	            exit 2
	        }

	        set vmnet_bridge [lindex $vmnet_info 0]
	        set gateway_ip [lindex $vmnet_info 1]

	        set octets [split $gateway_ip "."]
	        set guest_host 10

	        if { [lindex $octets 3] == $guest_host } {
	            set guest_host 11
	        }

	        set guest_ip \
	            "[lindex $octets 0].[lindex $octets 1].[lindex $octets 2].$guest_host"

	        puts "vmnet bridge: $vmnet_bridge"
	        puts "vmnet gateway: $gateway_ip"
	        puts "vmnet guest IP: $guest_ip"

	        send "root\r"
	    }
	    timeout {
	        exit 1
	    }
	}

	expect {
	    "# " {}
	    timeout {
	        exit 1
	    }
	}

	# Do not assume fixed virtio device numbers when both devices are present.
	send {for dev in /sys/bus/virtio/devices/virtio*; do basename "$(readlink "$dev/driver")"; done | grep -q '^virtio_net$'; echo VNET_DRIVER_RC:$?}
	send "\r"

	expect {
	    "VNET_DRIVER_RC:0" {}
	    "VNET_DRIVER_RC:1" {
	        exit 3
	    }
	    timeout {
	        exit 3
	    }
	}

	expect "# "

	send {for dev in /sys/bus/virtio/devices/virtio*; do basename "$(readlink "$dev/driver")"; done | grep -q '^virtio_snd$'; echo VSND_DRIVER_RC:$?}
	send "\r"

	expect {
	    "VSND_DRIVER_RC:0" {}
	    "VSND_DRIVER_RC:1" {
	        exit 4
	    }
	    timeout {
	        exit 4
	    }
	}

	# Configure the guest side of the vmnet shared network.
	expect "# "
	send "ip link set eth0 up\r"

	expect "# "
	send "ip addr flush dev eth0\r"

	expect "# "
	send "ip addr add $guest_ip/24 dev eth0\r"

	expect "# "
	send "ip addr show eth0\r"

	expect {
	    "$guest_ip/24" {}
	    timeout {
	        exit 5
	    }
	}

	expect "# "
	send "ping -c 3 -W 5 $gateway_ip\r"

	expect {
	    -re {3 packets transmitted, 3 packets received|3 packets transmitted, 3 received} {}
	    -re {3 packets transmitted, 0 packets received|3 packets transmitted, 0 received|100% packet loss} {
	        exit 6
	    }
	    timeout {
	        exit 6
	    }
	}

	# Verify that virtio-snd is usable in the same privileged rv32emu process
	# used by the vmnet backend.
	expect "# "
	send "cat /proc/asound/cards\r"

	expect {
	    "VirtIO SoundCard" {}
	    timeout {
	        exit 7
	    }
	}

	expect "# "
	send "aplay -l\r"

	expect {
	    "VirtIO SoundCard" {}
	    timeout {
	        exit 8
	    }
	}

	# Use the same bounded configuration as the regular virtio-snd CI test.
	expect "# "
	set timeout 60

	send {speaker-test -D hw:0,0 -c 1 -r 48000 -F S16_LE -t sine -b 1200000 -p 300000 -P 4 -l 1; echo SPEAKER_TEST_RC:$?}
	send "\r"

	expect {
	    "Playback open error:" {
	        exit 9
	    }
	    "Setting of hwparams failed:" {
	        exit 9
	    }
	    "Setting of swparams failed:" {
	        exit 9
	    }
	    "Transfer failed:" {
	        exit 9
	    }
	    "0 - Mono" {}
	    timeout {
	        exit 9
	    }
	}

	expect {
	    "Time per period =" {}
	    "Transfer failed:" {
	        exit 9
	    }
	    timeout {
	        exit 9
	    }
	}

	expect {
	    "SPEAKER_TEST_RC:0" {}
	    -re {SPEAKER_TEST_RC:([1-9][0-9]*)} {
	        exit 9
	    }
	    timeout {
	        exit 9
	    }
	}

	expect "# "

	# rv32emu uses Ctrl-A x to exit.
	send "\x01"
	send "x"

	expect eof
DONE
}

run_vmnet_vsnd_case
ret=$?
RET=$((${RET} + ${ret}))

printf "\nmacOS vmnet + virtio-snd Test: [ ${MESSAGES[$ret]}${COLOR_N} ]\n"

exit ${RET}
