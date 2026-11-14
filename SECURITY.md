# Security

XRecomp runs code recompiled from a game you own, with the permissions of an ordinary Android app, and builds that
code on the device with an embedded compiler. It has no network features: it never downloads code or sends data.

## Reporting a problem

Do not open a public issue for a security problem (for example: a crafted XBE or disc image that makes the Prepare
or the launcher write outside the app's folders, run a command, or read another app's data). Report it privately on
GitHub: [Security › Report a vulnerability](https://github.com/MisterAlex95/Xrecomp/security/advisories/new). We answer within two weeks.

## Scope

- In scope: the app (launcher, Prepare service, pack loader), `tools/prepare` and the pipeline it runs on untrusted
  input (XBE, ISO), the APK's build.
- Out of scope: what a recompiled game does inside its own sandbox (it is the game's code), devices that are rooted
  or run a modified app, and denial of service from a disc image that is merely large or slow.

Supported version: the latest release ([releases](docs/releases/README.md)) and `xrecomp-latest`.
