/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* import-globals-from common.js */
/* import-globals-from permissions.js */

// Configuration vars
let homeURL = Services.prefs.getStringPref("vulpis.startup.homepage", "https://www.google.com/");
// Bug 1586294 - Localize the privacy policy URL (Services.urlFormatter?)
let privacyPolicyURL = "https://www.mozilla.org/en-US/privacy/firefox/";
let reportIssueURL = "https://github.com/Rafaelgarra/tombriva-vr/issues";
let licenseURL =
  "https://github.com/Rafaelgarra/tombriva-vr/blob/tombriva-vr/README.md#independence-and-licensing";

// https://developer.mozilla.org/en-US/docs/Mozilla/Tech/XUL/browser
let browser = null;
// Keep track of the current Permissions request to only allow one outstanding
// request/prompt at a time.
let currentPermissionRequest = null;
// And, keep a queue of pending Permissions requests to resolve when the
// current request finishes
let pendingPermissionRequests = [];
// The following variable map to UI elements whose behavior changes depending
// on some state from the browser control
let urlInput = null;
let secureIcon = null;
let backButton = null;
let forwardButton = null;
let refreshButton = null;
let stopButton = null;

const { PrivateBrowsingUtils } = ChromeUtils.importESModule(
  "resource://gre/modules/PrivateBrowsingUtils.sys.mjs"
);
const { AppConstants } = ChromeUtils.importESModule(
  "resource://gre/modules/AppConstants.sys.mjs"
);
const { XPCOMUtils } = ChromeUtils.importESModule(
  "resource://gre/modules/XPCOMUtils.sys.mjs"
);
const { SearchService } = ChromeUtils.importESModule(
  "moz-src:///toolkit/components/search/SearchService.sys.mjs"
);

// Note: FxR UI uses a fork of browser-fullScreenAndPointerLock.js which removes
// the dependencies on browser.js.
// Bug 1587946 - Rationalize the fork of browser-fullScreenAndPointerLock.js
XPCOMUtils.defineLazyScriptGetter(
  this,
  "FullScreen",
  "chrome://fxr/content/fxr-fullScreen.js"
);
ChromeUtils.defineLazyGetter(this, "gSystemPrincipal", () =>
  Services.scriptSecurityManager.getSystemPrincipal()
);

// DOMFullscreenParent calls window.PointerlockFsWarning.showFullScreen() and
// .close() whenever the chrome document enters fullscreen. That object belongs
// to browser.xhtml, which the FxR shell does not use, so the calls were
// throwing "PointerlockFsWarning is undefined" and aborting the handler
// mid-way -- which then left the child actor stuck ("cannot send at the
// moment"). Fullscreen worked in spite of it, not because of it.
//
// A no-op stub is the right answer rather than a real warning panel: the
// warning exists to tell desktop users a page went fullscreen and how to
// leave. In FxR the panel IS the display, there is no desktop chrome to hide,
// and leaving is a controls button or the SteamVR dashboard.
if (typeof window.PointerlockFsWarning === "undefined") {
  window.PointerlockFsWarning = {
    showFullScreen() {},
    showPointerLock() {},
    close() {},
  };
}

window.addEventListener(
  "DOMContentLoaded",
  () => {
    urlInput = document.getElementById("eUrlInput");
    secureIcon = document.getElementById("eUrlSecure");
    backButton = document.getElementById("eBack");
    forwardButton = document.getElementById("eForward");
    refreshButton = document.getElementById("eRefresh");
    stopButton = document.getElementById("eStop");

    setupBrowser();
    setupNavButtons();
    setupUrlBar();
    setupProjectionControls();
    setupWindowControls();
  },
  { once: true }
);

