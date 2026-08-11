(() => {
  "use strict";

  const tray = document.querySelector(".window-tray");
  const trayItems = document.querySelector(".window-tray-items");
  let maximisedWindow = null;

  function randomUnit() {
    if (window.crypto?.getRandomValues) {
      const value = new Uint32Array(1);
      window.crypto.getRandomValues(value);
      return value[0] / 0x100000000;
    }
    return Math.random();
  }

  function hslToRgb(hue, saturation, lightness) {
    const chroma = (1 - Math.abs(2 * lightness - 1)) * saturation;
    const section = hue / 60;
    const x = chroma * (1 - Math.abs(section % 2 - 1));
    let rgb;

    if (section < 1)
      rgb = [chroma, x, 0];
    else if (section < 2)
      rgb = [x, chroma, 0];
    else if (section < 3)
      rgb = [0, chroma, x];
    else if (section < 4)
      rgb = [0, x, chroma];
    else if (section < 5)
      rgb = [x, 0, chroma];
    else
      rgb = [chroma, 0, x];

    const match = lightness - chroma / 2;
    return rgb.map(channel => Math.round((channel + match) * 255));
  }

  function mixColour(colour, target, amount) {
    return colour.map((channel, index) =>
      Math.round(channel + (target[index] - channel) * amount));
  }

  function hexColour(colour) {
    return `#${colour.map(channel =>
      channel.toString(16).padStart(2, "0")).join("")}`;
  }

  function relativeLuminance(colour) {
    const channels = colour.map(channel => {
      const value = channel / 255;
      return value <= 0.04045 ? value / 12.92 :
        ((value + 0.055) / 1.055) ** 2.4;
    });
    return channels[0] * 0.2126 + channels[1] * 0.7152 +
      channels[2] * 0.0722;
  }

  function randomiseWindowColours() {
    const windows = [...document.querySelectorAll("main .window")];
    const startingHue = randomUnit() * 360;

    windows.slice(1).forEach((win, index) => {
      const hue = (startingHue + index * 137.508 +
        (randomUnit() - 0.5) * 24 + 360) % 360;
      const saturation = 0.38 + randomUnit() * 0.34;
      const lightness = 0.32 + randomUnit() * 0.34;
      const frame = hslToRgb(hue, saturation, lightness);
      const luminance = relativeLuminance(frame);
      const whiteContrast = 1.05 / (luminance + 0.05);
      const blackContrast = (luminance + 0.05) / 0.05;

      win.style.setProperty("--window-frame", hexColour(frame));
      win.style.setProperty("--window-frame-light",
        hexColour(mixColour(frame, [255, 255, 255], 0.38)));
      win.style.setProperty("--window-frame-dark",
        hexColour(mixColour(frame, [0, 0, 0], 0.48)));
      win.style.setProperty("--window-frame-shine",
        hexColour(mixColour(frame, [255, 255, 255], 0.7)));
      win.style.setProperty("--window-title-highlight",
        hexColour(mixColour(frame, [255, 255, 255], 0.78)));
      win.style.setProperty("--window-title-shadow",
        hexColour(mixColour(frame, [0, 0, 0], 0.58)));
      win.style.setProperty("--window-title-ink",
        whiteContrast > blackContrast ? "#ffffff" : "#101010");
      win.classList.add("has-random-colour");
    });
  }

  function windowTitle(win) {
    return win.querySelector(".window-title")?.textContent.trim() || "Window";
  }

  function updateTrayVisibility() {
    const visible = trayItems.childElementCount !== 0;
    tray.hidden = !visible;
    document.body.classList.toggle("has-window-tray", visible);
  }

  function setMaximised(win, maximised) {
    const button = win.querySelector('[data-window-action="maximise"]');

    win.classList.toggle("is-maximised", maximised);
    if (button) {
      button.setAttribute("aria-pressed", String(maximised));
      button.setAttribute("aria-label",
        maximised ? "Restore this section" : "Maximise this section");
    }

    maximisedWindow = maximised ? win : null;
    document.body.classList.toggle("has-maximised-window", maximised);
  }

  function setShaded(win, shaded) {
    const button = win.querySelector('[data-window-action="shade"]');

    win.classList.toggle("is-shaded", shaded);
    button.setAttribute("aria-expanded", String(!shaded));
    button.setAttribute("aria-label",
      shaded ? "Unshade this section" : "Shade this section");
  }

  function restoreWindow(win, trayButton, focusAction) {
    win.hidden = false;
    trayButton.remove();
    updateTrayVisibility();
    win.scrollIntoView({ block: "center" });
    win.querySelector(`[data-window-action="${focusAction}"]`)?.focus();
  }

  function addToTray(win, kind, focusAction) {
    const restore = document.createElement("button");
    restore.type = "button";
    restore.className = kind;
    restore.textContent = windowTitle(win);
    restore.setAttribute("aria-label", `Restore ${windowTitle(win)}`);
    restore.addEventListener("click", () =>
      restoreWindow(win, restore, focusAction));
    trayItems.append(restore);
    updateTrayVisibility();
    restore.focus();
  }

  function iconifyWindow(win) {
    if (win === maximisedWindow)
      setMaximised(win, false);
    win.hidden = true;
    addToTray(win, "iconified", "minimise");
  }

  document.addEventListener("click", event => {
    const button = event.target.closest("[data-window-action]");
    if (!button)
      return;

    const win = button.closest(".window");
    if (!win)
      return;

    switch (button.dataset.windowAction) {
    case "shade":
      if (win === maximisedWindow)
        setMaximised(win, false);
      setShaded(win, !win.classList.contains("is-shaded"));
      break;
    case "minimise": {
      iconifyWindow(win);
      break;
    }
    case "maximise": {
      const maximise = win !== maximisedWindow;
      if (maximisedWindow && maximisedWindow !== win)
        setMaximised(maximisedWindow, false);
      if (maximise && win.classList.contains("is-shaded"))
        setShaded(win, false);
      setMaximised(win, maximise);
      break;
    }
    }
  });

  document.addEventListener("keydown", event => {
    if (event.key === "Escape" && maximisedWindow) {
      const win = maximisedWindow;
      setMaximised(win, false);
      win.querySelector('[data-window-action="maximise"]')?.focus();
    }
  });

  randomiseWindowColours();
})();
