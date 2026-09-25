import Link from "next/link";

export default function NotFound() {
  return (
    <>
      <header className="page-head">
        <h1>404</h1>
        <p>That page does not exist, or it moved.</p>
      </header>
      <div className="cta-row">
        <Link className="btn btn-primary" href="/">
          Home
        </Link>
        <Link className="btn btn-ghost" href="/forum">
          Forum
        </Link>
        <Link className="btn btn-ghost" href="/support">
          Support
        </Link>
      </div>
    </>
  );
}
