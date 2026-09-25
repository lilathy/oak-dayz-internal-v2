import { Router } from "express";
import rateLimit from "express-rate-limit";
import multer from "multer";
import fs from "node:fs";
import path from "node:path";
import { z } from "zod";
import { verifyCaptcha, type AccountUser } from "../accounts.js";
import { trackEvent } from "../analytics.js";
import { audit, cryptoRandomId, db, nowIso, type UserRow } from "../db.js";
import {
  FORUM_LIMITS,
  bumpPostCount,
  canModerate,
  checkPostingAllowed,
  excerpt,
  renderPostHtml,
  slugify,
  type ForumCategoryRow,
  type ForumPostRow,
  type ForumThreadRow,
} from "../forum.js";
import {
  PROFILE_LIMITS,
  avatarPathFor,
  clearAvatar,
  inspectAvatarBuffer,
  parsePresence,
  profileMutationCount,
  recordProfileMutation,
  sanitizeBio,
  saveAvatar,
} from "../forumProfile.js";
import { publicAuthor } from "../license.js";
import { clientIp, optionalAuth, requireAdmin, requireAuth, type AuthedRequest } from "../middleware/auth.js";

export const forumRouter = Router();
export const adminForumRouter = Router();

// Cheap outer guard; the real per-account limits live in checkPostingAllowed.
const writeLimiter = rateLimit({
  windowMs: 10 * 60 * 1000,
  max: 40,
  standardHeaders: true,
  legacyHeaders: false,
  keyGenerator: (req) => (req as AuthedRequest).user?.id ?? clientIp(req),
  message: { error: "rate_limited" },
});

const readLimiter = rateLimit({
  windowMs: 60 * 1000,
  max: 240,
  standardHeaders: true,
  legacyHeaders: false,
  message: { error: "rate_limited" },
});

// --------------------------------------------------------------- helpers

type AuthorRow = {
  id: string;
  username: string;
  role: string;
  created_at: string;
  forum_post_count: number | null;
  bio: string | null;
  avatar_ext: string | null;
  presence: string | null;
  last_seen_at: string | null;
};

function authorsFor(userIds: string[]): Map<string, ReturnType<typeof publicAuthor>> {
  const unique = [...new Set(userIds.filter(Boolean))];
  const map = new Map<string, ReturnType<typeof publicAuthor>>();
  if (!unique.length) return map;
  const placeholders = unique.map(() => "?").join(",");
  const rows = db
    .prepare(
      `SELECT id, username, role, created_at, forum_post_count, bio, avatar_ext, presence, last_seen_at
       FROM users WHERE id IN (${placeholders})`
    )
    .all(...unique) as AuthorRow[];
  for (const r of rows)
    map.set(r.id, publicAuthor({ ...r, forum_post_count: r.forum_post_count ?? 0 }));
  return map;
}

function publicCategory(c: ForumCategoryRow, stats?: { threads: number; posts: number }) {
  return {
    slug: c.slug,
    name: c.name,
    description: c.description,
    postRole: c.post_role,
    locked: !!c.locked,
    threads: stats?.threads ?? 0,
    posts: stats?.posts ?? 0,
  };
}

function publicThread(
  t: ForumThreadRow & { category_slug?: string; category_name?: string },
  authors: Map<string, ReturnType<typeof publicAuthor>>
) {
  return {
    id: t.id,
    title: t.title,
    slug: t.slug,
    categorySlug: t.category_slug,
    categoryName: t.category_name,
    author: authors.get(t.user_id) ?? null,
    lastPostAt: t.last_post_at,
    lastPoster: t.last_post_user_id ? authors.get(t.last_post_user_id) ?? null : null,
    replyCount: t.reply_count,
    viewCount: t.view_count,
    pinned: !!t.pinned,
    locked: !!t.locked,
    createdAt: t.created_at,
  };
}

function publicPost(
  p: ForumPostRow & { reactions?: number; reacted?: number },
  authors: Map<string, ReturnType<typeof publicAuthor>>,
  viewer?: UserRow
) {
  const deleted = !!p.deleted_at;
  const own = viewer?.id === p.user_id;
  const editable =
    own &&
    !deleted &&
    Date.now() - new Date(p.created_at).getTime() < FORUM_LIMITS.editWindowMs;
  return {
    id: p.id,
    threadId: p.thread_id,
    author: authors.get(p.user_id) ?? null,
    // Deleted posts keep their slot so reply context is not lost, but the text
    // never leaves the database.
    bodyHtml: deleted ? "" : p.body_html,
    body: deleted ? "" : p.body,
    deleted,
    createdAt: p.created_at,
    editedAt: p.edited_at,
    reactions: p.reactions ?? 0,
    reacted: !!p.reacted,
    canEdit: editable || (!!viewer && canModerate(viewer) && !deleted),
    canDelete: (own && !deleted) || (!!viewer && canModerate(viewer) && !deleted),
  };
}

