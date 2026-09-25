"use client";

import { useRouter, useSearchParams } from "next/navigation";
import { Suspense, useState } from "react";
import { api, friendlyError } from "@/lib/api";

function ResetForm() {
  const router = useRouter();
  const params = useSearchParams();
  const token = params.get("token") || "";
  const [password, setPassword] = useState("");
  const [msg, setMsg] = useState(token ? "" : "Missing reset token. Use the link from your email.");
  const [ok, setOk] = useState(false);

  return (
    <>
      <header className="page-head">
        <h1>Reset password</h1>
        <p>Choose a new password (min 10 characters).</p>
      </header>
      <div className="form-panel">
        <div className="field">
          <label>New password</label>
          <input type="password" value={password} onChange={(e) => setPassword(e.target.value)} />
        </div>
        <button
          className="btn btn-primary"
          type="button"
          disabled={!token}
          onClick={async () => {
            if (password.length < 10) {
              setOk(false);
              setMsg("Password must be at least 10 characters.");
              return;
            }
            try {
              await api("/v1/auth/reset-password", {
                method: "POST",
                body: JSON.stringify({ token, password }),
              });
              setOk(true);
              setMsg("Password updated. Redirecting to login…");
              setTimeout(() => router.push("/login"), 1200);
            } catch (e) {
              setOk(false);
              setMsg(friendlyError(e));
            }
          }}
        >
          Update password
        </button>
        {msg ? <p className={ok ? "ok" : "error"}>{msg}</p> : null}
      </div>
    </>
  );
}

export default function ResetPasswordPage() {
  return (
    <Suspense>
      <ResetForm />
    </Suspense>
  );
}
