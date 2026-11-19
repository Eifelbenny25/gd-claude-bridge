#!/usr/bin/env node
// MCP-Server: Claude Desktop <-> (stdio) <-> dieses Programm <-> (WebSocket) <-> Geode-Mod
// WICHTIG: Niemals console.log benutzen! stdout gehoert dem MCP-Protokoll.
// Fuer Debug-Ausgaben console.error nehmen.
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";
import { callGD } from "./gd-connection.js";

const INSTRUCTIONS = `
Du steuerst den Level-Editor von Geometry Dash (Mod "Claude Bridge").
Koordinaten: 1 Block = 30 Einheiten. Der Boden liegt bei y = 90, ein Block direkt
auf dem Boden hat seinen Mittelpunkt bei y = 105. Die erste Spalte hat x = 15,
die naechste x = 45 usw. (Mittelpunkt = 30 * Spalte + 15).
Haeufige Objekt-IDs: 1 = normaler Block, 8 = Stachel, 35 = gelbes Sprungpad,
36 = gelber Sprung-Orb, 12 = Wuerfel-Portal, 13 = Schiff-Portal.
Feste Bloecke (z. B. ID 1) speichert GD nur in 90-Grad-Schritten; Deko und Stacheln gehen frei.
Jedes Objekt hat eine "uid" (eindeutige Nummer). Damit verschiebst/aenderst/loeschst du es.
Nach gd_modify_objects bekommen Objekte NEUE uids (stehen in newUids).
Lies vor groesseren Aenderungen mit gd_get_level_info und gd_list_objects nach, was da ist.
Rueckgaengig: Hinzufuegen/Loeschen = 1x Strg+Z, Aendern = 2x Strg+Z.
Farbkanaele lassen sich NICHT per Strg+Z zuruecksetzen (merk dir die alten Werte aus "before").
`.trim();

const server = new McpServer(
  { name: "gd-claude-bridge", version: "1.0.0" },
  { instructions: INSTRUCTIONS }
);

function ok(data) {
  return { content: [{ type: "text", text: JSON.stringify(data, null, 2) }] };
}

function fail(err) {
  return { content: [{ type: "text", text: `Fehler: ${err.message}` }], isError: true };
}

async function run(cmd, params) {
  try {
    return ok(await callGD(cmd, params));
  } catch (err) {
    return fail(err);
  }
}

// Gemeinsame Felder fuer Objekt-Eigenschaften
const objectProps = {
  x: z.number().optional().describe("X-Position (1 Block = 30)"),
  y: z.number().optional().describe("Y-Position (Boden-Block = 105)"),
  rotation: z.number().optional().describe("Drehung in Grad"),
  scale: z.number().optional().describe("Einheitliche Skalierung (setzt scaleX und scaleY)"),
  scaleX: z.number().optional(),
  scaleY: z.number().optional(),
  flipX: z.boolean().optional(),
  flipY: z.boolean().optional(),
  color1: z.number().int().optional().describe("Haupt-Farbkanal (z. B. 1-999, 1004 = Obj)"),
  color2: z.number().int().optional().describe("Detail-Farbkanal"),
  groups: z.array(z.number().int()).optional().describe("Gruppen-IDs; leere Liste entfernt alle Gruppen"),
  editorLayer: z.number().int().optional(),
  zLayer: z.number().int().optional(),
  zOrder: z.number().int().optional(),
};

server.registerTool(
  "gd_ping",
  {
    description: "Prueft, ob Geometry Dash mit der Mod laeuft und ob der Editor offen ist.",
    inputSchema: {},
  },
  async () => run("ping", {})
);

server.registerTool(
  "gd_get_level_info",
  {
    description: "Liest Level-Infos: Name, Anzahl Objekte, Ausdehnung (minX/maxX), Laenge in Bloecken, Anzahl ausgewaehlter Objekte.",
    inputSchema: {},
  },
  async () => run("get_level_info", {})
);

