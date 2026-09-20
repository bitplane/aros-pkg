// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper
// The only script: copy buttons and filters that apply on change.
document.querySelectorAll("button.copy").forEach(b => b.addEventListener("click", async () => {
  try {
    await navigator.clipboard.writeText(b.dataset.copy);
    b.textContent = "Copied";
    setTimeout(() => { b.textContent = "Copy"; }, 1500);
  } catch { b.textContent = "Select and copy"; }
}));
document.querySelectorAll("select[data-autosubmit]").forEach(s => s.addEventListener("change", () => s.form.submit()));

// A panel people fold once stays folded for them (Get Pkg, for those who have it).
// Kept in this browser only; without storage the panel simply opens each time.
document.querySelectorAll("details[data-remember]").forEach(d => {
  const key = d.dataset.remember;
  try { if (localStorage.getItem(key) === "1") d.open = false; } catch {}
  d.addEventListener("toggle", () => {
    try { d.open ? localStorage.removeItem(key) : localStorage.setItem(key, "1"); } catch {}
  });
});

// The account menu is a <details>: it closes on a second click by itself, and
// here also when one clicks outside it or presses Escape.
document.addEventListener("click", e => {
  document.querySelectorAll("details.menu[open]").forEach(d => { if (!d.contains(e.target)) d.open = false; });
});
document.addEventListener("keydown", e => {
  if (e.key === "Escape") document.querySelectorAll("details.menu[open]").forEach(d => { d.open = false; });
});

// What a search matched, marked in a page's prose: the words come from ?q= and
// only text is touched, never the markup around it.
(function () {
  const q = new URLSearchParams(location.search).get("q");
  const prose = document.querySelector(".docs .prose");
  if (!q || !prose) return;
  const words = q.split(/\s+/).filter(w => w.length > 1).map(w => w.toLowerCase());
  if (words.length === 0) return;
  const walk = document.createTreeWalker(prose, NodeFilter.SHOW_TEXT);
  const nodes = [];
  for (let n = walk.nextNode(); n; n = walk.nextNode()) {
    if (n.parentElement.closest("pre, code, mark")) continue;
    if (words.some(w => n.nodeValue.toLowerCase().includes(w))) nodes.push(n);
  }
  let first = null;
  nodes.forEach(n => {
    const frag = document.createDocumentFragment();
    let text = n.nodeValue, i = 0;
    while (i < text.length) {
      let at = -1, len = 0;
      words.forEach(w => { const k = text.toLowerCase().indexOf(w, i); if (k >= 0 && (at < 0 || k < at)) { at = k; len = w.length; } });
      if (at < 0) { frag.appendChild(document.createTextNode(text.slice(i))); break; }
      if (at > i) frag.appendChild(document.createTextNode(text.slice(i, at)));
      const m = document.createElement("mark");
      m.textContent = text.slice(at, at + len);
      frag.appendChild(m);
      if (!first) first = m;
      i = at + len;
    }
    n.parentNode.replaceChild(frag, n);
  });
  if (first && !location.hash) first.scrollIntoView({ block: "center" });
})();
