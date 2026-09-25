import Database from "better-sqlite3";

const db = new Database("data/oak.sqlite");
const id = process.argv[2] || "8f5f8147-928b-43cb-a968-215066b056a6";
const exp = new Date(Date.now() + 86400000).toISOString();
const now = new Date().toISOString();
const r = db
  .prepare(`UPDATE licenses SET status='active', expires_at=?, updated_at=? WHERE user_id=?`)
  .run(exp, now, id);
console.log("license changes", r.changes, db.prepare(`SELECT status, expires_at FROM licenses WHERE user_id=?`).get(id));
console.log(
  "releases",
  db
    .prepare(
      `SELECT id, version, is_current, substr(sha256,1,12) AS sha FROM client_releases WHERE product_slug='dayz' ORDER BY created_at DESC LIMIT 3`
    )
    .all()
);
console.log(
  "runtime",
  db
    .prepare(
      `SELECT id, version, is_current FROM runtime_packages WHERE product_slug='dayz' ORDER BY created_at DESC LIMIT 3`
    )
    .all()
);
