import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { config } from "./config.js";

const __dirname = path.dirname(fileURLToPath(import.meta.url));

export const releasesRoot = path.resolve(
  process.env.RELEASES_PATH ?? path.join(__dirname, "..", "data", "releases")
);

export const clientsDir = path.join(releasesRoot, "clients");
export const launchersDir = path.join(releasesRoot, "launchers");

export function ensureReleaseDirs(): void {
  fs.mkdirSync(clientsDir, { recursive: true });
  fs.mkdirSync(launchersDir, { recursive: true });
}

// extend config usage
void config;
