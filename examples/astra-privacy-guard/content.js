// Astra Privacy Guard — Content Script executed by AstraJS VM
astra.storage.set("guard_status", "ACTIVE");
let status = astra.storage.get("guard_status");
console.log("Astra Privacy Guard injected:", status);
