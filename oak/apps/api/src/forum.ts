import { cryptoRandomId, db, nowIso, type UserRow } from "./db.js";
import { accountAgeMs, isMuted, type AccountUser } from "./accounts.js";
import { hasAnyActiveSubscription } from "./license.js";

// ------------------------------------------------------------------- types

export type ForumCategoryRow = {
  id: string;
  slug: string;
  name: string;
  description: string;
  position: number;
  post_role: "user" | "admin";
  locked: number;
  created_at: string;
};

export type ForumThreadRow = {
  id: string;
  category_id: string;
  user_id: string;
  title: string;
  slug: string;
  created_at: string;
  last_post_at: string;
  last_post_user_id: string | null;
  reply_count: number;
  view_count: number;
  pinned: number;
  locked: number;
  deleted_at: string | null;
  deleted_by: string | null;
};

export type ForumPostRow = {
  id: string;
  thread_id: string;
  user_id: string;
  body: string;
  body_html: string;
  created_at: string;
  edited_at: string | null;
  edited_by: string | null;
  deleted_at: string | null;
  deleted_by: string | null;
};

// ------------------------------------------------------------------ schema

export function ensureForumSchema(): void {
  db.exec(`
CREATE TABLE IF NOT EXISTS forum_categories (
  id           TEXT PRIMARY KEY,
  slug         TEXT NOT NULL UNIQUE COLLATE NOCASE,
  name         TEXT NOT NULL,
  description  TEXT NOT NULL DEFAULT '',
  position     INTEGER NOT NULL DEFAULT 0,
  post_role    TEXT NOT NULL DEFAULT 'user' CHECK(post_role IN ('user','admin')),
  locked       INTEGER NOT NULL DEFAULT 0,
  created_at   TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS forum_threads (
  id                TEXT PRIMARY KEY,
  category_id       TEXT NOT NULL REFERENCES forum_categories(id) ON DELETE CASCADE,
  user_id           TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  title             TEXT NOT NULL,
  slug              TEXT NOT NULL,
  created_at        TEXT NOT NULL,
  last_post_at      TEXT NOT NULL,
  last_post_user_id TEXT REFERENCES users(id) ON DELETE SET NULL,
  reply_count       INTEGER NOT NULL DEFAULT 0,
  view_count        INTEGER NOT NULL DEFAULT 0,
  pinned            INTEGER NOT NULL DEFAULT 0,
  locked            INTEGER NOT NULL DEFAULT 0,
  deleted_at        TEXT,
  deleted_by        TEXT
);
CREATE INDEX IF NOT EXISTS idx_threads_cat ON forum_threads(category_id, pinned DESC, last_post_at DESC);
CREATE INDEX IF NOT EXISTS idx_threads_user ON forum_threads(user_id, created_at);

CREATE TABLE IF NOT EXISTS forum_posts (
  id           TEXT PRIMARY KEY,
  thread_id    TEXT NOT NULL REFERENCES forum_threads(id) ON DELETE CASCADE,
  user_id      TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  body         TEXT NOT NULL,
  body_html    TEXT NOT NULL,
  created_at   TEXT NOT NULL,
  edited_at    TEXT,
  edited_by    TEXT,
  deleted_at   TEXT,
  deleted_by   TEXT
);
CREATE INDEX IF NOT EXISTS idx_posts_thread ON forum_posts(thread_id, created_at);
CREATE INDEX IF NOT EXISTS idx_posts_user ON forum_posts(user_id, created_at);

CREATE TABLE IF NOT EXISTS forum_reactions (
  post_id     TEXT NOT NULL REFERENCES forum_posts(id) ON DELETE CASCADE,
  user_id     TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  created_at  TEXT NOT NULL,
  PRIMARY KEY (post_id, user_id)
);

CREATE TABLE IF NOT EXISTS forum_reports (
  id           TEXT PRIMARY KEY,
  target_type  TEXT NOT NULL CHECK(target_type IN ('post','thread')),
  target_id    TEXT NOT NULL,
  reporter_id  TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  reason       TEXT NOT NULL,
  note         TEXT NOT NULL DEFAULT '',
  status       TEXT NOT NULL DEFAULT 'open' CHECK(status IN ('open','actioned','dismissed')),
  created_at   TEXT NOT NULL,
  handled_by   TEXT,
  handled_at   TEXT
);
CREATE INDEX IF NOT EXISTS idx_reports_status ON forum_reports(status, created_at);
CREATE UNIQUE INDEX IF NOT EXISTS idx_reports_dedupe
  ON forum_reports(target_type, target_id, reporter_id);
`);

  seedCategories();
}

