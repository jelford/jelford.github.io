+++
title = "Working with signals in Rust - part 2"
draft = true
+++

Signals are an essential part of process lifecycle on linux, but 
working with them is often poorly understood - probably because it's not 
obvious that special care is needed. In this second post of the series, we'll
look at another aspect of what makes signals difficult to work with correctly:
non-local behaviour.


<!-- more -->


This is a three-part series:
- [Part 1: what's a signal and restrictions on signal handlers](../working-with-signals-in-rust-pt1-whats-a-signal)
- Part 2: non-local behaviour of signals - spooky action at a distance (this post)
- [Part 3: signal coalescence - signals as a lossy channel](../working-with-signals-in-rust-pt3-signal-coalescing)

# so why are signals hard to work with? (part 2: non-local behaviour)

The next interesting thing about signals is this (from `man 7 signal` again):

> If a signal handler is invoked while a system call or library
> function call is blocked, then either:
>
> * the call is automatically restarted after the signal handler
>   returns; or
>
> * the call fails with the error EINTR.
>
> ...

Let's just dwell for a second on what that means: any time we receive a signal,
anywhere in our program (including between threads), we can have a system-call
return early. In _some_ cases, we can arrange for those calls to be
automatically restarted by virtue of the way we install our signal handler (this
is one of the things you get from using `sigaction` instead of `signal` when
to register your signal handler), but we can't catch all of them: in particular
we can't arrange for `sleep` (or a bunch of other important calls like `recv`
or `send` - on sockets) to automatically restart. 

This behaviour (of system-calls returning early with `EINTR`) can also happen
without signal handlers:

> On Linux, even in the absence of signal handlers, certain
> blocking interfaces can fail with the error EINTR after the
> process is stopped by one of the stop signals and then resumed
> via SIGCONT.  This behavior is not sanctioned by POSIX.1, and
> doesn't occur on other systems.

Not _all_ system calls are subject to this behaviour (e.g. local disk I/O), but
a good many of them are, and we have to be on our guard whenever we work with
signals. What are the implications for our `sleep` example though?

On the face of it, we might be thinking: this is actually good news! Here's a 
line of reasoning:
1. Our problem was that we don't have a way to wake up our `thread::sleep(...)` 
   call from our signal handler
2. It turns out that receiving a signal will wake up a `sleep` system call 
   anyway
3. Therefore the problem solves itself! And how!

No dice ⛔. It turns out that this non-local behaviour that causes `sleep` not
to sleep for its full duration is not the preferred interface of the Rust
standard library. In `std::thread::sleep`, if you ask to sleep for 5 seconds,
you will sleep for (at least...) 5 seconds. [Here's](https://github.com/rust-lang/rust/blob/1.51.0/library/std/src/thread/mod.rs#L762)
what the source code has to say on the matter:

> On Unix platforms, the underlying syscall may be interrupted by a
> spurious wakeup or signal handler. To ensure the sleep occurs for at least
> the specified duration, this function may invoke that system call multiple
> times.

To paraphrase: Rust is wise to this wakeup behaviour, and will restart
that `sleep` syscall for you. So, we're going to have to find another way to
ensure that our application wakes up when an interrupt occurs.

There is a possible positive here though: what if we skip Rust's `sleep`
implementation and go straight to `libc::sleep`? Actually, on the face of it
that's not a bad solution. We would need to introduce a bit of extra 
code to handle the case of early wake-up, and we still need to solve the
problem of communicating back to our application code from the signal handler,
but there's the beginning of a workable path forward in there. There's another issue
with that, though- it's not _directly_ related to signal handling, but it can
certainly be triggered by another signal elsewhere in your code, in another
example of non-local behaviour around signals.

[Here's `alarm()`](https://www.man7.org/linux/man-pages/man2/alarm.2.html), a
call you can use to arrange for a signal (`SIGALRM`) to be delivered at some 
point in the future. The idea is straightforward: you can use this to implement
timeouts, for example. But here's the problem:

> alarm() and setitimer(2) share the same timer; calls to one will
> interfere with use of the other.
> ...
> sleep(3) may be implemented using SIGALRM; mixing calls to
> alarm() and sleep(3) is a bad idea.

... and herein lies the issue: calls elsewhere in our process can affect our `sleep`.
In this case it's kind of coincidental that `alarm` happens to notify us via
signals, rather than some other mechanism, but that nonetheless, it makes our
plan more fragile. We can do better.
