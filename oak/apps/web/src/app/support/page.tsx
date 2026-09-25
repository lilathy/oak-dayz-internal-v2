"use client";

import Link from "next/link";
import { useEffect, useState } from "react";
import { api, friendlyError } from "@/lib/api";
import { AuthGate } from "@/components/AuthGate";

type Cat = { id: string; label: string };
type Ticket = { id: string; subject: string; category: string; status: string };
type TicketMessage = { id: string; username: string; isStaff?: boolean; createdAt: string; body: string };

function SupportView() {
  const [cats, setCats] = useState<Cat[]>([]);
  const [tickets, setTickets] = useState<Ticket[]>([]);
  const [category, setCategory] = useState("general");
  const [subject, setSubject] = useState("");
  const [body, setBody] = useState("");
  const [msg, setMsg] = useState("");
  const [ok, setOk] = useState(false);
  const [openId, setOpenId] = useState<string | null>(null);
  const [threadTitle, setThreadTitle] = useState("");
  const [messages, setMessages] = useState<TicketMessage[]>([]);
  const [reply, setReply] = useState("");

  async function loadList() {
    const data = await api<{ tickets: Ticket[] }>("/v1/tickets/mine");
    setTickets(data.tickets || []);
  }

  async function openTicket(id: string) {
    setOpenId(id);
    const data = await api<{ ticket: Ticket; messages: TicketMessage[] }>(`/v1/tickets/${id}`);
    setThreadTitle(`${data.ticket.subject} · ${data.ticket.status}`);
    setMessages(data.messages || []);
  }

  useEffect(() => {
    api<{ categories: Cat[] }>("/v1/tickets/categories")
      .then((d) => {
        setCats(d.categories || []);
        if (d.categories?.[0]) setCategory(d.categories[0].id);
      })
      .catch(() => {});
    // Tickets belong to the signed-in user, so they are fetched client side.
    // eslint-disable-next-line react-hooks/set-state-in-effect
    loadList().catch(() => {});
  }, []);

  return (
    <>
      <header className="page-head">
        <h1>Support</h1>
        <p>Tickets for billing, HWID, or technical issues. Check the FAQ first if you can.</p>
      </header>

      <div className="block">
        <h2>New ticket</h2>
        <div className="form-panel" style={{ maxWidth: 560 }}>
          <div className="field">
            <label>Category</label>
            <select value={category} onChange={(e) => setCategory(e.target.value)}>
              {cats.map((c) => (
                <option key={c.id} value={c.id}>
                  {c.label}
                </option>
              ))}
            </select>
          </div>
          <div className="field">
            <label>Subject</label>
            <input value={subject} onChange={(e) => setSubject(e.target.value)} />
          </div>
          <div className="field">
            <label>Message</label>
            <textarea rows={5} value={body} onChange={(e) => setBody(e.target.value)} />
          </div>
          <button
            className="btn btn-primary"
            type="button"
            onClick={async () => {
              try {
                const r = await api<{ ticket: { id: string } }>("/v1/tickets", {
                  method: "POST",
                  body: JSON.stringify({ category, subject: subject.trim(), body: body.trim() }),
                });
                setOk(true);
                setMsg("Ticket created.");
                setSubject("");
                setBody("");
                await loadList();
                openTicket(r.ticket.id);
              } catch (e) {
                setOk(false);
                setMsg(friendlyError(e));
              }
            }}
          >
            Submit ticket
          </button>
          {msg ? <p className={ok ? "ok" : "error"}>{msg}</p> : null}
        </div>
      </div>

      <div className="block">
        <h2>Your tickets</h2>
        {tickets.length === 0 ? (
          <p className="muted">No tickets yet.</p>
        ) : (
          <table style={{ width: "100%", borderCollapse: "collapse" }}>
            <thead>
              <tr>
                <th align="left">Subject</th>
                <th>Category</th>
                <th>Status</th>
                <th />
              </tr>
            </thead>
            <tbody>
              {tickets.map((t) => (
                <tr key={t.id}>
                  <td>{t.subject}</td>
                  <td className="mono">{t.category}</td>
                  <td>{t.status}</td>
                  <td>
                    <button type="button" className="nav-link" onClick={() => openTicket(t.id)}>
                      Open
                    </button>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </div>

      {openId ? (
        <div className="block">
          <h2>{threadTitle}</h2>
          {messages.map((m) => (
            <div key={m.id} style={{ padding: "12px 0", borderBottom: "1px solid var(--line)" }}>
              <div className="muted" style={{ fontSize: 12 }}>
                {m.username}
                {m.isStaff ? " (staff)" : ""} · {(m.createdAt || "").slice(0, 19)}
              </div>
              <div style={{ marginTop: 6, whiteSpace: "pre-wrap" }}>{m.body}</div>
            </div>
          ))}
          <div className="form-panel" style={{ marginTop: 16, maxWidth: 560 }}>
            <div className="field">
              <label>Reply</label>
              <textarea rows={3} value={reply} onChange={(e) => setReply(e.target.value)} />
            </div>
            <button
              className="btn btn-primary"
              type="button"
              disabled={reply.trim().length < 2}
              onClick={async () => {
                try {
                  await api(`/v1/tickets/${openId}/messages`, {
                    method: "POST",
                    body: JSON.stringify({ body: reply.trim() }),
                  });
                  setReply("");
                  await openTicket(openId);
                  await loadList();
                } catch (e) {
                  setOk(false);
                  setMsg(friendlyError(e));
                }
              }}
            >
              Send reply
            </button>
          </div>
        </div>
      ) : null}

      <p className="muted">
        Also see <Link href="/faq">FAQ</Link>.
      </p>
    </>
  );
}

export default function SupportPage() {
  return (
    <AuthGate>
      <SupportView />
    </AuthGate>
  );
}
