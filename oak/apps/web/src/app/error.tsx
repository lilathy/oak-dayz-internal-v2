"use client";

import Link from "next/link";

export default function GlobalError({ reset }: { error: Error; reset: () => void }) {
  return (
    <>
      <header className="page-head">
        <h1>Something broke</h1>
        <p>The page hit an unexpected error. Nothing was lost.</p>
      </header>
      <div className="cta-row">
        <button className="btn btn-primary" type="button" onClick={reset}>
          Try again
        </button>
        <Link className="btn btn-ghost" href="/">
          Home
        </Link>
        <Link className="btn btn-ghost" href="/support">
          Contact support
        </Link>
      </div>
    </>
  );
}
