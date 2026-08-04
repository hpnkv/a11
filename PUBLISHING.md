# Publishing A11

Releases are tag-only. A push to `main` can run checks and documentation, but
cannot publish a package or create a GitHub release.

## Versions and tags

Python and C++ share the version in [`VERSION`](VERSION). Python's build backend
reads it as dynamic package metadata, while CMake uses it for `PROJECT_VERSION`,
the native module's `__version__`, the tracing scope, and the installed
`a11ConfigVersion.cmake`.

TypeScript uses `js/package.json` as its independent source of truth;
`js/package-lock.json` is generated from it.

Use these annotated tags:

- `a11-vX.Y.Z` builds CPython 3.11–3.15 wheels for macOS arm64/x86_64 and
  manylinux aarch64/x86_64, builds an sdist and Linux x86_64 C++ SDK archive,
  publishes the Python files to PyPI, and creates a GitHub release containing
  every artifact.
- `npm-vX.Y.Z` tests and publishes both `@curiositystack/a11` and `aeleven` to
  npm. It may have a different version from the Python/C++ release.

The workflows reject a tag whose version differs from its source file. Before
tagging, update the relevant source and generated lock/stub files on a normal
branch, merge it, then run (substituting the real version):

```sh
git switch main
git pull --ff-only
git tag -a a11-v0.1.6 -m "A11 0.1.6"
git push origin a11-v0.1.6

git tag -a npm-v0.1.4 -m "A11 TypeScript 0.1.4"
git push origin npm-v0.1.4
```

Tags are immutable release inputs. If publishing partially fails after a
registry accepts a version, fix the issue and release a new version; do not
move the old tag or rebuild an already published version.

## PyPI trusted publishing

No PyPI API key is required. On pypi.org, open the `a11-kit` project and add a
GitHub Actions trusted publisher with:

- owner: this repository's GitHub owner;
- repository: this repository's name;
- workflow: `release.yml`;
- environment: `release`.

If `a11-kit` has not been published yet, create the same configuration as a
pending publisher from the PyPI account's **Publishing** page.

In GitHub, create the `release` environment under **Settings → Environments**.
Add required reviewers if releases should need an approval. The workflow's
`id-token: write` permission lets PyPI verify the job through OIDC.

## npm trusted publishing

For each npm package (`@curiositystack/a11` and `aeleven`), configure a trusted
publisher on npmjs.com with this repository, workflow `npm.yml`, and environment
`npm`. Then create the matching `npm` environment in GitHub. npm trusted
publishing uses GitHub OIDC, so `NPM_TOKEN` is not needed. The workflow uses
Node 24 and publishes with provenance.

npm requires a package to exist before its trusted publisher can be attached.
For a brand-new package name, an owner must perform the one-time initial
publish with a granular automation token, then configure trusted publishing and
remove that token from GitHub.

If the npm account requires two-factor authentication, choose the setting that
allows automation/granular tokens or trusted publishers. The GitHub actor still
needs permission to read the repository, and the npm account configuring the
publisher must own both package names.

## GitHub releases

No key needs to be added. GitHub supplies a short-lived `GITHUB_TOKEN`; the
workflow grants it `contents: write` only in the final release job. Repository
Actions settings must allow workflows read/write access, or explicitly allow
the workflow permission override. The GitHub release is created only after all
builds and the PyPI upload succeed.

For additional protection, add required reviewers to both GitHub environments
and create tag rulesets restricting `a11-v*` and `npm-v*` creation/deletion to
release maintainers.
