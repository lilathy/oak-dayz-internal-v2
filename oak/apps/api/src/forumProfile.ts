/**
 * Forum profile surfaces: bio, avatar, presence.
 * Strict validation — treat every byte as hostile.
 */
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { cryptoRandomId, db, nowIso } from "./db.js";

const __dirname = path.dirname(fileURLToPath(import.meta.url));

export const avatarsDir = path.resolve(
  process.env.AVATARS_PATH ?? path.join(__dirname, "..", "data", "avatars")
);

export function ensureAvatarsDir(): void {
  fs.mkdirSync(avatarsDir, { recursive: true });
}

export const PROFILE_LIMITS = {
  bioMax: 280,
  bioChangePerHour: 10,
  avatarMaxBytes: 256 * 1024,
  avatarMaxDim: 512,
  avatarChangesPerHour: 3,
  /** Shown as online when last_seen within this window (and not forced offline). */
  onlineWindowMs: 5 * 60 * 1000,
  awayWindowMs: 30 * 60 * 1000,
} as const;

export type PresencePreference = "online" | "away" | "offline" | "invisible";

const PRESENCE_OK = new Set<PresencePreference>(["online", "away", "offline", "invisible"]);

export function ensureForumProfileSchema(): void {
  for (const col of [
    `ALTER TABLE users ADD COLUMN bio TEXT NOT NULL DEFAULT ''`,
    `ALTER TABLE users ADD COLUMN avatar_ext TEXT`,
    `ALTER TABLE users ADD COLUMN presence TEXT NOT NULL DEFAULT 'online'`,
    `ALTER TABLE users ADD COLUMN bio_updated_at TEXT`,
    `ALTER TABLE users ADD COLUMN avatar_updated_at TEXT`,
  ]) {
    try {
      db.exec(col);
    } catch {
      /* already exists */
    }
  }
  ensureAvatarsDir();
}

/** Strip control chars, normalize, enforce length. Plain text only — no HTML/Markdown. */
export function sanitizeBio(raw: unknown): { ok: true; bio: string } | { ok: false; error: string } {
  if (typeof raw !== "string") return { ok: false, error: "invalid_bio" };
  // Reject null bytes / most controls early (keep \n \r \t then flatten).
  if (/[\u0000-\u0008\u000B\u000C\u000E-\u001F\u007F]/.test(raw)) {
    return { ok: false, error: "invalid_bio" };
  }
  let bio = raw.normalize("NFC");
  bio = bio.replace(/\r\n/g, "\n").replace(/\r/g, "\n");
  bio = bio.replace(/[ \t]+\n/g, "\n");
  bio = bio.replace(/\n{3,}/g, "\n\n");
  bio = bio.trim();
  if (bio.length > PROFILE_LIMITS.bioMax) return { ok: false, error: "bio_too_long" };
  // No HTML / script-looking fragments as plain bio (defense in depth; we also escape on render).
  if (/[<>]/.test(bio)) return { ok: false, error: "bio_forbidden_chars" };
  // Block obvious spam floods of the same char.
  if (bio.length >= 20 && /^(.)\1+$/u.test(bio)) return { ok: false, error: "bio_spammy" };
  return { ok: true, bio };
}

export function parsePresence(raw: unknown): PresencePreference | null {
  if (typeof raw !== "string") return null;
  const v = raw.trim().toLowerCase() as PresencePreference;
  return PRESENCE_OK.has(v) ? v : null;
}

export type ImageKind = "png" | "jpg" | "webp";

function readU32BE(buf: Buffer, offset: number): number {
  return buf.readUInt32BE(offset);
}

