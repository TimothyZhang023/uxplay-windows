#!/usr/bin/env python3
"""Windows AirPlay BLE discovery beacon used by uxplay-windows."""

import argparse
import ipaddress
import os
import socket
import struct
import sys
import threading
import time

import psutil

try:
    import winrt.windows.foundation.collections
    import winrt.windows.devices.bluetooth.advertisement as ble_adv
    import winrt.windows.storage.streams as streams
except ImportError as exc:
    print(f"[beacon] FATAL missing WinRT package: {exc}", flush=True)
    sys.exit(1)


publisher = None
publisher_status = "STOPPED"
status_event = threading.Event()
advertised_port = None
advertised_address = None


def on_status_changed(sender, args):
    global publisher, publisher_status, advertised_port, advertised_address
    if sender is not publisher:
        return
    publisher_status = args.status.name
    error = getattr(args, "error", None)
    print(f"[beacon] STATUS {publisher_status} error={error}", flush=True)
    status_event.set()
    if publisher_status in ("ABORTED", "STOPPED"):
        publisher = None
        advertised_port = None
        advertised_address = None


def stop_advertising():
    global publisher, publisher_status, advertised_port, advertised_address
    old_publisher = publisher
    publisher = None
    publisher_status = "STOPPED"
    advertised_port = None
    advertised_address = None
    if old_publisher is not None:
        try:
            old_publisher.stop()
        except Exception as exc:
            print(f"[beacon] stop warning: {exc}", flush=True)


def start_advertising(ipv4_str: str, port: int, timeout: float = 5.0) -> bool:
    global publisher, publisher_status, advertised_port, advertised_address
    stop_advertising()

    mfg_data = bytearray([0x09, 0x08, 0x13, 0x30])
    mfg_data.extend(ipaddress.ip_address(ipv4_str).packed)
    mfg_data.extend(port.to_bytes(2, "big"))

    writer = streams.DataWriter()
    writer.write_bytes(mfg_data)
    manufacturer = ble_adv.BluetoothLEManufacturerData()
    manufacturer.company_id = 0x004C
    manufacturer.data = writer.detach_buffer()

    advertisement = ble_adv.BluetoothLEAdvertisement()
    advertisement.manufacturer_data.append(manufacturer)

    candidate = ble_adv.BluetoothLEAdvertisementPublisher(advertisement)
    publisher = candidate
    publisher_status = "CREATED"
    status_event.clear()
    candidate.add_status_changed(on_status_changed)

    try:
        candidate.start()
    except Exception as exc:
        print(f"[beacon] ABORTED start failed: {exc}", flush=True)
        publisher = None
        return False

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        status_event.wait(min(0.5, max(0.0, deadline - time.monotonic())))
        status_event.clear()
        if publisher_status == "STARTED" and publisher is candidate:
            advertised_port = port
            advertised_address = ipv4_str
            print(f"[beacon] ADVERTISING {ipv4_str}:{port}", flush=True)
            return True
        if publisher is None or publisher_status in ("ABORTED", "STOPPED"):
            return False

    print("[beacon] ABORTED timed out waiting for Bluetooth STARTED", flush=True)
    stop_advertising()
    return False


def read_ble_file(path: str):
    if not os.path.isfile(path):
        return None, None
    try:
        with open(path, "rb") as stream:
            data = stream.read()
        if len(data) < 7:
            return None, None
        port = struct.unpack_from("<H", data, 0)[0]
        pid = struct.unpack_from("<I", data, 2)[0]
        name = data[6:].split(b"\0", 1)[0].decode("utf-8")
        if not psutil.pid_exists(pid):
            return None, None
        process_name = psutil.Process(pid).name().lower()
        expected_name = os.path.basename(name).lower()
        if expected_name and not process_name.startswith(expected_name):
            return None, None
        return port, pid
    except (OSError, UnicodeError, struct.error, psutil.Error):
        return None, None


def route_selected_ipv4():
    for target in (("224.0.0.251", 5353), ("8.8.8.8", 53)):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.connect(target)
            address = sock.getsockname()[0]
            if address and not address.startswith("127."):
                return address
        except OSError:
            pass
        finally:
            sock.close()
    return None


def local_ipv4_candidates():
    primary = route_selected_ipv4()
    stats = psutil.net_if_stats()
    found = []
    for interface, addresses in psutil.net_if_addrs().items():
        if interface in stats and not stats[interface].isup:
            continue
        for address in addresses:
            if address.family != socket.AF_INET:
                continue
            try:
                parsed = ipaddress.ip_address(address.address)
            except ValueError:
                continue
            if parsed.is_loopback or parsed.is_unspecified or parsed.is_multicast:
                continue
            item = (address.address, interface)
            if item not in found:
                found.append(item)

    found.sort(key=lambda item: (item[0] != primary, item[0].startswith("169.254."), item[1]))
    return found


def main():
    parser = argparse.ArgumentParser(description="AirPlay BLE beacon for uxplay-windows")
    parser.add_argument("--path", default=os.path.expanduser("~/.uxplay.ble"))
    parser.add_argument("--ipv4", default=None, help="Advertise only this IPv4 address")
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--cycle-interval", type=float, default=5.0)
    args = parser.parse_args()

    if args.ipv4:
        ipaddress.ip_address(args.ipv4)
        candidates = [(args.ipv4, "override")]
    else:
        candidates = local_ipv4_candidates()
    if not candidates:
        print("[beacon] FATAL no active IPv4 address", flush=True)
        return 2

    print(f"[beacon] WATCHING {args.path}", flush=True)
    print("[beacon] CANDIDATES " + ", ".join(f"{ip} ({name})" for ip, name in candidates), flush=True)

    # Validate the WinRT Bluetooth publisher before telling the GUI that the
    # fallback is usable. Port 9 is only probe metadata; no network connection
    # is made.
    if not start_advertising(candidates[0][0], 9):
        print("[beacon] FATAL Bluetooth advertising unavailable", flush=True)
        return 3
    stop_advertising()
    print("[beacon] READY", flush=True)

    address_index = 0
    last_cycle = 0.0
    last_refresh = 0.0
    current_port = None

    try:
        while True:
            now = time.monotonic()
            if not args.ipv4 and now - last_refresh >= 15.0:
                refreshed = local_ipv4_candidates()
                if refreshed and refreshed != candidates:
                    candidates = refreshed
                    address_index = 0
                    stop_advertising()
                    print("[beacon] NETWORK_CHANGED " +
                          ", ".join(f"{ip} ({name})" for ip, name in candidates), flush=True)
                last_refresh = now

            port, _ = read_ble_file(args.path)
            if port is None:
                if publisher is not None:
                    stop_advertising()
                current_port = None
                time.sleep(args.interval)
                continue

            should_rotate = len(candidates) > 1 and now - last_cycle >= args.cycle_interval
            should_start = publisher is None or port != current_port or should_rotate
            if should_start:
                if should_rotate:
                    address_index = (address_index + 1) % len(candidates)
                address = candidates[address_index][0]
                if start_advertising(address, port):
                    current_port = port
                    last_cycle = time.monotonic()
                else:
                    address_index = (address_index + 1) % len(candidates)
                    time.sleep(min(2.0, args.interval))
                    continue

            time.sleep(args.interval)
    except KeyboardInterrupt:
        stop_advertising()
        print("[beacon] EXIT", flush=True)
        return 0


if __name__ == "__main__":
    sys.exit(main())
