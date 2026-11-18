"""Read MCU ACK mirrors without sending bytes to the navigation UART."""
import argparse
import json
import time
from datetime import datetime, timezone
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--baud', type=int, default=115200)
    parser.add_argument('--seconds', type=float, default=60)
    parser.add_argument('--until-acks', type=int, default=0)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.seconds <= 0 or args.until_acks < 0:
        parser.error('seconds must be positive and until-acks nonnegative')
    import serial

    start = time.monotonic()
    lines, acks = [], []
    pending = bytearray()
    print(f'Listening on {args.port} @ {args.baud}; no navigation bytes will be sent.', flush=True)
    try:
        with serial.Serial(args.port, args.baud, timeout=0.1) as link:
            link.reset_input_buffer()
            while time.monotonic() - start < args.seconds:
                pending.extend(link.read(link.in_waiting or 1))
                while b'\n' in pending:
                    raw, rest = pending.split(b'\n', 1)
                    pending = bytearray(rest)
                    line = raw.decode('ascii', errors='replace').rstrip('\r')
                    lines.append(dict(elapsed_seconds=time.monotonic() - start, text=line))
                    print(line, flush=True)
                    if line.startswith('OK ') and line[3:].isdigit():
                        acks.append(int(line[3:]))
                if args.until_acks and len(acks) >= args.until_acks:
                    break
    except KeyboardInterrupt:
        pass
    report = dict(timestamp_utc=datetime.now(timezone.utc).isoformat(), port=args.port,
                  baud=args.baud, elapsed_seconds=time.monotonic() - start,
                  received_acks=acks, lines=lines, incomplete_hex=pending.hex(),
                  note='Observed ACKs only; compare with the phone test vector and user display confirmation.')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(f'Captured {len(acks)} ACK(s). Report: {args.output}', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
