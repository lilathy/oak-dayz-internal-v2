"use client";

import Link from "next/link";
import { useState } from "react";
import { api, friendlyError, timeAgo } from "@/lib/api";

export type Author = {
  id: string;
  username: string;
  role: string;
  createdAt: string;
  postCount: number;
  bio?: string;
  bioHtml?: string;
  avatarUrl?: string | null;
  presence?: {
    status: "online" | "away" | "offline";
    preference?: string;
    lastSeenAt?: string | null;
  };
};

export type Thread = {
  id: string;
  title: string;
  slug: string;
  categorySlug?: string;
  categoryName?: string;
  author: Author | null;
  lastPostAt: string;
  lastPoster: Author | null;
  replyCount: number;
  viewCount: number;
  pinned: boolean;
  locked: boolean;
  createdAt: string;
};

export type Post = {
  id: string;
  threadId: string;
  author: Author | null;
  bodyHtml: string;
  body: string;
  deleted: boolean;
  createdAt: string;
  editedAt: string | null;
  reactions: number;
  reacted: boolean;
  canEdit: boolean;
  canDelete: boolean;
};

/**
 * Post bodies arrive as HTML that the API built itself from an escaped copy of
 * the author's Markdown, so there is no user-controlled markup in this string.
 */
export function PostBody({ html }: { html: string }) {
  return <div className="post-body" dangerouslySetInnerHTML={{ __html: html }} />;
}

export function RoleTag({ role }: { role: string }) {
  if (role !== "admin") return null;
  return <span className="tag tag-staff">Staff</span>;
}

export function PresenceDot({ status }: { status?: string }) {
  const s = status === "online" || status === "away" ? status : "offline";
  return <span className={`presence-dot presence-${s}`} title={s} aria-label={s} />;
}

export function AuthorAvatar({ author, size = 28 }: { author: Author | null; size?: number }) {
  if (!author) return <span className="author-avatar author-avatar-empty" style={{ width: size, height: size }} />;
  if (author.avatarUrl) {
    const src = author.avatarUrl.startsWith("http")
      ? author.avatarUrl
      : `${process.env.NEXT_PUBLIC_API_URL ?? "http://127.0.0.1:8787"}${author.avatarUrl}`;
    return (
      // eslint-disable-next-line @next/next/no-img-element
      <img
        className="author-avatar"
        src={src}
        alt=""
        width={size}
        height={size}
        style={{ width: size, height: size }}
      />
    );
  }
  const initial = (author.username?.[0] ?? "?").toUpperCase();
  return (
    <span className="author-avatar author-avatar-fallback" style={{ width: size, height: size }}>
      {initial}
    </span>
  );
}

export function AuthorLine({ author, at }: { author: Author | null; at: string }) {
  if (!author) return <span className="muted">deleted user</span>;
  return (
    <span className="author-line">
      <AuthorAvatar author={author} size={22} />
      <PresenceDot status={author.presence?.status} />
      <Link href={`/forum/members/${encodeURIComponent(author.username)}`} className="author-name">
        {author.username}
      </Link>
      <RoleTag role={author.role} />
      <span className="muted"> · {timeAgo(at)}</span>
    </span>
  );
}

export function ThreadRow({ thread }: { thread: Thread }) {
  return (
    <li className="thread-row">
      <div className="thread-main">
        <div className="thread-flags">
          {thread.pinned ? <span className="tag tag-pin">Pinned</span> : null}
          {thread.locked ? <span className="tag tag-lock">Locked</span> : null}
        </div>
        <Link href={`/forum/t/${thread.id}`} className="thread-title">
          {thread.title}
        </Link>
        <div className="thread-meta muted">
          by {thread.author?.username ?? "deleted user"}
          {thread.categoryName ? (
            <>
              {" in "}
              <Link href={`/forum/c/${thread.categorySlug}`}>{thread.categoryName}</Link>
            </>
          ) : null}
        </div>
      </div>
      <div className="thread-stats">
        <span>
          <b>{thread.replyCount}</b> replies
        </span>
        <span>
          <b>{thread.viewCount}</b> views
        </span>
      </div>
      <div className="thread-last muted">{timeAgo(thread.lastPostAt)}</div>
    </li>
  );
}

export function Pagination({
  page,
  total,
  pageSize,
  onPage,
}: {
  page: number;
  total: number;
  pageSize: number;
  onPage: (n: number) => void;
}) {
  const pages = Math.max(1, Math.ceil(total / pageSize));
  if (pages <= 1) return null;
  const window = [page - 2, page - 1, page, page + 1, page + 2].filter(
    (n) => n >= 1 && n <= pages
  );
  return (
    <nav className="pager">
      <button className="btn btn-ghost" disabled={page <= 1} onClick={() => onPage(page - 1)}>
        Prev
      </button>
      {window[0]! > 1 ? <span className="muted">…</span> : null}
      {window.map((n) => (
        <button
          key={n}
          className={`pager-num${n === page ? " is-active" : ""}`}
          onClick={() => onPage(n)}
        >
          {n}
        </button>
      ))}
      {window[window.length - 1]! < pages ? <span className="muted">…</span> : null}
      <button className="btn btn-ghost" disabled={page >= pages} onClick={() => onPage(page + 1)}>
        Next
      </button>
    </nav>
  );
}

export const FORMAT_HINT =
  "Markdown: **bold**, *italic*, `code`, ```blocks```, > quotes, - lists, [text](https://link)";

export function ReportButton({
  targetType,
  targetId,
}: {
  targetType: "post" | "thread";
  targetId: string;
}) {
  const [open, setOpen] = useState(false);
  const [reason, setReason] = useState("spam");
  const [note, setNote] = useState("");
  const [msg, setMsg] = useState("");
  const [sent, setSent] = useState(false);

  if (sent) return <span className="muted small">Reported</span>;

  return (
    <span className="report-wrap">
      <button className="link-btn" type="button" onClick={() => setOpen((v) => !v)}>
        Report
      </button>
      {open ? (
        <span className="report-pop">
          <select value={reason} onChange={(e) => setReason(e.target.value)}>
            <option value="spam">Spam or advertising</option>
            <option value="abuse">Abuse or harassment</option>
            <option value="off_topic">Off topic</option>
            <option value="illegal">Illegal content</option>
            <option value="other">Other</option>
          </select>
          <input
            placeholder="Optional detail"
            value={note}
            maxLength={500}
            onChange={(e) => setNote(e.target.value)}
          />
          <button
            className="btn btn-primary"
            type="button"
            onClick={async () => {
              try {
                await api("/v1/forum/reports", {
                  method: "POST",
                  body: JSON.stringify({ targetType, targetId, reason, note }),
                });
                setSent(true);
              } catch (e) {
                setMsg(friendlyError(e));
              }
            }}
          >
            Send
          </button>
          {msg ? <span className="error small">{msg}</span> : null}
        </span>
      ) : null}
    </span>
  );
}
