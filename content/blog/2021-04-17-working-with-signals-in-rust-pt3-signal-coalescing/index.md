+++
title = "Working with signals in Rust - part 3"
draft = true
+++

Signals are an essential part of process lifecycle on linux, but 
working with them is often poorly understood - probably because it's not 
obvious that special care is needed. In this third post of the series, we'll
look at a final aspect of what makes signals difficult to work with correctly:
signal coalescence.

<!-- more -->

This is a three-part series:
- [Part 1: what's a signal and restrictions on signal handlers](../working-with-signals-in-rust-pt1-whats-a-signal)
- [Part 2: non-local behaviour of signals - spooky action at a distance](../working-with-signals-in-rust-pt2-nonlocal-behaviour) 
- Part 3: signal coalescence - signals as a lossy channel (this post)

# so why are signals hard to work with? (part 3: signal coalescing)

