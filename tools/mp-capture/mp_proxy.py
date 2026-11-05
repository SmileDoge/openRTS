#!/usr/bin/env python3
"""RTS multiplayer test proxy (docs/reverse/multiplayer.md).

Sits between rts1.exe clients and rts_server.exe, logs the packages and can
carry the helper (assistant driver) link through itself so that no player
needs an open port.

  observe  pass everything through, log it
  helper   the helper is sent to the proxy's relay instead of the engineer;
           the relay connects on to the engineer (the engineer still listens)
  both     the engineer is told to connect out to the relay as well (0x461F
           type 1 instead of type 2); the proxy answers the server for it
           (0x461F type 4); the relay joins the two connections by session

Clients connect to --listen (default 127.0.0.1:7800) instead of the server.

--dump FILE writes every frame as it arrived, whole, to a binary capture:
"MPDUMP1\0", then per frame f64 time (s), u8 kind (0 client->server,
1 server->client, 2 relay a->b, 3 relay b->a), u32 connection (the client
number, or the relay session), u32 n, n bytes (u16 id, u32 length, body;
the login reply has no length field). mp_dump.py reads it.
"""
import argparse
import socket
import struct
import sys
import threading
import time

LOG_LOCK = threading.Lock()
LOG_FILE = None
DUMP_FILE = None
DUMP_LOCK = threading.Lock()
START = time.time()


def dump(kind, conn, data):
    if DUMP_FILE is None:
        return
    with DUMP_LOCK:
        DUMP_FILE.write(struct.pack('<dBII', time.time() - START, kind, conn & 0xFFFFFFFF, len(data)))
        DUMP_FILE.write(data)
        DUMP_FILE.flush()


def log(*parts):
    line = '[%8.2f] %s' % (time.time() - START, ' '.join(str(p) for p in parts))
    with LOG_LOCK:
        print(line, flush=True)
        if LOG_FILE:
            LOG_FILE.write(line + '\n')
            LOG_FILE.flush()


def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        try:
            chunk = sock.recv(n - len(buf))
        except OSError:
            return None
        if not chunk:
            return None
        buf += chunk
    return bytes(buf)


def utf16z(data):
    return data.decode('utf-16-le', errors='replace').split('\x00', 1)[0]


INTERESTING = {0x4600, 0x4500, 0x4601, 0x4608, 0x461E, 0x461F, 0x4611, 0x4616, 0x4703, 0x1700, 0x1701}
NAMES = {
    0x4601: 'assign train', 0x4603: 'train position', 0x4608: 'startup data', 0x460A: 'junction',
    0x460C: 'signal', 0x460D: 'time', 0x461E: 'wait for engineer', 0x461F: 'helper link',
    0x4620: 'train physics', 0x462D: 'player position', 0x4611: 'dispatcher message',
    0x4616: 'server suspended', 0x1700: 'node hello',
}


class Counter:
    def __init__(self, label):
        self.label = label
        self.counts = {}
        self.shown = 0
        self.lock = threading.Lock()

    def add(self, pid, length):
        with self.lock:
            n, b = self.counts.get(pid, (0, 0))
            self.counts[pid] = (n + 1, b + length)
            self.shown += 1
            return self.shown

    def summary(self):
        with self.lock:
            items = sorted(self.counts.items())
            self.counts = {}
        if items:
            return '%s: %s' % (self.label, ' '.join('%04x x%d (%dB)' % (k, n, b) for k, (n, b) in items))
        return None


COUNTERS = []
COUNTERS_LOCK = threading.Lock()


def new_counter(label):
    c = Counter(label)
    with COUNTERS_LOCK:
        COUNTERS.append(c)
    return c


def stats_loop(period):
    while True:
        time.sleep(period)
        with COUNTERS_LOCK:
            counters = list(COUNTERS)
        for c in counters:
            s = c.summary()
            if s:
                log('  stats', s)


def note(counter, direction, who, pid, length, detail=''):
    shown = counter.add(pid, length)
    if pid in INTERESTING or shown <= 40:
        log('%s %s %04x len %d %s %s' % (who, direction, pid, length, NAMES.get(pid, ''), detail))


