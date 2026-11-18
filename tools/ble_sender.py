"""Scan, inspect, and send navigation frames through a selected BLE GATT characteristic."""
import argparse
import asyncio
import json
from pathlib import Path

from sim_sender import FLAG_FIX_OK, build_state


async def run(args):
    from bleak import BleakClient, BleakScanner

    if args.scan:
        devices = await BleakScanner.discover(timeout=args.scan_seconds, return_adv=True)
        rows = [dict(address=device.address, name=advert.local_name or device.name,
                     service_uuids=advert.service_uuids, rssi=advert.rssi)
                for device, advert in devices.values()
                if not args.name or args.name.lower() in (advert.local_name or device.name or '').lower()]
        print(json.dumps(rows, indent=2, ensure_ascii=True))
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(rows, indent=2), encoding='utf-8')
        return 0

    device = await BleakScanner.find_device_by_address(args.address, timeout=args.scan_seconds)
    if device is None:
        raise RuntimeError('Selected device was not found. Check power, advertising, and other connections.')
    async with BleakClient(device, timeout=20) as client:
        if args.inspect:
            services = [dict(uuid=service.uuid, characteristics=[
                dict(uuid=char.uuid, handle=char.handle, properties=char.properties)
                for char in service.characteristics]) for service in client.services]
            print(json.dumps(services, indent=2))
            if args.output:
                args.output.parent.mkdir(parents=True, exist_ok=True)
                args.output.write_text(json.dumps(services, indent=2), encoding='utf-8')
            return 0

        write_char = client.services.get_characteristic(args.write_uuid)
        if write_char is None or not ({'write', 'write-without-response'} & set(write_char.properties)):
            raise RuntimeError('Selected characteristic does not support writes.')
        expected, received, other = [], [], []
        pending = bytearray()

        def on_notify(_characteristic, data):
            nonlocal pending
            pending.extend(data)
            while b'\n' in pending:
                line, rest = pending.split(b'\n', 1)
                pending = bytearray(rest)
                line = line.decode('ascii', errors='replace').strip()
                if line.startswith('OK ') and line[3:].isdigit():
                    received.append(int(line[3:]))
                elif line:
                    other.append(line)

        if args.notify_uuid:
            await client.start_notify(args.notify_uuid, on_notify)
        start = asyncio.get_running_loop().time()
        for index in range(args.frames):
            seq = index % 256
            packet = build_state(seq, (index // 10) % 7, 500 - index % 50 * 10,
                                 30 + index % 30, FLAG_FIX_OK, 12000, 18)
            await client.write_gatt_char(write_char, packet, response='write' in write_char.properties)
            expected.append(seq)
            await asyncio.sleep(max(0, start + (index + 1) / args.hz - asyncio.get_running_loop().time()))
        if args.exercise:
            bad_seq = args.frames % 256
            broken = bytearray(build_state(bad_seq, 1, 200, 45, FLAG_FIX_OK, 12000, 18))
            broken[6] ^= 0xFF
            await client.write_gatt_char(write_char, broken, response='write' in write_char.properties)
            await asyncio.sleep(0.2)
            for seq in ((bad_seq + 1) % 256, (bad_seq + 2) % 256):
                packet = build_state(seq, 1, 200, 45, FLAG_FIX_OK, 12000, 18)
                await client.write_gatt_char(write_char, packet, response='write' in write_char.properties)
                expected.append(seq)
                if seq == (bad_seq + 1) % 256:
                    print('Pause 4 seconds: expected signal lost, then resume.', flush=True)
                    await asyncio.sleep(4)
        await asyncio.sleep(1)
        report = dict(address=args.address, write_uuid=write_char.uuid, notify_uuid=args.notify_uuid,
                      frames_written=len(expected) + int(args.exercise), valid_frames_written=len(expected),
                      expected=expected, received=received,
                      crc_fault_injected=args.exercise, pause_seconds=4 if args.exercise else 0,
                      other=other, incomplete_notification_hex=pending.hex(),
                      passed=(expected == received and not other and not pending) if args.notify_uuid else None,
                      note=('MCU ACK sequence verified via BLE notifications.' if args.notify_uuid else
                            'Without notifications, successful GATT writes do not verify MCU parsing.'))
        if args.notify_uuid:
            await client.stop_notify(args.notify_uuid)
        print(json.dumps(report, indent=2))
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(report, indent=2), encoding='utf-8')
        return 1 if report['passed'] is False else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scan', action='store_true')
    parser.add_argument('--name', help='Optional scan name substring filter')
    parser.add_argument('--scan-seconds', type=float, default=8)
    parser.add_argument('--address')
    parser.add_argument('--inspect', action='store_true')
    parser.add_argument('--write-uuid', help='Choose from the inspected GATT service, not a guessed UUID')
    parser.add_argument('--notify-uuid', help='MCU ACK notification characteristic, if available')
    parser.add_argument('--frames', type=int, default=50)
    parser.add_argument('--exercise', action='store_true', help='Inject a bad CRC and pause/resume within the connection')
    parser.add_argument('--hz', type=float, default=5)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.frames < 1 or args.hz <= 0 or args.scan_seconds <= 0:
        parser.error('frames, hz and scan-seconds must be positive')
    if not args.scan and not args.address:
        parser.error('--address is required for inspecting or sending')
    if not args.scan and not args.inspect and not args.write_uuid:
        parser.error('--write-uuid is required for sending')
    try:
        limit = args.scan_seconds + (10 if args.scan else 40 + args.frames / args.hz)
        return asyncio.run(asyncio.wait_for(run(args), timeout=limit))
    except (ImportError, RuntimeError, OSError) as exc:
        parser.exit(1, f'BLE error: {exc}\n')


if __name__ == '__main__':
    raise SystemExit(main())
