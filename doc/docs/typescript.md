# TypeScript API

The TypeScript library exposes the same wire messages, actions, nodes, sessions,
and transports as Python. Its API reference is generated from the checked
TypeScript declarations on every documentation build.

[Open the TypeScript API reference](typescript/index.html){ .md-button .md-button--primary }

Install it under the local name `a11` (the name used in imports):

```sh
npm install a11@npm:@curiositystack/a11
```

```ts
import { Action, Session } from 'a11';
```

The identical distribution is also published as `aeleven`. To select that
registry package while retaining the same import name, use
`npm install a11@npm:aeleven`. npm aliases are recorded in the consuming
application's `package.json`; npm does not provide server-side aliases between
package records.

For a complete browser example, continue with [Browser clients](guides/browser-clients.md).
