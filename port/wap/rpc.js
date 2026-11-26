function installQuotaRpc(writePacket, readPacket, registryHost = false) {
  const pollMs = 16;
  const handlers = new Map();
  const pending = new Map();
  // The Node endpoint serializes claims for this instance's two endpoints.
  const owners = registryHost ? new Set() : null;
  let next = 0;
  let closed = false;

  function validate(name) {
    if (typeof name !== 'string' || !name.length) throw new TypeError('Expected RPC method name');
    if (name === 'set' || name === 'get') throw new Error('Reserved RPC method: ' + name);
  }

  function duplicate(name) { return new Error('RPC method already registered: ' + name); }

  function claim(name) {
    validate(name);
    if (owners.has(name)) throw duplicate(name);
    owners.add(name);
  }

  function encode(message) {
    const wire = JSON.stringify(message);
    if (encodeURIComponent(wire).replace(/%[0-9A-F]{2}/gi, 'x').length > 1024)
      throw new RangeError('RPC message exceeds 1024 bytes');
    return wire;
  }

  async function send(wire) {
    while (!closed) {
      if (writePacket(wire)) return;
      await new Promise(resolve => setTimeout(resolve, pollMs));
    }
    throw new Error('RPC peer closed');
  }

  function request(kind, name, args) {
    const id = ++next;
    const wire = encode({kind, id, name, args});
    return new Promise((resolve, reject) => {
      pending.set(id, {resolve, reject});
      send(wire).catch(error => { pending.delete(id); reject(error); });
    });
  }

  async function receive(wire) {
    const message = JSON.parse(wire);
    if (message.kind === 'return' || message.kind === 'error') {
      const call = pending.get(message.id);
      if (!call) return;
      pending.delete(message.id);
      if (message.kind === 'return') call.resolve(message.data);
      else call.reject(new Error(message.data));
      return;
    }
    let reply;
    try {
      let result;
      if (message.kind === 'claim' && registryHost) {
        claim(message.name);
      } else if (message.kind === 'call') {
        if (registering.has(message.name)) await registering.get(message.name);
        const handler = handlers.get(message.name);
        if (!handler) throw new Error('Unknown RPC method: ' + message.name);
        result = await handler(...message.args);
      } else throw new Error('Invalid RPC request');
      reply = encode({kind:'return', id:message.id, data:result ?? null});
    } catch (err) {
      reply = encode({kind:'error', id:message.id, data:String(err.message || err).slice(0,128)});
    }
    await send(reply);
  }

  let timer;
  function stop() {
    closed = true;
    clearInterval(timer);
    for (const call of pending.values()) call.reject(new Error('RPC peer closed'));
    pending.clear();
  }
  timer = setInterval(() => {
    try {
      for (let count = 0; count < 8; ++count) {
        const wire = readPacket();
        if (wire === undefined) break;
        receive(wire).catch(stop);
      }
    } catch { stop(); }
  }, pollMs);

  const registering = new Map();
  const api = Object.freeze({
    async set(name, handler) {
      validate(name);
      if (typeof handler !== 'function') throw new TypeError('Expected an RPC handler');
      if (closed) throw new Error('RPC peer closed');
      if (handlers.has(name) || registering.has(name)) throw duplicate(name);
      if (registryHost) {
        claim(name);
        handlers.set(name, handler);
      } else {
        const ready = request('claim', name).then(() => { handlers.set(name, handler); });
        registering.set(name, ready);
        try { await ready; }
        finally { registering.delete(name); }
      }
    },
    async get(name, ...args) {
      validate(name);
      return request('call', name, args);
    }
  });
  Object.defineProperty(globalThis, 'rpc', {value:api});
}
