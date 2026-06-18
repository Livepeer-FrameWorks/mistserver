export const state = {
  sBar: null,
  tBar: null,
  capabilitiesToggle: false,
  laststats: {},
  EFGconfig: {
    thumbURL: "http://localhost:8080/"
  },
  thumbURL: "http://localhost:8080/"
};

export function saveConfig(cfg) {
  send({ui_settings: Object.assign(state.EFGconfig, cfg)}).then(() => {
    console.log("Config saved");
  }).catch((e) => {
    console.log("Error while saving config", e);
  });
}

export const connectionStatus = document.createElement("div");
connectionStatus.classList.add("connection-status");

export const versions = document.createElement("div");
versions.classList.add("versions");
versions._values = [false, false];
versions._set = function(index, value) {
  if (this._values[index] != value) {
    this._values[index] = value;
    this.innerText = this._values.filter((v) => v).join(" and ");
  }
};

export function send(command) {
  return new Promise(function(resolve, reject) {
    connectionStatus.innerText = "Connecting..";
    fetch("/api2", {
      method: "POST",
      headers: {"Content-Type": "application/json"},
      body: JSON.stringify(command)
    }).then((r) => {
      connectionStatus.setAttribute("data-status", "connected");
      connectionStatus.innerText = "\u2705 Connected";

      if (r.status < 400) {
        r.json().then((d) => {
          if (location.hash.includes("debug")) {
            console.log(Object.keys(command)[0], "API request successful", {
              command: command,
              response: d
            });
          }
          resolve(d);
        }).catch((e) => {
          connectionStatus.setAttribute("data-status", "failed");
          connectionStatus.innerText = "\u274C Could not parse server response";
          reject(e);
        });
      } else {
        reject("API call received HTTP status " + r.status);
      }
    }).catch((e) => {
      connectionStatus.setAttribute("data-status", "failed");
      connectionStatus.innerText = "\u274C Failed to connect to server";
      reject(e);
    });
  });
}
