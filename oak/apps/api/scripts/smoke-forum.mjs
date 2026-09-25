/**
 * End-to-end checks for the hardened auth flow and the forum abuse guards.
 * Run with the API already listening: `node scripts/smoke-forum.mjs`
 */
import Database from "better-sqlite3";
import path from "node:path";
import { fileURLToPath } from "node:url";

const BASE = process.env.SMOKE_BASE ?? "http://127.0.0.1:8787";
const __dirname = path.dirname(fileURLToPath(import.meta.url));
const db = new Database(path.join(__dirname, "..", "data", "oak.sqlite"));

let pass = 0;
let fail = 0;

function check(name, ok, detail = "") {
  if (ok) {
    pass++;
    console.log(`  ok   ${name}`);
  } else {
    fail++;
    console.log(`  FAIL ${name} ${detail}`);
  }
}

async function call(pathname, { method = "GET", body, token, cookie, web = true } = {}) {
  const headers = {};
  if (web) headers["X-Oak-Client"] = "web";
  if (body) headers["Content-Type"] = "application/json";
  if (token) headers.Authorization = `Bearer ${token}`;
  if (cookie) headers.Cookie = cookie;
  const res = await fetch(`${BASE}${pathname}`, {
    method,
    headers,
    body: body ? JSON.stringify(body) : undefined,
  });
  const text = await res.text();
  let json = {};
  try {
    json = text ? JSON.parse(text) : {};
  } catch {
    json = { raw: text };
  }
  const setCookie = res.headers.getSetCookie?.() ?? [];
  return { status: res.status, json, setCookie };
}

const rand = Math.random().toString(36).slice(2, 8);
const username = `smoke_${rand}`;
const email = `${username}@example.test`;
const password = "Correct-Horse-9182!";

console.log("\n== auth hardening ==");

const weak = await call("/v1/auth/register", {
  method: "POST",
  body: { email: `w${rand}@example.test`, username: `weak_${rand}`, password: "password123" },
});
check("weak password rejected", weak.status === 400 && weak.json.error === "weak_password", JSON.stringify(weak.json));

const reserved = await call("/v1/auth/register", {
  method: "POST",
  body: { email: `r${rand}@example.test`, username: "administrator", password },
});
check("reserved username rejected", reserved.status === 409, JSON.stringify(reserved.json));

const reg = await call("/v1/auth/register", {
  method: "POST",
  body: { email, username, password },
});
check("register succeeds", reg.status === 201, JSON.stringify(reg.json));
check("no refresh token in web body", reg.status === 201 && !reg.json.refreshToken);

const cookieHeader = reg.setCookie.map((c) => c.split(";")[0]).join("; ");
const rtCookie = reg.setCookie.find((c) => c.startsWith("oak_rt="));
check("refresh cookie set", !!rtCookie, reg.setCookie.join("|"));
check("cookie is httpOnly", !!rtCookie && /HttpOnly/i.test(rtCookie));
check("cookie is sameSite scoped", !!rtCookie && /SameSite=(Lax|Strict)/i.test(rtCookie));
check("cookie scoped to /v1/auth", !!rtCookie && /Path=\/v1\/auth/i.test(rtCookie));

const refreshed = await call("/v1/auth/refresh", { method: "POST", body: {}, cookie: cookieHeader });
check("cookie refresh works", refreshed.status === 200 && !!refreshed.json.accessToken, JSON.stringify(refreshed.json));
check("rotated cookie returned", refreshed.setCookie.some((c) => c.startsWith("oak_rt=")));

const replay = await call("/v1/auth/refresh", { method: "POST", body: {}, cookie: cookieHeader });
check("used refresh token cannot be replayed", replay.status === 401, JSON.stringify(replay.json));
check(
  "reuse revokes the session family",
  replay.status === 401 && replay.json.error === "refresh_reuse",
  JSON.stringify(replay.json)
);

// Replay killed the rotated session — sign in again for the rest of the suite.
const reLogin = await call("/v1/auth/login", {
  method: "POST",
  body: { login: username, password },
});
check("re-login after reuse revoke works", reLogin.status === 200 && !!reLogin.json.accessToken);
let token = reLogin.json.accessToken;
const cookie2 = reLogin.setCookie.map((c) => c.split(";")[0]).join("; ");