server.registerTool(
  "gd_list_objects",
  {
    description: "Listet Objekte in einem Bereich (Rechteck x1..x2, y1..y2) mit uid, Objekt-ID, Position, Rotation, Skalierung, Farben und Gruppen.",
    inputSchema: {
      x1: z.number().optional(),
      x2: z.number().optional(),
      y1: z.number().optional(),
      y2: z.number().optional(),
      objectId: z.number().int().optional().describe("Nur Objekte mit dieser Objekt-ID"),
      group: z.number().int().optional().describe("Nur Objekte in dieser Gruppe"),
      limit: z.number().int().min(1).max(5000).optional().describe("Standard 300"),
      offset: z.number().int().min(0).optional().describe("Zum Blaettern"),
      includeRaw: z.boolean().optional().describe("Auch den rohen GD-Objekt-String mitschicken"),
    },
  },
  async ({ includeRaw, ...params }) => {
    try {
      const result = await callGD("list_objects", params);
      if (!includeRaw) result.objects.forEach((o) => delete o.raw);
      return ok(result);
    } catch (err) {
      return fail(err);
    }
  }
);

server.registerTool(
  "gd_get_selected_objects",
  {
    description: "Liest die Objekte, die der Nutzer gerade im Editor ausgewaehlt hat.",
    inputSchema: {
      includeRaw: z.boolean().optional(),
    },
  },
  async ({ includeRaw }) => {
    try {
      const result = await callGD("get_selected_objects", {});
      if (!includeRaw) result.objects.forEach((o) => delete o.raw);
      return ok(result);
    } catch (err) {
      return fail(err);
    }
  }
);

server.registerTool(
  "gd_add_objects",
  {
    description: "Fuegt Objekte hinzu (max. 5000 pro Aufruf). Rueckgaengig mit Strg+Z. Gibt die uids der neuen Objekte zurueck.",
    inputSchema: {
      objects: z
        .array(
          z.object({
            id: z.number().int().describe("Objekt-ID, z. B. 1 = Block, 8 = Stachel"),
            ...objectProps,
            extra: z.string().optional().describe("Weitere GD-Eigenschaften roh, z. B. '155,1,156,2'"),
          })
        )
        .optional(),
      objectString: z.string().optional().describe("Alternativ: roher GD-Objekt-String 'obj1;obj2;...'"),
    },
  },
  async (params) => run("add_objects", params)
);

server.registerTool(
  "gd_modify_objects",
  {
    description:
      "Aendert oder verschiebt Objekte (per uid). 'set' setzt Werte, dx/dy verschiebt relativ. Objekte werden dabei neu erstellt und bekommen NEUE uids. Rueckgaengig mit 2x Strg+Z.",
    inputSchema: {
      uids: z.array(z.number().int()).min(1),
      set: z.object({ id: z.number().int().optional(), ...objectProps }).optional(),
      dx: z.number().optional().describe("Verschiebung nach rechts (30 = 1 Block)"),
      dy: z.number().optional().describe("Verschiebung nach oben"),
      extra: z.string().optional().describe("Weitere GD-Eigenschaften roh, z. B. '155,1'"),
    },
  },
  async (params) => run("modify_objects", params)
);

server.registerTool(
  "gd_delete_objects",
  {
    description: "Loescht Objekte per uid. Rueckgaengig mit Strg+Z.",
    inputSchema: {
      uids: z.array(z.number().int()).min(1),
    },
  },
  async (params) => run("delete_objects", params)
);

server.registerTool(
  "gd_get_color_channels",
  {
    description: "Liest Farbkanaele (Startfarben). Ohne Angabe: BG(1000), Ground(1001), Line(1002), 3DL(1003), Obj(1004), G2(1009), MG(1013), MG2(1014) und 1-20.",
    inputSchema: {
      channels: z.array(z.number().int()).optional(),
    },
  },
  async (params) => run("get_color_channels", params)
);

server.registerTool(
  "gd_set_color_channel",
  {
    description: "Setzt die Startfarbe eines Farbkanals. NICHT per Strg+Z rueckgaengig machbar; die alten Werte stehen in 'before'.",
    inputSchema: {
      channel: z.number().int().min(1).max(1100),
      r: z.number().int().min(0).max(255).optional(),
      g: z.number().int().min(0).max(255).optional(),
      b: z.number().int().min(0).max(255).optional(),
      opacity: z.number().min(0).max(1).optional(),
      blending: z.boolean().optional(),
    },
  },
  async (params) => run("set_color_channel", params)
);

const transport = new StdioServerTransport();
await server.connect(transport);
console.error("gd-claude-bridge MCP-Server laeuft");