class Relay:
    """Joins helper links: by session, to the engineer (mode helper) or to the
    other side's outgoing connection (mode both)."""

    def __init__(self, mode, host, port):
        self.mode = mode
        self.host = host
        self.port = port
        self.targets = {}      # session -> (ip, port) of the engineer (mode helper)
        self.waiting = {}      # session -> (sock, hello) (mode both)
        # mode both: a later session of the same helper -> the session the
        # engineer was first told to connect with (it is told only once: a
        # second 0x461F type 1 makes rts1.exe drop its links, the new one too)
        self.alias = {}
        self.lock = threading.Lock()

    def serve(self):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((self.host, self.port))
        srv.listen(8)
        log('relay listening on %s:%d' % (self.host, self.port))
        while True:
            sock, addr = srv.accept()
            threading.Thread(target=self.handle, args=(sock, addr), daemon=True).start()

    def handle(self, sock, addr):
        hello = recv_exact(sock, 14)
        if not hello:
            log('relay: %s closed before hello' % (addr,))
            sock.close()
            return
        pid, length, node, session = struct.unpack('<HIII', hello)
        if pid != 0x1700 or length != 14:
            log('relay: %s sent %04x len %d instead of a node hello, closing' % (addr, pid, length))
            sock.close()
            return
        log('relay: hello from %s node %08x session %08x' % (addr, node, session))
        if self.mode == 'helper':
            with self.lock:
                target = self.targets.get(session)
            if not target:
                log('relay: no engineer known for session %08x' % session)
                sock.close()
                return
            try:
                peer = socket.create_connection(target, timeout=10)
                peer.settimeout(None)
            except OSError as e:
                log('relay: cannot reach the engineer at %s:%d: %s' % (target[0], target[1], e))
                sock.close()
                return
            peer.sendall(hello)
            log('relay: helper %s joined to engineer %s:%d' % (addr, target[0], target[1]))
            self.pipe(sock, peer, 'helper', 'engineer', session)
        else:
            with self.lock:
                key = self.alias.get(session, session)
                other = self.waiting.pop(key, None)
                if not other:
                    self.waiting[key] = (sock, hello)
            if key != session:
                log('relay: session %08x is the same pair as %08x' % (session, key))
            if not other:
                log('relay: waiting for the other side of session %08x' % key)
                threading.Thread(target=self.expire, args=(key, sock), daemon=True).start()
                return
            log('relay: both sides of session %08x connected, joining them' % key)
            self.pipe(other[0], sock, 'first', 'second', key)

    def expire(self, session, sock):
        time.sleep(300)
        with self.lock:
            entry = self.waiting.get(session)
            if entry and entry[0] is sock:
                del self.waiting[session]
            else:
                return
        log('relay: nobody joined session %08x in 300 s, closing' % session)
        sock.close()

    def pipe(self, a, b, name_a, name_b, session):
        counter = new_counter('relay %08x' % session)

        def run(src, dst, label):
            kind = 2 if label == '%s->%s' % (name_a, name_b) else 3
            while True:
                hdr = recv_exact(src, 6)
                if not hdr:
                    break
                pid, length = struct.unpack('<HI', hdr)
                if length < 6 or length > 0x1388000:
                    log('relay %s: bad frame %04x len %d, passing the rest raw' % (label, pid, length))
                    dst.sendall(hdr)
                    while True:
                        data = src.recv(65536)
                        if not data:
                            break
                        dst.sendall(data)
                    break
                body = recv_exact(src, length - 6) if length > 6 else b''
                if body is None:
                    break
                dump(kind, session, hdr + body)
                note(counter, label, 'relay', pid, length)
                try:
                    dst.sendall(hdr + body)
                except OSError:
                    break
            log('relay %s closed (session %08x)' % (label, session))
            for s in (src, dst):
                try:
                    s.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

        threading.Thread(target=run, args=(a, b, '%s->%s' % (name_a, name_b)), daemon=True).start()
        threading.Thread(target=run, args=(b, a, '%s->%s' % (name_b, name_a)), daemon=True).start()