const noRefresh = await call("/v1/auth/refresh", { method: "POST", body: {} });
check("refresh without cookie or token fails", noRefresh.status === 401);

console.log("\n== forum: unverified and brand new accounts ==");

const cats = await call("/v1/forum/categories");
check("categories are public", cats.status === 200 && cats.json.categories.length > 0);
const generalSlug = cats.json.categories.find((c) => c.slug === "general")?.slug ?? "general";

const anon = await call(`/v1/forum/categories/${generalSlug}/threads`, {
  method: "POST",
  body: { title: "anonymous spam thread", body: "hello" },
});
check("guests cannot post", anon.status === 401, JSON.stringify(anon.json));

const unverified = await call(`/v1/forum/categories/${generalSlug}/threads`, {
  method: "POST",
  token,
  body: { title: "unverified user thread", body: "hello there" },
});
check("unverified email blocked", unverified.status === 403 && unverified.json.error === "email_unverified", JSON.stringify(unverified.json));

// Mark the account verified, as clicking the emailed link would.
const userRow = db.prepare("SELECT id FROM users WHERE username = ?").get(username);
db.prepare("UPDATE users SET email_verified_at = ? WHERE id = ?").run(new Date().toISOString(), userRow.id);

const tooNew = await call(`/v1/forum/categories/${generalSlug}/threads`, {
  method: "POST",
  token,
  body: { title: "brand new account thread", body: "hello there" },
});
check(
  "verified free account blocked without subscription",
  tooNew.status === 403 && tooNew.json.error === "subscription_required",
  JSON.stringify(tooNew.json)
);

// Age the account past the cool-off window.
db.prepare("UPDATE users SET created_at = ? WHERE id = ?").run(
  new Date(Date.now() - 3 * 60 * 60 * 1000).toISOString(),
  userRow.id
);

// Fresh account-age gate: grant a sub then rewind created_at briefly.
db.prepare(
  `INSERT INTO licenses (id, user_id, status, plan, expires_at, created_at, updated_at)
   VALUES (?, ?, 'active', 'day7', ?, ?, ?)
   ON CONFLICT(user_id) DO UPDATE SET status='active', expires_at=excluded.expires_at, updated_at=excluded.updated_at`
).run(
  `lic_age_${rand}`,
  userRow.id,
  new Date(Date.now() + 7 * 86400000).toISOString(),
  new Date().toISOString(),
  new Date().toISOString()
);
db.prepare("UPDATE users SET created_at = ? WHERE id = ?").run(new Date().toISOString(), userRow.id);
const ageGate = await call(`/v1/forum/categories/${generalSlug}/threads`, {
  method: "POST",
  token,
  body: { title: "too new even with sub", body: "hello there" },
});
check(
  "new accounts are held back even with subscription",
  ageGate.status === 429 && ageGate.json.error === "account_too_new",
  JSON.stringify(ageGate.json)
);
db.prepare("UPDATE users SET created_at = ? WHERE id = ?").run(
  new Date(Date.now() - 3 * 60 * 60 * 1000).toISOString(),
  userRow.id
);
// Revoke for the dedicated subscription-gate section below.
db.prepare(`UPDATE licenses SET status = 'inactive', updated_at = ? WHERE user_id = ?`).run(
  new Date().toISOString(),
  userRow.id
);

console.log("\n== forum: subscription gate ==");

const noSub = await call(`/v1/forum/categories/${generalSlug}/threads`, {
  method: "POST",
  token,
  body: { title: "should be blocked without sub", body: "hello there paid only" },
});
check(
  "no subscription blocked from posting",
  noSub.status === 403 && noSub.json.error === "subscription_required",
  JSON.stringify(noSub.json)
);

// Grant an active paid license so the rest of the suite can exercise posting.
const licId = `lic_${rand}`;
db.prepare(
  `INSERT INTO licenses (id, user_id, status, plan, expires_at, created_at, updated_at)
   VALUES (?, ?, 'active', 'day7', ?, ?, ?)
   ON CONFLICT(user_id) DO UPDATE SET status='active', plan='day7', expires_at=excluded.expires_at, updated_at=excluded.updated_at`
).run(
  licId,
  userRow.id,
  new Date(Date.now() + 7 * 86400000).toISOString(),
  new Date().toISOString(),
  new Date().toISOString()
);