/** Magic-byte + dimension gate. No third-party image decoder required. */
export function inspectAvatarBuffer(
  buf: Buffer
): { ok: true; kind: ImageKind; width: number; height: number } | { ok: false; error: string } {
  if (!buf || buf.length < 24) return { ok: false, error: "avatar_invalid" };
  if (buf.length > PROFILE_LIMITS.avatarMaxBytes) return { ok: false, error: "avatar_too_large" };

  // PNG
  if (buf[0] === 0x89 && buf[1] === 0x50 && buf[2] === 0x4e && buf[3] === 0x47) {
    if (buf.toString("ascii", 12, 16) !== "IHDR") return { ok: false, error: "avatar_invalid" };
    const width = readU32BE(buf, 16);
    const height = readU32BE(buf, 20);
    if (!width || !height) return { ok: false, error: "avatar_invalid" };
    if (width > PROFILE_LIMITS.avatarMaxDim || height > PROFILE_LIMITS.avatarMaxDim) {
      return { ok: false, error: "avatar_too_large_dim" };
    }
    return { ok: true, kind: "png", width, height };
  }

  // JPEG
  if (buf[0] === 0xff && buf[1] === 0xd8 && buf[2] === 0xff) {
    let i = 2;
    while (i + 9 < buf.length) {
      if (buf[i] !== 0xff) {
        i++;
        continue;
      }
      const marker = buf[i + 1];
      if (marker === 0xd9 || marker === 0xda) break;
      const len = buf.readUInt16BE(i + 2);
      if (len < 2 || i + 2 + len > buf.length) break;
      // SOF0 / SOF1 / SOF2
      if (marker === 0xc0 || marker === 0xc1 || marker === 0xc2) {
        const height = buf.readUInt16BE(i + 5);
        const width = buf.readUInt16BE(i + 7);
        if (!width || !height) return { ok: false, error: "avatar_invalid" };
        if (width > PROFILE_LIMITS.avatarMaxDim || height > PROFILE_LIMITS.avatarMaxDim) {
          return { ok: false, error: "avatar_too_large_dim" };
        }
        return { ok: true, kind: "jpg", width, height };
      }
      i += 2 + len;
    }
    return { ok: false, error: "avatar_invalid" };
  }

  // WebP (RIFF....WEBP)
  if (
    buf.toString("ascii", 0, 4) === "RIFF" &&
    buf.toString("ascii", 8, 12) === "WEBP" &&
    buf.length >= 30
  ) {
    const fourcc = buf.toString("ascii", 12, 16);
    let width = 0;
    let height = 0;
    if (fourcc === "VP8 " && buf.length >= 30) {
      // Lossy: 14-byte VP8 payload header after chunk size
      width = buf.readUInt16LE(26) & 0x3fff;
      height = buf.readUInt16LE(28) & 0x3fff;
    } else if (fourcc === "VP8L" && buf.length >= 25) {
      const bits = buf.readUInt32LE(21);
      width = (bits & 0x3fff) + 1;
      height = ((bits >> 14) & 0x3fff) + 1;
    } else if (fourcc === "VP8X" && buf.length >= 30) {
      width = 1 + buf[24] + (buf[25] << 8) + (buf[26] << 16);
      height = 1 + buf[27] + (buf[28] << 8) + (buf[29] << 16);
    } else {
      return { ok: false, error: "avatar_invalid" };
    }
    if (!width || !height) return { ok: false, error: "avatar_invalid" };
    if (width > PROFILE_LIMITS.avatarMaxDim || height > PROFILE_LIMITS.avatarMaxDim) {
      return { ok: false, error: "avatar_too_large_dim" };
    }
    return { ok: true, kind: "webp", width, height };
  }

  return { ok: false, error: "avatar_unsupported_type" };
}

export function avatarPathFor(userId: string, ext: string): string {
  // userId is uuid-ish from our cryptoRandomId — still sanitize hard.
  if (!/^[a-zA-Z0-9_-]{8,64}$/.test(userId)) throw new Error("bad_user_id");
  if (!/^(png|jpg|webp)$/.test(ext)) throw new Error("bad_ext");
  return path.join(avatarsDir, `${userId}.${ext}`);
}

export function deleteAvatarFiles(userId: string): void {
  for (const ext of ["png", "jpg", "webp"] as const) {
    try {
      fs.unlinkSync(avatarPathFor(userId, ext));
    } catch {
      /* missing */
    }
  }
}

