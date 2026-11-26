function withTouchScroll(installRpc) {
  return function installTouchPage(writePacket, readPacket) {
    installRpc(writePacket, readPacket);
    let startY, skipClick;
    window.addEventListener('mousedown', event => {
      if (!event.isTrusted || event.button !== 0) return;
      startY = event.clientY;
      skipClick = false;
    }, true);
    window.addEventListener('mouseup', event => {
      if (!event.isTrusted || event.button !== 0 || startY === undefined) return;
      const delta = startY - event.clientY;
      startY = undefined;
      if (Math.abs(delta) < 20) return;
      skipClick = true;
      event.preventDefault();
      event.stopImmediatePropagation();
      document.scrollingElement?.scrollBy(0, delta);
    }, true);
    window.addEventListener('click', event => {
      if (!event.isTrusted || !skipClick) return;
      event.preventDefault();
      event.stopImmediatePropagation();
      skipClick = false;
    }, true);
  };
}
