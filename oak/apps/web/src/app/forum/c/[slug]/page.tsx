"use client";

import Link from "next/link";
import { useParams, useRouter, useSearchParams } from "next/navigation";
import { Suspense, useCallback, useEffect, useState } from "react";
import { api, friendlyError, useSession } from "@/lib/api";
import { Pagination, ThreadRow, type Thread } from "@/components/forum/Bits";

type CategoryPayload = {
  category: { slug: string; name: string; description: string; postRole: string; locked: boolean };
  threads: Thread[];
  page: number;
  pageSize: number;
  total: number;
};

function CategoryView() {
  const { slug } = useParams<{ slug: string }>();
  const params = useSearchParams();
  const router = useRouter();
  const { user, authed } = useSession();
  const page = Math.max(1, Number(params.get("page") ?? 1) || 1);
  const [data, setData] = useState<CategoryPayload | null>(null);
  const [err, setErr] = useState("");

  useEffect(() => {
    // Clearing first shows the loading state while switching category or page.
    // eslint-disable-next-line react-hooks/set-state-in-effect
    setData(null);
    api<CategoryPayload>(`/v1/forum/categories/${encodeURIComponent(slug)}?page=${page}`)
      .then(setData)
      .catch((e) => setErr(friendlyError(e)));
  }, [slug, page]);

  const goto = useCallback(
    (n: number) => router.push(`/forum/c/${slug}?page=${n}`),
    [router, slug]
  );

  if (err) {
    return (
      <>
        <header className="page-head">
          <h1>Not found</h1>
          <p>{err}</p>
        </header>
        <Link href="/forum">Back to the forum</Link>
      </>
    );
  }

  const canPost =
    !!data &&
    !data.category.locked &&
    (data.category.postRole !== "admin" || user?.role === "admin");

  return (
    <>
      <nav className="crumbs muted">
        <Link href="/forum">Forum</Link> / {data?.category.name ?? "…"}
      </nav>

      <header className="page-head">
        <h1>{data?.category.name ?? "Loading…"}</h1>
        <p>{data?.category.description}</p>
      </header>

      <div className="row-between">
        <span className="muted small">{data ? `${data.total} threads` : ""}</span>
        {canPost ? (
          <Link className="btn btn-primary" href={`/forum/c/${slug}/new`}>
            New thread
          </Link>
        ) : authed ? (
          <span className="muted small">This section is read only.</span>
        ) : (
          <Link className="btn btn-ghost" href={`/login?next=/forum/c/${slug}`}>
            Sign in to post
          </Link>
        )}
      </div>

      {!data ? (
        <p className="muted">Loading…</p>
      ) : data.threads.length === 0 ? (
        <p className="muted empty-note">No threads here yet. Start the first one.</p>
      ) : (
        <ul className="thread-list">
          {data.threads.map((t) => (
            <ThreadRow key={t.id} thread={t} />
          ))}
        </ul>
      )}

      {data ? (
        <Pagination page={data.page} total={data.total} pageSize={data.pageSize} onPage={goto} />
      ) : null}
    </>
  );
}

export default function CategoryPage() {
  return (
    <Suspense fallback={<p className="muted">Loading…</p>}>
      <CategoryView />
    </Suspense>
  );
}
