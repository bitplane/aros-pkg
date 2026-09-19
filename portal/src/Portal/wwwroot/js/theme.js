// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper
// Light, dark or the system's: kept in this browser only. Runs before the page
// paints; the buttons are wired once the header exists.
(function () {
  var root = document.documentElement;
  function apply(t) { if (t === "light" || t === "dark") root.dataset.theme = t; else delete root.dataset.theme; }
  var saved = "system";
  try { saved = localStorage.getItem("theme") || "system"; } catch (e) {}
  apply(saved);
  document.addEventListener("DOMContentLoaded", function () {
    var buttons = document.querySelectorAll(".theme button");
    function show(t) { buttons.forEach(function (b) { b.setAttribute("aria-pressed", b.dataset.theme === t ? "true" : "false"); }); }
    show(saved);
    buttons.forEach(function (b) { b.addEventListener("click", function () {
      var t = b.dataset.theme; apply(t); show(t);
      try { t === "system" ? localStorage.removeItem("theme") : localStorage.setItem("theme", t); } catch (e) {}
    }); });
  });
})();
