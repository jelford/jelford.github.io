+++
title = "Working with signals in Rust - some things that signal handlers can't handle"
+++
Signals are an essential part of process life-cycle on linux, but 
working with them is ... fraught - probably because it's not 
obvious that special care is needed. In this post, we'll look at what a
signal _is_ and just one of the challenging aspects: restrictions on signal handlers.

<!-- more -->

TL;DR: use [signal-hook](https://github.com/vorner/signal-hook). There's an example at the end.

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
- `SIGINT`: interactive interrupt. This is typically the result of the user
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

Aside on Windows: Contrary to occasional internet rumours, Windows 
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
We haven't handled our signal so much as suppressed it. So we'd like to have a
way to communicate the fact that an interrupt has occurred _back_ to the main 
thread of execution, wake up from our important long-running process (hey, 
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
"re-entrant" - and what _that_ means is that it must be safe to stop half way through and
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
>      safe functions, and (b) the signal handler itself is re-entrant
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
nonetheless, this is firmly _not_ going to be re-entrant.

All of this is moot anyway, because of 1.(a):

> the signal handler calls only async-signal-safe functions

What does it take for a function to be "async-signal-safe" anyway? Well,
it's a pretty uncomplicated answer: there is a list of functions at the bottom
of the `man` page which POSIX declares to be "async-signal-safe", and anything else
is out of bounds - and in particular the relevant `pthread_mutex_` functions are
off the menu. 

Just to give a taste of the sorts of things we can't do in a signal handler:
- anything involving locks, as we've seen above
- `stdio` (like `printf` - although we got away with it above)
- `malloc` (so, no allocating memory)
- ... the list goes on.

Knowing what you can and can't do gets even harder in a high-level language
like rust - for example, it might not be obvious that `println!()` takes a
lock] via a call to [io::stdout()](https://doc.rust-lang.org/std/io/fn.stdout.html):
that's a deadlock if you get re-interrupted while printing!

We'll spend the rest of this post by talking about some of the techniques we can use for
getting information out of our signal handlers and into our application code.

# communicating with the application from a signal handler.

Luckily, it's safe to say that the Unix process model is not completely broken. 
In the rest of this article we'll look at addressing 1: we'll stay at a low level 
and see a couple of ways we _can_ get information about signals back onto our 
application code safely, and escape the confines of "async-signal-safe" code.

## the self-pipe trick

The careful reader of the [signal-safety](https://man7.org/linux/man-pages/man7/signal-safety.7.html)
man page may have noticed a function that gives us something of an escape hatch: `write` is available.

`write` opens up the door to exfiltrating data from our signal-handler to another thread, in a "trick"
described in [D. J. Bernstein's article on the topic](https://cr.yp.to/docs/selfpipe.html). Let's see
that in action; we'll drop down to C since we're going to be talking with `libc` a lot for this, and
all Rust's `unsafe` ceremony doesn't add much here:

```c

static int pipefds[2] = {0};

void signal_handler(int signum)
{
    uint8_t empty[1] = {0};
    int write_fd = pipefds[1];
    write(write_fd, empty, 1);                 // 3
}

void handle_signal(int read_pipe_fd)
{
    uint8_t buff[1] = {0};
    read(read_pipe_fd, buff, 1);               // 4
    printf("Received signal\n");
}

int main()
{
    pipe(pipefds)                              // 1
    fcntl(pipefds[1], F_SETFD, O_NONBLOCK);

    int read_fd = pipefds[0];

    signal(SIGINT, signal_handler);            // 2

    while(true) {
        handle_signal(read_fd);
    }
}

```

Error handling and includes omitted for brevity; you can find a full listing [here](https://gist.github.com/jelford/80367bc790ad1a46a3aa25e413e4eaf2)
if you'd like to download and run it yourself.

So there's quite a bit to unpack here:
1. We set up a [`pipe`](https://www.man7.org/linux/man-pages/man2/pipe.2.html), which works just
   like a pipe in the shell: you write data in one end and read it out the other end. The contents
   of `pipefds` is just two file descriptors, which we'll use to shimmy data from the signal-handler
   into our main thread of execution. We set the writer up as non-blocking - we don't want any blocking
   in the signal handler.
2. Now we install our signal handler, same as before
3. In the signal handler, we're allowed to use `write` to send data back to the main thread - nice.
4. Finally, in the main thread, whenever we can read from our pipe, we know that the signal handler
   was fired.

Nice! So we're able to pull information from our signal handler into the main thread. Inside `handle_signal`,
we're back to a normal execution context, and we don't have to worry about all that signal-safety stuff we
talked about before. Since we're using a file descriptor on the read side, we can hook that into a normal
event loop (based on `poll`/`select`/`epoll` or whatever). Here, we'll just do blocking reads in a loop -
that's enough to show how it works.

## signalfd

Wouldn't it be convenient if we didn't have to set up these pipes and marshall data back to
the main thread ourselves? `signalfd` is exactly that. From
[the man page](https://man7.org/linux/man-pages/man2/signalfd.2.html):

> signalfd - create a file descriptor for accepting signals

Like we were discussing before: once we have a file descriptor, we can handle 
that using familiar tools like `select`, `poll`, and `epoll` - in our existing 
event loops, in our normal thread contexts. Let's see how that looks - and 
we'll stay in C since we're still speaking to `libc`:

```c
void handle_signal(int);

int main()
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);

    sigprocmask(SIG_SETMASK, &mask, NULL);                // 1

    int signal_fd = signalfd(-1, &mask, SFD_NONBLOCK);    // 2

    struct pollfd pollfd = {
        .fd = signal_fd,
        .events = POLLIN,
    };

    while (poll(&pollfd, 1, 5000) > 0)                    // 3
    {
        handle_signal(pollfd.fd);
    }
}

void handle_signal(int signal_fd)
{
    struct signalfd_siginfo siginfo;
    ssize_t s;
    s = read(signal_fd, &siginfo, sizeof(siginfo));       // 4
    if (s != sizeof(siginfo))
    {
        perror("read");
        exit(1);
    }

    uint32_t signo = siginfo.ssi_signo;
    char *signame = strsignal(signo);

    printf("Received signal %d (%s)\n", signo, signame);
}
```

Again, I've omitted all the error handling and includes for brevity; you can get a full copy
of the code [here](https://gist.github.com/jelford/d8850f357c26ee6840290f7ad89c097b) if you'd like to run it yourself.
Here's what's going on:

1. We start by letting the runtime know that we don't want interrupt signals to run according to their normal
   disposition. Instead, they should be blocked, and queued up for us to read synchronously.
2. Install a `signalfd` that will be used to read the signals that we just masked...
3. This is where the magic happens: we get notified that there's a signal for us to read - we call out to
   `handle_signal` process it.
4. The same info that would have been sent to us via a signal handler is available to read from our file
   descriptor.

And just as before: `handle_signal` is just _normal_ code executing in a _normal_ context. We have regained
access to all those convenient facilities like mutexes and message queues and memory allocation that make
life great. I've set up a little `poll` loop here for the sake of exposition, but just like in the self-pipe
trick, we can pass our file descriptor to `select` or `epoll` or whatever event loop you've got going in your application.

So, we're done, right? Well, not quite--
1. we did say we would deal with signals in _rust_, so in the next section we'll see that.
2. there are a couple of problems with `signalfd` that I haven't mentioned so far: most notably the interaction
   with child processes. Let's cover that before we move on.

In (1) above, we saw that we blocked the delivery of signals, since we want to handle them
ourselves through our `signalfd`. Here's the rub: signal masks are inherited by child processes,
while the whole `signalfd` infrastructure is not. This is a problem; it means that child processes
will:

1. not receive signals in the normal way, if they were masked in the parent process,
2. not handle them any other way either.

The child processes _could_ clear their signal masks - but in practice most of us don't do that
when we start our programmes. You could imagine letting the child processes inherit the `signalfd`,
but it's the same issue; they would need to arrange to handle the signals themselves. This is a bit
thorny - [maybe even a deal-breaker](https://ldpreload.com/blog/signalfd-is-useless) - and it's the
reason why, in practice, folks still use the self-pipe trick.

## the `signal-hook` library

Let's get to the point: the [`signal-hook`](https://github.com/vorner/signal-hook) library has what you need.
It implements a couple of the things you would hope for:
- [An `Iterator` over incoming signals](https://docs.rs/signal-hook/0.3.10/signal_hook/iterator/index.html), which you pull from the main thread
- Signal information is pulled out of the signal handler [via the self-pipe trick](https://docs.rs/signal-hook/0.3.10/signal_hook/low_level/pipe/index.html)
- And you're done!

And this time we really are done. `signal-hook` also provides convenient adapters 
[for use in a tokio event loop](https://docs.rs/signal-hook-tokio/0.3.0/signal_hook_tokio/),
or equivalently [for `async-std`](https://docs.rs/signal-hook-async-std/0.2.1/signal_hook_async_std/).

Let's wrap it up with an update to the example we started with:

```rust
use signal_hook::consts::*;
use signal_hook::iterator::Signals;
use crossbeam::channel::{select, self, Sender, Receiver, after};
use std::time::Duration;

fn await_interrupt(interrupt_notification_channel: Sender<()>) {
    let mut signals = Signals::new(&[                              // 1
        SIGINT,
    ]).unwrap();

    for s in &mut signals {                                        // 2
        interrupt_notification_channel.send(());                   // 3
    }
}

fn main() {
    let (interrupt_tx, interrupt_rx) = channel::unbounded();
    std::thread::spawn(move || { await_interrupt(interrupt_tx)});

    let timeout = after(Duration::from_secs(5));
    loop {
        select! {
            recv(interrupt_rx) -> _ => {                           // 4
                println!("Received interrupt notification");
                break;
            },
            recv(timeout) -> _ => {                                // 5
                println!("Finally finished the long task");
                break;
            }
        }
    }
}
```

In this example, I'm using `crossbeam` `channels` and `select` handle
multiplexing events (namely, timeouts vs. interrupt notifications),
but you could do the same thing without `crossbeam` in `tokio` or `async-std`
runtime - and `signal-handler`'s tokio adapter will let you do just that.
Let's go through the main points:

1. Installing the signal handler. This does - eventually - pretty much
   what you'd expect from the start of the post, with a [a call through to `libc::sigaction`](https://docs.rs/signal-hook-registry/1.4.0/src/signal_hook_registry/lib.rs.html#174).
2. Signal info is fed back to us via a self-pipe. `signal-hook` wraps
   that interaction up in a nice `Iterator` interface so that we never
   have to worry about.
3. We're in a normal, non-signal-handler execution context, so we
   can safely use a `crossbeam::channel` (or any other mechanism
   we like) to communicate between threads.
4. Finally, when we get an interrupt on the main thread, we can break
   out of our loop.
5. It's not really about signals, but it's nice to note that `crossbeam`
   also provides a nice timeout mechanism.

And that's really truly the end.

# Conclusion

We've seen that signal handlers are subject to some significant restrictions,
and we can use the "self pipe" technique to escape their shackles. `signal-hook`
makes dealing with this stuff convenient.

There are two other signals-related topics that I'd like to cover:
- event coalescing, which I mentioned this briefly at the end of the section on `signalfd`
- non-local behavior, which we haven't seen here but opens up its own fresh can of worms

If those topics sound interesting to you, do drop me a note to remind me to make it happen.
