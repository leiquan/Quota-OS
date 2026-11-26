import argparse
import os
import select
import secrets
import subprocess
import time
from pathlib import Path

parser = argparse.ArgumentParser(description="Verify the 4 MiB CLI admin gate on disposable disks.")
parser.add_argument("output", type=Path, help="new directory for the raw serial logs")
out = parser.parse_args().output.resolve()
out.mkdir(exist_ok=False)
disk = out/'admin.raw'
guest = ['bash', 'tools/run-cli.sh', '--quiet']
subprocess.run(['python3', 'tools/init-admin-disk.py', str(disk)], check=True)
secret = secrets.token_hex(16).encode()
wrong = secrets.token_hex(16).encode()
saved = 'admin-session-note'
quota = b'quota> '
setup = b'admin | Set password: '
confirm = b'admin | Confirm password: '
login = b'admin | Password: '
closed = b'Account storage unavailable. Login closed.'

class Guest:
    def __init__(self, name, volume):
        self.log = (out/(name+'.log')).open('wb')
        self.process = subprocess.Popen(guest, env=dict(os.environ, CLI_RAM='4M',
            CLI_DISK=str(volume), CLI_ROOT_DISK="", CLI_TEST='1'), stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    def __enter__(self):
        return self
    def __exit__(self, *_):
        self.process.terminate()
        self.process.wait(timeout=5)
        self.log.close()
    def read(self, end, expected=None):
        buf = b''
        deadline = time.monotonic() + 20
        while end not in buf:
            if time.monotonic() >= deadline:
                raise AssertionError(('missing prompt', end))
            if not select.select([self.process.stdout], [], [], .2)[0]:
                continue
            read = os.read(self.process.stdout.fileno(),4096)
            if not read:
                raise AssertionError('guest exited before the expected prompt')
            buf += read
        assert secret not in buf and wrong not in buf, "Password was echoed"
        self.log.write(buf)
        self.log.flush()
        if expected:
            assert expected in buf, expected
        if end != quota:
            assert quota not in buf
        return buf
    def send(self, letter, end, expected=None):
        data = letter if isinstance(letter, bytes) else letter.encode()
        self.process.stdin.write(data+b'\r')
        self.process.stdin.flush()
        return self.read(end, expected)

with Guest('initial', disk) as vm:
    vm.read(setup)
    vm.send(secret, confirm)
    vm.send(wrong, setup, b'Passwords do not match.')
    vm.send(secret, confirm)
    vm.send(secret, login)
    vm.send(secret, quota, b'4096 KiB RAM')
    tasklist = vm.send('tasks', quota)
    assert b'Test tier=' not in tasklist
    vm.send('write '+saved, quota, b'\r\nSaved\r\n')
    vm.send('logout', login)
    vm.send('cat Note.txt', login, b'Incorrect password.')
    time.sleep(.35)
    vm.send(secret, quota, b'4096 KiB RAM')
    vm.send('cat Note.txt', quota, b'\r\n'+saved.encode()+b'\r\n')
    vm.send('events', quota,b'tier1_misses=0 tier2_short=0 tier3_over=0')
with Guest('cold', disk) as vm:
    vm.read(login)
    vm.send(b'', login, b'Enter 1 to 64')
    vm.send(wrong, login, b'Incorrect password.')
    time.sleep(.35)
    vm.send(secret, quota)
    vm.send('cat Note.txt', quota, b'\r\n'+saved.encode()+b'\r\n')
    vm.send('run Test t2', quota, b'Test: EL0 completed')
    vm.send('events', quota,b'tier1_misses=0 tier2_short=0 tier3_over=0')
draft = out/'damaged.raw'
damaged = bytearray(disk.read_bytes())
damaged[63*512] ^= 1
draft.write_bytes(damaged)
with Guest('corrupt', draft) as vm:
    vm.read(closed)
with Guest('missing', '') as vm:
    vm.read(closed)
assert secret not in disk.read_bytes() and wrong not in disk.read_bytes()
(out/'admin.raw').unlink()
draft.unlink()
print('PASS 4 MiB setup, confirmation, hidden input, login, cold persistence, logout, corrupt and missing disk, quota')
