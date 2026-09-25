import Database from "better-sqlite3";
const db = new Database("data/oak.sqlite");
console.log("tickets", db.prepare(`SELECT id, substr(token_hash,1,8) h, consumed_at, expires_at, created_at FROM bootstrap_tickets ORDER BY created_at DESC LIMIT 5`).all());
console.log("leases", db.prepare(`SELECT id, expires_at, revoked_at, created_at FROM product_leases ORDER BY created_at DESC LIMIT 5`).all());
