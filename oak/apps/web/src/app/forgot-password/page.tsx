"use client";

import { useState } from "react";
import { api, friendlyError } from "@/lib/api";
import { Captcha, captchaConfigured } from "@/components/Captcha";

export default function ForgotPasswordPage() {
  const [email, setEmail] = useState("");
  const [msg, setMsg] = useState("");
  const [ok, setOk] = useState(false);
  const [captchaToken, setCaptchaToken] = useState("");

  return (
    <>
      <header className="page-head">
        <h1>Forgot password</h1>
        <p>We’ll email a reset link if that address has an account.</p>
      </header>
      <div className="form-panel">
        <div className="field">
          <label>Email</label>
          <input type="email" value={email} onChange={(e) => setEmail(e.target.value)} />
        </div>
        <Captcha onToken={setCaptchaToken} />
        <button
          className="btn btn-primary"
          type="button"
          disabled={!email.trim() || (captchaConfigured && !captchaToken)}
          onClick={async () => {
            try {
              const r = await api<{ message?: string }>("/v1/auth/forgot-password", {
                method: "POST",
                body: JSON.stringify({ email: email.trim(), captchaToken }),
              });
              setOk(true);
              setMsg(r.message || "If that email exists, a reset link was sent.");
            } catch (e) {
              setOk(false);
              setMsg(friendlyError(e));
            }
          }}
        >
          Send reset email
        </button>
        {msg ? <p className={ok ? "ok" : "error"}>{msg}</p> : null}
      </div>
    </>
  );
}
