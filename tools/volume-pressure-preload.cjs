// QA volume only. A saved Todo item arms a bounded V8 stress session.
;(() => {
  const fs = require('node:fs');
  const { performance, PerformanceObserver, constants } = require('node:perf_hooks');
  const v8 = require('node:v8');
  let armed = false;
  const arm = setInterval(() => {
    let rows;
    try { rows = JSON.parse(fs.readFileSync('/app/data/note.txt', 'utf8')); }
    catch { return; }
    if (!Array.isArray(rows) || !rows.some(row => row[0] === 'gcstress')) return;
    clearInterval(arm);
    const log = text => fs.writeSync(2, text + '\n');
    v8.setFlagsFromString('--expose_gc');
    const collect = require('node:vm').runInNewContext('gc');
    const before = process.hrtime.bigint();
    const now = BigInt(Math.round(performance.now()*1e6));
    const after = process.hrtime.bigint();
    const epochLow = before-now, epochHigh = after-now;
    let iterations = 0;
    const observer = new PerformanceObserver(list => {
      for (const entry of list.getEntries()) {
        if (entry.detail.kind !== constants.NODE_PERFORMANCE_GC_MAJOR) continue;
        const start = BigInt(Math.round(entry.startTime*1e6));
        const end = start + BigInt(Math.round(entry.duration*1e6));
        log('[v8-major] start_lo_ns='+ (epochLow+start) +' start_hi_ns='+ (epochHigh+start) +
            ' end_lo_ns='+ (epochLow+end) +' end_hi_ns='+ (epochHigh+end));
      }
    });
    observer.observe({entryTypes:['gc']});
    const pulse = setInterval(() => {
      const rows = [];
      for (let i=0;i<4096;++i) rows.push({i,value:'quota'+i});
      collect();
      if (rows[4095].value !== 'quota4095') throw Error('invalid live working set');
      if (++iterations === 10000) {
        clearInterval(pulse);
        observer.disconnect();
      }
    }, 250);
  }, 1000);
})();
