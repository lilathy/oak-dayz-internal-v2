import { config } from "./config.js";
import { hashPassword } from "./crypto.js";
import { cryptoRandomId, db, nowIso } from "./db.js";

async function main() {
  const existing = db
    .prepare(`SELECT id FROM users WHERE email = ? OR username = ?`)
    .get(config.adminEmail, config.adminUsername);

  if (existing) {
    console.log("Admin already exists — skip seed.");
    console.log(`  email: ${config.adminEmail}`);
    return;
  }

  const id = cryptoRandomId();
  const t = nowIso();
  const password_hash = await hashPassword(config.adminPassword);
  db.prepare(
    `INSERT INTO users (id, email, username, password_hash, role, banned, created_at, updated_at)
     VALUES (?, ?, ?, ?, 'admin', 0, ?, ?)`
  ).run(id, config.adminEmail.toLowerCase(), config.adminUsername, password_hash, t, t);

  db.prepare(
    `INSERT INTO licenses (id, user_id, status, plan, expires_at, created_at, updated_at)
     VALUES (?, ?, 'active', 'admin', NULL, ?, ?)`
  ).run(cryptoRandomId(), id, t, t);

  console.log("Seeded admin user:");
  console.log(`  email:    ${config.adminEmail}`);
  console.log(`  username: ${config.adminUsername}`);
  console.log(`  password: (from .env ADMIN_PASSWORD)`);
  console.log("Change the password after first login.");
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
