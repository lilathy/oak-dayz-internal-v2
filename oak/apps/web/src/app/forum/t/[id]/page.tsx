"use client";

import Link from "next/link";
import { useParams, useRouter, useSearchParams } from "next/navigation";
import { Suspense, useCallback, useEffect, useState } from "react";
import { api, friendlyError, timeAgo, useSession } from "@/lib/api";
import {
  AuthorLine,
  FORMAT_HINT,
  Pagination,
  PostBody,
  ReportButton,
  type Post,
  type Thread,
} from "@/components/forum/Bits";

type ThreadPayload = {
  thread: Thread & { deleted?: boolean };
  posts: Post[];
  page: number;
  pageSize: number;
  total: number;
  canModerate: boolean;
};

function ThreadView() {
  const { id } = useParams<{ id: string }>();
  const params = useSearchParams();
  const router = useRouter();
  const { authed, user } = useSession();
  const page = Math.max(1, Number(params.get("page") ?? 1) || 1);

  const [data, setData] = useState<ThreadPayload | null>(null);
  const [err, setErr] = useState("");
  const [reply, setReply] = useState("");
  const [replyErr, setReplyErr] = useState("");
  const [busy, setBusy] = useState(false);
  const [editing, setEditing] = useState<string | null>(null);
  const [editBody, setEditBody] = useState("");

  const load = useCallback(
    async (target = page) => {
      try {
        const d = await api<ThreadPayload>(`/v1/forum/threads/${encodeURIComponent(id)}?page=${target}`);
        setData(d);
      } catch (e) {
        setErr(friendlyError(e));
      }
    },
    [id, page]
  );

  useEffect(() => {
    // Threads are fetched on the client so moderation state and the viewer's
    // own reactions reflect the signed-in user rather than a cached shell.
    // eslint-disable-next-line react-hooks/set-state-in-effect
    void load(page);
  }, [load, page]);

  const lastPage = data ? Math.max(1, Math.ceil((data.total + 1) / data.pageSize)) : 1;

  async function submitReply() {
    setReplyErr("");
    setBusy(true);
    try {
      await api(`/v1/forum/threads/${encodeURIComponent(id)}/posts`, {
        method: "POST",
        body: JSON.stringify({ body: reply.trim() }),
      });
      setReply("");
      // Jump to wherever the new reply landed.
      if (lastPage !== page) router.push(`/forum/t/${id}?page=${lastPage}`);
      else await load(page);
    } catch (e) {
      setReplyErr(friendlyError(e));
    } finally {
      setBusy(false);
    }
  }

  async function moderate(patch: Record<string, unknown>) {
    try {
      await api(`/v1/admin/forum/threads/${encodeURIComponent(id)}`, {
        method: "PATCH",
        body: JSON.stringify(patch),
      });
      if (patch.deleted) router.push("/forum");
      else await load(page);
    } catch (e) {
      setReplyErr(friendlyError(e));
    }
  }

  async function toggleReaction(post: Post) {
    try {
      const r = await api<{ reactions: number; reacted: boolean }>(
        `/v1/forum/posts/${post.id}/react`,
        { method: post.reacted ? "DELETE" : "POST", body: post.reacted ? undefined : JSON.stringify({}) }
      );
      setData((prev) =>
        prev
          ? {
              ...prev,
              posts: prev.posts.map((p) =>
                p.id === post.id ? { ...p, reactions: r.reactions, reacted: r.reacted } : p
              ),
            }
          : prev
      );
    } catch (e) {
      setReplyErr(friendlyError(e));
    }
  }

  if (err) {
    return (
      <>
        <header className="page-head">
          <h1>Thread not found</h1>
          <p>{err}</p>
        </header>
        <Link href="/forum">Back to the forum</Link>
      </>
    );
  }
  if (!data) return <p className="muted" style={{ padding: "48px 0" }}>Loading…</p>;

  const t = data.thread;
  const canReply = authed && (!t.locked || data.canModerate);

  return (
    <>
      <nav className="crumbs muted">
        <Link href="/forum">Forum</Link> /{" "}
        <Link href={`/forum/c/${t.categorySlug}`}>{t.categoryName}</Link>
      </nav>

      <header className="page-head thread-head">
        <h1>{t.title}</h1>
        <p className="muted">
          {t.pinned ? <span className="tag tag-pin">Pinned</span> : null}
          {t.locked ? <span className="tag tag-lock">Locked</span> : null}
          Started by {t.author?.username ?? "deleted user"} · {timeAgo(t.createdAt)} ·{" "}
          {t.replyCount} replies · {t.viewCount} views
        </p>
      </header>

      {data.canModerate ? (
        <div className="mod-bar">
          <span className="muted small">Moderation</span>
          <button className="link-btn" onClick={() => moderate({ pinned: !t.pinned })}>
            {t.pinned ? "Unpin" : "Pin"}
          </button>
          <button className="link-btn" onClick={() => moderate({ locked: !t.locked })}>
            {t.locked ? "Unlock" : "Lock"}
          </button>
          <button
            className="link-btn danger"
            onClick={() => {
              if (confirm("Delete this thread?")) moderate({ deleted: true });
            }}
          >
            Delete thread
          </button>
        </div>
      ) : null}

      <ol className="post-list">
        {data.posts.map((p, i) => (
          <li key={p.id} className={`post${p.deleted ? " is-deleted" : ""}`} id={`p${p.id}`}>
            <div className="post-side">
              <div className="post-author">{p.author?.username ?? "deleted user"}</div>
              <div className="muted small">
                {p.author?.role === "admin" ? "Staff" : `${p.author?.postCount ?? 0} posts`}
              </div>
              <div className="muted small">joined {timeAgo(p.author?.createdAt)}</div>
            </div>

            <div className="post-main">
              <div className="post-head">
                <AuthorLine author={p.author} at={p.createdAt} />
                <span className="muted small">#{(page - 1) * data.pageSize + i + 1}</span>
              </div>

              {p.deleted ? (
                <p className="muted">This post was removed.</p>
              ) : editing === p.id ? (
                <div className="form-panel form-wide">
                  <textarea rows={8} value={editBody} onChange={(e) => setEditBody(e.target.value)} />
                  <div className="form-actions">
                    <button
                      className="btn btn-primary"
                      onClick={async () => {
                        try {
                          await api(`/v1/forum/posts/${p.id}`, {
                            method: "PATCH",
                            body: JSON.stringify({ body: editBody.trim() }),
                          });
                          setEditing(null);
                          await load(page);
                        } catch (e) {
                          setReplyErr(friendlyError(e));
                        }
                      }}
                    >
                      Save
                    </button>
                    <button className="btn btn-ghost" onClick={() => setEditing(null)}>
                      Cancel
                    </button>
                  </div>
                </div>
              ) : (
                <PostBody html={p.bodyHtml} />
              )}

              {p.editedAt && !p.deleted ? (
                <p className="muted small">edited {timeAgo(p.editedAt)}</p>
              ) : null}

              {!p.deleted ? (
                <div className="post-actions">
                  <button
                    className={`link-btn${p.reacted ? " is-on" : ""}`}
                    disabled={!authed || p.author?.id === user?.id}
                    onClick={() => toggleReaction(p)}
                  >
                    Like{p.reactions ? ` (${p.reactions})` : ""}
                  </button>
                  {p.canEdit ? (
                    <button
                      className="link-btn"
                      onClick={() => {
                        setEditing(p.id);
                        setEditBody(p.body);
                      }}
                    >
                      Edit
                    </button>
                  ) : null}
                  {p.canDelete ? (
                    <button
                      className="link-btn danger"
                      onClick={async () => {
                        if (!confirm("Delete this post?")) return;
                        try {
                          await api(`/v1/forum/posts/${p.id}`, { method: "DELETE" });
                          await load(page);
                        } catch (e) {
                          setReplyErr(friendlyError(e));
                        }
                      }}
                    >
                      Delete
                    </button>
                  ) : null}
                  {authed && p.author?.id !== user?.id ? (
                    <ReportButton targetType="post" targetId={p.id} />
                  ) : null}
                </div>
              ) : null}
            </div>
          </li>
        ))}
      </ol>

      <Pagination
        page={data.page}
        total={data.total}
        pageSize={data.pageSize}
        onPage={(n) => router.push(`/forum/t/${id}?page=${n}`)}
      />

      <div className="block reply-block">
        <h2>Reply</h2>
        {!authed ? (
          <p className="muted">
            <Link href={`/login?next=/forum/t/${id}`}>Sign in</Link> to join the discussion.
          </p>
        ) : t.locked && !data.canModerate ? (
          <p className="muted">This thread is locked.</p>
        ) : user?.emailVerified === false ? (
          <p className="error">
            Verify your email address before posting. <Link href="/account">Resend the link</Link>.
          </p>
        ) : (
          <div className="form-panel form-wide">
            <textarea
              rows={7}
              value={reply}
              maxLength={10000}
              onChange={(e) => setReply(e.target.value)}
              placeholder="Add something useful…"
            />
            <span className="muted small">{FORMAT_HINT}</span>
            <div className="form-actions">
              <button
                className="btn btn-primary"
                disabled={busy || reply.trim().length < 2 || !canReply}
                onClick={submitReply}
              >
                {busy ? "Posting…" : "Post reply"}
              </button>
            </div>
          </div>
        )}
        {replyErr ? <p className="error">{replyErr}</p> : null}
      </div>
    </>
  );
}

export default function ThreadPage() {
  return (
    <Suspense fallback={<p className="muted">Loading…</p>}>
      <ThreadView />
    </Suspense>
  );
}
