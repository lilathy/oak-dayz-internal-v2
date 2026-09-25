"use client";

import Link from "next/link";
import { useRouter } from "next/navigation";
import { useEffect, useState } from "react";
import { api, friendlyError, timeAgo } from "@/lib/api";
import type { Thread } from "@/components/forum/Bits";

type Category = {
  slug: string;
  name: string;
  description: string;
  postRole: "user" | "admin";
  locked: boolean;
  threads: number;
  posts: number;
};

export default function ForumIndex() {
  const router = useRouter();
  const [cats, setCats] = useState<Category[] | null>(null);
  const [latest, setLatest] = useState<Thread[]>([]);
  const [err, setErr] = useState("");
  const [q, setQ] = useState("");

  useEffect(() => {
    api<{ categories: Category[]; latest: Thread[] }>("/v1/forum/categories")
      .then((d) => {
        setCats(d.categories);
        setLatest(d.latest);
      })
      .catch((e) => setErr(friendlyError(e)));
  }, []);

  return (
    <>
      <header className="page-head">
        <h1>Forum</h1>
        <p>Guides, setup help, and community discussion. Read freely — sign in to post.</p>
      </header>

      <form
        className="forum-search"
        onSubmit={(e) => {
          e.preventDefault();
          if (q.trim().length >= 3) router.push(`/forum/search?q=${encodeURIComponent(q.trim())}`);
        }}
      >
        <input
          value={q}
          onChange={(e) => setQ(e.target.value)}
          placeholder="Search threads and posts…"
          aria-label="Search the forum"
        />
        <button className="btn btn-ghost" type="submit">
          Search
        </button>
      </form>

      {err ? <p className="error">{err}</p> : null}

      <div className="block">
        <h2>Sections</h2>
        {!cats ? (
          <p className="muted">Loading…</p>
        ) : (
          <ul className="cat-list">
            {cats.map((c) => (
              <li key={c.slug} className="cat-row">
                <div>
                  <Link href={`/forum/c/${c.slug}`} className="cat-name">
                    {c.name}
                  </Link>
                  {c.postRole === "admin" ? <span className="tag tag-staff">Read only</span> : null}
                  <p className="muted cat-desc">{c.description}</p>
                </div>
                <div className="cat-stats muted">
                  <span>
                    <b>{c.threads}</b> threads
                  </span>
                </div>
              </li>
            ))}
          </ul>
        )}
      </div>

      {latest.length ? (
        <div className="block">
          <h2>Recent activity</h2>
          <ul className="recent-list">
            {latest.map((t) => (
              <li key={t.id}>
                <Link href={`/forum/t/${t.id}`}>{t.title}</Link>
                <span className="muted">
                  {" "}
                  · {t.categoryName} · {timeAgo(t.lastPostAt)}
                </span>
              </li>
            ))}
          </ul>
        </div>
      ) : null}

      <p className="muted small">
        Be useful, stay on topic, no reselling or leaking. The full rules are in the{" "}
        <Link href="/tos">terms</Link>.
      </p>
    </>
  );
}