class Session:
    count = 0

    def __init__(self, client, addr, server_addr, relay, mode):
        Session.count += 1
        self.no = Session.count
        self.c = client
        self.addr = addr
        self.relay = relay
        self.mode = mode
        self.nick = '?'
        self.train = 0
        # A client always logs in first, so the server's first bytes are the
        # login reply (no length field) or a 2-byte error code.
        self.expect_reply = True
        self.server_lock = threading.Lock()
        self.helper_sessions = {}   # helper id -> the session this engineer was told to use
        self.s = socket.create_connection(server_addr)
        self.up = new_counter('#%d client->server' % self.no)
        self.down = new_counter('#%d server->client' % self.no)
        self.relay_ip = client.getsockname()[0]

    @property
    def who(self):
        return '#%d %s' % (self.no, self.nick)

    def start(self):
        threading.Thread(target=self.client_to_server, daemon=True).start()
        threading.Thread(target=self.server_to_client, daemon=True).start()

    def to_server(self, data):
        with self.server_lock:
            self.s.sendall(data)

    def close(self):
        for s in (self.c, self.s):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def client_to_server(self):
        first = True
        while True:
            hdr = recv_exact(self.c, 6)
            if not hdr:
                break
            pid, length = struct.unpack('<HI', hdr)
            if length < 6 or length > 0x1388000:
                log('%s client->server bad frame %04x len %d, closing' % (self.who, pid, length))
                break
            body = recv_exact(self.c, length - 6) if length > 6 else b''
            if body is None:
                break
            detail = ''
            if first and pid in (0x4600, 0x4500) and len(body) >= 4:
                version, n = struct.unpack_from('<HH', body)
                self.nick = utf16z(body[4:4 + 2 * n + 2])
                detail = '%s login, version %d, nick %s' % ('player' if pid == 0x4600 else 'dispatcher', version, self.nick)
            elif pid == 0x461F and len(body) >= 2:
                detail = 'subtype %d %s' % (struct.unpack_from('<H', body)[0], body[2:].hex())
            first = False
            dump(0, self.no, hdr + body)
            note(self.up, 'client->server', self.who, pid, length, detail)
            try:
                self.to_server(hdr + body)
            except OSError:
                break
        log('%s client closed' % self.who)
        self.close()

    def read_login_reply(self):
        head = recv_exact(self.s, 2)
        if not head:
            return None
        pid, = struct.unpack('<H', head)
        if pid != 0x4600:
            log('%s login refused, code %04x' % (self.who, pid))
            return head
        parts = [head]
        version = recv_exact(self.s, 2)
        n_raw = recv_exact(self.s, 2)
        if not version or not n_raw:
            return None
        n, = struct.unpack('<H', n_raw)
        text = recv_exact(self.s, 2 * n + 2)
        flags = recv_exact(self.s, 2)
        m_raw = recv_exact(self.s, 2)
        if text is None or flags is None or m_raw is None:
            return None
        m, = struct.unpack('<H', m_raw)
        route = recv_exact(self.s, 2 * m + 2)
        counts = recv_exact(self.s, 16)
        if route is None or counts is None:
            return None
        log('%s logged in: version %d, route %s, season %d weather %d, counts %s; %s' % (
            self.who, struct.unpack('<H', version)[0], utf16z(route), flags[0], flags[1],
            struct.unpack('<4I', counts), utf16z(text).replace('\r\n', ' | ')))
        reply = b''.join(parts + [version, n_raw, text, flags, m_raw, route, counts])
        dump(1, self.no, reply)
        return reply

    def server_to_client(self):
        while True:
            if self.expect_reply:
                reply = self.read_login_reply()
                self.expect_reply = False
                if reply is None:
                    break
                try:
                    self.c.sendall(reply)
                except OSError:
                    break
                continue
            hdr = recv_exact(self.s, 6)
            if not hdr:
                break
            pid, length = struct.unpack('<HI', hdr)
            if length < 6 or length > 0x1388000:
                log('%s server->client bad frame %04x len %d, passing the rest raw' % (self.who, pid, length))
                self.c.sendall(hdr)
                while True:
                    data = self.s.recv(65536)
                    if not data:
                        break
                    self.c.sendall(data)
                break
            body = recv_exact(self.s, length - 6) if length > 6 else b''
            if body is None:
                break
            dump(1, self.no, hdr + body)
            detail = ''
            if pid == 0x4601 and len(body) >= 4:
                self.train, = struct.unpack_from('<I', body)
                detail = 'train id %d' % self.train
            elif pid == 0x461F and len(body) >= 2:
                body, detail = self.helper_link(body)
            note(self.down, 'server->client', self.who, pid, length, detail)
            if body is None:
                continue
            out = struct.pack('<HI', pid, 6 + len(body)) + body
            try:
                self.c.sendall(out)
            except OSError:
                break
        log('%s server closed' % self.who)
        self.close()

    def helper_link(self, body):
        """0x461F from the server: 1 to the helper, 2 to the engineer."""
        sub, = struct.unpack_from('<H', body)
        if sub == 1 and len(body) >= 44:
            name = utf16z(body[2:34])
            ip = socket.inet_ntoa(body[34:38])
            port, session = struct.unpack_from('<HI', body, 38)
            detail = 'type 1 (to helper): engineer %s at %s:%d session %08x' % (name, ip, port, session)
            if self.mode in ('helper', 'both'):
                with self.relay.lock:
                    self.relay.targets[session] = (ip, port)
                body = body[:34] + socket.inet_aton(self.relay_ip) + struct.pack('<H', self.relay.port) + body[40:]
                detail += ' -> sent to the relay %s:%d' % (self.relay_ip, self.relay.port)
            return body, detail
        if sub == 2 and len(body) >= 47:
            add = body[2]
            helper_id, = struct.unpack_from('<I', body, 3)
            name_raw = body[7:39]
            v131, session = struct.unpack_from('<II', body, 39)
            detail = 'type 2 (to engineer): %s helper %s id %d session %08x (%d)' % (
                'add' if add else 'remove', utf16z(name_raw), helper_id, session, v131)
            if self.mode == 'both' and add:
                answer = struct.pack('<HIHIHII', 0x461F, 6 + 16, 4, helper_id, self.relay.port, session, self.train)
                try:
                    self.to_server(answer)
                except OSError:
                    pass
                first = self.helper_sessions.get(helper_id)
                if first is not None:
                    with self.relay.lock:
                        self.relay.alias[session] = first
                    detail += ' -> same helper again: kept the engineer on session %08x, answered the server (port %d)' % (
                        first, self.relay.port)
                    return None, detail
                self.helper_sessions[helper_id] = session
                fake = (struct.pack('<H', 1) + name_raw + socket.inet_aton(self.relay_ip)
                        + struct.pack('<HIIIH', self.relay.port, session, 0, 0, 0))
                detail += ' -> engineer told to connect out to %s:%d; answered the server for it (port %d, train %d)' % (
                    self.relay_ip, self.relay.port, self.relay.port, self.train)
                return fake, detail
            return body, detail
        return body, 'subtype %d %s' % (sub, body[2:].hex())


