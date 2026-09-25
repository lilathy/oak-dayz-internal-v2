import Link from "next/link";

const FAQS: { q: string; a: React.ReactNode }[] = [
  {
    q: "How do I get access?",
    a: (
      <>
        Create an account, buy a <Link href="/plans">plan</Link> or redeem a key on{" "}
        <Link href="/account">Account</Link>, then download the launcher.
      </>
    ),
  },
  {
    q: "Where is the client DLL?",
    a: (
      <>
        You don’t download it from the website. The launcher fetches the latest client from the API
        when you inject.
      </>
    ),
  },
  {
    q: "HWID locked to another PC?",
    a: (
      <>
        Use <strong>Reset hardware ID</strong> on your <Link href="/account">Account</Link> page
        (password + email confirmation). Still stuck? Open a{" "}
        <Link href="/support">Support</Link> ticket.
      </>
    ),
  },
  {
    q: "Account banned?",
    a: <>Login shows the ban reason. Open a ticket only if you believe it was a mistake.</>,
  },
  {
    q: "Forgot password?",
    a: (
      <>
        Use <Link href="/forgot-password">Forgot password</Link> — we’ll email a reset link.
      </>
    ),
  },
  {
    q: "Why can’t I post on the forum?",
    a: (
      <>
        Posting needs a verified email, and new accounts wait a few minutes before their first post.
        Links unlock once you’ve taken part in a few discussions. Limits keep spam off the{" "}
        <Link href="/forum">forum</Link>.
      </>
    ),
  },
  {
    q: "Someone is spamming or being abusive",
    a: (
      <>
        Use the report button on the post rather than replying. Reports go to moderation. See the{" "}
        <Link href="/tos">forum rules</Link>.
      </>
    ),
  },
  {
    q: "Is my session safe on a shared PC?",
    a: (
      <>
        Sign out when you’re done. From <Link href="/account">Account</Link> you can revoke
        individual sessions or sign out everywhere.
      </>
    ),
  },
];

export default function FaqPage() {
  return (
    <>
      <header className="page-head">
        <h1>FAQ</h1>
        <p>Short answers. Open a ticket if you still need help.</p>
      </header>
      <ul className="faq-list">
        {FAQS.map((item) => (
          <li key={item.q}>
            <details>
              <summary>{item.q}</summary>
              <p>{item.a}</p>
            </details>
          </li>
        ))}
      </ul>
    </>
  );
}
