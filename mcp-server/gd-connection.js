// Verbindung zur Geode-Mod per WebSocket (nur 127.0.0.1).
import WebSocket from "ws";

const PORT = Number(process.env.GD_BRIDGE_PORT || 1414);
const URL = `ws://127.0.0.1:${PORT}`;

let socket = null;
let connecting = null;
let nextId = 1;
const pending = new Map();

function connect() {
  if (socket && socket.readyState === WebSocket.OPEN) return Promise.resolve(socket);
  if (connecting) return connecting;

  connecting = new Promise((resolve, reject) => {
    const ws = new WebSocket(URL);
    const timer = setTimeout(() => {
      ws.terminate();
      reject(new Error(`Keine Verbindung zu ${URL}. Laeuft Geometry Dash mit der Mod "Claude Bridge"?`));
    }, 3000);

    ws.on("open", () => {
      clearTimeout(timer);
      socket = ws;
      resolve(ws);
    });

    ws.on("message", (data) => {
      let msg;
      try {
        msg = JSON.parse(data.toString());
      } catch {
        return;
      }
      const entry = pending.get(msg.id);
      if (!entry) return;
      pending.delete(msg.id);
      clearTimeout(entry.timer);
      if (msg.ok) entry.resolve(msg.result);
      else entry.reject(new Error(msg.error || "Unbekannter Fehler"));
    });

    ws.on("close", () => {
      if (socket === ws) socket = null;
      for (const [id, entry] of pending) {
        clearTimeout(entry.timer);
        entry.reject(new Error("Verbindung zu Geometry Dash wurde getrennt"));
        pending.delete(id);
      }
    });

    ws.on("error", (err) => {
      clearTimeout(timer);
      reject(new Error(`Keine Verbindung zu ${URL} (${err.message}). Laeuft Geometry Dash mit der Mod "Claude Bridge"?`));
    });
  }).finally(() => {
    connecting = null;
  });

  return connecting;
}

// Schickt einen Befehl an die Mod und wartet auf die Antwort.
export async function callGD(cmd, params = {}, timeoutMs = 20000) {
  const ws = await connect();
  const id = nextId++;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      pending.delete(id);
      reject(new Error(`Zeitueberschreitung bei "${cmd}". Ist das Spiel eingefroren oder pausiert?`));
    }, timeoutMs);
    pending.set(id, { resolve, reject, timer });
    ws.send(JSON.stringify({ id, cmd, params }));
  });
}

export function closeGD() {
  if (socket) socket.close();
}

export const GD_URL = URL;
