#!/usr/bin/env bash

# Get the directory of this script
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "${SCRIPT_DIR}/common.sh"

check_platform

RET=0

backend="${VNET_BACKEND:-tap}"

if [[ "${backend}" != "tap" ]]; then
    print_error "Unsupported virtio-net backend for this test: ${backend}"
    exit 2
fi

if [[ "${OS_TYPE}" != "Linux" ]]; then
    print_warning "Skipping virtio-net TAP test on non-Linux host"
    exit 0
fi

register_cleanup cleanup_emulator

TIMEOUT=${BOOT_TIMEOUT:-60}

COLOR_G='\e[32;01m' # Green
COLOR_R='\e[31;01m' # Red
COLOR_Y='\e[33;01m' # Yellow
COLOR_N='\e[0m'     # No color

MESSAGES=("${COLOR_G}OK!"
    "${COLOR_R}Fail to boot"
    "${COLOR_R}Fail to login"
    "${COLOR_R}Fail to run commands"
    "${COLOR_R}Fail to ping host TAP"
)

OPTS_BASE=" -k build/linux-image/Image -i build/linux-image/rootfs.cpio"
RUN_LINUX="sudo -E build/rv32emu ${OPTS_BASE} -x vnet:tap"

printf "${COLOR_Y}===== Test option: ${OPTS_BASE} -x vnet:tap =====${COLOR_N}\n"

ASSERT expect <<- DONE
	set timeout ${TIMEOUT}
	set tap_if ""

	spawn ${RUN_LINUX}

	expect {
	    -re {allocated TAP interface: (tap[0-9]+)} {
	        set tap_if \$expect_out(1,string)
	        exp_continue
	    }
	    "buildroot login:" {
	        if { "\$tap_if" == "" } {
	            set tap_if [exec sh -c {ip -o link show | awk -F': ' '\$2 ~ /^tap[0-9]+/ {print \$2; exit}'}]
	        }

	        exec sudo ip addr replace 192.168.100.1/24 dev \$tap_if
	        exec sudo ip link set \$tap_if up

	        send "root\\r"
	    }
	    timeout {
	        exit 1
	    }
	}

	expect "# "
	send "readlink /sys/bus/virtio/devices/virtio0/driver\\r"
	expect {
	    "virtio_net" {}
	    timeout { exit 2 }
	}

	expect "# "
	send "ip link set eth0 up\\r"

	expect "# "
	send "ip addr add 192.168.100.2/24 dev eth0\\r"

	expect "# "
	send "ip addr show eth0\\r"
	expect {
	    "192.168.100.2/24" {}
	    timeout { exit 3 }
	}

	expect "# "
	send "ping -c 3 -W 5 192.168.100.1\\r"
	expect {
	    -re {3 packets transmitted, 3 packets received|3 packets transmitted, 3 received} {
	        send "\\x01"
	        send "x"
	        exit 0
	    }
	    -re {3 packets transmitted, 0 packets received|3 packets transmitted, 0 received|100% packet loss} {
	        exit 4
	    }
	    timeout {
	        exit 4
	    }
	}
DONE

ret=$?
RET=$((${RET} + ${ret}))
cleanup

printf "\nVirtio-net TAP Test: [ ${MESSAGES[$ret]}${COLOR_N} ]\n"

exit ${RET}
