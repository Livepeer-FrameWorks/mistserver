import { el } from './dom_helpers.js';

export function copy(text) {
  return new Promise(function (resolve, reject) {
    navigator.permissions.query({ name: "clipboard-write" }).then(function(result) {
      if (result.state === "granted" || result.state === "prompt") {
        navigator.clipboard.writeText(text).then(function(){
          resolve();
        }).catch(function(){
          throw "Failed";
        });
      }
      else {
        throw "Not permitted";
      }
    }).catch(function(e){

      const textArea = document.createElement("textarea");
      textArea.value = text;
      textArea.textContent = text;
      textArea.style.position = "absolute";
      textArea.style.opacity = 0.00001;
      const focussed = document.activeElement;
      function addToDOM(ele, tryHere) {
        if (!tryHere) { tryHere = focussed; }
        if (tryHere.checkVisibility()) {
          tryHere.appendChild(ele);
          return true;
        }
        if (tryHere.parentNode) { return addToDOM(ele, tryHere.parentNode); }
        return addToDOM(ele, document.body);
        return false;
      }
      addToDOM(textArea);
      textArea.focus();
      if (document.activeElement != textArea) {
        if (focussed)
        reject("Copy failed (could not obtain focus)");
        return;
      }
      textArea.setSelectionRange(0, text.length);

      let yay = false;
      try {
        yay = document.execCommand('copy');
      } catch (err) {
        console.errror(err);
      }
      textArea.parentNode.removeChild(textArea);
      focussed.focus();
      if (yay) {
        resolve();
      }
      else {
        reject(e);
      }
    });
  });
}

export function upload(accept) {
  if (!accept) {
    accept = {
      description: "text files",
      accept: {
        "text/*": []
      }
    };
  }
  if (!Array.isArray(accept)) {
    accept = [accept];
  }

  return new Promise(function(resolve, reject){
    if (window.showOpenFilePicker) {
      try {
        showOpenFilePicker({
          startIn: "downloads",
          types: accept
        }).then(function(handles){
          handles[0].getFile().then(resolve).catch(reject);
        }).catch(reject);
      }
      catch (e) {
        reject(e);
      }
    }
    else {
      const input = el('input', {type: 'file', style: {display: 'none'}});
      input.addEventListener('change', function(){
        if (this.files && this.files.length) {
          const resolved = resolve(this.files[0]);
          if (resolved instanceof Promise) {
            resolved.finally(function(){ input.remove(); });
          }
          else {
            setTimeout(function(){ input.remove(); }, 1);
          }
          return;
        }
      });
      document.body.appendChild(input);
      input.click();
    }

  });
}
