// The same endpoint runs in the isolated page preload and in the Node backend.
(() => {
  const handlers = new Map();
  const pending = new Map();
  let next = 0, closed = false, send, activate = () => {};
  function stop() {
    closed = true;
    for (const call of pending.values()) call.reject(new Error('RPC peer closed'));
    pending.clear();
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
  if (process.type === 'renderer') {
    const {contextBridge, ipcRenderer} = require('electron');
    send = wire => ipcRenderer.invoke('quota:packet',wire);
    ipcRenderer.on('quota:packet', (_event,wire) => receive(wire).catch(stop));
    ipcRenderer.on('quota:closed',stop);
    contextBridge.executeInMainWorld({func:install, args:[rpc]});
  } else {
    const fs = require('node:fs');
    let timer;
    const buffer = Buffer.alloc(1024);
    activate = () => {
      if (timer) return;
      timer = setInterval(() => {
        try {
          for (let i=0;i<8;++i) {
            const n = fs.readSync(3,buffer);
            if (!n) { clearInterval(timer); stop(); break; }
            receive(buffer.toString('utf8',0,n)).catch(stop);
          }
        } catch(error) {
          if (error.code !== 'EAGAIN') { clearInterval(timer); stop(); }
        }
      }, 16);
    };
    send = async wire => {
      const packet = Buffer.from(wire);
      for (;;) {
        if (closed) throw new Error('RPC peer closed');
        try { fs.writeSync(3,packet); return; }
        catch(error) {
          if (error.code !== 'EAGAIN') throw error;
          await new Promise(resolve => setTimeout(resolve,16));
        }
      }
    };
    install(rpc);
  }
})();
