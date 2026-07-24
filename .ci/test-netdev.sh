#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "${SCRIPT_DIR}/common.sh"

check_platform

backend="${1:-user}"

if [[ "${backend}" != "user" && "${backend}" != "tap" ]]; then
    print_error "Usage: $0 [user|tap]"
    exit 2
fi

case "${backend}" in
    user)
        guest_ip="10.0.2.15"
        gateway_ip="10.0.2.2"
        ;;
    tap)
        if [[ "${OS_TYPE}" != "Linux" ]]; then
            print_warning "Skipping TAP virtio-net test on non-Linux host"
            exit 0
        fi
        guest_ip="192.168.100.2"
        gateway_ip="192.168.100.1"
        ;;
esac

register_cleanup cleanup_emulator

if [[ -n "${BOOT_TIMEOUT:-}" ]]; then
    TIMEOUT="${BOOT_TIMEOUT}"
elif [[ "${OS_TYPE}" == "Darwin" || "${OS_TYPE}" == "macOS" ]]; then
    TIMEOUT=900
else
    TIMEOUT=60
fi

if [[ "${backend}" == "tap" ]]; then
    spawn_cmd="sudo -E build/rv32emu"
else
    spawn_cmd="build/rv32emu"
fi

ASSERT expect <<- DONE
    set timeout ${TIMEOUT}
    set tap_if ""

    spawn {*}${spawn_cmd} \
        -k build/linux-image/Image \
        -i build/linux-image/rootfs.cpio \
        -x vnet:${backend}

    expect {
        -re {allocated TAP interface: (tap[0-9]+)} {
            set tap_if \$expect_out(1,string)
            exp_continue
        }
        "buildroot login:" {
            if { "${backend}" == "tap" } {
                if { "\$tap_if" == "" } {
                    set tap_if [exec sh -c {ip -o link show | awk -F': ' '\$2 ~ /^tap[0-9]+/ {print \$2; exit}'}]
                }

                exec sudo ip addr replace ${gateway_ip}/24 dev \$tap_if
                exec sudo ip link set \$tap_if up
            }

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
    send "ip addr add ${guest_ip}/24 dev eth0\\r"

    expect "# "
    send "ip addr show eth0\\r"
    expect {
        "${guest_ip}/24" {}
        timeout { exit 3 }
    }

    expect "# "
    send "ping -c 3 -W 5 ${gateway_ip}\\r"
    expect {
        -re {3 packets transmitted, 3 packets received|3 packets transmitted, 3 received} {
            send "\\x01"
            send "x"
            exit 0
        }
        -re {3 packets transmitted, 0 packets received|3 packets transmitted, 0 received|100% packet loss} {
            exit 4
        }
        "# " {
            exit 4
        }
        timeout {
            exit 4
        }
    }
DONE

print_success "virtio-net ${backend} boot test passed"
