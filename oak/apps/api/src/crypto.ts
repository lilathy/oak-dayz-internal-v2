import crypto from "node:crypto";
import argon2 from "argon2";

const ARGON_OPTS: argon2.Options = {
  type: argon2.argon2id,
  memoryCost: 19456,
  timeCost: 2,
  parallelism: 1,
};

export async function hashPassword(password: string): Promise<string> {
  return argon2.hash(password, ARGON_OPTS);
}

export async function verifyPassword(hash: string, password: string): Promise<boolean> {
  try {
    return await argon2.verify(hash, password);
  } catch {
    return false;
  }
}

export function sha256(input: string): string {
  return crypto.createHash("sha256").update(input, "utf8").digest("hex");
}

/** Never store raw HWID — hash with a server pepper via JWT secrets mix is ok for local. */
export function hashHwid(rawHwid: string, pepper: string): string {
  return sha256(`${pepper}:hwid:${rawHwid.trim().toLowerCase()}`);
}

export function hwidHint(rawHwid: string): string {
  const t = rawHwid.trim();
  if (t.length <= 8) return "****";
  return `${t.slice(0, 4)}…${t.slice(-4)}`;
}

export function newRefreshToken(): string {
  return crypto.randomBytes(48).toString("base64url");
}