export function saveAvatar(userId: string, buf: Buffer, kind: ImageKind): string {
  deleteAvatarFiles(userId);
  const dest = avatarPathFor(userId, kind);
  // Atomic-ish write
  const tmp = dest + ".tmp";
  fs.writeFileSync(tmp, buf, { flag: "w" });
  fs.renameSync(tmp, dest);
  db.prepare(`UPDATE users SET avatar_ext = ?, avatar_updated_at = ?, updated_at = ? WHERE id = ?`).run(
    kind,
    nowIso(),
    nowIso(),
    userId
  );
  return kind;
}

export function clearAvatar(userId: string): void {
  deleteAvatarFiles(userId);
  db.prepare(`UPDATE users SET avatar_ext = NULL, avatar_updated_at = ?, updated_at = ? WHERE id = ?`).run(
    nowIso(),
    nowIso(),
    userId
  );
}

export function countRecentAvatarUpdates(userId: string): number {
  return profileMutationCount(userId, "avatar", 60 * 60 * 1000);
}

/** Hourly counters for bio / avatar mutations. */
export function ensureProfileRateSchema(): void {
  db.exec(`
CREATE TABLE IF NOT EXISTS forum_profile_mutations (
  id          TEXT PRIMARY KEY,
  user_id     TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  kind        TEXT NOT NULL CHECK(kind IN ('bio','avatar','presence')),
  created_at  TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_profile_mut_user
  ON forum_profile_mutations(user_id, kind, created_at);
`);
}

export function recordProfileMutation(userId: string, kind: "bio" | "avatar" | "presence"): void {
  db.prepare(
    `INSERT INTO forum_profile_mutations (id, user_id, kind, created_at) VALUES (?, ?, ?, ?)`
  ).run(cryptoRandomId(), userId, kind, nowIso());
}

export function profileMutationCount(
  userId: string,
  kind: "bio" | "avatar" | "presence",
  windowMs: number
): number {
  const since = new Date(Date.now() - windowMs).toISOString();
  const row = db
    .prepare(
      `SELECT COUNT(*) AS n FROM forum_profile_mutations
       WHERE user_id = ? AND kind = ? AND created_at > ?`
    )
    .get(userId, kind, since) as { n: number };
  return row.n;
}

export function escapePlainText(s: string): string {
  return s
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

export type ComputedPresence = {
  /** What others see. */
  status: "online" | "away" | "offline";
  /** User preference (may be invisible). */
  preference: PresencePreference;
  lastSeenAt: string | null;
};

export function computePresence(opts: {
  preference?: string | null;
  lastSeenAt?: string | null;
  /** When true, reveal real preference (self view). */
  self?: boolean;
}): ComputedPresence {
  const pref = (parsePresence(opts.preference ?? "online") ?? "online") as PresencePreference;
  const last = opts.lastSeenAt ? new Date(opts.lastSeenAt).getTime() : 0;
  const age = last ? Date.now() - last : Number.POSITIVE_INFINITY;

  if (!opts.self && (pref === "invisible" || pref === "offline")) {
    return { status: "offline", preference: opts.self ? pref : "offline", lastSeenAt: null };
  }

  let status: "online" | "away" | "offline" = "offline";
  if (pref === "away") status = age <= PROFILE_LIMITS.awayWindowMs ? "away" : "offline";
  else if (pref === "online") {
    if (age <= PROFILE_LIMITS.onlineWindowMs) status = "online";
    else if (age <= PROFILE_LIMITS.awayWindowMs) status = "away";
    else status = "offline";
  } else if (pref === "offline") status = "offline";
  else if (pref === "invisible") {
    // Self view only
    status =
      age <= PROFILE_LIMITS.onlineWindowMs
        ? "online"
        : age <= PROFILE_LIMITS.awayWindowMs
          ? "away"
          : "offline";
  }

  return {
    status,
    preference: pref,
    lastSeenAt: opts.self ? opts.lastSeenAt ?? null : status === "offline" ? null : opts.lastSeenAt ?? null,
  };
}

export function avatarPublicUrl(userId: string, ext: string | null | undefined): string | null {
  if (!ext || !/^(png|jpg|webp)$/.test(ext)) return null;
  return `/v1/forum/avatars/${encodeURIComponent(userId)}.${ext}`;
}
