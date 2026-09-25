import { Router } from "express";
import { z } from "zod";
import rateLimit from "express-rate-limit";
import { audit, cryptoRandomId, db, nowIso, type UserRow } from "../db.js";
import { clientIp, requireAdmin, requireAuth, type AuthedRequest } from "../middleware/auth.js";

export const ticketsRouter = Router();

const CATEGORIES = ["general", "hwid_reset", "billing", "technical", "other"] as const;

const createLimiter = rateLimit({
  windowMs: 60 * 60 * 1000,
  max: 20,
  standardHeaders: true,
  legacyHeaders: false,
  message: { error: "rate_limited" },
});

ticketsRouter.get("/categories", (_req, res) => {
  res.json({
    categories: [
      { id: "general", label: "General support" },
      { id: "hwid_reset", label: "HWID reset" },
      { id: "billing", label: "Billing / license" },
      { id: "technical", label: "Technical / launcher" },
      { id: "other", label: "Other" },
    ],
  });
});

ticketsRouter.get("/mine", requireAuth, (req: AuthedRequest, res) => {
  const rows = db
    .prepare(
      `SELECT id, category, subject, status, created_at, updated_at
       FROM tickets WHERE user_id = ? ORDER BY updated_at DESC LIMIT 50`
    )
    .all(req.user!.id);
  res.json({ tickets: rows });
});

ticketsRouter.post("/", requireAuth, createLimiter, (req: AuthedRequest, res) => {
  const parsed = z
    .object({
      category: z.enum(CATEGORIES),
      subject: z.string().min(3).max(120),
      body: z.string().min(10).max(4000),
    })
    .safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body", details: parsed.error.flatten() });
    return;
  }
  const id = cryptoRandomId();
  const t = nowIso();
  const u = req.user!;
  db.prepare(
    `INSERT INTO tickets (id, user_id, category, subject, status, created_at, updated_at)
     VALUES (?, ?, ?, ?, 'open', ?, ?)`
  ).run(id, u.id, parsed.data.category, parsed.data.subject, t, t);
  db.prepare(
    `INSERT INTO ticket_messages (id, ticket_id, author_id, is_staff, body, created_at)
     VALUES (?, ?, ?, 0, ?, ?)`
  ).run(cryptoRandomId(), id, u.id, parsed.data.body, t);
  audit("ticket.create", {
    actorId: u.id,
    targetId: id,
    meta: { category: parsed.data.category },
    ip: clientIp(req),
  });
  res.status(201).json({ ticket: { id, category: parsed.data.category, subject: parsed.data.subject, status: "open" } });
});

ticketsRouter.get("/:id", requireAuth, (req: AuthedRequest, res) => {
  const ticket = db.prepare(`SELECT * FROM tickets WHERE id = ?`).get(req.params.id) as
    | {
        id: string;
        user_id: string;
        category: string;
        subject: string;
        status: string;
        created_at: string;
        updated_at: string;
      }
    | undefined;
  if (!ticket) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  if (ticket.user_id !== req.user!.id && req.user!.role !== "admin") {
    res.status(403).json({ error: "forbidden" });
    return;
  }
  const messages = db
    .prepare(
      `SELECT m.id, m.body, m.is_staff, m.created_at, u.username
       FROM ticket_messages m JOIN users u ON u.id = m.author_id
       WHERE m.ticket_id = ? ORDER BY m.created_at ASC`
    )
    .all(ticket.id);
  res.json({
    ticket: {
      id: ticket.id,
      category: ticket.category,
      subject: ticket.subject,
      status: ticket.status,
      createdAt: ticket.created_at,
      updatedAt: ticket.updated_at,
      userId: ticket.user_id,
    },
    messages: messages.map((m: any) => ({
      id: m.id,
      body: m.body,
      isStaff: !!m.is_staff,
      username: m.username,
      createdAt: m.created_at,
    })),
  });
});

ticketsRouter.post("/:id/messages", requireAuth, createLimiter, (req: AuthedRequest, res) => {
  const parsed = z.object({ body: z.string().min(1).max(4000) }).safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body" });
    return;
  }
  const ticket = db.prepare(`SELECT * FROM tickets WHERE id = ?`).get(req.params.id) as
    | { id: string; user_id: string; status: string }
    | undefined;
  if (!ticket) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  const isStaff = req.user!.role === "admin";
  if (ticket.user_id !== req.user!.id && !isStaff) {
    res.status(403).json({ error: "forbidden" });
    return;
  }
  if (ticket.status === "closed" && !isStaff) {
    res.status(409).json({ error: "ticket_closed" });
    return;
  }
  const t = nowIso();
  db.prepare(
    `INSERT INTO ticket_messages (id, ticket_id, author_id, is_staff, body, created_at)
     VALUES (?, ?, ?, ?, ?, ?)`
  ).run(cryptoRandomId(), ticket.id, req.user!.id, isStaff ? 1 : 0, parsed.data.body, t);
  const newStatus = isStaff ? "pending" : "open";
  db.prepare(`UPDATE tickets SET status = ?, updated_at = ? WHERE id = ?`).run(newStatus, t, ticket.id);
  res.status(201).json({ ok: true });
});

export const adminTicketsRouter = Router();
adminTicketsRouter.use(requireAdmin);

adminTicketsRouter.get("/tickets", (req, res) => {
  const status = typeof req.query.status === "string" ? req.query.status : "";
  const limit = Math.min(Number(req.query.limit ?? 100) || 100, 300);
  let sql = `SELECT t.id, t.category, t.subject, t.status, t.created_at, t.updated_at,
                    u.username, u.email, t.user_id
             FROM tickets t JOIN users u ON u.id = t.user_id WHERE 1=1`;
  const params: unknown[] = [];
  if (["open", "pending", "resolved", "closed"].includes(status)) {
    sql += ` AND t.status = ?`;
    params.push(status);
  }
  sql += ` ORDER BY t.updated_at DESC LIMIT ?`;
  params.push(limit);
  res.json({ tickets: db.prepare(sql).all(...params) });
});

adminTicketsRouter.patch("/tickets/:id", (req: AuthedRequest, res) => {
  const parsed = z
    .object({ status: z.enum(["open", "pending", "resolved", "closed"]) })
    .safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body" });
    return;
  }
  const row = db.prepare(`SELECT id FROM tickets WHERE id = ?`).get(req.params.id);
  if (!row) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  db.prepare(`UPDATE tickets SET status = ?, updated_at = ? WHERE id = ?`).run(
    parsed.data.status,
    nowIso(),
    req.params.id
  );
  audit("admin.ticket_status", {
    actorId: req.user!.id,
    targetId: req.params.id,
    meta: parsed.data,
    ip: clientIp(req),
  });
  res.json({ ok: true });
});
