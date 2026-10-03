#!/usr/bin/env python3
"""Drive Avian's JDWP server: handshake, ClassPrepare, breakpoint, step."""

import os
import select
import socket
import struct
import subprocess
import sys
import threading
import time

HANDSHAKE = b"JDWP-Handshake"


PROC = None
def die(msg):
    extra = ""
    if PROC is not None:
        if PROC.poll() is None:
            PROC.kill()
        try:
            out, err2 = PROC.communicate(timeout=2)
            extra = " rc=%s out=%r err=%r" % (PROC.returncode, out, err2)
        except Exception as e:
            extra = " " + str(e)
    sys.stderr.write("jdwp-test: %s%s\n" % (msg, extra))
    sys.exit(1)


class Conn(object):
    def __init__(self, sock):
        self.sock = sock
        self.buf = b""
        self.replies = {}
        self.events = []
        self.lock = threading.Lock()
        self.cv = threading.Condition(self.lock)
        self.alive = True
        self.next_id = 1
        self.thread = threading.Thread(target=self._read)
        self.thread.daemon = True
        self.thread.start()

    def _read_exact(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(4096)
            if not chunk:
                return False
            self.buf += chunk
        return True

    def _read(self):
        try:
            while True:
                if not self._read_exact(11):
                    break
                hdr, self.buf = self.buf[:11], self.buf[11:]
                length, ident, flags = struct.unpack(">IIB", hdr[:9])
                extra = struct.unpack(">H", hdr[9:11])[0]
                body_len = length - 11
                if body_len:
                    if not self._read_exact(body_len):
                        break
                    body, self.buf = self.buf[:body_len], self.buf[body_len:]
                else:
                    body = b""
                with self.cv:
                    if flags & 0x80:
                        self.replies[ident] = (extra, body)
                    else:
                        self.events.append((hdr[9], hdr[10], body))
                    self.cv.notify_all()
        finally:
            with self.cv:
                self.alive = False
                self.cv.notify_all()

    def send(self, cmdset, cmd, payload=b""):
        ident = self.next_id
        self.next_id += 1
        length = 11 + len(payload)
        packet = struct.pack(">IIBBB", length, ident, 0, cmdset, cmd) + payload
        self.sock.sendall(packet)
        deadline = time.time() + 15
        with self.cv:
            while ident not in self.replies:
                if not self.alive:
                    die("connection closed waiting for reply to %d/%d" % (cmdset, cmd))
                remaining = deadline - time.time()
                if remaining <= 0:
                    die("timeout waiting for reply to %d/%d" % (cmdset, cmd))
                self.cv.wait(remaining)
            error, body = self.replies.pop(ident)
        if error:
            die("command %d/%d failed with error %d" % (cmdset, cmd, error))
        return body

    def send_raw(self, cmdset, cmd, payload=b""):
        ident = self.next_id
        self.next_id += 1
        length = 11 + len(payload)
        packet = struct.pack(">IIBBB", length, ident, 0, cmdset, cmd) + payload
        self.sock.sendall(packet)
        deadline = time.time() + 20
        with self.cv:
            while ident not in self.replies:
                if not self.alive:
                    return -1, b""
                remaining = deadline - time.time()
                if remaining <= 0:
                    die("timeout waiting for reply to %d/%d" % (cmdset, cmd))
                self.cv.wait(remaining)
            error, body = self.replies.pop(ident)
        return error, body

    def wait_event(self, kind, timeout=15):
        deadline = time.time() + timeout
        with self.cv:
            while True:
                for i, ev in enumerate(self.events):
                    if event_kinds(ev[2]).count(kind):
                        return self.events.pop(i)
                if not self.alive:
                    die("connection closed waiting for event %d" % kind)
                remaining = deadline - time.time()
                if remaining <= 0:
                    die("timeout waiting for event %d; have %s"
                        % (kind, [event_kinds(e[2]) for e in self.events]))
                self.cv.wait(remaining)




def u4(n):
    return struct.pack(">I", n)


def u8(n):
    return struct.pack(">Q", n)


def ustring(s):
    b = s.encode("utf-8")
    return u4(len(b)) + b


def read_str(buf, i):
    n = struct.unpack_from(">I", buf, i)[0]
    i += 4
    s = buf[i:i + n].decode("utf-8", "replace")
    return s, i + n


def event_kinds(body):
    if len(body) < 5:
        return []
    count = struct.unpack_from(">I", body, 1)[0]
    kinds = []
    i = 5
    for _ in range(count):
        if i >= len(body):
            break
        kinds.append(body[i])
        i += 1
    return kinds


def parse_events(body):
    """Return list of (kind, request_id, rest_index). rest starts after request id."""
    policy = body[0]
    count = struct.unpack_from(">I", body, 1)[0]
    i = 5
    out = []
    for _ in range(count):
        kind = body[i]
        rid = struct.unpack_from(">I", body, i + 1)[0]
        out.append((kind, rid, i + 5, policy))
        # We only need the first matching event's location; callers pass `i`.
        break
    return out


def u1(n):
    return bytes([n & 0xFF])


def tagged_int(n):
    return u1(ord("I")) + u4(n & 0xFFFFFFFF)


def read_value(buf, i):
    tag = buf[i]
    i += 1
    if tag in (ord("J"), ord("D")):
        bits = struct.unpack_from(">Q", buf, i)[0]
        return tag, bits, i + 8
    if tag in (ord("L"), ord("["), ord("s"), ord("t"), ord("g"), ord("l"), ord("c")):
        bits = struct.unpack_from(">Q", buf, i)[0]
        return tag, bits, i + 8
    if tag in (ord("C"), ord("S")):
        bits = struct.unpack_from(">H", buf, i)[0]
        return tag, bits, i + 2
    if tag in (ord("B"), ord("Z")):
        return tag, buf[i], i + 1
    if tag == ord("V"):
        return tag, 0, i
    bits = struct.unpack_from(">I", buf, i)[0]
    return tag, bits, i + 4


def method_map(conn, type_id):
    methods = conn.send(2, 5, u8(type_id))
    n = struct.unpack_from(">I", methods, 0)[0]
    i = 4
    found = {}
    for _ in range(n):
        mid = struct.unpack_from(">Q", methods, i)[0]
        i += 8
        name, i = read_str(methods, i)
        spec, i = read_str(methods, i)
        i += 4
        found[(name, spec)] = mid
    return found


def field_map(conn, type_id):
    fields = conn.send(2, 4, u8(type_id))
    n = struct.unpack_from(">I", fields, 0)[0]
    i = 4
    found = {}
    for _ in range(n):
        fid = struct.unpack_from(">Q", fields, i)[0]
        i += 8
        name, i = read_str(fields, i)
        spec, i = read_str(fields, i)
        i += 4
        found[(name, spec)] = fid
    return found


def event_method(events):
    return struct.unpack_from(">Q", events, 5 + 1 + 4 + 8 + 1 + 8)[0]


def event_index(events):
    return struct.unpack_from(">Q", events, 5 + 1 + 4 + 8 + 1 + 8 + 8)[0]


def event_thread(events):
    return struct.unpack_from(">Q", events, 5 + 1 + 4)[0]


def assert_caps(conn, where):
    cap = conn.send(1, 12)
    if len(cap) != 7 or cap[0] != 1 or cap[1] != 1:
        die("%s Capabilities %r" % (where, cap))
    newer = conn.send(1, 17)
    if len(newer) != 32:
        die("%s CapabilitiesNew length %s" % (where, len(newer)))
    for i in (0, 1, 7, 10, 15, 16, 20):
        if newer[i] != 1:
            die("%s CapabilitiesNew[%s]=%s" % (where, i, newer[i]))
    # canGetBytecodes is not implemented; the bit must stay off.
    if newer[2] != 0:
        die("%s CapabilitiesNew[2] unexpectedly set" % where)


def classes_contain(buf, needle):
    n = struct.unpack_from(">I", buf, 0)[0]
    i = 4
    for _ in range(n):
        i += 1 + 8
        sig, i = read_str(buf, i)
        i += 4
        if needle in sig:
            return True
    return False


def bad_redefine_bytes():
    import tempfile
    javac = os.path.join(os.environ.get("JAVA_HOME", ""), "bin", "javac")
    if not os.path.exists(javac):
        javac = "javac"
    tmp = tempfile.mkdtemp(prefix="jdwp-bad-")
    src = os.path.join(tmp, "JdwpDebug.java")
    with open(src, "w") as f:
        f.write("public class JdwpDebug { public static void main(String[] a) {} }\n")
    subprocess.check_call(
        [javac, "-g", "-source", "1.8", "-target", "1.8", "-d", tmp, src])
    with open(os.path.join(tmp, "JdwpDebug.class"), "rb") as f:
        return f.read()


def redefine_bytes():
    import tempfile
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "redefine", "JdwpDebug.java")
    javac = os.path.join(os.environ.get("JAVA_HOME", ""), "bin", "javac")
    if not os.path.exists(javac):
        javac = "javac"
    tmp = tempfile.mkdtemp(prefix="jdwp-redef-")
    subprocess.check_call(
        [javac, "-g", "-source", "1.8", "-target", "1.8", "-d", tmp, src])
    with open(os.path.join(tmp, "JdwpDebug.class"), "rb") as f:
        return f.read()


