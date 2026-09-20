/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * Lado de chrome da ponte de midia imersiva do FxR.
 *
 * Nao decide nada: existe para completar o par de ator e para trazer de volta
 * o relato do renderer. O relato importa porque, com o headset no rosto, nao
 * ha devtools -- o `dump` coloca o estado no stdout do processo pai, onde o
 * terminal que abriu o FxR consegue ler.
 *
 * A notificacao tambem e publicada para observadores, para que o fxrui possa
 * exibir o estado na propria interface quando isso for util.
 */
export class FxRMediaParent extends JSWindowActorParent {
  receiveMessage(message) {
    if (message.name === "FxRMedia:Status") {
      const text = message.data.text;
      dump("FxRMedia: " + text + "\n");
      Services.obs.notifyObservers(null, "fxr-media-status", text);
    }
    return undefined;
  }
}
