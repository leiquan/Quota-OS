const gallery = document.querySelector(".gallery");
const controls = document.querySelector(".controls");
const total = gallery.children.length;
const current = () => Math.round(gallery.scrollLeft / gallery.clientWidth);

controls.hidden = false;
controls.addEventListener("click", event => {
  const step = Number(event.target.dataset.step);
  if (!step) return;
  const next = (current() + step + total) % total;
  gallery.scrollTo({ left: next * gallery.clientWidth });
});
gallery.addEventListener("scroll", () => {
  controls.querySelector("output").textContent = (current() + 1) + " / " + total;
}, { passive: true });