// Create XUL browser object
function setupBrowser() {
  // Note: createXULElement is undefined when this page is not loaded
  // via chrome protocol
  if (document.createXULElement) {
    browser = document.createXULElement("browser");
    browser.setAttribute("type", "content");
    browser.setAttribute("remote", "true");
    // Sem isto os JSWindowActors que declaram messageManagerGroups: ["browsers"]
    // -- DOMFullscreen entre eles -- nunca sao instanciados para esta janela:
    // JSWindowActorProtocol::Matches rejeita com "doesn't match message manager
    // group" (dom/ipc/jsactor/JSWindowActorProtocol.cpp:345). O atributo e lido
    // na criacao do frameLoader (nsFrameLoader.cpp:2986), portanto tem de ser
    // definido ANTES de anexar o elemento ao DOM; sem ele o loader cai no
    // message manager global da janela em vez do grupo.
    //
    // Efeito medido antes da correcao: o conteudo chamava requestFullscreen()
    // e o DOMFullscreenParent nao recebia mensagem nenhuma.
    browser.setAttribute("messagemanagergroup", "browsers");
    browser.classList.add("browser_instance");
    document.getElementById("eBrowserContainer").appendChild(browser);

    browser.loadUrlWithSystemPrincipal = function (url) {
      let uri = typeof url === "string" ? Services.io.newURI(url) : url;
      this.loadURI(uri, { triggeringPrincipal: gSystemPrincipal });
    };

    // Expose this function for Permissions to be used on this browser element
    // in other parts of the frontend
    browser.fxrPermissionPrompt = permissionPrompt;

    urlInput.value = homeURL;

    browser.fxrProgressListener = {
        QueryInterface: ChromeUtils.generateQI([
          "nsIWebProgressListener",
          "nsISupportsWeakReference",
        ]),
        onLocationChange() {
          // When URL changes, update the URL in the URL bar and update
          // whether the back/forward buttons are enabled.
          urlInput.value = browser.currentURI.spec;

          backButton.disabled = !browser.canGoBack;
          forwardButton.disabled = !browser.canGoForward;
        },
        onStateChange(aWebProgress, aRequest, aStateFlags) {
          if (aStateFlags & Ci.nsIWebProgressListener.STATE_STOP) {
            // Network requests are complete. Disable (hide) the stop button
            // and enable (show) the refresh button
            refreshButton.disabled = false;
            stopButton.disabled = true;
          } else {
            // Network requests are outstanding. Disable (hide) the refresh
            // button and enable (show) the stop button
            refreshButton.disabled = true;
            stopButton.disabled = false;
          }
        },
        onSecurityChange(aWebProgress, aRequest, aState) {
          // Update the Secure Icon when the security status of the
          // content changes
          if (aState & Ci.nsIWebProgressListener.STATE_IS_SECURE) {
            secureIcon.style.visibility = "visible";
          } else {
            secureIcon.style.visibility = "hidden";
          }
        },
      };
    browser.addProgressListener(
      browser.fxrProgressListener,
      Ci.nsIWebProgress.NOTIFY_LOCATION |
        Ci.nsIWebProgress.NOTIFY_SECURITY |
        Ci.nsIWebProgress.NOTIFY_STATE_REQUEST
    );

    window.addEventListener("unload", () => {
      browser.removeProgressListener(browser.fxrProgressListener);
      browser.fxrProgressListener = null;
    }, { once: true });
    browser.loadUrlWithSystemPrincipal(homeURL);
    FullScreen.init();

    // Send this notification to start and allow background scripts for
    // WebExtensions, since this FxR UI doesn't participate in typical
    // startup activities
    Services.obs.notifyObservers(window, "extensions-late-startup");
  }
}

function setupNavButtons() {
  let aryNavButtons = [
    "eBack",
    "eForward",
    "eRefresh",
    "eStop",
    "eHome",
    "ePrefs",
  ];

  function navButtonHandler() {
    if (!this.disabled) {
      switch (this.id) {
        case "eBack":
          browser.goBack();
          break;

        case "eForward":
          browser.goForward();
          break;

        case "eRefresh":
          browser.reload();
          break;

        case "eStop":
          browser.stop();
          break;

        case "eHome":
          browser.loadUrlWithSystemPrincipal(homeURL);
          break;

        case "ePrefs":
          openSettings();
          break;
      }
    }
  }

  for (let btnName of aryNavButtons) {
    let elem = document.getElementById(btnName);
    elem.addEventListener("click", navButtonHandler);
  }
}

function setupUrlBar() {
  // Navigate to new value when the user presses "Enter"
  urlInput.addEventListener("keypress", async function (e) {
    if (e.key == "Enter") {
      // Use the URL Fixup Service in case the user wants to search instead
      // of directly navigating to a location.
      await SearchService.init();

      let valueToFixUp = urlInput.value;
      let flags =
        Services.uriFixup.FIXUP_FLAG_FIX_SCHEME_TYPOS |
        Services.uriFixup.FIXUP_FLAG_ALLOW_KEYWORD_LOOKUP;
      if (PrivateBrowsingUtils.isWindowPrivate(window)) {
        flags |= Services.uriFixup.FIXUP_FLAG_PRIVATE_CONTEXT;
      }
      let { preferredURI } = Services.uriFixup.getFixupURIInfo(
        valueToFixUp,
        flags
      );

      browser.loadUrlWithSystemPrincipal(preferredURI.spec);
      browser.focus();
    }
  });

  // Upon focus, highlight the whole URL
  urlInput.addEventListener("focus", function () {
    urlInput.select();
  });
}