console.log("\n== forum: content safety ==");

const xssBody = [
  "<img src=x onerror=alert(1)>",
  "<script>alert(2)</script>",
  "[click me](javascript:alert(3))",
  "[data uri](data:text/html;base64,PHNjcmlwdD4=)",
  "**bold** and `code` and ~~gone~~",
  "> quoted line",
  "- item one",
].join("\n\n");

const thread = await call(`/v1/forum/categories/${generalSlug}/threads`, {
  method: "POST",
  token,
  body: { title: "XSS payload rendering check", body: xssBody },
});
check("thread created", thread.status === 201, JSON.stringify(thread.json));
const threadId = thread.json.thread?.id;

const view = await call(`/v1/forum/threads/${threadId}`);
const html = view.json.posts?.[0]?.bodyHtml ?? "";
check("no raw img tag", !/<img/i.test(html), html.slice(0, 160));
check("no script tag", !/<script/i.test(html));
check("no javascript: href", !/href="javascript:/i.test(html));
check("no data: href", !/href="data:/i.test(html));
check("no event handler inside a tag", !/<[^>]*\son\w+\s*=/i.test(html), html.slice(0, 200));
check("payload survives only as escaped text", html.includes("&lt;img src=x onerror=alert(1)&gt;"));
check("bold rendered", /<strong>bold<\/strong>/.test(html), html.slice(0, 200));
check("inline code rendered", /<code>code<\/code>/.test(html));
check("blockquote rendered", /<blockquote>/.test(html));
check("list rendered", /<li>item one<\/li>/.test(html));
check("view counted", (view.json.thread?.viewCount ?? 0) >= 1);

console.log("\n== forum: spam guards ==");

/** Pretends the author's last post was a while ago so the cooldown is not in the way. */
function agePosts(seconds) {
  db.prepare("UPDATE forum_posts SET created_at = ? WHERE user_id = ?").run(
    new Date(Date.now() - seconds * 1000).toISOString(),
    userRow.id
  );
}

agePosts(60);
const dupe = await call(`/v1/forum/threads/${threadId}/posts`, {
  method: "POST",
  token,
  body: { body: xssBody },
});
check("duplicate content rejected", dupe.status === 429 && dupe.json.error === "duplicate_post", JSON.stringify(dupe.json));

const linkPost = await call(`/v1/forum/threads/${threadId}/posts`, {
  method: "POST",
  token,
  body: { body: "buy cheap keys at https://spam.example.com right now" },
});
check("links blocked for untrusted accounts", linkPost.status === 429 && linkPost.json.error === "links_not_allowed_yet", JSON.stringify(linkPost.json));

agePosts(60);
const first = await call(`/v1/forum/threads/${threadId}/posts`, {
  method: "POST",
  token,
  body: { body: "A genuine first reply with enough substance." },
});
check("valid reply accepted", first.status === 201, JSON.stringify(first.json));

const flood = await call(`/v1/forum/threads/${threadId}/posts`, {
  method: "POST",
  token,
  body: { body: "Second reply straight away, should be throttled." },
});
check("cooldown enforced", flood.status === 429 && flood.json.error === "posting_too_fast", JSON.stringify(flood.json));

const tooShort = await call(`/v1/forum/threads/${threadId}/posts`, {
  method: "POST",
  token,
  body: { body: "" },
});
check("empty body rejected", tooShort.status === 400);

// Give the account enough history to earn link privileges, then confirm that
// a hostile URL still cannot break out of the href attribute.
db.prepare("UPDATE users SET forum_post_count = 25 WHERE id = ?").run(userRow.id);
agePosts(600);

const linkThread = await call(`/v1/forum/categories/${generalSlug}/threads`, {
  method: "POST",
  token,
  body: {
    title: "Link handling for trusted accounts",
    body: '[quote break](https://example.com/"onmouseover="alert(1)) and https://example.com/real',
  },
});
check("trusted account may post links", linkThread.status === 201, JSON.stringify(linkThread.json));
if (linkThread.status === 201) {
  const linkView = await call(`/v1/forum/threads/${linkThread.json.thread.id}`);
  const linkHtml = linkView.json.posts?.[0]?.bodyHtml ?? "";
  check("quote-smuggling url not linkified", !/href="[^"]*onmouseover/i.test(linkHtml), linkHtml.slice(0, 220));
  check("clean url linkified", /<a href="https:\/\/example\.com\/real"/.test(linkHtml), linkHtml.slice(0, 220));
  check("links are nofollow ugc", /rel="nofollow ugc noopener noreferrer"/.test(linkHtml));
}

const badSearch = await call("/v1/forum/search?q=%25");
check("wildcard-only search rejected", badSearch.status === 400, JSON.stringify(badSearch.json));

const search = await call("/v1/forum/search?q=payload");
check("search finds the thread", search.status === 200 && search.json.threads.length >= 1, JSON.stringify(search.json).slice(0, 160));

console.log("\n== forum: reactions, reports, moderation ==");

const selfLike = await call(`/v1/forum/posts/${first.json.post.id}/react`, { method: "POST", token, body: {} });
check("cannot like own post", selfLike.status === 400 && selfLike.json.error === "cannot_react_own", JSON.stringify(selfLike.json));

const report = await call("/v1/forum/reports", {
  method: "POST",
  token,
  body: { targetType: "post", targetId: first.json.post.id, reason: "spam", note: "smoke test" },
});
check("report accepted", report.status === 201, JSON.stringify(report.json));

const dupeReport = await call("/v1/forum/reports", {
  method: "POST",
  token,
  body: { targetType: "post", targetId: first.json.post.id, reason: "spam" },
});
check("duplicate report deduped", dupeReport.status === 200 && dupeReport.json.alreadyReported, JSON.stringify(dupeReport.json));

const notAdmin = await call(`/v1/admin/forum/threads/${threadId}`, {
  method: "PATCH",
  token,
  body: { pinned: true },
});
check("users cannot moderate", notAdmin.status === 403, JSON.stringify(notAdmin.json));

const adminLogin = await call("/v1/auth/login", {
  method: "POST",
  body: { login: process.env.ADMIN_USERNAME ?? "admin", password: process.env.ADMIN_PASSWORD ?? "OakAdmin!ChangeMe" },
});
if (adminLogin.status !== 200) {
  check("admin login", false, JSON.stringify(adminLogin.json));
} else {
  const adminToken = adminLogin.json.accessToken;
  const pin = await call(`/v1/admin/forum/threads/${threadId}`, {
    method: "PATCH",
    token: adminToken,
    body: { pinned: true, locked: true },
  });
  check("admin can pin and lock", pin.status === 200, JSON.stringify(pin.json));

  const lockedReply = await call(`/v1/forum/threads/${threadId}/posts`, {
    method: "POST",
    token,
    body: { body: "Trying to reply to a locked thread." },
  });
  check("locked thread blocks replies", lockedReply.status === 403 && lockedReply.json.error === "thread_locked", JSON.stringify(lockedReply.json));

  const reports = await call("/v1/admin/forum/reports?status=open", { token: adminToken });
  check("admin sees the report queue", reports.status === 200 && reports.json.reports.length >= 1);

  const mute = await call(`/v1/admin/forum/users/${userRow.id}/mute`, {
    method: "POST",
    token: adminToken,
    body: { hours: 24, reason: "smoke test" },
  });
  check("admin can mute", mute.status === 200 && !!mute.json.mutedUntil, JSON.stringify(mute.json));

  await call(`/v1/admin/forum/threads/${threadId}`, {
    method: "PATCH",
    token: adminToken,
    body: { locked: false },
  });
  const mutedPost = await call(`/v1/forum/threads/${threadId}/posts`, {
    method: "POST",
    token,
    body: { body: "Muted users should not get through." },
  });
  check("muted user cannot post", mutedPost.status === 403 && mutedPost.json.error === "forum_muted", JSON.stringify(mutedPost.json));

  await call(`/v1/admin/forum/users/${userRow.id}/mute`, {
    method: "POST",
    token: adminToken,
    body: { hours: 0 },
  });
  await call(`/v1/admin/forum/threads/${threadId}`, {
    method: "PATCH",
    token: adminToken,
    body: { deleted: true },
  });
  const gone = await call(`/v1/forum/threads/${threadId}`);
  check("deleted thread hidden from guests", gone.status === 404, JSON.stringify(gone.json));
}

console.log("\n== settings hardening ==");

const boot = await call("/v1/site/bootstrap");
check("bootstrap has no raw analytics html", boot.status === 200 && !("analyticsScript" in boot.json), JSON.stringify(boot.json).slice(0, 160));

if (adminLogin.status === 200) {
  const badUrl = await call("/v1/admin/settings", {
    method: "PATCH",
    token: adminLogin.json.accessToken,
    body: { analytics_src: "javascript:alert(1)" },
  });
  check("javascript: analytics url rejected", badUrl.status === 400, JSON.stringify(badUrl.json));
}

console.log("\n== forum profile hardening ==");

const bioXss = await call("/v1/forum/profile", {
  method: "PATCH",
  token,
  body: { bio: '<script>alert(1)</script><img src=x onerror=alert(2)>' },
});
check("bio rejects HTML angle brackets", bioXss.status === 400 && bioXss.json.error === "bio_forbidden_chars", JSON.stringify(bioXss.json));

const bioLong = await call("/v1/forum/profile", {
  method: "PATCH",
  token,
  body: { bio: "x".repeat(400) },
});
check("oversized bio rejected", bioLong.status === 400 && bioLong.json.error === "bio_too_long", JSON.stringify(bioLong.json));

const bioOk = await call("/v1/forum/profile", {
  method: "PATCH",
  token,
  body: { bio: "DayZ survivor. No HTML here.", presence: "away" },
});
check("plain bio + presence accepted", bioOk.status === 200 && bioOk.json.member?.bio === "DayZ survivor. No HTML here.", JSON.stringify(bioOk.json));
check("bio escaped for HTML card", bioOk.json.member?.bioHtml === "DayZ survivor. No HTML here.", JSON.stringify(bioOk.json.member?.bioHtml));
check("presence preference stored", bioOk.json.member?.presence?.preference === "away", JSON.stringify(bioOk.json.member?.presence));

const badPresence = await call("/v1/forum/profile", {
  method: "PATCH",
  token,
  body: { presence: "hacked<script>" },
});
check("invalid presence rejected", badPresence.status === 400 && badPresence.json.error === "invalid_presence", JSON.stringify(badPresence.json));

// Tiny valid 1x1 PNG
const png = Buffer.from(
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==",
  "base64"
);
const fd = new FormData();
fd.append("avatar", new Blob([png], { type: "image/png" }), "dot.png");
const av = await fetch(`${BASE}/v1/forum/profile/avatar`, {
  method: "POST",
  headers: { Authorization: `Bearer ${token}`, "X-Oak-Client": "web" },
  body: fd,
});
const avJson = await av.json().catch(() => ({}));
check("tiny png avatar accepted", av.status === 200 && !!avJson.member?.avatarUrl, JSON.stringify(avJson));

if (avJson.member?.avatarUrl) {
  const img = await fetch(`${BASE}${avJson.member.avatarUrl}`);
  check("avatar served with image content-type", img.status === 200 && /image\/png/i.test(img.headers.get("content-type") || ""), img.headers.get("content-type"));
  check("avatar has nosniff", /nosniff/i.test(img.headers.get("x-content-type-options") || ""));
}

// Polyglot / fake image
const fake = Buffer.from("<html><script>alert(1)</script>");
const fd2 = new FormData();
fd2.append("avatar", new Blob([fake], { type: "image/png" }), "x.png");
const avBad = await fetch(`${BASE}/v1/forum/profile/avatar`, {
  method: "POST",
  headers: { Authorization: `Bearer ${token}`, "X-Oak-Client": "web" },
  body: fd2,
});
const avBadJson = await avBad.json().catch(() => ({}));
check("non-image bytes rejected", avBad.status === 400, JSON.stringify(avBadJson));

const trav = await call("/v1/forum/avatars/../../etc/passwd.png");
check("avatar path traversal rejected", trav.status === 404, JSON.stringify(trav.json));

const member = await call(`/v1/forum/members/${username}`);
check("member card shows bio", member.status === 200 && member.json.member?.bio === "DayZ survivor. No HTML here.");
check("member card does not leak email", member.status === 200 && !("email" in (member.json.member || {})));

// Leave no smoke-test rubbish behind.
db.prepare("DELETE FROM users WHERE username = ?").run(username);

console.log(`\n${pass} passed, ${fail} failed\n`);
process.exit(fail ? 1 : 0);
