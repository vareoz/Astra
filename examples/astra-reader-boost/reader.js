chrome.storage.local.set("boost_mode", "enabled");
let state = chrome.storage.local.get("boost_mode");
let slot = document.querySelector("#ext-boost-slot");
if (slot) {
  slot.textContent = "MV3 ReaderBoost: " + state;
}
console.log("Astra Reader Boost active:", state);