//
// Code to manage Settings UI
//

function openSettings() {
  let browserSettingsUI = document.createXULElement("browser");
  browserSettingsUI.setAttribute("type", "chrome");
  browserSettingsUI.classList.add("browser_settings");

  showModalContainer(browserSettingsUI);

  browserSettingsUI.loadURI(Services.io.newURI("chrome://fxr/content/prefs.html"), {
    triggeringPrincipal: gSystemPrincipal,
  });
}

function closeSettings() {
  clearModalContainer();
}

function showPrivacyPolicy() {
  closeSettings();
  browser.loadUrlWithSystemPrincipal(privacyPolicyURL);
}

function showLicenseInfo() {
  closeSettings();
  browser.loadUrlWithSystemPrincipal(licenseURL);
}

function showReportIssue() {
  closeSettings();
  browser.loadUrlWithSystemPrincipal(reportIssueURL);
}

//
// Code to manage Permissions UI
//

function permissionPrompt(aRequest) {
  let newPrompt;
  if (aRequest instanceof Ci.nsIContentPermissionRequest) {
    newPrompt = new FxrContentPrompt(aRequest, this, finishPrompt);
  } else {
    newPrompt = new FxrWebRTCPrompt(aRequest, this, finishPrompt);
  }

  if (currentPermissionRequest) {
    // There is already an outstanding request running. Cache this new request
    // to be prompted later
    pendingPermissionRequests.push(newPrompt);
  } else {
    currentPermissionRequest = newPrompt;
    currentPermissionRequest.showPrompt();
  }
}

function finishPrompt() {
  if (pendingPermissionRequests.length) {
    // Prompt the next request
    currentPermissionRequest = pendingPermissionRequests.shift();
    currentPermissionRequest.showPrompt();
  } else {
    currentPermissionRequest = null;
  }
}


//
// Code to manage Media Projection UI
//

// O par de ator FxRMedia atravessa a fronteira de processo entre esta janela
// (chrome) e a pagina (conteudo, remote="true"). Sem ele o botao nao alcanca
// o <video>, e a unica projecao possivel e a antiga: embrulhar a imagem ja
// composta da janela numa esfera do overlay, que deforma.
//
// messageManagerGroups: ["browsers"] casa com o messagemanagergroup definido
// em setupBrowser(); foi essa mesma correcao que destravou o DOMFullscreen.
function registerMediaActor() {
  try {
    ChromeUtils.registerWindowActor("FxRMedia", {
      parent: {
        esModuleURI: "resource:///actors/FxRMediaParent.sys.mjs",
      },
      child: {
        esModuleURI: "resource:///actors/FxRMediaChild.sys.mjs",
        events: { DOMDocElementInserted: {} },
      },
      messageManagerGroups: ["browsers"],
      allFrames: false,
    });
    // Controle positivo. Sem esta linha no log, "nao aconteceu nada" nao
    // distingue clique no botao errado de codigo que nem chegou a subir --
    // foi precisamente o que escondeu o no-op do forceAppWindowActive.
    dump("FxRMedia: ator registrado\n");
  } catch (e) {
    // O registro e por processo: a segunda janela do FxR encontra o ator ja
    // registrado, e isso nao e erro.
    dump("FxRMedia: registro dispensado (" + e + ")\n");
  }
}

// O ator da pagina carregada, ou null enquanto nao houver uma.
function getMediaActor() {
  try {
    return browser.browsingContext.currentWindowGlobal.getActor("FxRMedia");
  } catch (e) {
    dump("FxRMedia: ator indisponivel: " + e + "\n");
    return null;
  }
}

