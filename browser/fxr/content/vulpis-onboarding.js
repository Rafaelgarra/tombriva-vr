/* SPDX-License-Identifier: MPL-2.0 */
"use strict";

window.addEventListener("DOMContentLoaded", () => {
  const pref = "vulpis.onboarding.vrControls.v1.seen";
  if (Services.prefs.getBoolPref(pref, false)) return;
  const anchor = document.getElementById("eMediaProjectionToggle");
  if (!anchor) return;
  const guide = document.createElement("section");
  guide.id = "vulpis-vr-guide";
  guide.setAttribute("role", "dialog");
  guide.setAttribute("aria-labelledby", "vulpis-guide-title");
  const registry = new L10nRegistry();
  registry.registerSources([new L10nFileSource(
    "vulpis", "app", ["en-US", "pt-BR"],
    "chrome://fxr/content/locales/{locale}/"
  )]);
  const l10n = new DOMLocalization(["onboarding.ftl"], false, registry);
  for (const [tag, id, className] of [
    ["h2", "vulpis-guide-title", ""],
    ["p", "vulpis-guide-intro", ""],
    ["p", "vulpis-guide-controls", ""],
    ["button", "vulpis-guide-dismiss", ""],
  ]) {
    const element = document.createElementNS("http://www.w3.org/1999/xhtml", tag);
    element.setAttribute("data-l10n-id", id);
    if (tag === "h2") element.id = "vulpis-guide-title";
    if (tag === "button") element.type = "button";
    if (className) {
      element.className = className;
      element.setAttribute("role", "img");
    }
    guide.appendChild(element);
  }
  const gestures = document.createElementNS("http://www.w3.org/1999/xhtml", "div");
  gestures.className = "guide-gestures";
  for (const [kind, imageId, captionId] of [
    ["trigger", "vulpis-guide-controller", "vulpis-guide-recenter"],
    ["grip", "vulpis-guide-grip", "vulpis-guide-exit"],
  ]) {
    const figure = document.createElementNS("http://www.w3.org/1999/xhtml", "figure");
    const picture = document.createElementNS("http://www.w3.org/1999/xhtml", "div");
    picture.className = "controller-sprite " + kind;
    picture.setAttribute("role", "img");
    picture.setAttribute("data-l10n-id", imageId);
    const caption = document.createElementNS("http://www.w3.org/1999/xhtml", "figcaption");
    caption.setAttribute("data-l10n-id", captionId);
    figure.append(picture, caption);
    gestures.appendChild(figure);
  }
  guide.insertBefore(gestures, guide.querySelector('[data-l10n-id="vulpis-guide-controls"]'));
  guide.style.visibility = "hidden";
  document.body.appendChild(guide);
  l10n.connectRoot(guide);
  l10n.translateRoots().then(() => {
    guide.style.visibility = "visible";
    position();
  }).catch(console.error);
  function position() {
    const box = anchor.getBoundingClientRect();
    const width = guide.getBoundingClientRect().width;
    const left = Math.max(12, Math.min(innerWidth - width - 12, box.right - width));
    guide.style.left = left + "px";
    guide.style.bottom = Math.max(12, innerHeight - box.top + 14) + "px";
    guide.style.setProperty("--guide-arrow", Math.max(16, Math.min(width - 32, left + width - (box.left + box.width / 2) - 8)) + "px");
  }
  const resize = new ResizeObserver(position);
  resize.observe(anchor);
  resize.observe(guide);
  window.addEventListener("resize", position);
  guide.querySelector("button").addEventListener("click", () => {
    Services.prefs.setBoolPref(pref, true);
    resize.disconnect();
    window.removeEventListener("resize", position);
    l10n.disconnectRoot(guide);
    guide.remove();
    anchor.focus();
  }, { once: true });
  window.addEventListener("unload", () => { resize.disconnect(); l10n.disconnectRoot(guide); }, { once: true });
  requestAnimationFrame(position);
}, { once: true });