function getCategory(slug: string): ForumCategoryRow | undefined {
  return db.prepare(`SELECT * FROM forum_categories WHERE slug = ?`).get(slug) as
    | ForumCategoryRow
    | undefined;
}

function getThread(id: string): ForumThreadRow | undefined {
  return db.prepare(`SELECT * FROM forum_threads WHERE id = ?`).get(id) as ForumThreadRow | undefined;
}

function pageFrom(value: unknown): number {
  const n = Number(value);
  return Number.isFinite(n) && n >= 1 ? Math.min(Math.floor(n), 5000) : 1;
}

/** Deduped view counting so a refresh loop cannot inflate the number. */
const recentViews = new Map<string, number>();
function countView(threadId: string, ip: string): boolean {
  const key = `${ip}:${threadId}`;
  const now = Date.now();
  const last = recentViews.get(key);
  if (last && now - last < 30 * 60 * 1000) return false;
  recentViews.set(key, now);
  if (recentViews.size > 20_000) {
    for (const [k, v] of recentViews) if (now - v > 30 * 60 * 1000) recentViews.delete(k);
  }
  db.prepare(`UPDATE forum_threads SET view_count = view_count + 1 WHERE id = ?`).run(threadId);
  return true;
}

function guardResponse(fail: { error: string; retryAfterSec?: number; detail?: string }) {
  const status =
    fail.error === "banned" ||
    fail.error === "forum_muted" ||
    fail.error === "email_unverified" ||
    fail.error === "subscription_required"
      ? 403
      : 429;
  return { status, body: fail };
}

// ------------------------------------------------------------------ read

forumRouter.get("/categories", readLimiter, (_req, res) => {
  const cats = db
    .prepare(`SELECT * FROM forum_categories ORDER BY position ASC, name ASC`)
    .all() as ForumCategoryRow[];

  const stats = db
    .prepare(
      `SELECT t.category_id AS cid,
              COUNT(DISTINCT t.id) AS threads,
              COALESCE(SUM(t.reply_count), 0) + COUNT(DISTINCT t.id) AS posts
       FROM forum_threads t
       WHERE t.deleted_at IS NULL
       GROUP BY t.category_id`
    )
    .all() as { cid: string; threads: number; posts: number }[];
  const byId = new Map(stats.map((s) => [s.cid, s]));

  const latest = db
    .prepare(
      `SELECT t.* FROM forum_threads t
       WHERE t.deleted_at IS NULL
       ORDER BY t.last_post_at DESC LIMIT 8`
    )
    .all() as ForumThreadRow[];
  const catById = new Map(cats.map((c) => [c.id, c]));
  const authors = authorsFor([
    ...latest.map((t) => t.user_id),
    ...latest.map((t) => t.last_post_user_id ?? ""),
  ]);

  res.json({
    categories: cats.map((c) => publicCategory(c, byId.get(c.id))),
    latest: latest.map((t) =>
      publicThread(
        {
          ...t,
          category_slug: catById.get(t.category_id)?.slug,
          category_name: catById.get(t.category_id)?.name,
        },
        authors
      )
    ),
  });
});

forumRouter.get("/categories/:slug", readLimiter, (req, res) => {
  const cat = getCategory(req.params.slug);
  if (!cat) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  const page = pageFrom(req.query.page);
  const offset = (page - 1) * FORUM_LIMITS.pageSize;

  const total = (
    db
      .prepare(
        `SELECT COUNT(*) AS n FROM forum_threads WHERE category_id = ? AND deleted_at IS NULL`
      )
      .get(cat.id) as { n: number }
  ).n;

  const rows = db
    .prepare(
      `SELECT * FROM forum_threads
       WHERE category_id = ? AND deleted_at IS NULL
       ORDER BY pinned DESC, last_post_at DESC
       LIMIT ? OFFSET ?`
    )
    .all(cat.id, FORUM_LIMITS.pageSize, offset) as ForumThreadRow[];

  const authors = authorsFor([
    ...rows.map((t) => t.user_id),
    ...rows.map((t) => t.last_post_user_id ?? ""),
  ]);

  res.json({
    category: publicCategory(cat, { threads: total, posts: 0 }),
    threads: rows.map((t) =>
      publicThread({ ...t, category_slug: cat.slug, category_name: cat.name }, authors)
    ),
    page,
    pageSize: FORUM_LIMITS.pageSize,
    total,
  });
});

