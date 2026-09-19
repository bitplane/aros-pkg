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