function seedCategories(): void {
  const count = db.prepare(`SELECT COUNT(*) AS n FROM forum_categories`).get() as { n: number };
  if (count.n > 0) return;

  const defaults: Array<Omit<ForumCategoryRow, "id" | "created_at">> = [
    {
      slug: "announcements",
      name: "Announcements",
      description: "Release notes, downtime and status updates from the Oak team.",
      position: 10,
      post_role: "admin",
      locked: 0,
    },
    {
      slug: "general",
      name: "General Discussion",
      description: "Anything Oak or DayZ related that does not fit elsewhere.",
      position: 20,
      post_role: "user",
      locked: 0,
    },
    {
      slug: "help",
      name: "Help & Troubleshooting",
      description: "Injection problems, launcher errors and setup questions.",
      position: 30,
      post_role: "user",
      locked: 0,
    },
    {
      slug: "guides",
      name: "Guides & Configs",
      description: "Setups, config sharing and how-tos from the community.",
      position: 40,
      post_role: "user",
      locked: 0,
    },
    {
      slug: "suggestions",
      name: "Suggestions & Feedback",
      description: "Feature requests and ideas. Search before posting a duplicate.",
      position: 50,
      post_role: "user",
      locked: 0,
    },
    {
      slug: "off-topic",
      name: "Off Topic",
      description: "Everything else. Keep it civil.",
      position: 60,
      post_role: "user",
      locked: 0,
    },
  ];

  const insert = db.prepare(
    `INSERT INTO forum_categories (id, slug, name, description, position, post_role, locked, created_at)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?)`
  );
  const t = nowIso();
  const tx = db.transaction(() => {
    for (const c of defaults)
      insert.run(cryptoRandomId(), c.slug, c.name, c.description, c.position, c.post_role, c.locked, t);
  });
  tx();
}

// ------------------------------------------------------------------- slugs

export function slugify(input: string): string {
  const slug = input
    .toLowerCase()
    .normalize("NFKD")
    .replace(/[\u0300-\u036f]/g, "")
    .replace(/[^a-z0-9]+/g, "-")
    .replace(/^-+|-+$/g, "")
    .slice(0, 70);
  return slug || "thread";
}

// -------------------------------------------------------------- rendering

/**
 * Renders a strict, self-contained subset of Markdown.
 *
 * The input is HTML-escaped first and every tag in the output is produced by
 * this function, so user content can never introduce markup or scripts. No
 * third-party renderer or sanitiser is involved.
 */
export function renderPostHtml(markdown: string): string {
  const escaped = escapeHtml(markdown.replace(/\r\n/g, "\n"));

  // Fenced code is pulled out first so nothing inside it gets formatted.
  const codeBlocks: string[] = [];
  const withoutCode = escaped.replace(/```([\s\S]*?)```/g, (_m, code: string) => {
    codeBlocks.push(`<pre class="fm-code"><code>${code.replace(/^\n/, "")}</code></pre>`);
    return `\u0000CODE${codeBlocks.length - 1}\u0000`;
  });

  const lines = withoutCode.split("\n");
  const out: string[] = [];
  let paragraph: string[] = [];
  let listType: "ul" | "ol" | null = null;
  let inQuote = false;

  const flushParagraph = () => {
    if (paragraph.length) {
      out.push(`<p>${paragraph.join("<br />")}</p>`);
      paragraph = [];
    }
  };
  const closeList = () => {
    if (listType) {
      out.push(`</${listType}>`);
      listType = null;
    }
  };
  const closeQuote = () => {
    if (inQuote) {
      out.push("</blockquote>");
      inQuote = false;
    }
  };

  for (const line of lines) {
    const trimmed = line.trim();

    if (!trimmed) {
      flushParagraph();
      closeList();
      closeQuote();
      continue;
    }

    if (trimmed.startsWith("\u0000CODE")) {
      flushParagraph();
      closeList();
      closeQuote();
      out.push(trimmed);
      continue;
    }

    const quote = /^&gt;\s?(.*)$/.exec(trimmed);
    if (quote) {
      flushParagraph();
      closeList();
      if (!inQuote) {
        out.push("<blockquote>");
        inQuote = true;
      }
      out.push(`<p>${inline(quote[1]!)}</p>`);
      continue;
    }
    closeQuote();

    const heading = /^(#{1,3})\s+(.*)$/.exec(trimmed);
    if (heading) {
      flushParagraph();
      closeList();
      const level = Math.min(4, heading[1]!.length + 2); // h3..h4, never h1
      out.push(`<h${level}>${inline(heading[2]!)}</h${level}>`);
      continue;
    }

    const bullet = /^[-*]\s+(.*)$/.exec(trimmed);
    if (bullet) {
      flushParagraph();
      if (listType !== "ul") {
        closeList();
        out.push("<ul>");
        listType = "ul";
      }
      out.push(`<li>${inline(bullet[1]!)}</li>`);
      continue;
    }

    const numbered = /^\d{1,3}[.)]\s+(.*)$/.exec(trimmed);
    if (numbered) {
      flushParagraph();
      if (listType !== "ol") {
        closeList();
        out.push("<ol>");
        listType = "ol";
      }
      out.push(`<li>${inline(numbered[1]!)}</li>`);
      continue;
    }

    closeList();
    paragraph.push(inline(trimmed));
  }

  flushParagraph();
  closeList();
  closeQuote();

  return out
    .join("\n")
    .replace(/\u0000CODE(\d+)\u0000/g, (_m, i: string) => codeBlocks[Number(i)] ?? "");
}

