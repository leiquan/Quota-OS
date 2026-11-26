function installQuotaRpc(writePacket, readPacket) {
  const pollMs = 16;
  let timer;
  const handlers = new Map();
  const pending = new Map();
  let next = 0, closed = false;
  function stop() {
    closed = true;
    clearInterval(timer);
    for (const call of pending.values()) call.reject(new Error('RPC peer closed'));
    pending.clear();
  }
  function activate() {
    if (timer !== undefined) return;
    timer = setInterval(() => {
      try {
        for (let count = 0; count < 8; ++count) {
          const wire = readPacket();
          if (wire === undefined) break;
          receive(wire).catch(stop);
        }
      } catch {
        stop();
      }
    }, pollMs);
  }

  async function send(wire) {
    while (!closed) {
      if (writePacket(wire)) return;
      await new Promise(resolve => setTimeout(resolve, pollMs));
    }
    throw new Error('RPC peer closed');
  }

  async function receive(wire) {
    const message = JSON.parse(wire);
    if (message.kind === 'call') {
      let reply;
      try {
        const handler = handlers.get(message.name);
        if (!handler) throw new Error('Unknown RPC method: ' + message.name);
        const value = await handler(message.data);
        reply = encode({kind:'return', id:message.id, data:value ?? null});
      } catch (error) {
        reply = encode({kind:'error', id:message.id, data:String(error.message || error).slice(0,128)});
      }
      await send(reply);
    } else {
      const call = pending.get(message.id);
      if (!call) return;
      pending.delete(message.id);
      if (message.kind === 'return') call.resolve(message.data);
      else call.reject(new Error(String(message.data)));
    }
  }
  function encode(message) {
    const wire = JSON.stringify(message);
    if (encodeURIComponent(wire).replace(/%[0-9A-F]{2}/gi,'x').length > 1024)
      throw new RangeError('RPC message exceeds 1024 bytes');
    return wire;
  }
  function rpc(name, data) {
    if (closed) throw new Error('RPC peer closed');
    if (typeof name !== 'string' || !name.length) throw new TypeError('Expected RPC method name');
    activate();
    if (typeof data === 'function') {
      handlers.set(name, data);
      return;
    }
    return new Promise((resolve,reject) => {
      const id = ++next;
      const wire = encode({kind:'call', id, name, data:data ?? null});
      pending.set(id, {resolve,reject});
      Promise.resolve().then(() => send(wire)).catch(error => {
        pending.delete(id);
        reject(error);
      });
    });
  }
  // This function runs in the application world on both ends. Constructing
  // the proxy there keeps dynamic property access intact across isolation.
  function install(endpoint) {
    const methods = new Proxy(Object.create(null), {
      get(_target, name) {
        if (typeof name !== 'string' || name === 'then') return undefined;
        return (...args) => endpoint(name, args);
      },
      set(_target, name, handler) {
        if (typeof name !== 'string' || name === 'then' || typeof handler !== 'function')
          throw new TypeError('Assign an RPC handler function to a method name');
        endpoint(name, args => handler(...args));
        return true;
      }
    });
    Object.defineProperty(globalThis, 'rpc', {value:methods});
  }
  install(rpc);
}
