"use client";

import Link from "next/link";
import { useRouter, useSearchParams } from "next/navigation";
import { Suspense, useEffect, useState } from "react";
import { api, friendlyError } from "@/lib/api";
import { ThreadRow, type Thread } from "@/components/forum/Bits";

function SearchView() {
  const params = useSearchParams();
  const router = useRouter();
  const q = params.get("q") ?? "";
  const [term, setTerm] = useState(q);
  const [threads, setThreads] = useState<Thread[] | null>(null);
  const [err, setErr] = useState("");

  useEffect(() => {
    // Keeps the input in sync when the query arrives from the URL.
    // eslint-disable-next-line react-hooks/set-state-in-effect
    setTerm(q);
    if (q.trim().length < 3) {
      setThreads([]);
      return;
    }
    setThreads(null);
    setErr("");
    api<{ threads: Thread[] }>(`/v1/forum/search?q=${encodeURIComponent(q.trim())}`)
      .then((d) => setThreads(d.threads))
      .catch((e) => {
        setErr(friendlyError(e));
        setThreads([]);
      });
  }, [q]);

  return (
    <>
      <nav className="crumbs muted">
        <Link href="/forum">Forum</Link> / Search
      </nav>
      <header className="page-head">
        <h1>Search</h1>
        <p>Threads matching titles and post content.</p>
      </header>

      <form
        className="forum-search"
        onSubmit={(e) => {
          e.preventDefault();
          if (term.trim().length >= 3)
            router.push(`/forum/search?q=${encodeURIComponent(term.trim())}`);
        }}
      >
        <input value={term} onChange={(e) => setTerm(e.target.value)} aria-label="Search" />
        <button className="btn btn-ghost" type="submit">
          Search
        </button>
      </form>

      {err ? <p className="error">{err}</p> : null}
      {threads === null ? (
        <p className="muted">Searching…</p>
      ) : threads.length === 0 ? (
        <p className="muted empty-note">
          {q.trim().length < 3 ? "Type at least 3 characters." : "No threads matched."}
        </p>
      ) : (
        <ul className="thread-list">
          {threads.map((t) => (
            <ThreadRow key={t.id} thread={t} />
          ))}
        </ul>
      )}
    </>
  );
}

export default function SearchPage() {
  return (
    <Suspense fallback={<p className="muted">Loading…</p>}>
      <SearchView />
    </Suspense>
  );
}
