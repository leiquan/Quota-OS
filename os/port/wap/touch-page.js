function withTouchScroll(installRpc) {
  return function installTouchPage(writePacket, readPacket) {
    installRpc(writePacket, readPacket);
    document.addEventListener('keydown', event => {
      if (!event.isTrusted) return;
      const step = event.key === '\uE000' ? 48 : event.key === '\uE001' ? -48 : 0;
      if (!step) return;
      event.preventDefault();
      event.stopImmediatePropagation();
      document.scrollingElement?.scrollBy(0, step);
    }, true);
  };
}
