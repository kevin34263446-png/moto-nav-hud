"""Check real firmware ACKs; visual UI and watchdog reset need separate checks."""
import argparse
import json
import time
from datetime import datetime, timezone
from pathlib import Path

import serial
from sim_sender import FLAG_FIX_OK, FLAG_OFF_ROUTE, build_state


def run(port, baud, output, frames=100, vary_ui=False, dma_cases=False):
    results = []
    seq = 0
    with serial.Serial(port, baud, timeout=0.02, write_timeout=1) as link:
        link.reset_input_buffer()
        pending = bytearray()

        def frame():
            nonlocal seq
            current = seq
            seq = (seq + 1) % 256
            return current, build_state(current, 1, 200, 45, FLAG_FIX_OK, 12000, 18)

        def receive(seconds):
            nonlocal pending
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                pending.extend(link.read(link.in_waiting or 1))
            lines = []
            while b'\n' in pending:
                line, rest = pending.split(b'\n', 1)
                pending = bytearray(rest)
                lines.append(line.decode('ascii', errors='replace').rstrip('\r'))
            acks, other = [], []
            for line in lines:
                if line.startswith('OK ') and line[3:].isdigit():
                    acks.append(int(line[3:]))
                elif line:
                    other.append(line)
            return acks, other

        def record(name, expected, actual, other):
            passed = expected == actual and not other
            entry = dict(case=name, passed=passed, expected=expected, received=actual, other=other)
            results.append(entry)
            print(f'{name}: {"PASS" if passed else "FAIL"} '
                  f'expected={len(expected)} received={len(actual)}', flush=True)

        expected, actual, other = [], [], []
        stream_start = time.monotonic()
        for index in range(frames):
            current, packet = frame()
            if vary_ui:
                packet = build_state(
                    current, (index // 10) % 7, 500 - (index % 50) * 10,
                    30 + index % 30,
                    FLAG_FIX_OK | (FLAG_OFF_ROUTE if (index // 25) % 2 else 0),
                    max(0, 12000 - index * 3), max(0, 18 - index // 200))
            expected.append(current)
            link.write(packet)
            # Absolute pacing avoids accumulating Windows serial timeout jitter.
            got, unexpected = receive(max(0, stream_start + (index + 1) * 0.2 - time.monotonic()))
            actual.extend(got)
            other.extend(unexpected)
            if (index + 1) % 100 == 0:
                print(f'Stream progress: {index + 1}/{frames}', flush=True)
        got, unexpected = receive(0.3)
        stream_elapsed = time.monotonic() - stream_start
        record(f'{frames}_valid_frames_target_5hz', expected, actual + got, other + unexpected)

        # Each bad input is immediately followed by a known good frame.
        for mode in ('bad_crc', 'half_timeout', 'glued', 'noise', 'overlap_header', 'bad_version', 'bad_type'):
            current, packet = frame()
            next_seq, next_packet = frame()
            expected = [next_seq]
            if mode == 'bad_crc':
                broken = bytearray(packet)
                broken[6] ^= 0xFF
                link.write(broken + next_packet)
            elif mode == 'half_timeout':
                link.write(packet[:8])
                time.sleep(0.12)
                link.write(packet[8:] + next_packet)
            elif mode == 'glued':
                link.write(packet + next_packet)
                expected = [current, next_seq]
            elif mode == 'noise':
                link.write(b'\x00\xff\x12\x5a\xa5\x00' + next_packet)
            elif mode == 'overlap_header':
                link.write(b'\xa5' + next_packet)
            else:
                from sim_sender import crc16_modbus
                broken = bytearray(packet)
                broken[2 if mode == 'bad_version' else 3] = 0x7F
                broken[14:] = crc16_modbus(broken[:14]).to_bytes(2, 'little')
                link.write(broken + next_packet)
            actual, other = receive(0.5)
            record(mode, expected, actual, other)

        if dma_cases:
            for count in (2, 4, 5, 16, 80, 160):
                packets = [frame() for _ in range(count)]
                payload = b''.join(packet for _, packet in packets)
                link.write(payload)
                actual, other = receive(len(payload) * 10 / baud + 0.5)
                record(f'continuous_{len(payload)}_bytes', [n for n, _ in packets], actual, other)

            # Each 65-byte block advances the DMA cursor by one: cover all 64
            # offsets and the IDLE/TC coincidence without resetting the receiver.
            expected, actual, other = [], [], []
            for _ in range(64):
                packets = [frame() for _ in range(4)]
                expected.extend(n for n, _ in packets)
                link.write(b'\x00' + b''.join(packet for _, packet in packets))
                got, unexpected = receive(0.05)
                actual.extend(got)
                other.extend(unexpected)
            got, unexpected = receive(0.2)
            record('all_64_dma_offsets', expected, actual + got, other + unexpected)

            for split in (1, 15, 16, 31, 32, 33, 63):
                packets = [frame() for _ in range(4)]
                payload = b''.join(packet for _, packet in packets)
                link.write(payload[:split])
                link.flush()
                time.sleep(0.005)  # IDLE, but below the 50 ms protocol timeout.
                link.write(payload[split:])
                actual, other = receive(0.3)
                record(f'split_64_at_{split}', [n for n, _ in packets], actual, other)

            # Deliberate baud mismatch should cause UART framing errors. The
            # debugger metrics must also show errors/restarts before claiming coverage.
            link.baudrate = baud // 2
            link.write(b'\x00' * 8)
            link.flush()
            time.sleep(0.05)
            link.baudrate = baud
            actual, other = receive(0.2)
            record('baud_mismatch_no_valid_ack', [], actual, other)
            current, packet = frame()
            link.write(packet)
            actual, other = receive(0.5)
            record('valid_after_baud_mismatch', [current], actual, other)

        print('Pause 4 seconds: screen should show signal lost.', flush=True)
        actual, other = receive(4)
        record('disconnect_no_spurious_ack', [], actual, other)
        current, packet = frame()
        link.write(packet)
        actual, other = receive(0.6)
        record('resume_after_disconnect', [current], actual, other)

    report = dict(timestamp_utc=datetime.now(timezone.utc).isoformat(), port=port,
                  stream_elapsed_seconds=stream_elapsed,
                  stream_actual_hz=frames / stream_elapsed,
                  stream_payload='varying' if vary_ui else 'constant',
                  dma_cases=dma_cases,
                  baud=baud, passed=all(item['passed'] for item in results),
                  checks=results, visual_ui='not automatically verified',
                  watchdog_reset='not exercised')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(f'Report: {output}', flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--baud', type=int, default=115200)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--frames', type=int, default=100)
    parser.add_argument('--vary-ui', action='store_true', help='Vary navigation fields to exercise screen redraws')
    parser.add_argument('--dma-cases', action='store_true', help='DMA boundaries, continuous bursts, and baud error recovery')
    args = parser.parse_args()
    if args.frames < 1:
        parser.error('--frames must be positive')
    raise SystemExit(run(args.port, args.baud, args.output, args.frames, args.vary_ui, args.dma_cases))
