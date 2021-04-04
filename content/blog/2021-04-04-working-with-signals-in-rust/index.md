+++
title = "Working with signals in rust"
draft = true
+++
In this post, we'll take a look at linux signal handling in `rust`. Signals are
an essential part of process lifecycle on linux, but working with them is often
poorly understood - probably because it's not obvious that special care is needed.
By the end, we'll get to an idiomatic way to receive signal information in
application code.

<!-- more -->

# what's a signal anyway?

There are a few ways that processes can speak to one another on a 'nix system:
- file-like mechanisms (network sockets, unix domain sockets, pipes, and so on)
- IPC (either of the [System V variety](https://tldp.org/LDP/lpg/node21.html), 
  or [POSIX message queues](https://www.man7.org/linux/man-pages/man7/mq_overview.7.html))
- [D-Bus](https://www.freedesktop.org/wiki/Software/dbus/) (passing messages
  via system- or session-wide buses)
- ... probably a bunch of others
- finally: signals.

Signals are probably the most primitive mechanism, and unlike the others, you
don't have to opt in somehow to receiving them. In contrast to the file-likes
(which you would have to read), or message queues (which you would have to 
poll), or D-Bus (which you would have to register a handler on), signals are
just sent to you: you either handle them or you don't. That makes signals a
good fit for certain circumstances. Here are some examples of signals that you
might have seen in the past:

- `SIGSEGV`: invalid access to storage. This is typically sent to your process
  when you access memory you shouldn't (use-after-free, or plain old bad
  pointer arithmetic). If you've programmed in C, you've probably seen this.
  In "safe" languages, with luck you can go many years without seeing one of these.
- `SIGINT`: interractive interrupt. This is typically the result of the user
  hitting `Ctrl+C` on their keyboard. This is such a common interaction that
  friendly runtimes will often translate it into some more palatable 
  language-native form (e.g. in Python a [`KeyboardInterrupt` is raised](https://docs.python.org/3/library/exceptions.html#KeyboardInterrupt), 
  while on Java the [`interrupted` flag](https://docs.oracle.com/en/java/javase/14/docs/api/java.base/java/lang/Thread.html#interrupted())
  is set).
- `SIGKILL`: this signal will terminate your programme, unconditionally. I
  guess the clue's in the name 🤷

You can find a more complete list in [man 7 signal](https://www.man7.org/linux/man-pages/man7/signal.7.html).
A few signals have a default outcome if your process doesn't handle them, for
example: `SIGSEGV` or `SIGINT` will terminate your process. `SIGKILL`, on the
other hand, _can't_ be handled; it will always end your process. Others might
simply be ignored.

Aside on Windows: Windows [has signal support](https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/signal?view=msvc-160),
but it doesn't do all the same things as signals on 'nix, like:

> SIGINT is not supported for any Win32 application. When a CTRL+C interrupt
> occurs, Win32 operating systems generate a new thread to specifically handle 
> that interrupt. This can cause a single-thread application, such as one in 
> UNIX, to become multithreaded and cause unexpected behavior.

It also has its own mechanism for handling some of the things that are
typically done with Signals on 'nix: [Structured Exception Handling](https://docs.microsoft.com/en-us/windows/win32/debug/structured-exception-handling)(SEH).
In my opinion, Structured Exception Handling is actually a much nicer
interface than Signals (for some of the reasons we'll see below) - and it fits
really well with the C++ Exceptions model that's probably most familiar to folks
outside of the C/Go/Rust space. Unlike Signals, SEH uses language-level
features (`__try` / `__catch` blocks) - there's [an open issue](https://github.com/rust-lang/rust/issues/38963)
around them, but I don't know much more than that so I'll stop there.

# how can I handle signals?

So, we've got an idea of what a signal is, but how do we use them in our
application? In lots of cases, the answer is: don't. The default handlers for
signals like `SIGINT` or `SIGSEGV` will terminate your application, and in
many cases that's the right thing - e.g. in a console application, if your user
hits `Ctrl+C`, then they probably meant for your app to stop.

Not always though! Let's see a motivating example: let's say I have a function
that does some long-running task. If the user gets bored, I want them to be
able to terminate the process, and some message about the process not finishing.

Let's start with some example code:

```rust
fn handle_interrupt() {
    // We want this code to run if the user decided
    // to kill the process early.
    println!("Sorry we didn't get the chance to finish");
}

fn main() {
    println!("Hello");

    // wait for some long-running job to complete
    sleep(Duration::from_secs(10)); 

    // I _always_ want this to run, even if the user
    // aborted the above.
    println!("Goodbye");
}
```

If the user hits `Ctrl+C` during the call to `sleep`, then we want to call
`handle_interrupt`, and then shut down via `"Goodbye"` (without waiting the
full 10 seconds). We can hope to achieve that with a signal handler. To start
with, we'll bring in the [`libc`](https://crates.io/crates/libc) crate. Much of
what we're talking about here is to do with the low-level interfaces provided 
by Linux, so it'll make sense to work up from there.

```rust
extern "C" fn handle_interrupt(sig: libc::c_int) { // 1
    println!("Sorry we didn't get the chance to finish");
}

fn main() {
    println!("Hello");

    // All libc functions are unsafe
    unsafe { 
        libc::signal(libc::SIGINT, handle_interrupt as libc::sighandler_t); // 2
    }

    std::thread::sleep(Duration::from_secs(10)); 
    println!("Goodbye");
}
```

You can get the raw files [here (zip)](https://gist.github.com/jelford/d5afa01dc0f9c455d4effa4f51445d98/archive/f27eb6449beb85fc6c80b68054c1e33f06a23192.zip).
Let's go through the changes:

1. Our `handle_interrupt` function is going to be called from C, so it needs to
   be marked `extern "C"`. From [the Rust book](https://doc.rust-lang.org/book/ch19-01-unsafe-rust.html#using-extern-functions-to-call-external-code):
   
   > The "C" part defines which application binary interface (ABI) the external function uses: the ABI defines how to call the function at the assembly level. The "C" ABI is the most common and follows the C programming language’s ABI.
   
   Our signal handler is going to be called by something _outside_ of the Rust
   ecosystem, so it makes sense that we have to set it up to be compatible with
   the "C" ABI - that's the most general set of expectations for how a function
   should be called.

   While we're here, notice that the method signature has changed too. If we
   take a look in [man 3 signal](https://www.man7.org/linux/man-pages/man3/signal.3p.html), we'll see the function signature for `signal` is:
   ```
   void (*signal(int sig, void (*func)(int)))(int);
   ```
   ... which is C for `signal` being a function returning an `int` that takes
   two arguments:
   1. The signal number (identifying the signal for which we're installing a
      handler)
   2. A pointer to a function that takes a single `int` argument
   
   The (Rust) `libc` docs for `signal` don't make this terribly clear, with the
   signature:
   ```rust
   pub unsafe extern "C" fn signal(
      signum: c_int, 
      handler: sighandler_t
   ) -> sighandler_t

   type sighandler_t = size_t;
   ```
   This is most likely generated from the relevant C headers - so if you want
   to take a look at the "golden source", you can check `<signal.h>` on your
   own system (most likely under `/usr/include/signal.h`).

2. This is where we actually install the signal handler. This one reads:
   "When this thread receives a SIGINT, run handle_interrupt". Hopefully that
   one is relatively clear!

So, let's run the above:

```
$> cargo -q run
Hello
^CSorry we didn't get the chance to finish
^CSorry we didn't get the chance to finish
Goodbye
```

What we're seeing here is that we've successfully handled the interrupt: when
I press `Ctrl+C`, our signal handler is getting called, and then normal
execution resumes. Great news, right?

Well, not quite. There's one major issue we'd like to address: the user is
trying to terminate the process, but rather than shutting down gracefully,
as we set out to do, we're just throwing that request in the bin. We haven't
handled it so much as supressed it. So we'd like to have a way to communicate
the fact that an interrupt has occurred _back_ to the main thread of execution,
wake up from our important long-running proccess (hey, sleep is important!),
and shut down gracefully.

What we're going to see next is a brief excursion into the execution model of
signal handlers (what really happens to our thread), and then we'll look at
how we can communicate between signal handlers and application logic.

Brief aside: you might have noticed that `signal`'s man page [says](https://www.man7.org/linux/man-pages/man3/signal.3p.html#APPLICATION_USAGE):

> The sigaction() function provides a more comprehensive and
  reliable mechanism for controlling signals; new applications
  should use sigaction() rather than signal().

If we were going to stick with raw `libc` calls, then it would make sense for 
us to heed this warning and use `sigaction` instead. I skipped it this time 
because the setup for it is a little more verbose - but as we're about to see,
we can leverage a library to take care of this stuff for us (and when we take 
a peek under the covers, we'll see that `sigaction` is exactly the call that 
ends up being used). It doesn't make any difference to what we're talking about
though.

# so why are signals hard to work with? (part 1: communicating back to the application)

Okay, so we've covered signals and what they're used for, and as we've just
seen, on the face of it they're actually pretty straightforward to work with;
we just register a callback function and away we go. So why did I start off by
claiming that they're often "poorly understood"? Let's start by
taking a look at what's really happening with our control flow. Here's the same
code from above, but this time laid out as it would actually be
executed, in the case of a single `SIGINT` during our `sleep`:

```
...
                                       →-----------→ fn handle_interrupt(...) { // 2
sleep(Duration::from_secs(10)) // 1; --↑               println!("Sorry ..."); 
                             ↑---------------------← } // 3
println!("Goodbye");
```

In words, what's happening to our thread of execution is:
1. We enter our `sleep` function
2. A signal occurs, and we execute `handle_interrupt`
3. Our thread of execution returns to where it was in the `sleep` function

That sounds relatively simple, but consider:
* We don't have any way to express that relationship to the Rust type system.
  Therefore, we can't safely share state with the main logical thread of 
  execution.
* This signal-handler is _global_ across our thread group. Therefore if we were
  to share any state, that would lead to races
* Most importantly, it's not guaranteed that we won't receive _another_ signal
  while handling the first.

It's that last point that's the hardest to spot the implications of. What it
means is that anything that happens inside the signal handler must be
"re-entrant" - which means that it must be safe to stop half way through and
have _another_ instance of itself concurrently executing. This is one of the
requirements of thread safety (one that Rust normally allows us not to think
about), but it's worse than that: consider that in cases like `SIGSEGV`, our 
actions _inside_ the signal handler might trigger further calls (i.e. it can
be _inadvertently_ recursive, or recursive without any obvious sign of
recursion).

If you're thinking that it sounds like we've entered the danger zone at this
point, then you're dead right. Don't worry - [`man 7 signal-safety`](https://man7.org/linux/man-pages/man7/signal-safety.7.html)
is here to help us out:

>  To avoid problems with unsafe functions, there are two possible
>       choices:
>   1. Ensure that (a) the signal handler calls only async-signal-
>      safe functions, and (b) the signal handler itself is reentrant
>      with respect to global variables in the main program.
>
>   2. Block signal delivery in the main program when calling
>      functions that are unsafe or operating on global data that is
>      also accessed by the signal handler.

We can start by saying that 2 is not an option: we're exactly trying to handle
an incoming signal, so blocking delivery is going to defeat the whole object.

So how about option 1? Well, 1.(b) sort of _smells_ quite satisfiable: we're
working in Rust, and Rust won't let us modify global objects in a non-thread-safe
way... so does that also imply re-entrancy? No it does not 😅! Quite the
opposite in fact: the thing we'd lean on to coordinate execution with the main
thread would be a mutex. Rust's (unix) mutexes are built on POSIX mutex, and have
[this](https://github.com/rust-lang/rust/blob/master/library/std/src/sys/unix/mutex.rs#L29)
to say on the topic:

> ...we instead create the mutex with type PTHREAD_MUTEX_NORMAL which is 
> guaranteed to deadlock if we try to re-lock it from the same thread...

... and with good reason! If you want the details, I suggest you follow the link 
and have a good read - basically it's to avoid Undefined Behaviour...
nontheless, this is firmly _not_ going to be re-entrant.

All of this is moot anyway, because of 1.(a):

> the signal handler calls only async-signal-safe functions

What does it take for a function to be "async-signal-safe" anyway? Well,
it's a pretty uncomplicated answer: there is a list of functions at the bottom
of the `man` page which POSIX declares to be "async-signal-safe", and anything else
is out of bounds - and in particular the relevant `pthread_mutex_` functions are
off the menu.

We'll see some techniques for dealing with this particular issue below, but
first, some more reasons why signals are a rare joy to work with 👍 (these ones
are less relevant to our narrowly contrived example, but you didn't really come
here just to learn about `thread::sleep()`, right?)

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

This behavior (of system-calls returning early with `EINTR`) can also happen
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

To paraphrase: Rust is wise to this spurious wakeup behaviour, and will restart
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

and herein is the issue: calls elsewhere in our process can affect our `sleep`.
In this case it's kind of coincidental that `alarm` happens to notify us via
signals, rather than some other mechanism, but that nonetheless, it makes our
plan more fragile. We can do better.

# so why are signals hard to work with? (part 3: signal coalescing)



# communicating with the application from a signal handler.

So we're in danger of running into rather a bleak conclusion: 
1. we can register a callback easily enough, but we can't then use that callback
   to communicate anything interesting back to our main thread of execution. 
2. the arrival of signals can cause spooky action at a distance by causing our
   system calls to return early - but in our case, in Rust, we will carry on
   sleeping anyway
3. we don't always get the full detail of every signal. That's not relevant for
   our case, so let's just leave that one for another time.
   
Luckily, it's safe to say that the Unix process model is not completely broken. 
In this section we'll stay at a low level and see a couple of ways we _can_ get
information about signals back onto our application code.

There are two main things we can do

## signalfd

TODO: write about signalfd, which gives us a file handle from which we can read signals. Link [this article](https://ldpreload.com/blog/signalfd-is-useless)

## self-pipe

TODO: write about signal-pipes as implemented in [`signal-hook`](https://github.com/vorner/signal-hook/blob/master/src/low_level/pipe.rs)
Link [DJB's article on the topic](https://cr.yp.to/docs/selfpipe.html)

# the `signal-hook` library

`signal-hook` solves all this stuff. Use `Signal::forever`.

# `select` for multiplexing event and timers

We can get `signal_hook::low_level::pipe` to write to a `fd` and `select` on that with a timeout.

If you ask me, `crossbeam::select!` provides a more general cross-thread
coordination mechanism that should also give you sensible Windows support

# conclusion

* If we're happy to stick with linux-only then `signal_hook` gives us everything we want
* We can abstract away some of the details with `crossbeam`
