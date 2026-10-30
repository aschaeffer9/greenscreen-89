"""Tiny MQTT 3.1.1 broker over TLS (QoS 0 only, exact-topic subscriptions)
for testing the phone's matchmaking client. usage: broker.py PORT CERT KEY [delay_ms]"""
import asyncio, ssl, sys

subs = {}            # topic -> set(writer)
DELAY = (int(sys.argv[4]) if len(sys.argv) > 4 else 0) / 1000.0
stats = {'pub': 0}


def enc_len(n):
    out = bytearray()
    while True:
        b = n & 127; n >>= 7
        out.append(b | (128 if n else 0))
        if not n: return bytes(out)


async def read_pkt(r):
    h = (await r.readexactly(1))[0]
    n, mul = 0, 1
    while True:
        b = (await r.readexactly(1))[0]
        n += (b & 127) * mul; mul *= 128
        if not b & 128: break
    return h, await r.readexactly(n)


async def deliver(w, pkt):
    if DELAY: await asyncio.sleep(DELAY)
    try:
        w.write(pkt); await w.drain()
    except Exception:
        pass


def publish(topic, payload):
    body = len(topic.encode()).to_bytes(2, 'big') + topic.encode() + payload
    pkt = bytes([0x30]) + enc_len(len(body)) + body
    for o in list(subs.get(topic, ())):
        asyncio.ensure_future(deliver(o, pkt))


async def client(r, w):
    mine = []
    will = [None]
    try:
        h, body = await read_pkt(r)
        if h != 0x10 or body[2:6] != b'MQTT': return
        flags = body[7]; i = 10
        n = body[i] << 8 | body[i + 1]; i += 2 + n           # client id
        if flags & 0x04:                                       # last will
            n = body[i] << 8 | body[i + 1]; will_topic = body[i + 2:i + 2 + n].decode(); i += 2 + n
            n = body[i] << 8 | body[i + 1]; will_msg = body[i + 2:i + 2 + n]; i += 2 + n
            will[0] = (will_topic, will_msg)
        w.write(bytes([0x20, 2, 0, 0])); await w.drain()
        while True:
            h, body = await read_pkt(r)
            t = h & 0xF0
            if t == 0x80:                       # SUBSCRIBE
                pid = body[:2]; i = 2; rcs = bytearray()
                while i < len(body):
                    n = body[i] << 8 | body[i + 1]; topic = body[i + 2:i + 2 + n].decode(); i += 3 + n
                    subs.setdefault(topic, set()).add(w); mine.append(topic); rcs.append(0)
                w.write(bytes([0x90]) + enc_len(2 + len(rcs)) + pid + bytes(rcs)); await w.drain()
            elif t == 0x30:                     # PUBLISH (QoS 0)
                n = body[0] << 8 | body[1]; topic = body[2:2 + n].decode()
                stats['pub'] += 1
                pkt = bytes([0x30]) + enc_len(len(body)) + body
                for o in list(subs.get(topic, ())):
                    asyncio.ensure_future(deliver(o, pkt))
            elif t == 0xC0:
                w.write(bytes([0xD0, 0])); await w.drain()
            elif t == 0xE0:
                will[0] = None                  # clean goodbye: no will
                return
    except (asyncio.IncompleteReadError, ConnectionError):
        pass
    finally:
        for t in mine: subs.get(t, set()).discard(w)
        if will[0]: publish(*will[0])
        w.close()


async def main():
    ctx = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
    ctx.load_cert_chain(sys.argv[2], sys.argv[3])
    srv = await asyncio.start_server(client, '127.0.0.1', int(sys.argv[1]), ssl=ctx)
    print('broker up', flush=True)
    async with srv:
        await srv.serve_forever()

asyncio.run(main())
