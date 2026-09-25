"use client";

import Link from "next/link";
import { useParams, useRouter } from "next/navigation";
import { useState } from "react";
import { api, friendlyError, useSession } from "@/lib/api";
import { AuthGate } from "@/components/AuthGate";
import { Captcha, captchaConfigured } from "@/components/Captcha";
import { FORMAT_HINT } from "@/components/forum/Bits";

const TITLE_MIN = 6;
const TITLE_MAX = 140;
const BODY_MAX = 10000;

function Composer() {
  const { slug } = useParams<{ slug: string }>();
  const router = useRouter();
  const { user } = useSession();
  const [title, setTitle] = useState("");
  const [body, setBody] = useState("");
  const [captchaToken, setCaptchaToken] = useState("");
  const [err, setErr] = useState("");
  const [busy, setBusy] = useState(false);

  const titleOk = title.trim().length >= TITLE_MIN && title.trim().length <= TITLE_MAX;
  const bodyOk = body.trim().length >= 2 && body.length <= BODY_MAX;
  const captchaOk = !captchaConfigured || !!captchaToken;

  async function submit() {
    setErr("");
    setBusy(true);
    try {
      const r = await api<{ thread: { id: string } }>(
        `/v1/forum/categories/${encodeURIComponent(slug)}/threads`,
        {
          method: "POST",
          body: JSON.stringify({ title: title.trim(), body: body.trim(), captchaToken }),
        }
      );
      router.push(`/forum/t/${r.thread.id}`);
    } catch (e) {
      setErr(friendlyError(e));
      setBusy(false);
    }
  }

  return (
    <>
      <nav className="crumbs muted">
        <Link href="/forum">Forum</Link> / <Link href={`/forum/c/${slug}`}>{slug}</Link> / New
      </nav>

      <header className="page-head">
        <h1>New thread</h1>
        <p>Search first — a clear title gets answered faster.</p>
      </header>

      {user && user.emailVerified === false ? (
        <p className="error">
          Verify your email address before posting. <Link href="/account">Resend the link</Link>.
        </p>
      ) : (
        <p className="muted small">
          An active Oak subscription is required to post.{" "}
          <Link href="/plans">View plans</Link> · <Link href="/account">Forum profile</Link>
        </p>
      )}

      <div className="form-panel form-wide">
        <div className="field">
          <label htmlFor="title">Title</label>
          <input
            id="title"
            value={title}
            maxLength={TITLE_MAX}
            onChange={(e) => setTitle(e.target.value)}
            placeholder="Launcher says HWID mismatch after a Windows reinstall"
          />
          <span className="muted small">
            {title.trim().length}/{TITLE_MAX}
          </span>
        </div>

        <div className="field">
          <label htmlFor="body">Post</label>
          <textarea
            id="body"
            rows={14}
            value={body}
            maxLength={BODY_MAX}
            onChange={(e) => setBody(e.target.value)}
            placeholder="Describe what you did, what happened, and what you expected."
          />
          <span className="muted small">{FORMAT_HINT}</span>
        </div>

        <Captcha onToken={setCaptchaToken} />

        <div className="form-actions">
          <button
            className="btn btn-primary"
            type="button"
            disabled={busy || !titleOk || !bodyOk || !captchaOk}
            onClick={submit}
          >
            {busy ? "Posting…" : "Post thread"}
          </button>
          <Link className="btn btn-ghost" href={`/forum/c/${slug}`}>
            Cancel
          </Link>
        </div>
        {err ? <p className="error">{err}</p> : null}
      </div>
    </>
  );
}

export default function NewThreadPage() {
  return (
    <AuthGate>
      <Composer />
    </AuthGate>
  );
}
