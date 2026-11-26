// Run the shipped RPC code in separate JS globals over two FIFO packet queues.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const core = fs.readFileSync(path.join(__dirname, '../port/wap/rpc.js'), 'utf8');
const intervals = [];
const packets = [[], []];

function endpoint(side) {
  const ctx = vm.createContext({
    setTimeout, clearInterval,
    setInterval(fn, ms) {
      const timer = setInterval(fn, ms);
      intervals.push(timer);
      return timer;
    },
    write(packet) { packets[1 - side].push(packet); return true; },
    read() { return packets[side].shift(); }
  });
  vm.runInContext(core + ';installQuotaRpc(write, read, ' + (side === 1) + ')', ctx);
  return ctx.rpc;
}

async function check() {
  const page = endpoint(0), node = endpoint(1);
  for (const [first, peer, label] of [[page, node, 'page'], [node, page, 'node']]) {
    await first.set(label, async (a, b) => a - b);
    await assert.rejects(first.set(label, () => 0), /already registered/);
    await assert.rejects(peer.set(label, () => 1), /already registered/);
    assert.equal(await peer.get(label, 13, 5), 8);
    for (const name of ['set', 'get']) await assert.rejects(first.set(name, () => 0), /Reserved/);
    await assert.rejects(first.get('missing'), /Unknown/);
    console.log(label, 'duplicate, reserved, unknown and ordered arguments verified');
  }
  for (let i = 0; i < 8; ++i) {
    const name = 'race' + i;
    const outstanding = page.set(name, () => 'page');
    assert(packets[1].some(packet => JSON.parse(packet).name === name));
    const results = await Promise.allSettled([outstanding, node.set(name, () => 'node')]);
    assert.equal(results.filter(r => r.status === 'fulfilled').length, 1);
    assert.match(results.find(r => r.status === 'rejected').reason.message, /already registered/);
    const pageWon = results[0].status === 'fulfilled';
    assert.equal(await (pageWon ? node : page).get(name), pageWon ? 'page' : 'node');
    console.log(name, 'page claim in flight; winner:', pageWon ? 'page' : 'node');
  }
}

check().catch(error => { console.error(error); process.exitCode = 1; })
  .finally(() => intervals.forEach(clearInterval));