forumRouter.get("/threads/:id", readLimiter, optionalAuth, (req: AuthedRequest, res) => {
  const thread = getThread(req.params.id);
  if (!thread || (thread.deleted_at && !(req.user && canModerate(req.user)))) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  const cat = db.prepare(`SELECT * FROM forum_categories WHERE id = ?`).get(thread.category_id) as
    | ForumCategoryRow
    | undefined;

  const page = pageFrom(req.query.page);
  const offset = (page - 1) * FORUM_LIMITS.pageSize;
  const viewerId = req.user?.id ?? "";

  const total = (
    db.prepare(`SELECT COUNT(*) AS n FROM forum_posts WHERE thread_id = ?`).get(thread.id) as {
      n: number;
    }
  ).n;

  const posts = db
    .prepare(
      `SELECT p.*,
              (SELECT COUNT(*) FROM forum_reactions r WHERE r.post_id = p.id) AS reactions,
              (SELECT COUNT(*) FROM forum_reactions r WHERE r.post_id = p.id AND r.user_id = ?) AS reacted
       FROM forum_posts p
       WHERE p.thread_id = ?
       ORDER BY p.created_at ASC
       LIMIT ? OFFSET ?`
    )
    .all(viewerId, thread.id, FORUM_LIMITS.pageSize, offset) as (ForumPostRow & {
    reactions: number;
    reacted: number;
  })[];

  if (page === 1 && countView(thread.id, clientIp(req))) thread.view_count += 1;

  const authors = authorsFor([thread.user_id, ...posts.map((p) => p.user_id)]);

  res.json({
    thread: {
      ...publicThread(
        { ...thread, category_slug: cat?.slug, category_name: cat?.name },
        authors
      ),
      deleted: !!thread.deleted_at,
    },
    posts: posts.map((p) => publicPost(p, authors, req.user)),
    page,
    pageSize: FORUM_LIMITS.pageSize,
    total,
    canModerate: !!req.user && canModerate(req.user),
  });
});

forumRouter.get("/search", readLimiter, (req, res) => {
  const q = String(req.query.q ?? "").trim();
  if (q.length < 3 || q.length > 100) {
    res.status(400).json({ error: "query_too_short" });
    return;
  }
  // Escape LIKE wildcards so a query of "%" cannot scan the whole table.
  const needle = `%${q.replace(/[\\%_]/g, (m) => `\\${m}`)}%`;
  const page = pageFrom(req.query.page);
  const offset = (page - 1) * FORUM_LIMITS.pageSize;

  const rows = db
    .prepare(
      `SELECT DISTINCT t.*, c.slug AS category_slug, c.name AS category_name
       FROM forum_threads t
       JOIN forum_categories c ON c.id = t.category_id
       LEFT JOIN forum_posts p ON p.thread_id = t.id AND p.deleted_at IS NULL
       WHERE t.deleted_at IS NULL
         AND (t.title LIKE ? ESCAPE '\\' OR p.body LIKE ? ESCAPE '\\')
       ORDER BY t.last_post_at DESC
       LIMIT ? OFFSET ?`
    )
    .all(needle, needle, FORUM_LIMITS.pageSize, offset) as (ForumThreadRow & {
    category_slug: string;
    category_name: string;
  })[];

  const authors = authorsFor([
    ...rows.map((t) => t.user_id),
    ...rows.map((t) => t.last_post_user_id ?? ""),
  ]);
  res.json({
    query: q,
    threads: rows.map((t) => publicThread(t, authors)),
    page,
    pageSize: FORUM_LIMITS.pageSize,
  });
});

forumRouter.get("/members/:username", readLimiter, (req, res) => {
  const user = db
    .prepare(
      `SELECT id, username, role, created_at, forum_post_count, bio, avatar_ext, presence, last_seen_at
       FROM users WHERE username = ?`
    )
    .get(req.params.username) as AuthorRow | undefined;
  if (!user) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  const threads = db
    .prepare(
      `SELECT t.*, c.slug AS category_slug, c.name AS category_name
       FROM forum_threads t JOIN forum_categories c ON c.id = t.category_id
       WHERE t.user_id = ? AND t.deleted_at IS NULL
       ORDER BY t.created_at DESC LIMIT 10`
    )
    .all(user.id) as (ForumThreadRow & { category_slug: string; category_name: string })[];
  const authors = authorsFor([user.id, ...threads.map((t) => t.last_post_user_id ?? "")]);
  res.json({
    member: publicAuthor({ ...user, forum_post_count: user.forum_post_count ?? 0 }),
    threads: threads.map((t) => publicThread(t, authors)),
  });
});

// ----------------------------------------------------------------- write

const threadSchema = z.object({
  title: z.string().trim().min(FORUM_LIMITS.titleMin).max(FORUM_LIMITS.titleMax),
  body: z.string().trim().min(FORUM_LIMITS.bodyMin).max(FORUM_LIMITS.bodyMax),
  captchaToken: z.string().max(4096).optional(),
});

