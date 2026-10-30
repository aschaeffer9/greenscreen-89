// Runs the relay's Worker + Durable Object code against a small mock of the
// Cloudflare runtime (WebSocketPair, hibernation API) to check pairing,
// relaying, hibernation survival and partner-left handling.
import assert from 'node:assert/strict';

class MockWS {
  constructor(name) { this.name = name; this.sent = []; this.att = null; this.closed = false; this.other = null; }
  send(d) { if (this.closed) throw new Error('closed'); this.other.sent.push(d); }
  close() { this.closed = true; }
  serializeAttachment(v) { this.att = JSON.parse(JSON.stringify(v)); }
  deserializeAttachment() { return this.att; }
}
let n = 0;
globalThis.WebSocketPair = class {
  constructor() {
    const c = new MockWS('client' + n), s = new MockWS('server' + n); n++;
    c.other = s; s.other = c;
    // what the server sends shows up in client.sent and vice versa
    this[0] = c; this[1] = s;
  }
};
globalThis.Response = class { constructor(body, init) { Object.assign(this, init); } };
if (!globalThis.crypto) globalThis.crypto = (await import('node:crypto')).webcrypto;

const mod = await import('../src/index.js');
const accepted = [];
const ctx = { acceptWebSocket(ws) { accepted.push(ws); }, getWebSockets() { return accepted.filter(w => !w.closed); } };
let lobby = new mod.Lobby(ctx, {});

const r1 = await lobby.fetch({});
const c1 = r1.webSocket, s1 = accepted[0];
assert.equal(r1.status, 101);
assert.deepEqual(c1.sent, ['wait']);
const r2 = await lobby.fetch({});
const c2 = r2.webSocket, s2 = accepted[1];
assert.deepEqual(c1.sent, ['wait', 'paired']);
assert.deepEqual(c2.sent, ['paired']);

// hibernation: a fresh object instance must still relay using the attachments
lobby = new mod.Lobby(ctx, {});
const pkt = new Uint8Array([84, 56, 57, 1, 68, 1, 0, 0, 0x55, 0, 0, 0]).buffer;
await lobby.webSocketMessage(s1, pkt);
assert.equal(c2.sent.at(-1), pkt);
await lobby.webSocketMessage(s2, pkt);
assert.equal(c1.sent.at(-1), pkt);
await lobby.webSocketMessage(s1, 'text is ignored');
assert.equal(c2.sent.length, 2);

// a third player waits on their own
const r3 = await lobby.fetch({});
assert.deepEqual(r3.webSocket.sent, ['wait']);

// partner leaves
await lobby.webSocketClose(s1, 1000, '');
assert.equal(c2.sent.at(-1), 'left');
assert.ok(s2.closed);

// the waiting third player pairs with a fourth
const r4 = await lobby.fetch({});
assert.deepEqual(r4.webSocket.sent, ['paired']);
assert.deepEqual(r3.webSocket.sent, ['wait', 'paired']);

// worker routing
const env = { LOBBY: { idFromName: (x) => x, get: (id) => ({ fetch: () => ({ routed: id }) }) } };
const req = (u, up = 'websocket') => ({ url: u, headers: { get: (h) => h === 'Upgrade' ? up : null } });
assert.deepEqual(await mod.default.fetch(req('https://x/link?room=Ari%27s'), env), { routed: 'aris' });
assert.deepEqual(await mod.default.fetch(req('https://x/link'), env), { routed: 'public' });
assert.equal((await mod.default.fetch(req('https://x/link', null), env)).status, 426);
assert.equal((await mod.default.fetch(req('https://x/nope'), env)).status, 404);
console.log('relay mock tests: all passed');
