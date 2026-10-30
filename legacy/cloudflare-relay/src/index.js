// Tetris 89 link relay.
// Two phones open a WebSocket to /link?room=<name>. The first one waits; when
// a second arrives they are paired and every binary message from one is
// passed to the other - the virtual Game Boy link cable runs on top of that.
// Text messages to the phone: "wait" (no partner yet), "paired", "left".

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === '/') {
      return new Response('Tetris 89 link relay is running.\n', { headers: { 'content-type': 'text/plain' } });
    }
    if (url.pathname !== '/link') return new Response('not found', { status: 404 });
    if (request.headers.get('Upgrade') !== 'websocket') return new Response('expected websocket', { status: 426 });
    const room = (url.searchParams.get('room') || 'public').toLowerCase().replace(/[^a-z0-9-]/g, '').slice(0, 32) || 'public';
    const id = env.LOBBY.idFromName(room);
    return env.LOBBY.get(id).fetch(request);
  },
};

export class Lobby {
  constructor(ctx, env) {
    this.ctx = ctx;
    this.env = env;
  }

  // sockets survive hibernation; their pairing lives in their attachments
  info(ws) { return ws.deserializeAttachment() || {}; }

  peerOf(ws) {
    const me = this.info(ws);
    if (me.state !== 'paired') return null;
    for (const other of this.ctx.getWebSockets()) {
      if (other === ws) continue;
      const o = this.info(other);
      if (o.state === 'paired' && o.pair === me.pair) return other;
    }
    return null;
  }

  async fetch(request) {
    const pair = new WebSocketPair();
    const [client, server] = [pair[0], pair[1]];
    this.ctx.acceptWebSocket(server);

    let partner = null;
    for (const ws of this.ctx.getWebSockets()) {
      if (ws === server) continue;
      if (this.info(ws).state === 'waiting') { partner = ws; break; }
    }
    if (partner) {
      const pairId = crypto.randomUUID();
      partner.serializeAttachment({ state: 'paired', pair: pairId });
      server.serializeAttachment({ state: 'paired', pair: pairId });
      safeSend(partner, 'paired');
      safeSend(server, 'paired');
    } else {
      server.serializeAttachment({ state: 'waiting' });
      safeSend(server, 'wait');
    }
    return new Response(null, { status: 101, webSocket: client });
  }

  async webSocketMessage(ws, message) {
    if (typeof message === 'string') return;          // phones only send binary
    const peer = this.peerOf(ws);
    if (peer) safeSend(peer, message);
  }

  async webSocketClose(ws, code, reason) { this.drop(ws); }
  async webSocketError(ws, err) { this.drop(ws); }

  drop(ws) {
    const peer = this.peerOf(ws);
    ws.serializeAttachment({ state: 'gone' });
    if (peer) {
      peer.serializeAttachment({ state: 'gone' });
      safeSend(peer, 'left');
      try { peer.close(1000, 'partner left'); } catch (e) {}
    }
    try { ws.close(1000, 'bye'); } catch (e) {}
  }
}

function safeSend(ws, data) {
  try { ws.send(data); } catch (e) {}
}