forumRouter.post(
  "/categories/:slug/threads",
  requireAuth,
  writeLimiter,
  async (req: AuthedRequest, res) => {
    const parsed = threadSchema.safeParse(req.body);
    if (!parsed.success) {
      res.status(400).json({ error: "invalid_body", details: parsed.error.flatten() });
      return;
    }
    const cat = getCategory(req.params.slug);
    if (!cat) {
      res.status(404).json({ error: "not_found" });
      return;
    }
    const user = req.user as AccountUser;
    if (cat.locked || (cat.post_role === "admin" && user.role !== "admin")) {
      res.status(403).json({ error: "category_locked" });
      return;
    }
    if (!(await verifyCaptcha(parsed.data.captchaToken, clientIp(req)))) {
      res.status(400).json({ error: "captcha_failed" });
      return;
    }
    const fail = checkPostingAllowed(user, parsed.data.body, "thread");
    if (fail) {
      const { status, body } = guardResponse(fail);
      res.status(status).json(body);
      return;
    }

    const threadId = cryptoRandomId();
    const postId = cryptoRandomId();
    const t = nowIso();
    const html = renderPostHtml(parsed.data.body);

    db.transaction(() => {
      db.prepare(
        `INSERT INTO forum_threads
         (id, category_id, user_id, title, slug, created_at, last_post_at, last_post_user_id,
          reply_count, view_count, pinned, locked, deleted_at, deleted_by)
         VALUES (?, ?, ?, ?, ?, ?, ?, ?, 0, 0, 0, 0, NULL, NULL)`
      ).run(threadId, cat.id, user.id, parsed.data.title, slugify(parsed.data.title), t, t, user.id);
      db.prepare(
        `INSERT INTO forum_posts (id, thread_id, user_id, body, body_html, created_at)
         VALUES (?, ?, ?, ?, ?, ?)`
      ).run(postId, threadId, user.id, parsed.data.body, html, t);
      bumpPostCount(user.id, 1);
    })();

    audit("forum.thread_create", {
      actorId: user.id,
      targetId: threadId,
      meta: { category: cat.slug, title: parsed.data.title },
      ip: clientIp(req),
    });
    trackEvent("forum_thread", { userId: user.id, meta: { category: cat.slug }, ip: clientIp(req) });

    const authors = authorsFor([user.id]);
    res.status(201).json({
      thread: publicThread(
        {
          ...(getThread(threadId) as ForumThreadRow),
          category_slug: cat.slug,
          category_name: cat.name,
        },
        authors
      ),
    });
  }
);

forumRouter.post("/threads/:id/posts", requireAuth, writeLimiter, (req: AuthedRequest, res) => {
  const parsed = z
    .object({ body: z.string().trim().min(FORUM_LIMITS.bodyMin).max(FORUM_LIMITS.bodyMax) })
    .safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body", details: parsed.error.flatten() });
    return;
  }
  const thread = getThread(req.params.id);
  if (!thread || thread.deleted_at) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  const user = req.user as AccountUser;
  if (thread.locked && user.role !== "admin") {
    res.status(403).json({ error: "thread_locked" });
    return;
  }
  const cat = db.prepare(`SELECT * FROM forum_categories WHERE id = ?`).get(thread.category_id) as
    | ForumCategoryRow
    | undefined;
  if (cat?.locked && user.role !== "admin") {
    res.status(403).json({ error: "category_locked" });
    return;
  }
  const fail = checkPostingAllowed(user, parsed.data.body, "post");
  if (fail) {
    const { status, body } = guardResponse(fail);
    res.status(status).json(body);
    return;
  }

  const postId = cryptoRandomId();
  const t = nowIso();
  db.transaction(() => {
    db.prepare(
      `INSERT INTO forum_posts (id, thread_id, user_id, body, body_html, created_at)
       VALUES (?, ?, ?, ?, ?, ?)`
    ).run(postId, thread.id, user.id, parsed.data.body, renderPostHtml(parsed.data.body), t);
    db.prepare(
      `UPDATE forum_threads
       SET reply_count = reply_count + 1, last_post_at = ?, last_post_user_id = ?
       WHERE id = ?`
    ).run(t, user.id, thread.id);
    bumpPostCount(user.id, 1);
  })();

  trackEvent("forum_post", { userId: user.id, ip: clientIp(req) });

  const post = db.prepare(`SELECT * FROM forum_posts WHERE id = ?`).get(postId) as ForumPostRow;
  res.status(201).json({ post: publicPost(post, authorsFor([user.id]), req.user) });
});

