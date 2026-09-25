import Link from "next/link";

/**
 * Bump this whenever the wording below changes — it is the date shown to
 * users so they know whether they are looking at the current terms.
 */
const LAST_UPDATED = "August 3, 2026";

export default function TosPage() {
  return (
    <>
      <header className="page-head">
        <h1>Terms of Service</h1>
        <p>Last updated: {LAST_UPDATED}</p>
      </header>

      <div className="block">
        <p className="muted">
          These Terms of Service (&ldquo;Terms&rdquo;) are a binding agreement between you and{" "}
          <strong>Shine</strong> (&ldquo;Shine&rdquo;, &ldquo;we&rdquo;, &ldquo;us&rdquo;), a company
          organized in the United States, governing your access to and use of this website, our
          software products (including the Oak DayZ client and launcher), the forum, and any related
          services (together, the &ldquo;Service&rdquo;).
        </p>
        <p className="muted">
          <strong>
            By creating an account, purchasing a license, downloading the launcher, or otherwise
            continuing to use the Service, you agree to these Terms.
          </strong>{" "}
          If you do not agree, do not use the Service.
        </p>
      </div>

      <div className="block">
        <h2>1. Products &amp; licenses</h2>
        <p className="muted">
          Shine sells software licenses for DayZ and, over time, other games and products. Unless
          stated otherwise at the time of purchase, a license is:
        </p>
        <ul className="rules-list muted">
          <li>Time-limited (e.g. 1, 7, or 30 days) or, where offered, permanent/lifetime.</li>
          <li>Personal to your account and bound to a single hardware ID (HWID).</li>
          <li>Non-transferable and revocable by us for a breach of these Terms.</li>
          <li>A license to use the software, not a sale of it. We retain all ownership.</li>
        </ul>
        <p className="muted">
          These Terms apply to every current and future Shine product, license type, and plan, even
          ones not explicitly named here.
        </p>
      </div>

      <div className="block">
        <h2>2. Accounts, HWID &amp; security</h2>
        <ul className="rules-list muted">
          <li>You are responsible for your login credentials and everything done from your account.</li>
          <li>One account is bound to one hardware ID. HWID resets are handled through a support ticket.</li>
          <li>
            Sharing your account or license, or reselling access, is not permitted unless an
            administrator has explicitly authorized it in writing. Unauthorized sharing or reselling
            is treated as a ban-level violation.
          </li>
          <li>Report a compromised account to support immediately — we are not liable for losses caused by a compromised account.</li>
        </ul>
      </div>

      <div className="block">
        <h2>3. Payments &amp; refunds</h2>
        <p className="muted">
          Payments are processed by NOWPayments (cryptocurrency and, where enabled, card/fiat via
          their payment partners). By purchasing, you also agree to NOWPayments&rsquo; applicable terms
          and those of any fiat on-ramp partner shown at checkout. You are responsible for any taxes
          owed on your purchase. Prices, plans, and included features may change at any time without
          notice for future purchases.
        </p>
        <p className="muted">
          <strong>All sales are final.</strong> We do not offer refunds, in whole or in part, except at
          the sole discretion of an administrator, and only where the product is verifiably faulty on
          your system and our support team is unable to resolve the issue after a genuine attempt to
          fix it. There is no refund for a product that works as intended, for detection by a
          third-party anti-cheat, for a ban on the game platform, or for a change of mind.
        </p>
        <p className="muted">
          Chargebacks and payment disputes are treated as fraud against Shine. An account associated
          with a chargeback will be permanently banned, all remaining license time will be forfeited,
          and we may share information about the dispute with our payment processor.
        </p>
      </div>

      <div className="block">
        <h2>4. Acceptable use</h2>
        <ul className="rules-list muted">
          <li>No reverse engineering, decompiling, or attempting to extract the source of our software.</li>
          <li>No redistributing, leaking, cracking, or sublicensing the client, launcher, or license keys.</li>
          <li>No attacking, scraping, or interfering with our website, API, or infrastructure.</li>
          <li>No using the Service to violate any law or the rights of a third party.</li>
        </ul>
      </div>

      <div className="block">
        <h2>5. Third-party platforms &amp; risk</h2>
        <p className="muted">
          Our products are designed to modify the behavior of third-party games such as DayZ. Using
          them is very likely to violate the terms of service of the game and its publisher (e.g.
          Bohemia Interactive), and may result in a game ban, hardware ban, or loss of access to your
          game account, independent of anything Shine does. You use the Service entirely at your own
          risk. Shine is not affiliated with, endorsed by, or responsible to any game publisher, and we
          are not responsible for any action a third party takes against your game account, platform
          account, or hardware as a result of your use of the Service.
        </p>
      </div>

      <div className="block">
        <h2>6. Availability, updates &amp; no warranty</h2>
        <p className="muted">
          We do not guarantee that any product will remain undetected, compatible with the current
          game version, or available at all times. Products may stop working, be taken offline, or be
          discontinued at any time, including without advance notice, for reasons that include but
          are not limited to game updates and anti-cheat changes. The{" "}
          <Link href="/changelog">changelog</Link> shows when a product was last updated so you can
          judge for yourself how actively it is maintained before you buy.
        </p>
        <p className="muted">
          <strong>
            THE SERVICE IS PROVIDED &ldquo;AS IS&rdquo; AND &ldquo;AS AVAILABLE&rdquo;, WITHOUT
            WARRANTIES OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING MERCHANTABILITY, FITNESS FOR A
            PARTICULAR PURPOSE, OR NON-INFRINGEMENT.
          </strong>
        </p>
      </div>

      <div className="block">
        <h2>7. Limitation of liability</h2>
        <p className="muted">
          To the maximum extent permitted by law, Shine will not be liable for any indirect,
          incidental, special, consequential, or punitive damages, or for any loss of data, game
          account, or hardware access, arising from your use of the Service. Our total liability for
          any claim relating to the Service will not exceed the amount you paid to Shine in the 30 days
          before the claim arose.
        </p>
      </div>

      <div className="block">
        <h2>8. Bans, suspensions &amp; appeals</h2>
        <p className="muted">
          We may suspend, mute, or permanently ban any account that violates these Terms, at an
          administrator&rsquo;s discretion. A ban immediately ends the license: any remaining license
          time is forfeited and is not refunded or restored. Not every ban is permanent — length and
          severity are decided case by case by administrators, and their decision on any ban or appeal
          is final. If you believe a ban was made in error, you may appeal by opening a{" "}
          <Link href="/support">support ticket</Link>; an appeal does not pause or guarantee the
          outcome of the ban.
        </p>
      </div>

      <div className="block">
        <h2>9. Forum rules</h2>
        <p className="muted">
          The <Link href="/forum">forum</Link> is a shared space and moderated as one. Keep threads on
          topic and in the right section, search before opening a duplicate, and write enough detail
          that someone can actually help you.
        </p>
        <ul className="rules-list muted">
          <li>No advertising, referral links, unauthorized reselling, or key trading.</li>
          <li>No harassment, hate speech, doxxing, or threats.</li>
          <li>No leaking or redistributing the client, loader, or any paid material.</li>
          <li>No malware, phishing links, or fake &ldquo;fixes&rdquo;.</li>
          <li>No bumping, spam, flooding, or alternate accounts to evade limits.</li>
          <li>Report rule breaks with the report button instead of replying to them.</li>
        </ul>
        <p className="muted">
          Posting requires a verified email address. New accounts have a short waiting period and
          cannot post links until they have taken part in a few discussions. Moderators may edit,
          lock, move, or remove content, and may mute or ban accounts, without notice.
        </p>
        <p className="muted">
          You keep ownership of anything you post. By posting, you grant Shine a non-exclusive,
          worldwide, royalty-free license to host, display, and distribute that content as part of
          operating the Service (for example, showing your posts to other users).
        </p>
      </div>

      <div className="block">
        <h2>10. Support</h2>
        <p className="muted">
          Support is provided through the <Link href="/support">ticket system</Link>. We do not commit
          to a fixed response time, but tickets are handled as soon as reasonably possible.
        </p>
      </div>

      <div className="block">
        <h2>11. Privacy</h2>
        <p className="muted">
          We collect the information needed to run the Service: your email and username, a hashed
          password, a hashed hardware ID, license and payment status, and basic technical logs (such
          as IP address and timestamps) used for security, fraud prevention, and support. Payment card
          details are handled by NOWPayments and their fiat partners and never stored by us. We do not
          sell your personal information. We share information only where necessary to operate the
          Service (such as with NOWPayments to process a payment), to comply with the law, or to
          enforce these Terms. Contact
          support to request deletion of your account data, subject to what we are required to retain
          for legal, security, or fraud-prevention purposes.
        </p>
      </div>

      <div className="block">
        <h2>12. Changes to these Terms</h2>
        <p className="muted">
          We may update these Terms at any time. The date at the top of this page always reflects the
          latest version. Continuing to use the Service after a change takes effect means you accept
          the updated Terms.
        </p>
      </div>

      <div className="block">
        <h2>13. Governing law</h2>
        <p className="muted">
          These Terms are governed by the laws of the United States, without regard to conflict-of-law
          rules. Any dispute arising from these Terms or the Service will be resolved in a competent
          court within the United States.
        </p>
      </div>

      <div className="block">
        <h2>14. Contact</h2>
        <p className="muted">
          Use the <Link href="/support">Support</Link> ticket system for disputes, appeals, HWID
          resets, and any other question about these Terms.
        </p>
      </div>
    </>
  );
}
