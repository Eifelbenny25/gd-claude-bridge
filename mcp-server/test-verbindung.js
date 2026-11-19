// Kleiner Test OHNE Claude: spricht direkt mit der Mod.
// Aufruf:  node test-verbindung.js          (nur lesen)
//          node test-verbindung.js --block  (setzt zusaetzlich einen Testblock)
import { callGD, closeGD, GD_URL } from "./gd-connection.js";

try {
  console.log(`Verbinde mit ${GD_URL} ...`);
  const ping = await callGD("ping");
  console.log("Ping:", ping);

  if (!ping.inEditor) {
    console.log("Mod laeuft! Oeffne jetzt ein Level im Editor und starte den Test erneut.");
  } else {
    console.log("Level-Info:", await callGD("get_level_info"));

    if (process.argv.includes("--block")) {
      const added = await callGD("add_objects", { objects: [{ id: 1, x: 15, y: 105 }] });
      console.log("Testblock gesetzt:", added, "-> im Editor mit Strg+Z wieder entfernen.");
    }
  }
} catch (err) {
  console.error("FEHLER:", err.message);
  process.exitCode = 1;
} finally {
  closeGD();
}