forumRouter.patch("/posts/:id", requireAuth, writeLimiter, (req: AuthedRequest, res) => {
  const parsed = z
    .object({ body: z.string().trim().min(FORUM_LIMITS.bodyMin).max(FORUM_LIMITS.bodyMax) })
    .safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body" });
    return;
  }
  const post = db.prepare(`SELECT * FROM forum_posts WHERE id = ?`).get(req.params.id) as
    | ForumPostRow
    | undefined;
  if (!post || post.deleted_at) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  const user = req.user!;
  const mine = post.user_id === user.id;
  const withinWindow = Date.now() - new Date(post.created_at).getTime() < FORUM_LIMITS.editWindowMs;
  if (!canModerate(user) && (!mine || !withinWindow)) {
    res.status(403).json({ error: mine ? "edit_window_closed" : "not_your_post" });
    return;
  }
  const thread = getThread(post.thread_id);
  if (thread?.locked && !canModerate(user)) {
    res.status(403).json({ error: "thread_locked" });
    return;
  }

  // Edits re-run the full posting gate (subscription, links, mute, spam).
  const fail = checkPostingAllowed(user as AccountUser, parsed.data.body, "post");
  if (fail && fail.error !== "posting_too_fast" && fail.error !== "duplicate_post") {
    // Allow editing your own recent post without cooldown/dupe blocking the save,
    // but never skip subscription / mute / link / ban gates.
    if (
      fail.error === "subscription_required" ||
      fail.error === "banned" ||
      fail.error === "forum_muted" ||
      fail.error === "email_unverified" ||
      fail.error === "links_not_allowed_yet" ||
      fail.error === "too_many_links" ||
      fail.error === "account_too_new"
    ) {
      const { status, body } = guardResponse(fail);
      res.status(status).json(body);
      return;
    }
  }

  const t = nowIso();
  db.prepare(
    `UPDATE forum_posts SET body = ?, body_html = ?, edited_at = ?, edited_by = ? WHERE id = ?`
  ).run(parsed.data.body, renderPostHtml(parsed.data.body), t, user.id, post.id);
  if (!mine)
    audit("forum.post_edit_mod", { actorId: user.id, targetId: post.id, ip: clientIp(req) });

  const updated = db.prepare(`SELECT * FROM forum_posts WHERE id = ?`).get(post.id) as ForumPostRow;
  res.json({ post: publicPost(updated, authorsFor([updated.user_id]), user) });
});

forumRouter.delete("/posts/:id", requireAuth, writeLimiter, (req: AuthedRequest, res) => {
  const post = db.prepare(`SELECT * FROM forum_posts WHERE id = ?`).get(req.params.id) as
    | ForumPostRow
    | undefined;
  if (!post || post.deleted_at) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  const user = req.user!;
  if (post.user_id !== user.id && !canModerate(user)) {
    res.status(403).json({ error: "not_your_post" });
    return;
  }

  const t = nowIso();
  db.transaction(() => {
    db.prepare(`UPDATE forum_posts SET deleted_at = ?, deleted_by = ? WHERE id = ?`)
      .run(t, user.id, post.id);
    bumpPostCount(post.user_id, -1);
  })();
  audit("forum.post_delete", {
    actorId: user.id,
    targetId: post.id,
    meta: { author: post.user_id, moderator: post.user_id !== user.id },
    ip: clientIp(req),
  });
  res.json({ ok: true });
});

forumRouter.post("/posts/:id/react", requireAuth, writeLimiter, (req: AuthedRequest, res) => {
  const post = db.prepare(`SELECT id, user_id, deleted_at FROM forum_posts WHERE id = ?`)
    .get(req.params.id) as { id: string; user_id: string; deleted_at: string | null } | undefined;
  if (!post || post.deleted_at) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  if (post.user_id === req.user!.id) {
    res.status(400).json({ error: "cannot_react_own" });
    return;
  }
  // The primary key makes this idempotent, so double clicks cannot inflate it.
  db.prepare(
    `INSERT INTO forum_reactions (post_id, user_id, created_at) VALUES (?, ?, ?)
     ON CONFLICT(post_id, user_id) DO NOTHING`
  ).run(post.id, req.user!.id, nowIso());
  res.json({ ok: true, reactions: reactionCount(post.id), reacted: true });
});

forumRouter.delete("/posts/:id/react", requireAuth, writeLimiter, (req: AuthedRequest, res) => {
  db.prepare(`DELETE FROM forum_reactions WHERE post_id = ? AND user_id = ?`)
    .run(req.params.id, req.user!.id);
  res.json({ ok: true, reactions: reactionCount(req.params.id), reacted: false });
});

function reactionCount(postId: string): number {
  return (
    db.prepare(`SELECT COUNT(*) AS n FROM forum_reactions WHERE post_id = ?`).get(postId) as {
      n: number;
    }
  ).n;
}

// ----------------------------------------------------------- profile / avatar

const avatarUpload = multer({
  storage: multer.memoryStorage(),
  limits: { fileSize: PROFILE_LIMITS.avatarMaxBytes, files: 1 },
});

const profileWriteLimiter = rateLimit({
  windowMs: 60 * 60 * 1000,
  max: 30,
  standardHeaders: true,
  legacyHeaders: false,
  keyGenerator: (req) => (req as AuthedRequest).user?.id ?? clientIp(req),
  message: { error: "rate_limited" },
});

