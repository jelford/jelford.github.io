---
layout: default.liquid
title: James Elford
---
<h1 class="breadcrumbs">~jelford</h1>

## contact
* [email](mailto:james.p.elford@gmail.com)

## elsewhere

* [github](https://github.com/jelford)
* [linkedin](https://uk.linkedin.com/in/jelford)

## blog

{% assign most_recent_post = collections.posts.pages.first %}

Most recently, [{{most_recent_post.title}}]({{ most_recent_post.permalink }}):

> {{most_recent_post.excerpt}}

... the rest is [here](/blog.html)

## open source

Some things I've tried to help out with:
* [rustup](https://github.com/rust-lang/rustup/pulls?q=is%3Apr+is%3Aclosed+author%3Ajelford)
  The official installer for rust-lang
* [firejail](https://github.com/netblue30/firejail/pulls?q=is%3Apr+is%3Aclosed+author%3Ajelford)
  Application sandboxing on linux with namespaces, seccomp, and capabilities
* [networkzero](https://github.com/tjguk/networkzero/pulls?q=is%3Apr+is%3Amerged+author%3Ajelford+)
  Easy networking for in Python for educational settings

A few projects of my own...
* [activesoup](https://github.com/jelford/activesoup)
  A headless pure-python web scraping tool that mixes the convenience of `requests`
  with the querying power of `beautifulsoup`.
* [rumour](https://github.com/jelford/rumour)
  A re-implementation of the `serf`'s [gossip](https://www.serf.io/docs/internals/gossip.html)
  protocol, written in async rust
* [keyn](https://github.com/jelford/keyn)
  A firefox extension for browsing the web with the power of your keyboard
* [fixl](https://github.com/jelford/keyn)
  Another browser extension, to help with looking up FIX specs. Don't know what FIX is? Lucky you!
* [squirrel](https://github.com/jelford/squirrel)
  Automatic backups and queryable history while you work
* [ferret](https://github.com/jelford/ferret)
  Custom shebang for simpler python scripts that need more than the standard library



