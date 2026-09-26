#!/usr/bin/env python3
"""Exercise an unmodified demo411 ELF through USART2 and GPIO tracing.

Local experimental test; requires no Python packages. QEMU must be built with
blackpill-f411ce and the default log trace backend. Logs are kept in --output.
"""
import argparse
import hashlib
import json
import pathlib
import re
import select
import socket
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', required=True)
    parser.add_argument('--firmware', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    output = pathlib.Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    transcript = bytearray()
    pending = bytearray()
    report = {
        "firmware": str(pathlib.Path(args.firmware).resolve()),
        "firmware_sha256": hashlib.sha256(
            pathlib.Path(args.firmware).read_bytes()).hexdigest(),
    }
    with tempfile.TemporaryDirectory(prefix='f411-', dir='/tmp') as tmp:
        serial_path = str(pathlib.Path(tmp) / 'serial')
        qmp_path = str(pathlib.Path(tmp) / 'qmp')
        with (output / 'qemu.log').open('wb') as log:
            process = subprocess.Popen([
                str(pathlib.Path(args.qemu).resolve()), '-M', 'blackpill-f411ce',
                '-kernel', str(pathlib.Path(args.firmware).resolve()),
                '-display', 'none', '-monitor', 'none',
                '-icount', 'shift=7,sleep=off',
                '-chardev', f'socket,id=uart,path={serial_path},server=on,wait=on',
                '-serial', 'chardev:uart',
                '-qmp', f'unix:{qmp_path},server=on,wait=off',
                '-trace', 'enable=stm32f411_gpio', '-d', 'unimp,guest_errors',
            ], stdout=subprocess.DEVNULL, stderr=log)
            try:
                def connect(path):
                    deadline = time.monotonic() + 10
                    while time.monotonic() < deadline:
                        sock = socket.socket(socket.AF_UNIX)
                        try:
                            sock.connect(path)
                            return sock
                        except (FileNotFoundError, ConnectionRefusedError):
                            sock.close()
                            if process.poll() is not None:
                                raise RuntimeError('QEMU exited; inspect qemu.log')
                            time.sleep(0.02)
                    raise TimeoutError(f'Waiting for {path}')

                serial = connect(serial_path)

                def expect(needle, timeout=8):
                    deadline = time.monotonic() + timeout
                    while needle not in pending:
                        left = deadline - time.monotonic()
                        if left <= 0 or not select.select([serial], [], [], left)[0]:
                            raise AssertionError(f'Missing {needle!r}; got {bytes(pending)!r}')
                        data = serial.recv(4096)
                        if not data:
                            raise RuntimeError('Serial disconnected')
                        pending.extend(data)
                        transcript.extend(data)
                    end = pending.index(needle) + len(needle)
                    result = bytes(pending[:end])
                    del pending[:end]
                    return result

                def command(text, needle):
                    serial.sendall(text.encode() + b'\r')
                    result = expect(b'>')
                    assert needle in result, (text, result)
                    return result

                expect(b'STM32F411 SW ver=')
                expect(b'>')
                report['boot_and_scheduler'] = 'PASS'
                command('?', b'T Commands')
                report['serial_menu'] = 'PASS'
                command('TS123456', b'Time is: 12:34:56')
                command('TS?', b'Time is: 12:34:')
                # Judge progress in guest RTC time, not host wall time.
                deadline = time.monotonic() + 45
                while True:
                    result = command('TS?', b'Time is:')
                    match = re.search(rb'Time is: (\d+):(\d+):(\d+)', result)
                    assert match, result
                    h, m, sec = map(int, match.groups())
                    elapsed = h * 3600 + m * 60 + sec - (12 * 3600 + 34 * 60 + 56)
                    if elapsed >= 8:
                        break
                    assert time.monotonic() < deadline, 'Guest RTC did not advance'
                assert elapsed < 30, elapsed
                report['rtc_advances'] = 'PASS'
                command('TD?', b'Date is:')
                # The firmware has a pre-existing sscanf pointer-size bug in TD.
                command('TD260926', b'Date is:')
                date_result = command('TD?', b'Date is:')
                report['firmware_date_command'] = (
                    'PASS (parser still needs its independent C type fix)'
                    if b'2026/09/26' in date_result else
                    'KNOWN FIRMWARE BUG: ' + date_result.decode(errors='replace'))
                events = {}
                for ns, pin, level in re.findall(
                        r'stm32f411_gpio ns=(\d+) PA(\d+)=(\d+)',
                        (output / 'qemu.log').read_text()):
                    events.setdefault(int(pin), []).append(int(ns))
                for pin, period in ((6, 1_500_000_000), (7, 800_000_000)):
                    times = events.get(pin, [])
                    assert len(times) >= 3, (pin, times)
                    for a, b in zip(times, times[1:]):
                        assert abs((b - a) - period) < 30_000_000, (pin, b - a)
                report['freertos_gpio_periods'] = 'PASS'
                qmp = connect(qmp_path)
                qmp.settimeout(5)
                stream = qmp.makefile('rwb', buffering=0)
                json.loads(stream.readline())

                def execute(name):
                    stream.write(json.dumps({'execute': name}).encode() + b'\n')
                    while True:
                        message = json.loads(stream.readline())
                        if 'return' in message:
                            return message['return']
                        if 'error' in message:
                            raise AssertionError(message)

                execute('qmp_capabilities')
                execute('system_reset')
                expect(b'STM32F411 SW ver=')
                expect(b'>')
                command('TS?', b'Time is: 12:35:')
                report['system_reset_and_rtc_persistence'] = 'PASS'
                stream.close()
                qmp.close()
                serial.close()
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                (output / 'serial.log').write_bytes(transcript)
                (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