forumRouter.patch("/profile", requireAuth, profileWriteLimiter, (req: AuthedRequest, res) => {
  const parsed = z
    .object({
      bio: z.string().max(2000).optional(),
      presence: z.string().max(32).optional(),
    })
    .safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body" });
    return;
  }
  const user = req.user!;
  const updates: string[] = [];
  const vals: unknown[] = [];

  if (parsed.data.bio !== undefined) {
    if (profileMutationCount(user.id, "bio", 60 * 60 * 1000) >= PROFILE_LIMITS.bioChangePerHour) {
      res.status(429).json({ error: "bio_rate_limited" });
      return;
    }
    const clean = sanitizeBio(parsed.data.bio);
    if (!clean.ok) {
      res.status(400).json({ error: clean.error });
      return;
    }
    updates.push("bio = ?", "bio_updated_at = ?");
    vals.push(clean.bio, nowIso());
    recordProfileMutation(user.id, "bio");
  }

  if (parsed.data.presence !== undefined) {
    const pref = parsePresence(parsed.data.presence);
    if (!pref) {
      res.status(400).json({ error: "invalid_presence" });
      return;
    }
    if (profileMutationCount(user.id, "presence", 60 * 60 * 1000) >= 60) {
      res.status(429).json({ error: "presence_rate_limited" });
      return;
    }
    updates.push("presence = ?");
    vals.push(pref);
    recordProfileMutation(user.id, "presence");
  }

  if (!updates.length) {
    res.status(400).json({ error: "nothing_to_update" });
    return;
  }

  updates.push("updated_at = ?");
  vals.push(nowIso(), user.id);
  db.prepare(`UPDATE users SET ${updates.join(", ")} WHERE id = ?`).run(...vals);

  const row = db
    .prepare(
      `SELECT id, username, role, created_at, forum_post_count, bio, avatar_ext, presence, last_seen_at
       FROM users WHERE id = ?`
    )
    .get(user.id) as AuthorRow;
  audit("forum.profile_update", {
    actorId: user.id,
    meta: { bio: parsed.data.bio !== undefined, presence: parsed.data.presence !== undefined },
    ip: clientIp(req),
  });
  res.json({
    member: publicAuthor({ ...row, forum_post_count: row.forum_post_count ?? 0 }, { self: true }),
  });
});

forumRouter.post(
  "/profile/avatar",
  requireAuth,
  profileWriteLimiter,
  (req: AuthedRequest, res, next) => {
    avatarUpload.single("avatar")(req, res, (err: unknown) => {
      if (err) {
        const code = (err as { code?: string }).code;
        res.status(400).json({
          error: code === "LIMIT_FILE_SIZE" ? "avatar_too_large" : "avatar_upload_failed",
        });
        return;
      }
      next();
    });
  },
  (req: AuthedRequest, res) => {
    const user = req.user!;
    if (profileMutationCount(user.id, "avatar", 60 * 60 * 1000) >= PROFILE_LIMITS.avatarChangesPerHour) {
      res.status(429).json({ error: "avatar_rate_limited" });
      return;
    }
    const file = req.file;
    if (!file?.buffer?.length) {
      res.status(400).json({ error: "avatar_missing" });
      return;
    }
    const inspected = inspectAvatarBuffer(file.buffer);
    if (!inspected.ok) {
      res.status(400).json({ error: inspected.error });
      return;
    }
    try {
      saveAvatar(user.id, file.buffer, inspected.kind);
      recordProfileMutation(user.id, "avatar");
    } catch {
      res.status(500).json({ error: "avatar_save_failed" });
      return;
    }
    audit("forum.avatar_upload", {
      actorId: user.id,
      meta: { kind: inspected.kind, bytes: file.buffer.length },
      ip: clientIp(req),
    });
    const row = db
      .prepare(
        `SELECT id, username, role, created_at, forum_post_count, bio, avatar_ext, presence, last_seen_at
         FROM users WHERE id = ?`
      )
      .get(user.id) as AuthorRow;
    res.json({
      member: publicAuthor({ ...row, forum_post_count: row.forum_post_count ?? 0 }, { self: true }),
    });
  }
);

forumRouter.delete("/profile/avatar", requireAuth, profileWriteLimiter, (req: AuthedRequest, res) => {
  clearAvatar(req.user!.id);
  recordProfileMutation(req.user!.id, "avatar");
  const row = db
    .prepare(
      `SELECT id, username, role, created_at, forum_post_count, bio, avatar_ext, presence, last_seen_at
       FROM users WHERE id = ?`
    )
    .get(req.user!.id) as AuthorRow;
  res.json({
    member: publicAuthor({ ...row, forum_post_count: row.forum_post_count ?? 0 }, { self: true }),
  });
});

/** Public avatar bytes — path traversal blocked by allowlisted id+ext. */
forumRouter.get("/avatars/:file", readLimiter, (req, res) => {
  const m = /^([a-zA-Z0-9_-]{8,64})\.(png|jpg|webp)$/.exec(req.params.file ?? "");
  if (!m) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  const userId = m[1];
  const ext = m[2] as "png" | "jpg" | "webp";
  const row = db
    .prepare(`SELECT avatar_ext FROM users WHERE id = ?`)
    .get(userId) as { avatar_ext: string | null } | undefined;
  if (!row?.avatar_ext || row.avatar_ext !== ext) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  let filePath: string;
  try {
    filePath = avatarPathFor(userId, ext);
  } catch {
    res.status(404).json({ error: "not_found" });
    return;
  }
  if (!fs.existsSync(filePath)) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  res.setHeader("Cache-Control", "public, max-age=3600");
  res.setHeader("X-Content-Type-Options", "nosniff");
  const type = ext === "jpg" ? "image/jpeg" : ext === "png" ? "image/png" : "image/webp";
  res.type(type);
  fs.createReadStream(filePath).pipe(res);
});

