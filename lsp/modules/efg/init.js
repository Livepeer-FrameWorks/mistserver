import { APP_NAME, APP_TITLE } from '@brand';
import { state, connectionStatus, versions } from './api.js';
import { refresh } from './refresh.js';

function docReady(fn) {
  if (document.readyState === "complete" || document.readyState === "interactive") {
    setTimeout(fn, 1);
  } else {
    document.addEventListener("DOMContentLoaded", fn);
  }
}

docReady(() => {
  document.title = APP_TITLE;
  document.querySelector('header h1').textContent = APP_NAME;

  state.sBar = document.getElementById("sources");
  state.tBar = document.getElementById("targets");
  document.body.querySelector("header").append(connectionStatus);
  document.body.querySelector("header").append(versions);
  refresh();
  setInterval(refresh, 5000);
});
