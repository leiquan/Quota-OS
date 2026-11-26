for (const showcase of document.querySelectorAll(".showcase")) {
  initCarousel(showcase);
}

function initCarousel(showcase) {
  const gallery = showcase.querySelector(".gallery");
  const controls = showcase.querySelector(".controls");
  if (!gallery || !controls) return;
  const indicators = controls.querySelector(".indicators");
  const playback = controls.querySelector(".playback");
  const slides = [...gallery.children];
  const chinese = document.documentElement.lang === "zh-CN";
  const reducedMotion = matchMedia("(prefers-reduced-motion: reduce)");
  let paused = reducedMotion.matches;
  let hovered = false;
  let focused = false;
  let timer;

  const current = () => Math.round(gallery.scrollLeft / gallery.clientWidth);

  function goTo(index) {
    const next = (index + slides.length) % slides.length;
    gallery.scrollTo({
      left: next * gallery.clientWidth,
      behavior: Math.abs(next - current()) > 1 ? "instant" : "auto",
    });
  }

  const dots = slides.map((slide, index) => {
    const dot = document.createElement("button");
    dot.type = "button";
    dot.className = "indicator";
    dot.setAttribute("aria-label", slide.querySelector("img").alt);
    dot.addEventListener("click", () => goTo(index));
    indicators.append(dot);
    return dot;
  });

  function update() {
    const index = current();
    dots.forEach((dot, position) => dot.setAttribute("aria-current", position === index ? "true" : "false"));
    controls.querySelector("output").value = (index + 1) + " / " + slides.length;
  }

  function schedule() {
    clearTimeout(timer);
    playback.textContent = paused ? (chinese ? "播放" : "Play") : (chinese ? "暂停" : "Pause");
    if (paused || hovered || focused || document.hidden) return;
    timer = setTimeout(() => {
      goTo(current() + 1);
      schedule();
    }, 6500);
  }

  controls.hidden = false;
  controls.addEventListener("click", event => {
    const step = Number(event.target.dataset.step);
    if (step) goTo(current() + step);
    schedule();
  });
  playback.addEventListener("click", () => {
    paused = !paused;
    schedule();
  });
  gallery.addEventListener("scroll", update, { passive: true });
  gallery.addEventListener("keydown", event => {
    if (!["ArrowLeft", "ArrowRight"].includes(event.key)) return;
    event.preventDefault();
    goTo(current() + (event.key === "ArrowLeft" ? -1 : 1));
    schedule();
  });
  showcase.addEventListener("pointerenter", event => {
    hovered = event.pointerType === "mouse";
    schedule();
  });
  showcase.addEventListener("pointerleave", () => {
    hovered = false;
    schedule();
  });
  showcase.addEventListener("focusin", () => {
    focused = true;
    schedule();
  });
  showcase.addEventListener("focusout", event => {
    focused = showcase.contains(event.relatedTarget);
    schedule();
  });
  gallery.addEventListener("touchstart", () => {
    paused = true;
    schedule();
  }, { passive: true });
  document.addEventListener("visibilitychange", schedule);
  reducedMotion.addEventListener("change", event => {
    paused = event.matches;
    schedule();
  });
  update();
  schedule();
}