const REPORT_REASONS = ["spam", "abuse", "off_topic", "illegal", "other"] as const;

forumRouter.post("/reports", requireAuth, writeLimiter, (req: AuthedRequest, res) => {
  const parsed = z
    .object({
      targetType: z.enum(["post", "thread"]),
      targetId: z.string().min(8).max(64),
      reason: z.enum(REPORT_REASONS),
      note: z.string().trim().max(500).optional(),
    })
    .safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body" });
    return;
  }
  const { targetType, targetId } = parsed.data;
  const exists =
    targetType === "post"
      ? db.prepare(`SELECT id FROM forum_posts WHERE id = ?`).get(targetId)
      : db.prepare(`SELECT id FROM forum_threads WHERE id = ?`).get(targetId);
  if (!exists) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  try {
    db.prepare(
      `INSERT INTO forum_reports (id, target_type, target_id, reporter_id, reason, note, status, created_at)
       VALUES (?, ?, ?, ?, ?, ?, 'open', ?)`
    ).run(
      cryptoRandomId(),
      targetType,
      targetId,
      req.user!.id,
      parsed.data.reason,
      parsed.data.note ?? "",
      nowIso()
    );
  } catch (e) {
    // The unique index means one report per person per item.
    if (e instanceof Error && e.message.includes("UNIQUE")) {
      res.json({ ok: true, alreadyReported: true });
      return;
    }
    throw e;
  }
  res.status(201).json({ ok: true });
});

// ------------------------------------------------------------ moderation

adminForumRouter.use(requireAdmin);

adminForumRouter.patch("/forum/threads/:id", (req: AuthedRequest, res) => {
  const parsed = z
    .object({
      pinned: z.boolean().optional(),
      locked: z.boolean().optional(),
      deleted: z.boolean().optional(),
      title: z.string().trim().min(FORUM_LIMITS.titleMin).max(FORUM_LIMITS.titleMax).optional(),
      categorySlug: z.string().max(64).optional(),
    })
    .safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body" });
    return;
  }
  const thread = getThread(req.params.id);
  if (!thread) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  const d = parsed.data;

  let categoryId: string | null = null;
  if (d.categorySlug) {
    const cat = getCategory(d.categorySlug);
    if (!cat) {
      res.status(404).json({ error: "category_not_found" });
      return;
    }
    categoryId = cat.id;
  }

  db.prepare(
    `UPDATE forum_threads SET
       pinned = COALESCE(?, pinned),
       locked = COALESCE(?, locked),
       title = COALESCE(?, title),
       slug = COALESCE(?, slug),
       category_id = COALESCE(?, category_id),
       deleted_at = CASE WHEN ? IS NULL THEN deleted_at WHEN ? = 1 THEN ? ELSE NULL END,
       deleted_by = CASE WHEN ? IS NULL THEN deleted_by WHEN ? = 1 THEN ? ELSE NULL END
     WHERE id = ?`
  ).run(
    d.pinned == null ? null : d.pinned ? 1 : 0,
    d.locked == null ? null : d.locked ? 1 : 0,
    d.title ?? null,
    d.title ? slugify(d.title) : null,
    categoryId,
    d.deleted == null ? null : 1,
    d.deleted ? 1 : 0,
    nowIso(),
    d.deleted == null ? null : 1,
    d.deleted ? 1 : 0,
    req.user!.id,
    thread.id
  );

  audit("forum.thread_moderate", {
    actorId: req.user!.id,
    targetId: thread.id,
    meta: d,
    ip: clientIp(req),
  });
  res.json({ ok: true });
});

