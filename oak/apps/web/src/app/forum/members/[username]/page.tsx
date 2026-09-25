"use client";

import Link from "next/link";
import { useParams } from "next/navigation";
import { useEffect, useState } from "react";
import { api, friendlyError, timeAgo } from "@/lib/api";
import {
  AuthorAvatar,
  PresenceDot,
  ThreadRow,
  type Author,
  type Thread,
} from "@/components/forum/Bits";

export default function MemberPage() {
  const { username } = useParams<{ username: string }>();
  const [member, setMember] = useState<Author | null>(null);
  const [threads, setThreads] = useState<Thread[]>([]);
  const [err, setErr] = useState("");

  useEffect(() => {
    api<{ member: Author; threads: Thread[] }>(
      `/v1/forum/members/${encodeURIComponent(username)}`
    )
      .then((d) => {
        setMember(d.member);
        setThreads(d.threads);
      })
      .catch((e) => setErr(friendlyError(e)));
  }, [username]);

  if (err) {
    return (
      <>
        <header className="page-head">
          <h1>Member not found</h1>
          <p>{err}</p>
        </header>
        <Link href="/forum">Back to the forum</Link>
      </>
    );
  }

  const status = member?.presence?.status ?? "offline";

  return (
    <>
      <nav className="crumbs muted">
        <Link href="/forum">Forum</Link> / Members
      </nav>
      <header className="page-head member-head">
        <AuthorAvatar author={member} size={72} />
        <div>
          <h1>
            {member?.username ?? "…"} <PresenceDot status={status} />
          </h1>
          <p className="muted">
            {member ? (
              <>
                <span className={`presence-label presence-${status}`}>{status}</span>
                {" · "}
                {member.role === "admin" ? "Staff · " : ""}
                {member.postCount} posts · joined {timeAgo(member.createdAt)}
              </>
            ) : (
              "Loading…"
            )}
          </p>
          {member?.bioHtml ? (
            <p className="member-bio" dangerouslySetInnerHTML={{ __html: member.bioHtml }} />
          ) : member?.bio ? (
            <p className="member-bio">{member.bio}</p>
          ) : null}
        </div>
      </header>

      <div className="block">
        <h2>Recent threads</h2>
        {threads.length === 0 ? (
          <p className="muted">No threads yet.</p>
        ) : (
          <ul className="thread-list">
            {threads.map((t) => (
              <ThreadRow key={t.id} thread={t} />
            ))}
          </ul>
        )}
      </div>
    </>
  );
}