function inline(text: string): string {
  // Inline code is protected from the other transforms.
  const codes: string[] = [];
  let s = text.replace(/`([^`]+)`/g, (_m, code: string) => {
    codes.push(`<code>${code}</code>`);
    return `\u0001C${codes.length - 1}\u0001`;
  });

  s = s.replace(/\[([^\]\n]{1,120})\]\(([^)\s]{1,500})\)/g, (match, label: string, href: string) =>
    safeUrl(href) ? anchor(href, label) : match
  );

  // Bare URLs, but not ones already inside a rendered anchor.
  s = s.replace(/(^|[\s(])((?:https?:\/\/)[^\s<)]{4,500})/g, (_m, pre: string, url: string) =>
    safeUrl(url) ? `${pre}${anchor(url, truncate(url, 60))}` : `${pre}${url}`
  );

  s = s.replace(/\*\*([^*\n]{1,300})\*\*/g, "<strong>$1</strong>");
  s = s.replace(/(^|[^*])\*([^*\n]{1,300})\*/g, "$1<em>$2</em>");
  s = s.replace(/~~([^~\n]{1,300})~~/g, "<del>$1</del>");

  return s.replace(/\u0001C(\d+)\u0001/g, (_m, i: string) => codes[Number(i)] ?? "");
}

function anchor(href: string, label: string): string {
  // href is already HTML-escaped by escapeHtml and scheme-checked by safeUrl.
  return `<a href="${href}" rel="nofollow ugc noopener noreferrer" target="_blank">${label}</a>`;
}

function safeUrl(href: string): boolean {
  // The candidate is already HTML-escaped, so an entity here means the author
  // was trying to smuggle a quote or angle bracket into the attribute.
  if (/&(quot|apos|#0*39|lt|gt);/i.test(href)) return false;
  // Only absolute http(s). Blocks javascript:, data:, vbscript: and friends.
  return /^https?:\/\/[^\s"'<>]+$/i.test(href);
}

function truncate(s: string, n: number): string {
  return s.length <= n ? s : s.slice(0, n - 1) + "…";
}

export function escapeHtml(s: string): string {
  return s
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

export function countLinks(markdown: string): number {
  return (markdown.match(/https?:\/\//gi) ?? []).length;
}

/** Short preview used in thread lists, with all formatting stripped. */
export function excerpt(markdown: string, max = 180): string {
  const plain = markdown
    .replace(/```[\s\S]*?```/g, " ")
    .replace(/[`*_~>#]/g, "")
    .replace(/\[([^\]]*)\]\([^)]*\)/g, "$1")
    .replace(/\s+/g, " ")
    .trim();
  return plain.length <= max ? plain : plain.slice(0, max - 1) + "…";
}

// ---------------------------------------------------------------- limits

