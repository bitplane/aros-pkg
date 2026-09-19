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