def main():
    global LOG_FILE
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--mode', choices=('observe', 'helper', 'both'), default='observe')
    ap.add_argument('--listen', default='127.0.0.1:7800', help='where the clients connect')
    ap.add_argument('--server', default='127.0.0.1:800', help='rts_server.exe')
    ap.add_argument('--relay', default='127.0.0.1:7801', help='the helper link relay')
    ap.add_argument('--log', default='mp_proxy.log')
    ap.add_argument('--stats', type=float, default=15.0, help='seconds between package counts')
    ap.add_argument('--dump', help='write every frame, whole, to this binary capture (read with mp_dump.py)')
    a = ap.parse_args()
    LOG_FILE = open(a.log, 'a', encoding='utf-8')
    if a.dump:
        global DUMP_FILE
        DUMP_FILE = open(a.dump, 'wb')
        DUMP_FILE.write(b'MPDUMP1\0')
    lh, lp = a.listen.rsplit(':', 1)
    sh, sp = a.server.rsplit(':', 1)
    rh, rp = a.relay.rsplit(':', 1)
    relay = Relay(a.mode, rh, int(rp))
    if a.mode != 'observe':
        threading.Thread(target=relay.serve, daemon=True).start()
    threading.Thread(target=stats_loop, args=(a.stats,), daemon=True).start()
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((lh, int(lp)))
    srv.listen(16)
    log('mode %s; clients connect to %s:%s, server %s:%s' % (a.mode, lh, lp, sh, sp))
    while True:
        c, addr = srv.accept()
        try:
            session = Session(c, addr, (sh, int(sp)), relay, a.mode)
        except OSError as e:
            log('client %s: cannot reach the server: %s' % (addr, e))
            c.close()
            continue
        log('#%d client connected from %s:%d' % (session.no, addr[0], addr[1]))
        session.start()


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
