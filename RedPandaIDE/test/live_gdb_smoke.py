"""Exercise the MI operations used by the IDE against a real isolated target."""
import argparse
import queue
import re
import socket
import subprocess
import threading
import time
from pathlib import Path

parser = argparse.ArgumentParser()
for name in ('gdb', 'server', 'exe', 'source', 'log'):
    parser.add_argument('--' + name, required=True)
a = parser.parse_args()
transcript = []
messages = queue.Queue()
stops = []
sequence = 0
no_window = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
with socket.socket() as listener:
    listener.bind(('127.0.0.1', 0))
    port = listener.getsockname()[1]
server = subprocess.Popen([a.server, '--once', f'127.0.0.1:{port}', a.exe],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                          creationflags=no_window)
gdb = subprocess.Popen([a.gdb, '--interpreter=mi2', '--quiet'], stdin=subprocess.PIPE,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, encoding='utf-8', errors='replace',
                       creationflags=no_window)
def reader():
    for line in gdb.stdout:
        line = line.rstrip()
        transcript.append(line)
        messages.put(line)
threading.Thread(target=reader, daemon=True).start()
def until(predicate, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            line = messages.get(timeout=max(.01, deadline-time.monotonic()))
        except queue.Empty:
            break
        if line.startswith('*stopped'):
            stops.append(line)
        if predicate(line):
            return line
    raise AssertionError('GDB response timed out')
def command(text, error=False):
    global sequence
    sequence += 1
    token = str(sequence)
    transcript.append('>>> ' + text)
    gdb.stdin.write(token + text + '\n')
    gdb.stdin.flush()
    result = until(lambda line: line.startswith(token + '^'))
    assert ('^error' in result) == error, result
    return result

def stop():
    if not stops:
        until(lambda line: line.startswith('*stopped'))
    return stops.pop(0)

def resume():
    stops.clear()
    command('-exec-continue')

try:
    command('-gdb-set confirm off')
    command('-gdb-set mi-async on')
    command('-file-exec-and-symbols "' + Path(a.exe).as_posix() + '"')
    command(f'-target-select remote 127.0.0.1:{port}')
    source = Path(a.source).as_posix()
    result = command(f'-break-insert --source "{source}" --line 9')
    number = re.search(r'number="(\d+)"', result).group(1)
    resume()
    assert 'line="9"' in stop()
    command('-var-create watch @ watch_value')
    assert 'value="1234"' in command('-var-assign watch "1234"')
    assert 'value="1234"' in command('-data-evaluate-expression watch_value')
    # A string literal must survive MI argument parsing intact.
    quoted_expression = '"sizeof(\\"a b\\")"'
    assert 'value="4"' in command('-data-evaluate-expression ' + quoted_expression)
    assert 'value="4"' in command('-var-create quoted @ ' + quoted_expression)
    command('-var-delete quoted')
    conditional = command(f'-break-insert -c "sizeof(\\"a b\\") == 4" --source "{source}" --line 10')
    conditional_number = re.search(r'number="(\d+)"', conditional).group(1)
    command('-break-delete ' + conditional_number)
    command('-var-assign watch "not_a_valid_symbol"', error=True)
    assert 'value="1234"' in command('-data-evaluate-expression watch_value')
    command(f'-break-condition {number} sizeof("a b") == 4 && ticks >= 3')
    resume()
    assert 'line="9"' in stop()
    assert 'value="3"' in command('-data-evaluate-expression ticks')
    # The IDE's zero-based caret line 9 must request actual source line 10.
    stops.clear()
    command(f'-exec-until "{source}:10"')
    assert 'line="10"' in stop()
    stops.clear()
    command('-exec-finish')
    assert 'function-finished' in stop()
    resume()
    assert 'line="9"' in stop()
    stops.clear()
    command('-exec-step')
    assert 'line="10"' in stop()
    memory = command('-data-read-memory-bytes "&watch_value + 0" 3')
    assert re.search(r'contents="[0-9a-fA-F]{6}"', memory), memory
    # Create a data breakpoint while the target is running, using the same
    # interrupt/apply/resume sequence as the production scheduler.
    command(f'-break-delete {number}')
    resume()
    command('-exec-interrupt')
    stop()
    watchpoint = command('-break-watch "ticks"')
    watchpoint_number = re.search(r'number="(\d+)"', watchpoint).group(1)
    resume()
    assert 'watchpoint-trigger' in stop()
    command('-break-delete ' + watchpoint_number)
    resume()
    # The IDE scheduler pauses a running remote target before mutating breakpoints.
    command('-exec-interrupt')
    stop()
    result = command(f'-break-insert --source "{source}" --line 10')
    number = re.search(r'number="(\d+)"', result).group(1)
    resume()
    assert 'line="10"' in stop()
    command(f'-break-delete {number}')
    resume()
    command('-exec-interrupt')
    assert 'reason=' in stop()
    command('-data-evaluate-expression "keep_running = 0"')
    resume()
    assert 'exited' in stop()
    command('-gdb-exit')
    print('PASS: real GDB/MI assignment/readback, quoted expressions/conditions, run-to-cursor, step/finish, short memory read, running watchpoint/breakpoint edits, interrupt and normal exit')
finally:
    if gdb.poll() is None:
        try:
            command('-exec-interrupt')
            command('-interpreter-exec console "kill"')
            command('-gdb-exit')
        except Exception:
            pass
        gdb.terminate()
        gdb.wait(timeout=5)
    if server.poll() is None:
        server.terminate()
        server.wait(timeout=5)
    Path(a.log).write_text('\n'.join(transcript) + '\n', encoding='utf-8')
