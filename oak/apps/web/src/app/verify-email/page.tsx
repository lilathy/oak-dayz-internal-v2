"use client";

import Link from "next/link";
import { useSearchParams } from "next/navigation";
import { Suspense, useEffect, useState } from "react";
import { api, friendlyError, reloadUser } from "@/lib/api";

function VerifyView() {
  const params = useSearchParams();
  const token = params.get("token") ?? "";
  const [state, setState] = useState<"working" | "done" | "failed">(() =>
    token ? "working" : "failed"
  );
  const [msg, setMsg] = useState(() =>
    token ? "" : "This link is missing its token."
  );

  useEffect(() => {
    if (!token) return;
    api("/v1/auth/verify", { method: "POST", body: JSON.stringify({ token }) })
      .then(async () => {
        await reloadUser();
        setState("done");
      })
      .catch((e) => {
        setState("failed");
        setMsg(friendlyError(e));
      });
  }, [token]);

  return (
    <>
      <header className="page-head">
        <h1>Email verification</h1>
        <p>
          {state === "working"
            ? "Checking your link…"
            : state === "done"
              ? "Your email address is confirmed."
              : "We could not confirm this link."}
        </p>
      </header>
      {state === "failed" ? <p className="error">{msg}</p> : null}
      {state === "done" ? (
        <p className="ok">
          You can now post on the <Link href="/forum">forum</Link>.
        </p>
      ) : null}
      <p className="muted">
        <Link href="/account">Back to your account</Link>
      </p>
    </>
  );
}

export default function VerifyEmailPage() {
  return (
    <Suspense fallback={<p className="muted">Loading…</p>}>
      <VerifyView />
    </Suspense>
  );
}