export const FORUM_LIMITS = {
  /** New accounts must settle before posting — kills drive-by spam registrations. */
  minAccountAgeMs: 15 * 60 * 1000,
  /** Gap between two posts by the same author. */
  postCooldownMs: 20 * 1000,
  threadsPerHour: 5,
  postsPerHour: 20,
  /** Links are held back until an account has some history. */
  linkTrustPosts: 5,
  linkTrustAgeMs: 24 * 60 * 60 * 1000,
  maxLinksPerPost: 5,
  /** Reposting identical text is treated as flooding. */
  duplicateWindowMs: 10 * 60 * 1000,
  /** How long an author may still edit their own post. */
  editWindowMs: 30 * 60 * 1000,
  titleMin: 6,
  titleMax: 140,
  bodyMin: 2,
  bodyMax: 10_000,
  pageSize: 20,
} as const;

export type GuardFailure = { error: string; retryAfterSec?: number; detail?: string };

function countSince(table: "forum_threads" | "forum_posts", userId: string, sinceMs: number): number {
  const since = new Date(Date.now() - sinceMs).toISOString();
  const row = db
    .prepare(`SELECT COUNT(*) AS n FROM ${table} WHERE user_id = ? AND created_at > ?`)
    .get(userId, since) as { n: number };
  return row.n;
}

/**
 * Everything that must be true before a user is allowed to write, in the order
 * that produces the most useful message. Returns null when the post may proceed.
 */
export function checkPostingAllowed(
  user: AccountUser,
  body: string,
  kind: "thread" | "post"
): GuardFailure | null {
  if (user.banned) return { error: "banned" };
  if (isMuted(user)) return { error: "forum_muted", detail: user.forum_muted_until ?? undefined };
  if (!user.email_verified_at) return { error: "email_unverified" };

  // Paid subscription required to create or reply. Staff may always post
  // (announcements / moderation). Browse remains public.
  if (user.role !== "admin" && !hasAnyActiveSubscription(user.id)) {
    return { error: "subscription_required" };
  }

  const age = accountAgeMs(user);
  if (age < FORUM_LIMITS.minAccountAgeMs) {
    return {
      error: "account_too_new",
      retryAfterSec: Math.ceil((FORUM_LIMITS.minAccountAgeMs - age) / 1000),
    };
  }

  const last = db
    .prepare(`SELECT created_at FROM forum_posts WHERE user_id = ? ORDER BY created_at DESC LIMIT 1`)
    .get(user.id) as { created_at: string } | undefined;
  if (last) {
    const elapsed = Date.now() - new Date(last.created_at).getTime();
    if (elapsed < FORUM_LIMITS.postCooldownMs) {
      return {
        error: "posting_too_fast",
        retryAfterSec: Math.ceil((FORUM_LIMITS.postCooldownMs - elapsed) / 1000),
      };
    }
  }

  const hour = 60 * 60 * 1000;
  if (kind === "thread" && countSince("forum_threads", user.id, hour) >= FORUM_LIMITS.threadsPerHour)
    return { error: "thread_limit_reached" };
  if (countSince("forum_posts", user.id, hour) >= FORUM_LIMITS.postsPerHour)
    return { error: "post_limit_reached" };

  const links = countLinks(body);
  if (links > 0) {
    const trusted =
      (user.forum_post_count ?? 0) >= FORUM_LIMITS.linkTrustPosts ||
      age >= FORUM_LIMITS.linkTrustAgeMs ||
      user.role === "admin";
    if (!trusted) return { error: "links_not_allowed_yet" };
    if (links > FORUM_LIMITS.maxLinksPerPost) return { error: "too_many_links" };
  }

  const since = new Date(Date.now() - FORUM_LIMITS.duplicateWindowMs).toISOString();
  const dupe = db
    .prepare(
      `SELECT id FROM forum_posts WHERE user_id = ? AND body = ? AND created_at > ? LIMIT 1`
    )
    .get(user.id, body, since);
  if (dupe) return { error: "duplicate_post" };

  return null;
}

export function bumpPostCount(userId: string, delta: number): void {
  db.prepare(
    `UPDATE users SET forum_post_count = MAX(0, COALESCE(forum_post_count, 0) + ?) WHERE id = ?`
  ).run(delta, userId);
}

export function canModerate(user: UserRow): boolean {
  return user.role === "admin";
}
