# Documentation analytics and privacy

The A11 documentation measures a small adoption funnel so maintenance can
focus on the points where developers encounter difficulty:

`visit → install → successful example → return → real project`

The stages use observable, minimal signals:

- `visit` records a documentation load;
- `install` records use of the install link, not a package-manager command;
- `successful example` is emitted after a live demo action succeeds;
- `return` records a visit at least one day after the preceding visit;
- `real project` records use of the adoption-report link. Submitted adoption
  reports remain the confirmation that a project exists.

The tracker is served with the documentation. It sets no cookies, creates no
visitor identifier, and does not fingerprint the browser. It sends only the
funnel stage and a page or link title to the configured collection endpoint.
It respects the browser's Do Not Track setting.

A timestamp remains in local browser storage so a visit on a later day can be
counted as a return. The timestamp is not sent and does not identify a person.
Disabling local storage disables that stage without affecting the site.

The tracker is inactive when the documentation build has no analytics
endpoint. Source code for the tracker is
[`assets/analytics.js`](assets/analytics.js).
