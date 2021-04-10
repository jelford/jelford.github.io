+++
title = "Working with signals in Rust - part 1"
draft = true
+++
Signals are an essential part of process life-cycle on linux, but 
working with them is often poorly understood - probably because it's not 
obvious that special care is needed. In this first post of the series, we'll look at what a
signal _is_ and the first challenging aspect: restrictions on signal handlers. 
By the end of the series, we'll have self-contained, simple system for working
with signals on linux.

<!-- more -->

This is a three-part series:
- Part 1: what's a signal and restrictions on signal handlers (this post)
- [Part 2: non-local behaviour of signals - spooky action at a distance](../2021-04-10-working-with-signals-in-rust-pt2-nonlocal-behavior) 
- [Part 3: signal coalescence - signals as a lossy channel](../2021-04-17-working-with-signals-in-rust-pt3-signal-coalescing)

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
poll), or D-Bus (which is built on top of sockets and you would have to 
register a handler on), signals are just sent to you: you either handle them 
or you don't. That makes signals a good fit for certain circumstances. 
Here are some examples of signals that you might have seen in the past:

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

Aside on Windows: Contrary to occassoinal internet rumours, Windows 
[has signal support](https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/signal?view=msvc-160),
but it doesn't do all the same things as signals on 'nix, like:

> SIGINT is not supported for any Win32 application. When a CTRL+C interrupt
> occurs, Win32 operating systems generate a new thread to specifically handle 
> that interrupt. This can cause a single-thread application, such as one in 
> UNIX, to become multithreaded and cause unexpected behavior.

It also has its own mechanism for handling some of the things that are
typically done with Signals on 'nix: [Structured Exception Handling](https://docs.microsoft.com/en-us/windows/win32/debug/structured-exception-handling)(SEH).
In my opinion, Structured Exception Handling is actually a much nicer
interface than Signals (for some of the reasons we'll see in this series) - and it fits
really well with the C++ Exceptions model that's probably most familiar to folks
outside of the C/Go/Rust space. Unlike signals, SEH uses language-level
features (`__try` / `__catch` blocks) - there's [an open issue](https://github.com/rust-lang/rust/issues/38963)
around them in Rust, but I don't know much more than that so I'll stop there.

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

So, let's run the above (the comments to the right are timings):

```
$> cargo -q run
Hello                                            #  0s
^CSorry we didn't get the chance to finish       #  3s
^CSorry we didn't get the chance to finish       #  5s
^CSorry we didn't get the chance to finish       #  6s
Goodbye                                          # 10s
```

What we're seeing here is that we've successfully handled the interrupt: when
I press `Ctrl+C`, our signal handler is getting called, and then normal
execution resumes. Great news, right?

Well, not quite. There's one major issue we'd like to address: the user is
trying to terminate the process, but rather than shutting down gracefully,
as we set out to do, we're just throwing that request in the bin. We call the
signal handler, but then the `sleep` starts up again right where it left off. 
We haven't handled our signal so much as supressed it. So we'd like to have a
way to communicate the fact that an interrupt has occurred _back_ to the main 
thread of execution, wake up from our important long-running proccess (hey, 
sleep is important!), and shut down gracefully.

What we're going to see next is a brief excursion into the execution model of
signal handlers (what really happens to our thread), and then we'll look at
how we can communicate between signal handlers and application logic.

Brief aside: you might have noticed that `signal`'s man page [says](https://www.man7.org/linux/man-pages/man3/signal.3p.html#APPLICATION_USAGE):

> The sigaction() function provides a more comprehensive and
  reliable mechanism for controlling signals; new applications
  should use sigaction() rather than signal().

If we were going to stick with raw `libc` calls, then it would make sense for 
us to heed this warning and use `sigaction` instead. I skipped it this time 
because the setup for it is a little more verbose - but as we're going to see,
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

We'll wrap up this post by talking about some of the techniques we can use for
getting information out of our signal handlers and into our application code.
The [next post](TODO) will cover another area of difficulty we encounter when
we work with signals.

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

## the `signal-hook` library

# updating our example 

# Conclusion

We've seen that signal handlers are subject to some significant restrictions,
and we can use the "self pipe" technique to escape their shackles. That got us
to the point of a functional way to interrupt our programme flow - but it also
necessitated submitting our core application logic to a new event loop, and
gave us our first taste of the next problem with signals: non-local behavour.
That'll be the theme of the [next post](TODO).
