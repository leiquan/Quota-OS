(() => {
  const fs = require('node:fs');
  const buffer = Buffer.alloc(1024);

  function readPacket() {
    try {
      const size = fs.readSync(3, buffer);
      if (!size) throw new Error('RPC peer closed');
      return buffer.toString('utf8', 0, size);
    } catch (error) {
      if (error.code !== 'EAGAIN') throw error;
    }
  }

  function writePacket(wire) {
    const packet = Buffer.from(wire);
    try {
      if (fs.writeSync(3, packet) !== packet.length) throw new Error('Incomplete RPC packet');
      return true;
    } catch (error) {
      if (error.code !== 'EAGAIN') throw error;
      return false;
    }
  }

  installQuotaRpc(writePacket, readPacket);
})();