function setupProjectionControls() {
  registerMediaActor();

  const projBar = document.getElementById("eMediaProjectionBar");
  const toggleBtn = document.getElementById("eMediaProjectionToggle");

  if (toggleBtn && projBar) {
    toggleBtn.addEventListener("click", () => {
      projBar.hidden = !projBar.hidden;
    });
  }

  // Em tela cheia a barra de navegacao some, e com ela o botao que alterna a
  // barra de projecao; o CSS entao a mantem sempre visivel. Aqui ela esmaece
  // quando o laser fica parado ou fora do painel, e volta no primeiro
  // movimento. O temporizador so e armado por um mousemove: se o laser nao
  // chegar a esta janela, a barra nunca some.
  const AUTOHIDE_MS = 3000;
  let autohideTimer = 0;
  const inFullScreen = () =>
    document.documentElement.hasAttribute("inFullScreen");

  function clearAutohide() {
    if (autohideTimer) {
      clearTimeout(autohideTimer);
      autohideTimer = 0;
    }
  }

  function armAutohide() {
    clearAutohide();
    autohideTimer = setTimeout(() => {
      autohideTimer = 0;
      if (!inFullScreen()) {
        return;
      }
      // Com o laser em cima da propria barra ela fica.
      if (projBar.matches(":hover")) {
        armAutohide();
        return;
      }
      projBar.classList.add("autohidden");
    }, AUTOHIDE_MS);
  }

  if (projBar) {
    window.addEventListener(
      "mousemove",
      () => {
        if (!inFullScreen()) {
          return;
        }
        projBar.classList.remove("autohidden");
        armAutohide();
      },
      true
    );

    // Entrar ou sair da tela cheia sempre recomeca com a barra visivel.
    let wasFullScreen = inFullScreen();
    new MutationObserver(() => {
      const now = inFullScreen();
      if (now !== wasFullScreen) {
        wasFullScreen = now;
        clearAutohide();
        projBar.classList.remove("autohidden");
      }
    }).observe(document.documentElement, { attributes: true });
  }

  const projButtons = [
    { id: "eProjExit", mode: "exit" },
    { id: "eProj2D", mode: "2d" },
    { id: "eProj180SBS", mode: "180-mesh" },
    { id: "eProj360", mode: "360" },
    { id: "eProjCinema", mode: "curved" },
  ];

  function updateActiveButton(mode) {
    // Sair sempre volta ao painel; o botao de voltar nao e um modo.
    if (mode === "exit") {
      mode = "2d";
    }
    for (let item of projButtons) {
      let btn = document.getElementById(item.id);
      if (!btn) continue;
      if (item.mode === mode) {
        btn.classList.add("active");
      } else if (item.mode !== "exit") {
        btn.classList.remove("active");
      }
    }
  }

  let mediaEntryPending = false;
  let mediaEntryGeneration = 0;
  for (let item of projButtons) {
    let btn = document.getElementById(item.id);
    if (!btn) continue;
    btn.addEventListener("click", async () => {
      // Qual botao e por qual caminho. Resolve de vez a duvida de mapeamento
      // entre a posicao na faixa e o modo, que so a UI conhecia.
      dump("FxRMedia: clique em " + item.id + " modo=" + item.mode + "\n");
      if (item.mode === "exit") {
        mediaEntryGeneration++;
        // A saida cobre os dois caminhos, porque os dois podem estar ativos:
        // encerra a sessao WebXR, se houver, e desfaz a projecao do overlay.
        let xrActor = getMediaActor();
        if (xrActor) {
          xrActor.sendAsyncMessage("FxRMedia:Exit", {});
        }
        if (typeof FxRMediaStub !== "undefined") {
          FxRMediaStub.setProjectionMode("exit");
        }
        if (document.fullscreenElement) {
          document.exitFullscreen().catch(() => {});
        } else if (window.fullScreen) {
          window.fullScreen = false;
        }
      } else if (item.mode === "360" || item.mode === "180-mesh") {
        if (mediaEntryPending) return;
        const xrActor = getMediaActor();
        if (!xrActor) {
          dump("FxRMedia: sem ator; pedido nao enviado\n");
          return;
        }
        const generation = ++mediaEntryGeneration;
        mediaEntryPending = true;
        try {
          dump("FxRMedia: fechando dashboard antes de Project\n");
          await closeSteamVRDashboard();
          if (generation !== mediaEntryGeneration || xrActor !== getMediaActor()) return;
          FxRMediaStub.setProjectionMode("2d");
          dump("FxRMedia: dashboard fechado; enviando Project para o conteudo\n");
          xrActor.sendAsyncMessage("FxRMedia:Project", { mode: item.mode });
          updateActiveButton(item.mode);
        } catch (error) {
          dump("FxRMedia: falha na transicao dashboard/VR: " + error + "\n");
        } finally {
          mediaEntryPending = false;
        }
      } else {
        if (typeof FxRMediaStub !== "undefined") {
          FxRMediaStub.setProjectionMode(item.mode);
        }
        updateActiveButton(item.mode);
      }
    });
  }

  window.addEventListener("fxr-projection-changed", (e) => {
    if (e.detail && e.detail.mode) {
      updateActiveButton(e.detail.mode);
    }
  });

  // A sessao imersiva pode acabar sem clique na barra: R1 do controle, falha
  // ao entrar ou pagina fechada. Toda saida passa pelo relato "sessao
  // liberada" do renderer (FxRMediaParent o publica), e ai o painel volta.
  const sessionObserver = (subject, topic, text) => {
    if (typeof text === "string" && text.startsWith("sessao liberada")) {
      updateActiveButton("2d");
    }
  };
  Services.obs.addObserver(sessionObserver, "fxr-media-status");
  window.addEventListener(
    "unload",
    () => Services.obs.removeObserver(sessionObserver, "fxr-media-status"),
    { once: true }
  );
}