def main():
    if len(sys.argv) not in (3, 4):
        die("usage: jdwp-test.py <avian> <classpath> [compile|interpret]")
    avian, classpath = sys.argv[1], sys.argv[2]
    process = sys.argv[3] if len(sys.argv) == 4 else "compile"
    global PROC
    proc = subprocess.Popen(
        [avian, "-cp", classpath,
         "-Xrunjdwp:transport=dt_socket,server=y,suspend=y,address=0",
         "JdwpDebug"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    PROC = proc
    port = None
    err = b""
    deadline = time.time() + 30
    while time.time() < deadline:
        ready, _, _ = select.select([proc.stderr], [], [], 0.2)
        if ready:
            chunk = os.read(proc.stderr.fileno(), 4096)
            if not chunk:
                break
            err += chunk
            with open("/tmp/jdwp-child.err","ab") as ef:
                ef.write(chunk)
            for line in err.splitlines():
                if line.startswith(b"Listening for transport dt_socket at address:"):
                    port = int(line.rsplit(b":", 1)[1].strip())
                    break
        if port is not None:
            break
        if proc.poll() is not None:
            break
    if port is None:
        out, _ = proc.communicate() if proc.poll() is not None else (b"", b"")
        die("no listen line; stderr=%r stdout=%r" % (err, out))

    sock = socket.create_connection(("127.0.0.1", port), 10)
    sock.sendall(HANDSHAKE)
    got = b""
    while len(got) < len(HANDSHAKE):
        chunk = sock.recv(len(HANDSHAKE) - len(got))
        if not chunk:
            die("handshake closed")
        got += chunk
    if got != HANDSHAKE:
        die("bad handshake %r" % got)

    conn = Conn(sock)
    # VMStart may already be queued.
    version = conn.send(1, 1)
    desc, i = read_str(version, 0)
    major, minor = struct.unpack_from(">II", version, i)
    if major != 1 or minor < 6:
        die("unexpected version %s %s.%s" % (desc, major, minor))
    sizes = conn.send(1, 7)
    if sizes != struct.pack(">IIIII", 8, 8, 8, 8, 8):
        die("unexpected id sizes %r" % sizes)
    assert_caps(conn, "server=y")
    err, _ = conn.send_raw(1, 99)
    if err != 99:
        die("unknown command error %s" % err)

    # ClassPrepare for JdwpDebug, suspend all.
    body = bytes([8, 2]) + u4(1) + bytes([5]) + ustring("JdwpDebug")
    req = conn.send(15, 1, body)
    (prepare_id,) = struct.unpack(">I", req)

    conn.send(1, 9)  # VM.Resume
    ev = conn.wait_event(8)
    events = ev[2]
    # Find the ClassPrepare event and its type id.
    count = struct.unpack_from(">I", events, 1)[0]
    i = 5
    type_id = None
    for _ in range(count):
        kind = events[i]
        rid = struct.unpack_from(">I", events, i + 1)[0]
        i += 5
        if kind == 8:
            i += 8  # thread
            i += 1  # tag
            type_id = struct.unpack_from(">Q", events, i)[0]
            i += 8
            sig, i = read_str(events, i)
            status = struct.unpack_from(">I", events, i)[0]
            i += 4
            if rid != prepare_id:
                die("ClassPrepare request id %s != %s" % (rid, prepare_id))
            if "JdwpDebug" not in sig:
                die("unexpected class signature %s" % sig)
            break
        else:
            die("unexpected event %d before ClassPrepare" % kind)
    if type_id is None:
        die("no ClassPrepare")

    classes = conn.send(1, 3)
    if not classes_contain(classes, "LJdwpDebug;"):
        die("AllClasses missing JdwpDebug")
    bysig = conn.send(1, 2, ustring("LJdwpDebug;"))
    bn = struct.unpack_from(">I", bysig, 0)[0]
    if bn < 1 or struct.unpack_from(">Q", bysig, 5)[0] != type_id:
        die("ClassesBySignature %r" % bysig)
    sig = conn.send(2, 1, u8(type_id))
    jni, _ = read_str(sig, 0)
    if jni != "LJdwpDebug;":
        die("Signature %s" % jni)
    src, _ = read_str(conn.send(2, 7, u8(type_id)), 0)
    if src != "JdwpDebug.java":
        die("SourceFile %s" % src)
    err, _ = conn.send_raw(2, 12, u8(type_id))
    if err != 99:
        die("unimplemented ReferenceType command error %s" % err)

    methods = conn.send(2, 5, u8(type_id))
    n = struct.unpack_from(">I", methods, 0)[0]
    i = 4
    marker = None
    for _ in range(n):
        mid = struct.unpack_from(">Q", methods, i)[0]
        i += 8
        name, i = read_str(methods, i)
        spec, i = read_str(methods, i)
        flags = struct.unpack_from(">I", methods, i)[0]
        i += 4
        if name == "marker" and spec == "(I)I":
            marker = mid
            if (flags & 0x8) == 0:
                die("marker is not static, flags %s" % flags)
    if marker is None:
        die("marker method not found")
    lines = conn.send(6, 1, u8(type_id) + u8(marker))
    start, end, nlines = struct.unpack_from(">QQI", lines, 0)
    if start != 0 or end < start or nlines < 1:
        die("LineTable start %s end %s lines %s" % (start, end, nlines))
    line_no = struct.unpack_from(">I", lines, 8 + 8 + 4 + 8)[0]
    if line_no < 1:
        die("LineTable line %s" % line_no)

    # Breakpoint at bci 0, suspend all. Modifier kind 7 is Location.
    loc = bytes([1]) + u8(type_id) + u8(marker) + u8(0)
    body = bytes([2, 2]) + u4(1) + bytes([7]) + loc
    conn.send(15, 1, body)
    conn.send(1, 9)  # resume from ClassPrepare

    ev = conn.wait_event(2)
    events = ev[2]
    kind = events[5]
    if kind != 2:
        die("expected breakpoint, got %d" % kind)
    # kind(1) rid(4) thread(8) tag(1) class(8) method(8) index(8)
    base = 5
    method_id = struct.unpack_from(">Q", events, base + 1 + 4 + 8 + 1 + 8)[0]
    index = struct.unpack_from(">Q", events, base + 1 + 4 + 8 + 1 + 8 + 8)[0]
    if method_id != marker or index != 0:
        die("breakpoint location method %s index %s" % (method_id, index))

    threads = conn.send(1, 4)
    tn = struct.unpack_from(">I", threads, 0)[0]
    if tn < 1:
        die("no threads")
    main_id = struct.unpack_from(">Q", threads, 4)[0]
    # The root thread is not necessarily first. Prefer a thread that has frames.
    frames = None
    chosen = None
    for k in range(tn):
        tid = struct.unpack_from(">Q", threads, 4 + 8 * k)[0]
        try:
            # FrameCount
            fc = conn.send(11, 7, u8(tid))
        except SystemExit:
            raise
        count = struct.unpack(">I", fc)[0]
        if count:
            fr = conn.send(11, 6, u8(tid) + u4(0) + u4(1))
            frames = fr
            chosen = tid
            break
    if frames is None:
        die("no suspended thread with frames (threads=%d)" % tn)
    fn = struct.unpack_from(">I", frames, 0)[0]
    if fn < 1:
        die("empty frames")
    # frameId(8) tag(1) class(8) method(8) index(8)
    top_method = struct.unpack_from(">Q", frames, 4 + 8 + 1 + 8)[0]
    top_index = struct.unpack_from(">Q", frames, 4 + 8 + 1 + 8 + 8)[0]
    if top_method != marker or top_index != 0:
        die("top frame method %s index %s, wanted marker@0" % (top_method, top_index))

    # Single step, min size, into, count 1, this thread, suspend all.
    step = u8(chosen) + u4(0) + u4(0)
    body = bytes([1, 2]) + u4(2) + bytes([10]) + step + bytes([1]) + u4(1)
    conn.send(15, 1, body)
    conn.send(1, 9)
    ev = conn.wait_event(1)
    events = ev[2]
    if events[5] != 1:
        die("expected single step")
    step_index = struct.unpack_from(">Q", events, 5 + 1 + 4 + 8 + 1 + 8 + 8)[0]
    if step_index == 0:
        die("single step did not advance")

    # Variable table and the argument local, while stopped in marker.
    vtab = conn.send(6, 2, u8(type_id) + u8(marker))
    arg_count, slots = struct.unpack_from(">II", vtab, 0)
    if arg_count < 1 or slots < 1:
        die("variable table argCnt %s slots %s" % (arg_count, slots))
    i = 8
    names = []
    for _ in range(slots):
        i += 8  # code index
        name, i = read_str(vtab, i)
        sig, i = read_str(vtab, i)
        length, slot = struct.unpack_from(">II", vtab, i)
        i += 8
        names.append((name, sig, slot, length))
    if not any(n == "x" and sig == "I" and slot == 0 for n, sig, slot, length in names):
        die("marker locals missing x: %s" % names)
    vgen = conn.send(6, 5, u8(type_id) + u8(marker))
    gi = 8
    gen_x = None
    for _ in range(struct.unpack_from(">I", vgen, 4)[0]):
        gi += 8
        gname, gi = read_str(vgen, gi)
        gsig, gi = read_str(vgen, gi)
        ggen, gi = read_str(vgen, gi)
        gi += 8
        if gname == "x" and gsig == "I":
            gen_x = ggen
    if gen_x != "":
        die("local generic signature %r" % gen_x)
    frame_id = struct.unpack_from(">Q", frames, 4)[0]
    got = conn.send(16, 1, u8(chosen) + u8(frame_id) + u4(1) + u4(0) + u1(ord("I")))
    nvals = struct.unpack_from(">I", got, 0)[0]
    tag, bits, _ = read_value(got, 4)
    if nvals != 1 or tag != ord("I") or bits != 41:
        die("GetValues x got n=%s tag=%s bits=%s" % (nvals, tag, bits))
    conn.send(16, 2, u8(chosen) + u8(frame_id) + u4(1) + u4(0) + tagged_int(99))
    got = conn.send(16, 1, u8(chosen) + u8(frame_id) + u4(1) + u4(0) + u1(ord("I")))
    tag, bits, _ = read_value(got, 4)
    if tag != ord("I") or bits != 99:
        die("SetValues did not stick tag=%s bits=%s" % (tag, bits))
    conn.send(16, 2, u8(chosen) + u8(frame_id) + u4(1) + u4(0) + tagged_int(41))
    this_obj = conn.send(16, 3, u8(chosen) + u8(frame_id))
    if this_obj[0] != ord("L") or struct.unpack_from(">Q", this_obj, 1)[0] != 0:
        die("static ThisObject %r" % this_obj)

    def step_req(depth):
        step = u8(chosen) + u4(0) + u4(depth)
        conn.send(15, 1, bytes([1, 2]) + u4(2) + bytes([10]) + step + bytes([1]) + u4(1))
        conn.send(1, 9)
        ev = conn.wait_event(1)
        if ev[2][5] != 1:
            die("expected step depth %s" % depth)
        return event_method(ev[2]), event_index(ev[2])

    over_method, over_index = step_req(1)
    if over_method != marker or over_index == step_index:
        die("step over method %s index %s (into was %s)" % (over_method, over_index, step_index))
    out_method, out_index = step_req(2)
    main_m = None
    # resolved below once the method map exists; checked immediately after
    stepped_out = (out_method, out_index)

    methods = method_map(conn, type_id)
    fields = field_map(conn, type_id)
    plus = methods.get(("plus", "(I)I"))
    bump = methods.get(("bump", "()V"))
    leaf = methods.get(("leaf", "()V"))
    early = methods.get(("early", "()I"))
    fail = methods.get(("fail", "()I"))
    field_n = fields.get(("n", "I"))
    field_f = fields.get(("f", "F"))
    field_d = fields.get(("d", "D"))
    peek = methods.get(("peek", "()I"))
    main_id_m = methods.get(("main", "([Ljava/lang/String;)V"))
    if not plus or not bump or not leaf or not field_n or not field_f or not field_d or not early or not fail or not peek or not main_id_m:
        die("missing members %s" % (methods, ))
    if stepped_out[0] != main_id_m:
        die("step out landed in %s not main" % (stepped_out[0],))

    def loc(method, index):
        return u1(1) + u8(type_id) + u8(method) + u8(index)

    conn.send(15, 1, u1(2) + u1(2) + u4(1) + u1(7) + loc(bump, 0))
    conn.send(15, 1, u1(2) + u1(2) + u4(1) + u1(7) + loc(early, 0))
    conn.send(15, 1, u1(2) + u1(2) + u4(1) + u1(7) + loc(leaf, 0))
    # Field modification, this field only, suspend all.
    for fid in (field_n, field_f, field_d):
        conn.send(15, 1, u1(21) + u1(2) + u4(1) + u1(9) + u8(type_id) + u8(fid))
    # Exception, all types, caught and uncaught, only in JdwpDebug.
    conn.send(15, 1, u1(4) + u1(2) + u4(2) + u1(8) + u8(0) + u1(1) + u1(1)
              + u1(5) + ustring("JdwpDebug"))
    conn.send(15, 1, u1(6) + u1(0) + u4(0))  # ThreadStart, suspend none
    conn.send(15, 1, u1(7) + u1(0) + u4(0))  # ThreadDeath, suspend none
    # Monitor events, suspend none so they do not stall the script.
    for kind in (43, 44, 45, 46):
        conn.send(15, 1, u1(kind) + u1(0) + u4(0))
    conn.send(15, 1, u1(9) + u1(0) + u4(1) + u1(5) + ustring("Unloaded"))
    conn.send(15, 2, u1(8) + u4(prepare_id))  # clear ClassPrepare

    def mod_value(events, want_field):
        got_field = struct.unpack_from(">Q", events, 52)[0]
        if got_field != want_field:
            die("modification field %s wanted %s" % (got_field, want_field))
        i = 5 + 1 + 4 + 8 + 1 + 8 + 8 + 8 + 1 + 8 + 8 + 1 + 8
        tag, bits, _ = read_value(events, i)
        return tag, bits

    conn.send(1, 9)
    ev = conn.wait_event(2)
    events = ev[2]
    bump_method = struct.unpack_from(">Q", events, 5 + 1 + 4 + 8 + 1 + 8)[0]
    if bump_method != bump:
        die("expected bump breakpoint, method %s" % bump_method)
    threads = conn.send(1, 4)
    tn = struct.unpack_from(">I", threads, 0)[0]
    inst_frame = None
    inst_thread = None
    for k in range(tn):
        tid = struct.unpack_from(">Q", threads, 4 + 8 * k)[0]
        err, fc = conn.send_raw(11, 7, u8(tid))
        if err or len(fc) < 4:
            continue
        if struct.unpack(">I", fc)[0] == 0:
            continue
        fr = conn.send(11, 6, u8(tid) + u4(0) + u4(1))
        top = struct.unpack_from(">Q", fr, 4 + 8 + 1 + 8)[0]
        if top == bump:
            inst_frame = struct.unpack_from(">Q", fr, 4)[0]
            inst_thread = tid
            break
    if inst_frame is None:
        die("no bump frame")
    this_obj = conn.send(16, 3, u8(inst_thread) + u8(inst_frame))
    if this_obj[0] not in (ord("L"), ord("s"), ord("[")):
        die("instance ThisObject tag %s" % this_obj[0])
    this_id = struct.unpack_from(">Q", this_obj, 1)[0]
    if this_id == 0:
        die("instance ThisObject was null")
    ref = conn.send(9, 1, u8(this_id))
    if ref[0] != 1 or struct.unpack_from(">Q", ref, 1)[0] != type_id:
        die("ReferenceType %r" % ref)
    fvals = conn.send(9, 2, u8(this_id) + u4(1) + u8(field_n))
    tag, bits, _ = read_value(fvals, 4)
    if tag != ord("I") or bits != 0:
        die("field n before bump is %s/%s" % (tag, bits))
    insts = conn.send(2, 16, u8(type_id) + u4(0))
    icount = struct.unpack_from(">I", insts, 0)[0]
    if icount < 1:
        die("ReferenceType.Instances returned %s" % icount)
    refs = conn.send(9, 10, u8(this_id) + u4(0))
    rcount = struct.unpack_from(">I", refs, 0)[0]
    if rcount < 1:
        die("ReferringObjects returned 0")
    held = field_map(conn, type_id).get(("held", "LJdwpDebug;"))
    if not held:
        die("no static held")
    sval = conn.send(2, 6, u8(type_id) + u4(1) + u8(held))
    tag, bits, _ = read_value(sval, 4)
    if tag not in (ord("L"), ord("s")) or bits == 0:
        die("static held tag=%s bits=%s" % (tag, bits))
    inv = conn.send(9, 6, u8(this_id) + u8(inst_thread) + u8(type_id) + u8(peek) + u4(0) + u4(1))
    tag, bits, i = read_value(inv, 0)
    extag, exid, _ = read_value(inv, i)
    if tag != ord("I") or bits != 0 or exid != 0:
        die("peek() tag=%s bits=%s ex=%s" % (tag, bits, exid))
    err, _ = conn.send_raw(1, 14, u4(1) + u8(this_id) + u4(1))
    if err:
        die("DisposeObjects %s" % err)
    err, _ = conn.send_raw(9, 1, u8(this_id))
    if err != 20:
        die("disposed id ReferenceType error %s" % err)

    def expect_mod(fid, tag_want, bits_want, label):
        conn.send(1, 9)
        ev = conn.wait_event(21)
        tag, bits = mod_value(ev[2], fid)
        if tag != tag_want or bits != bits_want:
            die("%s modification tag=%s bits=%s want %s/%s" % (label, tag, bits, tag_want, bits_want))

    # After peek(), so that invoke's own getfield is not the access event.
    conn.send(15, 1, u1(20) + u1(2) + u4(1) + u1(9) + u8(type_id) + u8(field_n))
    conn.send(1, 9)
    ev = conn.wait_event(20)
    acc = ev[2]
    if event_method(acc) != bump:
        die("field access method %s wanted %s index %s" % (event_method(acc), bump, event_index(acc)))
    acc_field = struct.unpack_from(">Q", acc, 52)[0]
    if acc_field != field_n:
        die("field access id %s" % acc_field)
    expect_mod(field_n, ord("I"), 1, "int")
    expect_mod(field_f, ord("F"), struct.unpack(">I", struct.pack(">f", 1.5))[0], "float")
    expect_mod(field_d, ord("D"), struct.unpack(">Q", struct.pack(">d", 2.5))[0], "double")

    conn.send(1, 9)
    ev = conn.wait_event(4)
    events = ev[2]
    ex_thread = struct.unpack_from(">Q", events, 5 + 1 + 4)[0]
    throw_method = struct.unpack_from(">Q", events, 5 + 1 + 4 + 8 + 1 + 8)[0]
    throw_index = struct.unpack_from(">Q", events, 5 + 1 + 4 + 8 + 1 + 8 + 8)[0]
    boom = methods.get(("boom", "()V"))
    if throw_method != boom:
        die("exception location method %s wanted %s" % (throw_method, boom))
    # After the tagged exception comes the catch location.
    i = 5 + 1 + 4 + 8 + (1 + 8 + 8 + 8)
    _, _, i = read_value(events, i)
    catch_index = struct.unpack_from(">Q", events, i + 1 + 8 + 8)[0]
    if throw_index == catch_index:
        die("exception stopped at the handler index %s" % throw_index)
    # The suspended top frame is the throw, not the catch.
    fr = conn.send(11, 6, u8(ex_thread) + u4(0) + u4(1))
    top = struct.unpack_from(">Q", fr, 4 + 8 + 1 + 8)[0]
    top_index = struct.unpack_from(">Q", fr, 4 + 8 + 1 + 8 + 8)[0]
    if top != boom or top_index != throw_index:
        die("suspended frame method %s index %s, event %s" % (top, top_index, throw_index))

    conn.send(1, 9)
    started = conn.wait_event(6)[2]
    if event_thread(started) == 0:
        die("ThreadStart thread id is 0")
    died = conn.wait_event(7)[2]
    if event_thread(died) == 0:
        die("ThreadDeath thread id is 0")
    def check_monitor(kind, body):
        if body[5] != kind:
            die("monitor event %s" % body[5])
        if event_thread(body) == 0:
            die("monitor %s thread 0" % kind)
        otag, oid, oi = read_value(body, 5 + 1 + 4 + 8)
        if oid == 0:
            die("monitor %s object 0" % kind)
        return oi + 1 + 8 + 8 + 8

    for kind in (43, 44, 46):
        check_monitor(kind, conn.wait_event(kind)[2])
    # Thread.sleep also waits. The worker's mon.wait(30) must show up.
    saw_worker_wait = False
    for _ in range(6):
        body = conn.wait_event(45)[2]
        oi = check_monitor(45, body)
        timeout = struct.unpack_from(">Q", body, oi)[0]
        if timeout == 30:
            saw_worker_wait = True
            break
    if not saw_worker_wait:
        die("no monitor wait with timeout 30")
    unloaded = conn.wait_event(9)[2]
    usig, _ = read_str(unloaded, 5 + 1 + 4)
    if usig != "LUnloaded;":
        die("class unload signature %s" % usig)
    ev = conn.wait_event(2)
    events = ev[2]
    early_method = struct.unpack_from(">Q", events, 5 + 1 + 4 + 8 + 1 + 8)[0]
    if early_method != early:
        die("expected early breakpoint, method %s" % early_method)
    # ForceEarlyReturn of 9. The method's own return is 7.
    threads = conn.send(1, 4)
    tn = struct.unpack_from(">I", threads, 0)[0]
    early_thread = chosen
    for k in range(tn):
        tid = struct.unpack_from(">Q", threads, 4 + 8 * k)[0]
        err, fc = conn.send_raw(11, 7, u8(tid))
        if err or len(fc) < 4 or struct.unpack(">I", fc)[0] == 0:
            continue
        fr = conn.send(11, 6, u8(tid) + u4(0) + u4(1))
        top = struct.unpack_from(">Q", fr, 4 + 8 + 1 + 8)[0]
        if top == early:
            early_thread = tid
            break
    err, body = conn.send_raw(11, 14, u8(early_thread) + u1(ord("D")) + u8(0))
    if err != 34:
        die("ForceEarlyReturn type mismatch error %s" % err)
    err, body = conn.send_raw(11, 14, u8(early_thread) + tagged_int(9))
    if err:
        die("ForceEarlyReturn failed: %s" % err)

    # Generic signatures on the nested Box.
    found = conn.send(1, 2, ustring("LJdwpDebug$Box;"))
    n = struct.unpack_from(">I", found, 0)[0]
    if n < 1:
        die("Box not loaded")
    box_id = struct.unpack_from(">Q", found, 5)[0]
    sigg = conn.send(2, 13, u8(box_id))
    jni, i = read_str(sigg, 0)
    gen, i = read_str(sigg, i)
    # JVMS Signature attribute, not a source-level "Box<T>" string.
    if "Box" not in jni or not gen.startswith("<T:") or "Ljava/lang/Object;" not in gen:
        die("class generic %s / %s" % (jni, gen))
    meths = conn.send(2, 15, u8(box_id))
    mn = struct.unpack_from(">I", meths, 0)[0]
    i = 4
    got_get = None
    for _ in range(mn):
        i += 8
        name, i = read_str(meths, i)
        spec, i = read_str(meths, i)
        generic, i = read_str(meths, i)
        i += 4
        if name == "get":
            got_get = generic
    if got_get != "()TT;":
        die("method generic %r" % got_get)
    flds = conn.send(2, 14, u8(box_id))
    fn = struct.unpack_from(">I", flds, 0)[0]
    i = 4
    got_val = None
    for _ in range(fn):
        i += 8
        name, i = read_str(flds, i)
        spec, i = read_str(flds, i)
        generic, i = read_str(flds, i)
        i += 4
        if name == "value":
            got_val = generic
    if got_val != "TT;":
        die("field generic %r" % got_val)

    conn.send(1, 9)
    ev = conn.wait_event(2)
    events = ev[2]
    leaf_method = struct.unpack_from(">Q", events, 5 + 1 + 4 + 8 + 1 + 8)[0]
    if leaf_method != leaf:
        die("expected leaf breakpoint, method %s" % leaf_method)

    inv = conn.send(3, 3, u8(type_id) + u8(chosen) + u8(plus) + u4(1) + tagged_int(2) + u4(1))
    tag, bits, i = read_value(inv, 0)
    extag, exid, _ = read_value(inv, i)
    if tag != ord("I") or bits != 3 or exid != 0:
        die("plus(2) tag=%s bits=%s ex=%s" % (tag, bits, exid))

    inv = conn.send(3, 3, u8(type_id) + u8(chosen) + u8(fail) + u4(0) + u4(1))
    tag, bits, i = read_value(inv, 0)
    extag, exid, _ = read_value(inv, i)
    if exid == 0:
        die("fail() did not report an exception")
    fr = conn.send(11, 6, u8(chosen) + u4(0) + u4(1))
    top = struct.unpack_from(">Q", fr, 4 + 8 + 1 + 8)[0]
    if top != leaf:
        die("throwing invoke unwound the suspended frame, top %s" % top)

    bad = bad_redefine_bytes()
    err, body = conn.send_raw(1, 18, u4(1) + u8(type_id) + u4(len(bad)) + bad)
    if err != 64:
        die("schema-incompatible redefine error %s" % err)
    blob = redefine_bytes()
    err, body = conn.send_raw(1, 18, u4(1) + u8(type_id) + u4(len(blob)) + blob)
    if err:
        die("redefine failed %s" % err)
    inv = conn.send(3, 3, u8(type_id) + u8(chosen) + u8(plus) + u4(1) + tagged_int(2) + u4(1))
    tag, bits, i = read_value(inv, 0)
    extag, exid, _ = read_value(inv, i)
    if tag != ord("I") or bits != 12 or exid != 0:
        die("redefined plus(2) tag=%s bits=%s ex=%s" % (tag, bits, exid))

    threads = conn.send(1, 4)
    tn = struct.unpack_from(">I", threads, 0)[0]
    leaf_frame = None
    leaf_thread = chosen
    for k in range(tn):
        tid = struct.unpack_from(">Q", threads, 4 + 8 * k)[0]
        err, fc = conn.send_raw(11, 7, u8(tid))
        if err or len(fc) < 4 or struct.unpack(">I", fc)[0] == 0:
            continue
        fr = conn.send(11, 6, u8(tid) + u4(0) + u4(1))
        top = struct.unpack_from(">Q", fr, 4 + 8 + 1 + 8)[0]
        if top == leaf:
            leaf_frame = struct.unpack_from(">Q", fr, 4)[0]
            leaf_thread = tid
            break
    if leaf_frame is None:
        die("no leaf frame to pop")
    err, body = conn.send_raw(16, 4, u8(leaf_thread) + u8(0))
    if err != 30:
        die("PopFrames invalid frame error %s" % err)
    fr = conn.send(11, 6, u8(leaf_thread) + u4(0) + u4(2))
    fn = struct.unpack_from(">I", fr, 0)[0]
    if fn < 2:
        die("leaf has no caller frame (%s)" % fn)
    caller = struct.unpack_from(">Q", fr, 4 + (8 + 1 + 8 + 8 + 8))[0]
    err, body = conn.send_raw(16, 4, u8(leaf_thread) + u8(caller))
    if err != 33:
        die("PopFrames non-current frame error %s" % err)
    err, body = conn.send_raw(16, 4, u8(leaf_thread) + u8(leaf_frame))
    if err:
        die("PopFrames failed: %s" % err)

    conn.send(1, 9)  # run to completion; Dispose would suppress VMDeath
    death = conn.wait_event(99, timeout=20)
    if death[2][5] != 99:
        die("VMDeath kind %s" % death[2][5])
    sock.close()
    try:
        out, err2 = proc.communicate(timeout=20)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, err2 = proc.communicate()
        die("process hung; stdout=%r stderr=%r" % (out, err2))
    if proc.returncode != 0:
        die("exit %s stdout=%r stderr=%r" % (proc.returncode, out, err2))
    if b"result=42" not in out or b"early=9" not in out:
        die("missing result stdout=%r" % out)
    if b"leaf=0" not in out:
        die("PopFrames did not skip leaf stdout=%r" % out)
    run_client(avian, classpath)
    sys.stdout.write("jdwp-test: ok\n")


def run_client(avian, classpath):
    global PROC
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    port = srv.getsockname()[1]
    proc = subprocess.Popen(
        [avian, "-cp", classpath,
         "-Xrunjdwp:transport=dt_socket,server=n,suspend=y,address=127.0.0.1:%d" % port,
         "JdwpDebug"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    PROC = proc
    srv.settimeout(20)
    try:
        sock, _ = srv.accept()
    except socket.timeout:
        die("server=n did not connect")
    sock.sendall(HANDSHAKE)
    got = b""
    while len(got) < len(HANDSHAKE):
        chunk = sock.recv(len(HANDSHAKE) - len(got))
        if not chunk:
            die("client handshake closed")
        got += chunk
    if got != HANDSHAKE:
        die("bad client handshake %r" % got)
    conn = Conn(sock)
    version = conn.send(1, 1)
    desc, i = read_str(version, 0)
    major, minor = struct.unpack_from(">II", version, i)
    if not desc or major != 1 or minor < 6:
        die("client version %s %s.%s" % (desc, major, minor))
    sizes = conn.send(1, 7)
    if sizes != struct.pack(">IIIII", 8, 8, 8, 8, 8):
        die("client id sizes %r" % sizes)
    assert_caps(conn, "server=n")
    err, _ = conn.send_raw(1, 6)
    if err:
        die("Dispose error %s" % err)
    # Dispose resumes the VM. A further Resume races the process exit.
    sock.close()
    out, err2 = proc.communicate(timeout=20)
    if proc.returncode != 0:
        die("client exit %s stdout=%r stderr=%r" % (proc.returncode, out, err2))
    if b"result=42" not in out or b"early=7" not in out or b"leaf=1" not in out:
        die("client stdout=%r stderr=%r" % (out, err2))
    if b"Connecting to transport dt_socket" not in err2:
        die("client missing connect line %r" % err2)



if __name__ == "__main__":
    main()
