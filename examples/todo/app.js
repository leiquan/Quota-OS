const fs = require('node:fs');
const file = '/app/data/note.txt';

rpc.set('list', () => {
  try {
    return JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch (error) {
    if (error.code === 'ENOENT') return [];
    throw error;
  }
});

rpc.set('save', tasks => {
  const json = JSON.stringify(tasks);
  if (Buffer.byteLength(json) > 496) throw Error('Task storage full');
  fs.writeFileSync(file, json);
});
