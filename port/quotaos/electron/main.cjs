const { app, BrowserWindow, ipcMain } = require('electron');
const fs = require('node:fs');
fs.writeSync(2, '[wap] Electron main entered\n');

const documentPath = '/app/app.html';
// The first WAP format exposes a single self-contained UI document.
const html = fs.readFileSync(documentPath);
const documentURL = 'data:text/html;charset=utf-8;base64,' + html.toString('base64');
let window;

function checkSender(event) {
  if (!window || event.sender !== window.webContents ||
      event.senderFrame !== window.webContents.mainFrame ||
      event.senderFrame.url !== documentURL) throw new Error('Unknown application document.');
}

app.disableHardwareAcceleration();
// This desktop hosts one document per app instance. Let its renderer start on demand.
app.commandLine.appendSwitch('disable-features', 'SpareRendererForSitePerProcess');
app.commandLine.appendSwitch('disable-software-rasterizer');
app.whenReady().then(async () => {
  fs.writeSync(2, `[wap] opening guest app.html bytes=${html.length}\n`);
  window = new BrowserWindow({
    width: 624, height: 256, useContentSize: true, title: 'Demo', show: false,
    webPreferences: {
      offscreen: true, contextIsolation: true, sandbox: true, nodeIntegration: false,
      preload: '/usr/lib/quota-runtime/preload.cjs'
    }
  });
  fs.writeSync(2, '[wap] Electron BrowserWindow created\n');
  window.webContents.once('dom-ready', () => fs.writeSync(2, '[wap] Electron DOM ready\n'));
  window.webContents.once('paint', (_event, _dirty, image) => {
    const { width, height } = image.getSize();
    fs.writeSync(2, `[wap] Electron paint size=${width}x${height}\n`);
  });
  window.webContents.on('did-finish-load', () => {
    fs.writeSync(2, '[wap] Electron document loaded\n');
    window.webContents.invalidate();
  });
  window.webContents.on('did-fail-load', (_event, code, description) =>
    fs.writeSync(2, `[wap] Electron document load failed ${code} ${description}\n`));
  window.webContents.on('render-process-gone', (_event, details) =>
    fs.writeSync(2, `[wap] Electron renderer exited ${details.reason} ${details.exitCode}\n`));
  const output = fs.openSync('/dev/quota-window', fs.constants.O_RDWR | fs.constants.O_NONBLOCK);
  const input = Buffer.alloc(12);
  const timer = setInterval(() => {
    for (let i = 0; i < 32; ++i) {
      try { fs.readSync(output, input); }
      catch (error) { if (error.code === 'EAGAIN') break; throw error; }
      const kind = input.readUInt32LE(0), x = input.readUInt32LE(4), y = input.readUInt32LE(8);
      if (kind === 3) {
        const keyCode = x === 8 ? 'Backspace' : x === 13 ? 'Enter' : String.fromCodePoint(x);
        window.webContents.sendInputEvent({ type: 'keyDown', keyCode });
        if (x >= 32 || x === 13)
          window.webContents.sendInputEvent({ type: 'char', keyCode: String.fromCodePoint(x) });
        window.webContents.sendInputEvent({ type: 'keyUp', keyCode });
      } else {
        window.webContents.sendInputEvent({ type: kind === 1 ? 'mouseDown' : 'mouseUp', x, y, button: 'left', clickCount: 1 });
      }
    }
  }, 16);
  window.on('closed', () => { clearInterval(timer); fs.closeSync(output); });
  let first = true;
  function publish(image) {
    const { width, height } = image.getSize();
    if (width !== 624 || height !== 256) return;
    const bitmap = image.toBitmap();
    const stride = width * 4;
    if (bitmap.length !== stride * height) return;
    for (let y = 0; y < height; ++y)
      fs.writeSync(output, bitmap, y * stride, stride);
    if (first) fs.writeSync(2, '[wap] Electron first frame published\n');
    first = false;
  }
  window.webContents.on('paint', (_event, _rect, image) => publish(image));
  window.webContents.once('did-finish-load', async () => {
    if (!first) return;
    try {
      const image = await window.webContents.capturePage();
      if (first && !image.isEmpty()) publish(image);
    } catch (error) {
      fs.writeSync(2, '[wap] First frame capture: ' + error.message + '\n');
    }
  });
  window.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
  window.webContents.on('will-navigate', (event, url) => {
    if (url !== documentURL) event.preventDefault();
  });
  let connected = fs.existsSync('/app/app.js');
  let peerClosed = false;
  const packet = Buffer.alloc(1024);
  const relay = setInterval(() => {
    if (!connected || window.webContents.isLoadingMainFrame()) return;
    try {
      for (let i=0;i<8;++i) {
        const n = fs.readSync(3,packet);
        if (!n) { connected = false; peerClosed = true; break; }
        window.webContents.send('quota:packet',packet.toString('utf8',0,n));
      }
    } catch(error) {
      if (error.code !== 'EAGAIN') { connected = false; peerClosed = true; }
    }
    if (peerClosed) window.webContents.send('quota:closed');
  },16);
  window.once('closed', () => clearInterval(relay));
  ipcMain.handle('quota:packet', async (event, wire) => {
    checkSender(event);
    if (typeof wire !== 'string' || Buffer.byteLength(wire)>1024)
      throw new Error('Invalid RPC packet');
    const bytes = Buffer.from(wire);
    while (connected) {
      try { fs.writeSync(3,bytes); return; }
      catch(error) {
        if (error.code !== 'EAGAIN') throw error;
        await new Promise(resolve => setTimeout(resolve,16));
      }
    }
    throw new Error('RPC backend unavailable');
  });
  window.show();
  await window.loadURL(documentURL);
});
app.on('window-all-closed', () => app.quit());