adminForumRouter.get("/forum/reports", (req, res) => {
  const status = String(req.query.status ?? "open");
  const rows = db
    .prepare(
      `SELECT r.*, u.username AS reporter_username
       FROM forum_reports r JOIN users u ON u.id = r.reporter_id
       WHERE r.status = ? ORDER BY r.created_at DESC LIMIT 100`
    )
    .all(["open", "actioned", "dismissed"].includes(status) ? status : "open") as Array<{
    id: string;
    target_type: "post" | "thread";
    target_id: string;
    reporter_username: string;
    reason: string;
    note: string;
    status: string;
    created_at: string;
  }>;

  const reports = rows.map((r) => {
    const context =
      r.target_type === "post"
        ? (db
            .prepare(
              `SELECT p.body, p.thread_id, p.deleted_at, u.username
               FROM forum_posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?`
            )
            .get(r.target_id) as
            | { body: string; thread_id: string; deleted_at: string | null; username: string }
            | undefined)
        : (db
            .prepare(
              `SELECT t.title AS body, t.id AS thread_id, t.deleted_at, u.username
               FROM forum_threads t JOIN users u ON u.id = t.user_id WHERE t.id = ?`
            )
            .get(r.target_id) as
            | { body: string; thread_id: string; deleted_at: string | null; username: string }
            | undefined);
    return {
      id: r.id,
      targetType: r.target_type,
      targetId: r.target_id,
      threadId: context?.thread_id ?? null,
      author: context?.username ?? null,
      excerpt: context ? excerpt(context.body, 240) : "(deleted)",
      removed: !!context?.deleted_at,
      reporter: r.reporter_username,
      reason: r.reason,
      note: r.note,
      status: r.status,
      createdAt: r.created_at,
    };
  });
  res.json({ reports });
});

adminForumRouter.patch("/forum/reports/:id", (req: AuthedRequest, res) => {
  const parsed = z.object({ status: z.enum(["actioned", "dismissed"]) }).safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body" });
    return;
  }
  const result = db
    .prepare(`UPDATE forum_reports SET status = ?, handled_by = ?, handled_at = ? WHERE id = ?`)
    .run(parsed.data.status, req.user!.id, nowIso(), req.params.id);
  if (result.changes !== 1) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  audit("forum.report_handle", {
    actorId: req.user!.id,
    targetId: req.params.id,
    meta: parsed.data,
    ip: clientIp(req),
  });
  res.json({ ok: true });
});

adminForumRouter.post("/forum/users/:id/mute", (req: AuthedRequest, res) => {
  const parsed = z
    .object({ hours: z.number().int().min(0).max(24 * 365), reason: z.string().max(200).optional() })
    .safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body" });
    return;
  }
  const target = db.prepare(`SELECT id FROM users WHERE id = ?`).get(req.params.id);
  if (!target) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  // Zero hours lifts the mute.
  const until =
    parsed.data.hours > 0
      ? new Date(Date.now() + parsed.data.hours * 60 * 60 * 1000).toISOString()
      : null;
  db.prepare(`UPDATE users SET forum_muted_until = ? WHERE id = ?`).run(until, req.params.id);
  audit("forum.mute", {
    actorId: req.user!.id,
    targetId: req.params.id,
    meta: { until, reason: parsed.data.reason ?? null },
    ip: clientIp(req),
  });
  res.json({ ok: true, mutedUntil: until });
});

adminForumRouter.post("/forum/categories", (req: AuthedRequest, res) => {
  const parsed = z
    .object({
      slug: z.string().trim().min(2).max(48).regex(/^[a-z0-9-]+$/),
      name: z.string().trim().min(2).max(64),
      description: z.string().trim().max(300).default(""),
      position: z.number().int().min(0).max(9999).default(100),
      postRole: z.enum(["user", "admin"]).default("user"),
    })
    .safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body", details: parsed.error.flatten() });
    return;
  }
  if (getCategory(parsed.data.slug)) {
    res.status(409).json({ error: "already_exists" });
    return;
  }
  db.prepare(
    `INSERT INTO forum_categories (id, slug, name, description, position, post_role, locked, created_at)
     VALUES (?, ?, ?, ?, ?, ?, 0, ?)`
  ).run(
    cryptoRandomId(),
    parsed.data.slug,
    parsed.data.name,
    parsed.data.description,
    parsed.data.position,
    parsed.data.postRole,
    nowIso()
  );
  audit("forum.category_create", {
    actorId: req.user!.id,
    meta: parsed.data,
    ip: clientIp(req),
  });
  res.status(201).json({ ok: true });
});

adminForumRouter.patch("/forum/categories/:slug", (req: AuthedRequest, res) => {
  const parsed = z
    .object({
      name: z.string().trim().min(2).max(64).optional(),
      description: z.string().trim().max(300).optional(),
      position: z.number().int().min(0).max(9999).optional(),
      postRole: z.enum(["user", "admin"]).optional(),
      locked: z.boolean().optional(),
    })
    .safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body" });
    return;
  }
  const cat = getCategory(req.params.slug);
  if (!cat) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  const d = parsed.data;
  db.prepare(
    `UPDATE forum_categories SET
       name = COALESCE(?, name),
       description = COALESCE(?, description),
       position = COALESCE(?, position),
       post_role = COALESCE(?, post_role),
       locked = COALESCE(?, locked)
     WHERE id = ?`
  ).run(
    d.name ?? null,
    d.description ?? null,
    d.position ?? null,
    d.postRole ?? null,
    d.locked == null ? null : d.locked ? 1 : 0,
    cat.id
  );
  audit("forum.category_update", {
    actorId: req.user!.id,
    targetId: cat.id,
    meta: d,
    ip: clientIp(req),
  });
  res.json({ ok: true });
});