function setupWindowControls() {
  const toggle = document.getElementById("eWindowDock");
  const location = document.getElementById("eWindowLocation");
  const hint = document.getElementById("eWindowHint");
  const cinema = document.getElementById("eProjCinema");
  let docked = true;
  let timeout;
  function request() {
    toggle.disabled = true;
    clearTimeout(timeout);
    timeout = setTimeout(() => {
      toggle.disabled = false;
      hint.textContent = "SteamVR nÃ£o confirmou a troca. Tente novamente.";
    }, 4000);
    ChromeUtils.setFxrWindowDocked(!docked);
  }
  toggle.addEventListener("click", request);
  function closeBrowser() {
    setTimeout(() => Services.startup.quit(Ci.nsIAppStartup.eForceQuit), 0);
  }
  document.getElementById("eWindowClose").addEventListener("click", closeBrowser);
  const observer = (_subject, _topic, state) => {
    if (state === "quit") { closeBrowser(); return; }
    clearTimeout(timeout);
    toggle.disabled = false;
    const wasDocked = docked;
    docked = state === "docked";
    toggle.title = docked ? "Soltar janela" : "Recolher ao menu SteamVR";
    toggle.setAttribute("aria-label", toggle.title);
    toggle.dataset.docked = String(docked);
    location.textContent = docked ? "Menu SteamVR" : "Janela solta";
    hint.textContent = docked ? "Solte para mover a tela ou usar Cinema." :
      "Se o menu estiver aberto, feche-o pelo botÃ£o SteamVR. Segure as setas para mover.";
    if (state === "detached" && wasDocked) {
      closeSteamVRDashboard().catch(error => {
        document.getElementById("eWindowTools").dataset.commandError = "true";
        hint.textContent = "Feche o menu pelo botÃ£o SteamVR do controle.";
        console.error("FxR dashboard close:", error);
      });
    }
    if (state === "docked") document.getElementById("eWindowTools").dataset.commandError = "false";
    if (state === "unavailable") hint.textContent = "Dashboard indisponÃ­vel; a janela continua solta.";
    cinema.disabled = docked;
    cinema.title = docked ? "Solte a janela para usar Cinema" : "Tela cinema curva e ampliada";
    if (docked) window.dispatchEvent(new CustomEvent("fxr-projection-changed", {detail: {mode: "2d"}}));
  };
  Services.obs.addObserver(observer, "fxr-window-state");
  window.addEventListener("unload", () => {
    clearTimeout(timeout);
    Services.obs.removeObserver(observer, "fxr-window-state");
  }, {once: true});
  observer(null, null, "docked");
  ChromeUtils.setFxrWindowDocked(true);
}

async function closeSteamVRDashboard() {
  const local = Services.dirsvc.get("LocalAppData", Ci.nsIFile).path;
  const paths = await IOUtils.readJSON(PathUtils.join(local, "openvr", "openvrpaths.vrpath"));
  const runtime = paths.runtime?.[0];
  if (!runtime) throw new Error("SteamVR runtime path unavailable");
  const file = Cc["@mozilla.org/file/local;1"].createInstance(Ci.nsIFile);
  file.initWithPath(PathUtils.join(runtime, "bin", "win64", "vrcmd.exe"));
  const process = Cc["@mozilla.org/process/util;1"].createInstance(Ci.nsIProcess);
  process.init(file);
  process.startHidden = true;
  process.noShell = true;
  await new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      if (process.isRunning) process.kill();
      reject(new Error("SteamVR dashboard command timed out"));
    }, 4000);
    try {
      process.runwAsync(["--hidedashboard"], 1, {
        observe(_subject, topic) {
          clearTimeout(timer);
          if (topic === "process-finished" && process.exitValue === 0) resolve();
          else reject(new Error("SteamVR dashboard command failed"));
        }
      });
    } catch (error) { clearTimeout(timer); reject(error); }
  });
}
