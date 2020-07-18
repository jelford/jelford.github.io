---
title: By any other name
published_date: "2020-07-18 17:51:27 +00:00"
layout: post.liquid
is_draft: false
---
In this post we'll take a look under the covers at what happens we look up hosts.

If you're here, you'll have an idea that the answer is basically "DNS" - but 
that's not the whole story. Let's make things concrete; we're going to answer 
the question: how does the following snippet of (pseudo-)code figure out what 
host to connect to?

```c++
conn = socket::connect("www.jameselford.com", 443);
```

DNS is just one part of the answer.

## tl;dr

_Most_ programs on a normal linux system will look in `/etc/nsswitch.conf` to
figure out how to resolve hostnames. But lots of programs won't. Pretty
much everything will find your DNS servers in `/etc/resolv.conf`. Generally,
this process will involve a call to `getaddrinfo`, provided by your system's 
standard C library.

If that's what you came to find out, then you can go back to the internet now;
thanks for your time. Stick around if you want to get into altogether too much
detail about how this all happens.

## what's coming up

We'll see:
- How our program ends up calling into `libc` (by diving into the source)
- How `libc` resolves our hostname (with a little help from `strace`)
- and along the way, some opportunities to tweak how our system does host lookup

We'll start in Rust land, because that's kind of my jam, but
you could do a similar journey for any high-level programming language. Maybe
you hate jam. Maybe you're more of a Python / marmalade / Ruby sort of person.
That's fine too; ultimately it doesn't matter how you do it, so long as there's
a thick layer of sugar on your breakfast. In C, you'd just skip the first step 
and go straight to `libc`. I guess in this analogy, C is the toast.

## check the source - peeking into the (Rust) standard library

In Rust, the pseudo-code above looks like this:

```Rust
let c = TcpStream::connect("www.jameselford.com:443");
```

... so `TcpStream::connect` is our jumping-off point. `connect` takes any
argument with a corresponding `ToSocketAddrs` implementation - and the 
standard library comes with implementors for [a whole range of sensible types](https://doc.rust-lang.org/std/net/trait.ToSocketAddrs.html#implementors).

[Here's](https://doc.rust-lang.org/src/std/net/addr.rs.html#969-979) the
implementation for `str`:

```Rust
// accepts strings like 'localhost:12345'
#[stable(feature = "rust1", since = "1.0.0")]
impl ToSocketAddrs for str {
    type Iter = vec::IntoIter<SocketAddr>;
    fn to_socket_addrs(&self) -> io::Result<vec::IntoIter<SocketAddr>> {
        // try to parse as a regular SocketAddr first
        if let Ok(addr) = self.parse() {
            return Ok(vec![addr].into_iter());
        }

        resolve_socket_addr(self.try_into()?)
    }
}
```

The call to `parse` near the top brings some extra conversion magic into the mix,
but all that's doing is leaning on [`SocketAddr`'s `FromStr` implementation](https://doc.rust-lang.org/src/std/net/parser.rs.html#318-326),
which I won't list here, because it's just dealing with the case that the `str`
is already a straightforward `SocketAddr` (e.g. `192.168.1.1:80`). That's not 
our case.

Next comes [`resolve_socket_addr`](https://doc.rust-lang.org/src/std/net/addr.rs.html#936).
"Resolve" - that's a familiar word from the world of DNS; sounds like it could 
be what we're looking for. Let's dig in:

```Rust
fn resolve_socket_addr(lh: LookupHost) -> io::Result<vec::IntoIter<SocketAddr>> {
    let p = lh.port();
    let v: Vec<_> = lh
        .map(|mut a| {
            a.set_port(p);
            a
        })
        .collect();
    Ok(v.into_iter())
}
```

Uh... huh... looks like a simple transform of the `lh` input argument
into the result. Also, I can't help noticing, this function returns a `Result`
(which is what we expect! ... we're looking for something that can reach out
and hit remote DNS servers, over the network after all), but this function is
infallible: it can only return `Ok(...)` at the end.

Just one thing: our `str` has become `lh: LookupHost`, and it appears to have all
the answers. Where did that come from? Let's back up to our `to_socket_addrs`
function - notice the last line:

```Rust
resolve_socket_addr(self.try_into()?)
```

That `try_into` is where the magic's happening. The name `try_...` and the 
`Result` return type (indicated by the `?`) are good hints that _something's_
going on here. On first pass, I assumed it was "just" a straightforward
conversion into another more convenient type for the lookup, but in fact,
`try_into` _is_ the lookup, as we'll see. Let's skip over to the
implementation... but... how exactly do we find that? No mention of `LookupHost`
on [doc.rust-lang.org's normally trusty search](https://doc.rust-lang.org/std/index.html?search=lookuphost).

## further into the source - standard library internals

So we need to look deeper than what's publicly exposed by the standard
library, into the implementation details. 

This is good news: it means we're getting to the heart of it.
"Where do host names come from?" is exactly the sort of thing that we want
the standard library to take care of for us, so we knew we'd have to look
behind the curtain at some point.

Okay, enough narrative, let's see some code. 

We'll shift over to the Rust sources: [here's](https://github.com/rust-lang/rust/blob/master/src/libstd/sys_common/net.rs#L163)
the `TryFrom` implementation for `LookupHost` we were after (this allows us to
use `try_into` to `to_socket_addrs` above). We'll skip over that as it just splits 
the `str` up into a `(host, port)` pair, and calls `try_into()` again, which is
defined immediately below in the same file:

```Rust
impl<'a> TryFrom<(&'a str, u16)> for LookupHost {
    type Error = io::Error;

    fn try_from((host, port): (&'a str, u16)) -> io::Result<LookupHost> {
        init();

        let c_host = CString::new(host)?;
        let mut hints: c::addrinfo = unsafe { mem::zeroed() };
        hints.ai_socktype = c::SOCK_STREAM;
        let mut res = ptr::null_mut();
        unsafe {
            cvt_gai(c::getaddrinfo(c_host.as_ptr(), ptr::null(), &hints, &mut res))
                .map(|_| LookupHost { original: res, cur: res, port })
        }
    }
}
```

Ah hah! Unsafe code! With a strong whiff of FFI about it! We're getting to the
good stuff now. That call to [`getaddrinfo`](https://linux.die.net/man/3/getaddrinfo))
is the final step that takes us into the `libc` - which is, ultimately, where 
the sausage gets made. 

There is one more hop here:

```Rust
c::getaddrinfo(...)
```

It's natural to read that as "call `getaddrinfo` from a C library", but `c` is
just a Rust namespace like any other, so this function is being imported from
somewhere. Scroll up to the top, and you'll see it comes from:

```Rust
use crate::sys::net::netc as c;
```

`sys` is where the Rust standard library keeps its platform-specific code, so
the implementation will depend on the current platform. On _unix_ platforms,
`sys::net::netc` is [defined as](https://github.com/rust-lang/rust/blob/master/src/libstd/sys/unix/net.rs#L17) backing on to `libc`:

```Rust
pub extern crate libc as netc;
```

... but that needn't be the case everywhere - for example on the [wasi platform](https://github.com/rust-lang/rust/blob/master/src/libstd/sys/wasi/net.rs#L376)
`netc` is defined as a native Rust module, with no `libc` in sight. 

Okay, that's enough technicalities: for our purposes, this `c::getaddrinfo` is
a call through to `libc`.

## quick asside on FFI and `libc`

FFI is short for ["foreign function interface"](https://doc.rust-lang.org/book/ch19-01-unsafe-rust.html?highlight=ffi#using-extern-functions-to-call-external-code). 
Rust leans on existing libraries C for a whole bunch of functionality, and in 
this case, for functionality built into `libc`. When languages call into other
languages that exist outside their own ecosystem (in this case, Rust to C), 
that's FFI.

`libc` is a widely-scoped C library that's provided as part of POSIX. It
handles all sorts of common functionality - hostname resolution is one area,
but in fact Rust delegates to `libc` for pretty much anything that touches the 
network, opens files, spawns new processes, ... in short: anything that 
requires making System Calls. Rust isn't alone in leaning on a `libc` for 
this; most programming languages eventually call through to `libc` (Python, 
Ruby, and Java all do, for example). That's good news; from this point on, what
we learn translates well across languages. Go is a notable exception here - 
but more on that later.

POSIX only specifies the _interface_ that `libc` has to provide, and
there are several implementations. On Linux, the most common implementation of
`libc` is [`glibc`](https://www.gnu.org/software/libc/), so that's what we'll 
talk about next, but others do exist. Again, more on that later.

## rephrasing the question

Now that we've established that we call through to `libc`'s `getaddrinfo`, and
that `libc` is commonly implemented by `glibc`, we can rephrase the question as:

> how does `glibc` implement `getaddrinfo`?

Before we dig into that, let's take stock. We've arrived at the conclusion that
we call through to the commonly used `libc`. We've said that pretty much every
language does the same thing. That means that the answers we're looking for 
will be applicable pretty much everywhere, including in C programs. There's
going to be some prior art on this.

## what does the internet say?

What do we know already about how our programs find hosts?
- _Eventually_ we know that we'll connect to a DNS server and make a request
- We probably know that at some stage the `hosts` file comes into it
- We know that DNS configuration is system-wide. When we connect to a network,
  our computers automatically figure out what DNS to use (or we configure it
  ourselves).
- Maybe some caching?!
- `localhost` is always the loopback interface - presumably we don't ask the
  DNS servers for that?
- There's something special about the `.local` domain?

Let's start with the global configuration of DNS servers. If we search for
"dns linux" we get a few interesting hits:
- There's [this article](https://support.rackspace.com/how-to/changing-dns-settings-on-linux/)
  pointing us to [resolv.conf](https://www.man7.org/linux/man-pages/man5/resolv.conf.5.html),
  which contains DNS configuration.
- The [Arch Wiki entry on domain name resolution](https://wiki.archlinux.org/index.php/Domain_name_resolution#Overwriting_of_/etc/resolv.conf)
  tells us that `resolv.conf` is generally overwritten by network managers like
  GNOME's aptly-named [NetworkManager](https://wiki.gnome.org/Projects/NetworkManager).
  That's re-assuring, as it joins the dots from the familiar configuration 
  options we get when we go to our environment's Network Settings screen, to 
  something that `libc` and friends can find.
- The same page also tells us that `glibc`'s implementation of `getaddrinfo` is
  backed by [`NSS`](https://en.wikipedia.org/wiki/Name_Service_Switch), which
  reads from [nsswitch.conf](https://man7.org/linux/man-pages/man5/nsswitch.conf.5.html). Hopefully we'll be able to see where that happens.

Let's have a look in those files and see what we can see. Here's the contents
of my `resolv.conf`:

```
j@.. ~/s/dns-experiment> cat /etc/resolv.conf 
# Generated by NetworkManager
nameserver 192.168.1.2
nameserver 8.8.8.8
nameserver 8.8.4.4
```

It checks out:
- `192.168.1.2` is the address of my local DNS resolver (a [pihole](https://pi-hole.net/),
  if you're interested)
- `8.8.8.8` and `8.8.4.4` are Google's public DNS, which I have configured as
  my fallback DNS. 
- The file mentions that it's generated by `NetworkManager`, which tallies up
  with what we saw above.

And here's the relevant section of my local `nsswitch.conf` (used by `glibc`):
```
# Generated by authselect on Thu Jun 18 09:08:01 2020
# Do not modify this file manually.

--- snip ---

hosts:      files mdns4_minimal [NOTFOUND=return] dns myhostname
```

`authselect` is something new - looks like on my Fedora system there's one more
layer of machinary involved in generating this file, but let's not worry about
that for now. 

The interesting part is what it has to say about host resolution: it mentions
`dns`, but also three sources of naming information that we haven't discussed
before: `files`, `mdns4_minimal`, and `myhostname`. 

The `hosts` line is read in order.
- `files` means what it sounds like: it tells `glibc` to look in local files.
  For host lookup, that means `/etc/hosts`.
- `mdns4_minimal` doesn't have a special meaning. The `man` page says this
  about how these names are translated into meaning:

  > Libraries called /lib/libnss_SERVICE.so.X will provide the named SERVICE.

  So, `mdns4_minimal` will be implemented by a separate dynamic library called
  `libnss_mdns4_minimal.so.2` (the `2` depends on the `glibc` version; for
  `2.1` and above it's `2`). A small note here; for me these files are in
  `/lib64`, not `/lib`.

  So, what _is_ `mdns4_minimal`? Well, it comes from the `nss-mdns` project,
  which provides [Multicast DNS](https://en.wikipedia.org/wiki/Multicast_DNS#Protocol_overview),
  a system used for host discover on local networks. This is what allows us to 
  resolve `.local` hostnames.
- `dns` gets a shout out as explicitly allowed in the docs, but I notice the
  presence of `libnss_dns.so.2` in my `/lib64` directory. Now that I look,
  there's a `libnss_files.so.2` in there too. I guess we'll see how
  that fits together when we dig into `glibc`.
- `myhostname` is a `systemd` module ([docs](https://www.freedesktop.org/software/systemd/man/libnss_myhostname.so.2.html))
  that will resolve the local system's hostname.

Something that tripped me up: `[NOTFOUND=return]`.
It says that if `mdns4_minimal` returns `NOTFOUND`, then we can stop early and not
bother with querying `dns` or `myhostname`. My question was: since 
`mdns4_minimal` is only going to be able to find `.local` hosts, doesn't this
action prevent us from moving on to use real `dns` for everything else? In fact
`NOTFOUND` is only returned if `mdns4_minimal` both believes itself responsible for 
looking up the name _and_ then fails to do so. Otherwise,
`mdns4_minimal` [will return](https://github.com/lathiat/nss-mdns/blob/master/src/nss.c#L130)
an `UNAVAIL` status, and resolution will continue.

That gives us a clearer picture of what we're expecting to find `glibc`. Let's
finally proceed.

## what does `glibc` do?

### read the code

In this section, we'll take a brief look at the sources of `glibc`, but we
won't dig through all the details.

[Here's](https://sourceware.org/git/?p=glibc.git;a=blob;f=sysdeps/posix/getaddrinfo.c;h=ed04e564f9711298fd7bb14fd53f13c8053694d2;hb=HEAD#l2147)
`glibc`'s implementation of `getaddrinfo`. It's about 350 lines long, and
you're welcome to pick through, but it delegates the real work of lookup to
another function, `gaih_inet` ([further up](https://sourceware.org/git/?p=glibc.git;a=blob;f=sysdeps/posix/getaddrinfo.c;h=ed04e564f9711298fd7bb14fd53f13c8053694d2;hb=HEAD#l327)
in the same file). That function's just shy of 800 lines, and we're along 
the right lines, since references to some of the concepts we were looking 
at above are creeping in now:
- a reference to `/etc/nsswitch.conf` in the comments
- calls through to functions with `nss` in the name, like `__nss_database_lookup2`
  and `__nss_lookup_function`. 
  
Notice that `__nss_lookup_function` takes the name of another function as an
argument. If we take a peek [the sources](https://sourceware.org/git/?p=glibc.git;a=blob;f=nss/nsswitch.c;hb=efedd1ed3d211941fc66d14ba245be3552b2616a#l392) 
for `__nss_lookup_function`, we'll see that it's responsible for dynamically
loading in further libraries - presumably this is what links us up to the
`libnss_SERVICE.so.X` files we saw above, eventually resolving the required
function with `__libc_dlsym`.

I don't know about you, but I find these sources a bit hard going; alongside
the business logic of looking up the various resolver functions, there's a lot
of housekeeping. 

Time for a change of tack.

### `strace` to the rescue

There is another way we can get to the bottom of what's going on inside the
loveable but inscrutable yak that is `glibc`: `strace`. If you haven't seen
`strace` before, then I promise you're going to love it: `strace` monitors all
System calls from and signals to a given process. 

We've mentioned System Calls in passing already when talking about the role
of `libc` in Rust - I said `libc` was used for:

> anything that touches the network, opens files, spawns new processes ...in 
> short - anything that requires making System Calls

So what exactly is a System Call? Well it's anything that you need to ask the
Kernel to do for you. Anything that touches the Real World.

So, `strace` provides us with a straightforward way to answer the question of
what a given program does - really _does_ - without looking at the source
code. So let's put together a simple program that will call `getaddrinfo`,
and `strace` it.

You can find the sources [here](https://git.sr.ht/~jelford/hostname-lookup/tree/master/src/main.rs), 
but it boils down to a call to `getaddrinfo`, then printing out the results.

We can run it through `strace` with:
```bash
target=x86_64-unknown-linux-gnu
cargo build -q --target ${target}
strace -e desc,network,file -o "strace-${target}.log" -f "./target/${target}/debug/dns-experiment" www.jameselford.com

# ipv4 185.199.108.153: jelford.github.io
# ipv4 185.199.110.153: <unknown canonical addr>
# ipv4 185.199.109.153: <unknown canonical addr>
# ipv4 185.199.111.153: <unknown canonical addr>
```

The `strace` incantation is:
- `-e desc,network,file`: report any system calls relating to file descriptors,
  the network, and files.
- `-o`: put the trace into a separate file, rather than interweaving with the
  output of the process being traced
- `-f`: follow any process forks. 

The program's output is telling us that `www.jameselford.com` resolves to
`185.199.108.153`, which has a canonocal name of `jelford.github.io` (so, now
you know that this blog is hosted on [GitHub Pages](https://pages.github.com/)).
But we could have got that from `dig`; we came here for the `strace` output!

You can see it [here](https://git.sr.ht/~jelford/hostname-lookup/blob/master/strace-x86_64-unknown-linux-gnu.log).
It makes for pretty dense reading, so I'll surface the interesting stuff,
snipping out most of the calls I don't think are relevant. 

Let's start at the top:

```
139893 execve("./target/x86_64-unknown-linux-gnu/debug/dns-experiment", ["./target/x86_64-unknown-linux-gn"..., "www.jameselford.com"], 0x7ffd292c7e10 /* 46 vars */) = 0
139893 access("/etc/ld.so.preload", R_OK) = -1 ENOENT (No such file or directory)
139893 openat(AT_FDCWD, "/etc/ld.so.cache", O_RDONLY|O_CLOEXEC) = 3
139893 openat(AT_FDCWD, "/lib64/libdl.so.2", O_RDONLY|O_CLOEXEC) = 3
139893 read(3, "\177ELF\2\1\1\0\0\0\0\0\0\0\0\0\3\0>\0\1\0\0\0p\"\0\0\0\0\0\0"..., 832) = 832
139893 fstat(3, {st_mode=S_IFREG|0755, st_size=36800, ...}) = 0
139893 mmap(NULL, 8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) = 0x7fda8e374000
139893 mmap(NULL, 24688, PROT_READ, MAP_PRIVATE|MAP_DENYWRITE, 3, 0) = 0x7fda8e36d000
139893 mmap(0x7fda8e36f000, 8192, PROT_READ|PROT_EXEC, MAP_PRIVATE|MAP_FIXED|MAP_DENYWRITE, 3, 0x2000) = 0x7fda8e36f000
139893 mmap(0x7fda8e371000, 4096, PROT_READ, MAP_PRIVATE|MAP_FIXED|MAP_DENYWRITE, 3, 0x4000) = 0x7fda8e371000
139893 mmap(0x7fda8e372000, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_FIXED|MAP_DENYWRITE, 3, 0x4000) = 0x7fda8e372000
139893 mmap(0x7fda8e373000, 112, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS, -1, 0) = 0x7fda8e373000
139893 close(3)                         = 0
```

I did mention that it was going to be dense.

A note on how to read this: 
- the first word is the name of the systemcall. 
- following are all the arguments, in brackets
- then the return value at the end, after an `=`

`strace` tries hard to convert arguments and return values into the 
corresponding names like you'd find the in C header files, and even gives us
the text description for errors. Helpful, right?

So, what's happening in this snippet is that a shared library (`libdl` 
in this case) is opened, read, mapped into memory (notice `PROT_EXEC` - 
which maps the libraries into _executable_ memory, which is what we need if 
we're going to run code from them!), then closed. This is the first of many 
shared libs that get loaded at the start of the process. The whole process is
easy to recognize in the trace once you know what's going on, so, but I'll
spare you the housekeeping for the rest:

```
139893 openat(AT_FDCWD, "/lib64/libpthread.so.0", O_RDONLY|O_CLOEXEC) = 3
139893 openat(AT_FDCWD, "/lib64/libgcc_s.so.1", O_RDONLY|O_CLOEXEC) = 3
139893 openat(AT_FDCWD, "/lib64/libc.so.6", O_RDONLY|O_CLOEXEC) = 3
```

At this point we've got the following libraries loaded:

- `libdl`: will be used later for dynamically loading more shared libraries.
- `libpthread`: threading support. 
- `libgcc`: GCC runtime support. Rust doesn't compile with GCC, but `glibc`
  does, so our executable must load `libgcc`. Even C has _some_ runtime.
- `libc`: this is `glibc`, on my system, and by now we were expecting to see it

Let's move on. We're looking to see what `glibc` does next:  

```
139893 socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0) = 3
139893 connect(3, {sa_family=AF_UNIX, sun_path="/var/run/nscd/socket"}, 110) = -1 ENOENT (No such file or directory)
```

In this section, we're attempting to connect to [`nscd`](https://linux.die.net/man/8/nscd),
the "Network service cache daemon". It doesn't appear to be running, and
`systemctl` denies all knowledge too. I've got local `man` pages for `nscd`
but there's no sign of it on my `$PATH` and a quick `rg` on a copy of the POSIX
spec doesn't show anything up. `locate '*ncsd*'` turns up a few hits in `man`
pages and embedded in flatpaks, but nothing in the main system. I guess this 
isn't important anymore (just maintained in `glibc` for backwards 
compatibility). I'd love to hear about it if you know differently.

```
139893 openat(AT_FDCWD, "/etc/nsswitch.conf", O_RDONLY|O_CLOEXEC) = 3
139893 read(3, "# Generated by authselect on Thu"..., 4096) = 2556
```

Okay, so this is loading `nsswitch.conf`, the first of the config files we 
mentioned earlier. Looks like `glibc` looks at that first, to decide what 
it'll do next. This ties up with what we're expecting; so far so good. We can 
see the `read` call getting the familiar "generated by authselect..." header 
that we saw before.

Where does it go from there? As a quick reminder, `nsswitch.conf` listed:

```
hosts:      files mdns4_minimal [NOTFOUND=return] dns myhostname
```

So, we're expecting it to consult the `files` source first. Back to the trace:

```
139893 openat(AT_FDCWD, "/etc/host.conf", O_RDONLY|O_CLOEXEC) = 3
139893 read(3, "multi on\n", 4096)      = 9
```

Not quite what we were expecting. Here's the contents of my `host.conf`:

```
j@.. ~/s/dns-experiment> cat /etc/host.conf
multi on
```

[`man host.conf`](https://www.man7.org/linux/man-pages/man5/host.conf.5.html)
tells us that this line tells the resolver value some details of how to
interpret `/etc/hosts`. Okay. So we're still expecting to `openat("/etc/hosts")` 
soon...

```
139893 openat(AT_FDCWD, "/etc/resolv.conf", O_RDONLY|O_CLOEXEC) = 3
139893 read(3, "# Generated by NetworkManager\nna"..., 4096) = 91
```

We saw earlier that this file specifies our DNS nameservers. We shouldn't need
those yet as we haven't even gotten to `/etc/hosts` - but another look at
[`man resolv.conf`](https://www.man7.org/linux/man-pages/man5/resolv.conf.5.html)
reveals other options that may be relevant to the resolution process. Onwards...

```
139893 openat(AT_FDCWD, "/lib64/libnss_files.so.2", O_RDONLY|O_CLOEXEC) = 3
139893 openat(AT_FDCWD, "/etc/hosts", O_RDONLY|O_CLOEXEC) = 3
```

Here we are - the `hosts` file. `files`, along with `dns`, got its own special
mention in the `nsswitch.conf` man page, but it looks like this source is read
using the same general mechanism as the other sources; load an appropriately
named `.so` and hook into that. I won't drag you through the sources, but
`glibc` does ship a module called `nss_files`, and sure enough, [it knows](https://sourceware.org/git/?p=glibc.git;a=blob;f=nss/nss_files/files-init.c;h=b36220e48022da5a80c1a800e8600592e6af41d0;hb=8cde977077b3568310c743b21a905ca9ab286724#l33)
where to look for the `hosts` file.

My `hosts` file is empty, so we won't find anything interesting in there. Now
we're expecting to see our application move on to `mdns4_minimal`, and `dns`.
It should never get to the `myhostname` since - assuming you're reading this,
we're going ot find a DNS entry for `www.jameselford.com` from DNS.

Here's `mdns4_minimal`:
```
139893 openat(AT_FDCWD, "/lib64/libnss_mdns4_minimal.so.2", O_RDONLY|O_CLOEXEC) = 3
```

Nothing to see here. We're not looking for a `.local` domain, so that's just
going to return without further ado. Next is DNS:

```
139893 openat(AT_FDCWD, "/lib64/libresolv.so.2", O_RDONLY|O_CLOEXEC) = 3
139893 openat(AT_FDCWD, "/lib64/libnss_dns.so.2", O_RDONLY|O_CLOEXEC) = 3
139893 socket(AF_INET, SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK, IPPROTO_IP) = 3
139893 setsockopt(3, SOL_IP, IP_RECVERR, [1], 4) = 0
139893 connect(3, {sa_family=AF_INET, sin_port=htons(53), sin_addr=inet_addr("192.168.1.2")}, 16) = 0
139893 poll([{fd=3, events=POLLOUT}], 1, 0) = 1 ([{fd=3, revents=POLLOUT}])
139893 sendto(3, "\271\253\1\0\0\1\0\0\0\0\0\0\3www\vjameselford\3com"..., 37, MSG_NOSIGNAL, NULL, 0) = 37
139893 poll([{fd=3, events=POLLIN}], 1, 5000) = 1 ([{fd=3, revents=POLLIN}])
...
139893 recvfrom(3, "\271\253\201\200\0\1\0\5\0\0\0\0\3www\vjameselford\3com"..., 1024, 0, {sa_family=AF_INET, sin_port=htons(53), sin_addr=inet_addr("192.168.1.2")}, [28->16]) = 132
...
139893 openat(AT_FDCWD, "/etc/gai.conf", O_RDONLY|O_CLOEXEC) = -1 ENOENT (No such file or directory)
```

That's more interesting! `libresolve` is also part of the wider `glibc` package 
(description [here](https://sourceware.org/git/?p=glibc.git;a=blob;f=resolv/README;h=514e9bb617e710f16126c1474561965a2b35653d;hb=8cde977077b3568310c743b21a905ca9ab286724)).
We can use [`ldd`](https://www.man7.org/linux/man-pages/man1/ldd.1.html) to see
that `lib_resolve` is brought in as a dependency of `libnss_dns`:
```
j@.. ~/s/dns-experiment> ldd /lib64/libnss_dns.so.2 
	linux-vdso.so.1 (0x00007ffda434b000)
	libresolv.so.2 => /lib64/libresolv.so.2 (0x00007f50fc674000)
	libc.so.6 => /lib64/libc.so.6 (0x00007f50fc4aa000)
	/lib64/ld-linux-x86-64.so.2 (0x00007f50fc6b3000)
```

We can also see that, finally, we're making a DNS call:
- `connect(... sin_addr=inet_addr("192.168.1.2")...)` - that's a call out to my
  DNS server (that we saw configured in `/etc/resolv.conf`).
- `poll` is used to wait for the socket to be "ready" - whether that's to
  finish the business of connecting, sending data, or reading data. `poll` is
  used in asynchronous IO - which is a big topic, so I'll just draw attention
  to the `SOCK_NONBLOCK` option passed when the socket is initialized. This is 
  what configures the socket to work in non-blocking (asynchronous) mode.
- `sendto` and `recvfrom` - these are the calls that actually send our DNS
  requests out to the world, then read the results back.

The final attempt to read [`gai.conf`](https://www.man7.org/linux/man-pages/man5/gai.conf.5.html)
allows some customization of the order with which results are returned from
`getaddrinfo`. 

Great, we're done! Well... not quite. 

Before our application can print out the results we get... this...:

```
139893 socket(AF_NETLINK, SOCK_RAW|SOCK_CLOEXEC, NETLINK_ROUTE) = 3
139893 getsockname(3, {sa_family=AF_NETLINK, nl_pid=139893, nl_groups=00000000}, [12]) = 0

# Params truncated
139893 sendto(3, { {len=20, type=RTM_GETADDR, flags=NLM_F_REQUEST|NLM_F_DUMP, seq=1594996449, pid=0}, {ifa_family=AF_UNSPEC, ...} }, 20, 0, {sa_family=AF_NETLINK, nl_pid=0, nl_groups=00000000}, 12) = 20
139893 recvmsg(3, ...) = 252     
139893 recvmsg(3, ...) = 72      
139893 recvmsg(3, ...) = 20
...
139893 socket(AF_INET, SOCK_DGRAM|SOCK_CLOEXEC, IPPROTO_IP) = 3
139893 connect(3, {sa_family=AF_INET, sin_port=htons(0), sin_addr=inet_addr("185.199.108.153")}, 16) = 0
139893 getsockname(3, {sa_family=AF_INET, sin_port=htons(39670), sin_addr=inet_addr("10.150.1.131")}, [28->16]) = 0
139893 connect(3, {sa_family=AF_UNSPEC, sa_data="\0\0\0\0\0\0\0\0\0\0\0\0\0\0"}, 16) = 0
139893 connect(3, {sa_family=AF_INET, sin_port=htons(0), sin_addr=inet_addr("185.199.110.153")}, 16) = 0
139893 getsockname(3, {sa_family=AF_INET, sin_port=htons(41427), sin_addr=inet_addr("10.150.1.131")}, [28->16]) = 0
139893 connect(3, {sa_family=AF_UNSPEC, sa_data="\0\0\0\0\0\0\0\0\0\0\0\0\0\0"}, 16) = 0
139893 connect(3, {sa_family=AF_INET, sin_port=htons(0), sin_addr=inet_addr("185.199.109.153")}, 16) = 0
139893 getsockname(3, {sa_family=AF_INET, sin_port=htons(46916), sin_addr=inet_addr("10.150.1.131")}, [28->16]) = 0
139893 connect(3, {sa_family=AF_UNSPEC, sa_data="\0\0\0\0\0\0\0\0\0\0\0\0\0\0"}, 16) = 0
139893 connect(3, {sa_family=AF_INET, sin_port=htons(0), sin_addr=inet_addr("185.199.111.153")}, 16) = 0
139893 getsockname(3, {sa_family=AF_INET, sin_port=htons(54403), sin_addr=inet_addr("10.150.1.131")}, [28->16]) = 0
...
```

`AF_NETLINK` is a socket family that provides socket-based communication 
between the Kernel and Userspace ([man pages](https://www.man7.org/linux/man-pages/man7/netlink.7.html)),
and the `NETLINK_ROUTE` family being connected to is responsible for routing.
The truncated `recvmsg` calls after we establish the `AF_NETLINK` connection
contain a lot of info about my local network interfaces - interface names, IP
addresses, that sort of thing. I've truncated them just because they don't play
well with the blog's markdown parser. All of this happens after the DNS lookup
has finished, [here](https://sourceware.org/git/?p=glibc.git;a=blob;f=sysdeps/posix/getaddrinfo.c;hb=8cde977077b3568310c743b21a905ca9ab286724#l2293):

```c
/* Now we definitely need the interface information.  */
if (! check_pf_called)
    __check_pf (&seen_ipv4, &seen_ipv6, &in6ai, &in6ailen);
```

Having gotten information about the various network interfaces, `glibc` goes
ahead and "`connects`" to the IP addresses it got back from the host lookup
procedure. There's no real _connecting_ taking place here though; the socket is
set up with type `SOCK_DGRAM`, which means it's _connectionless_. The source
has this comment:
```
/* We overwrite the type with SOCK_DGRAM since we do not
   want connect() to connect to the other side.  If we
   cannot determine the source address remember this
   fact.  */
```

The game here seems to be to do just enough to call `getsockname`, and use the
information it gets back to sort the final output list of `getaddrinfo`. Why 
does it go to so much effort to sort the output list? There aren't really too 
many hints right in the code, but let's rewind to the `man` page for 
`getaddrinfo`, which mentions:

> Normally, the application should try using the addresses in the order in which they are returned.
> The sorting function used within getaddrinfo() is defined in RFC 3484

[Here's](https://www.ietf.org/rfc/rfc3484.txt) that RFC, which I don't intend
to get into, but I will note that it covers sorting both on _destination_
address (i.e. the host we intend to connect _to_), and the _source_ address
(the interface that we connect _from_). I don't see that _source_ address is
included in the return value of `getaddrinfo`, but it looks like it _is_ used
as part of determining the order of results. 

Okay, _finally_ we can print out the results that we saw at the top of the 
section - a series of`write` calls on file descriptor `1` (which is stdout):

```
139893 write(1, "ipv4 185.199.108.153: jelford.gi"..., 40) = 40
139893 write(1, "ipv4 185.199.110.153: <unknown c"..., 47) = 47
139893 write(1, "ipv4 185.199.109.153: <unknown c"..., 47) = 47
139893 write(1, "ipv4 185.199.111.153: <unknown c"..., 47) = 47
139893 +++ exited with 0 +++
```

Phew! That was quite a journey. 

## summary so far

Let's wrap up what we've seen so far:
- Rust's standard library calls out (after jumping through a few hoops of its own)
  to `libc`'s `getaddrinfo`. 
- Assuming that `libc` is `glibc`, it then:
  - checks for `nscd`, a caching daemon that doesn't seem to exist on my host
  - reads `nsswitch.conf` to determine what to do next. Based on that it...
  - checks in `host.conf` and `resolv.conf` to determine how to interpret what 
    it finds when it...
  - reads the `hosts` file (in case of statically configured naming info),
  - tries `mdns4_minimal` (in case of `.local` domains),
  - queries DNS (using the DNS server it found earlier in `resolv.conf`),
  - sorts the results according RFC-3484, using information about local
    interfaces, and...
  - finally returns a list of host lookup results

That gives us a pretty clear picture of where to look if we want to configure
or understand how hostname lookup is working on our system:
- `/etc/nsswitch.conf` is used to determine how to look up hosts (maintained by
  `authselect`)
- `/etc/hosts` allows us to configure a static set of names
- `.local` domains have their own special `mdns4` thing going on,
- and finally, `/etc/resolv.conf` lists our name servers 
  (maintained by `NetworkManager`)

Simple. This feels like a good place to stop, so... let's just look at one more
thing...

## what if we're not using `glibc`?

Now... `getaddrinfo` is specified by POSIX, but the rest of the details are not.
`nss` is a `glibc` (well... inspired by Sun) invention, so everything from that
point on is implementation dependant.

This matters when:
- We're not using `glibc` as our `libc`.
  - Other 'nixes, like BSD, come with their own `libc`'s. 
  - The normal alternative on _linux_ is `musl`,  which, apart from being the
    default choice when producing static binaries in Rust, is the system-wide
    `libc` for some Linux distributions - in particular [alpine](https://www.alpinelinux.org/),
    a popular choice for container base images.
- We're not even using `libc`. Go makes its syscalls directly (well, on some
  platforms - including Linux) - what about other fundamental `libc`
  functions like `getaddrinfo`?

Let's round this out with a quick look an `strace` generated by the same Rust
program as above, but this time targetting `musl`:

```bash
target=x86_64-unknown-linux-musl
cargo build -q --target ${target}
strace -e desc,network,file -o "strace-${target}.log" -f "./target/${target}/debug/dns-experiment" www.jameselford.com

# ipv4 185.199.110.153: jelford.github.io
# ipv4 185.199.109.153: jelford.github.io
# ipv4 185.199.108.153: jelford.github.io
# ipv4 185.199.111.153: jelford.github.io
```

Right away we notice that we get different results with `musl` vs `glibc`!
`musl` has come back with canonical names for all results, whereas `glibc`
didn't. The ordering is also different, though we'd have to look into RFC-3484
to know what to make of that.

You can find the full log of the `strace` [here](https://git.sr.ht/~jelford/hostname-lookup/blob/master/strace-x86_64-unknown-linux-musl.log).
The first thing you'll notice is that it's a _lot_ shorter than the 
`glibc`-based trace; 25 lines for the `musl` binary vs. 170 for the `glibc` one.

The first thing that's gone is all the shared library loading. That's about 50
lines from the start of the `glibc` trace. The next thing to note is that we're
not `strace`ing _everything_ here - just syscalls related to file descriptors,
network, and file operations. So, we shouldn't read too much into a line-count
comparison.

So, what does the `musl` version do? I'll summarize here, but encourage you to
look through the trace - it's only 25 lines!
- First up, we go straight to reading the `hosts` file
- Nothing there, so next we'll read `resolv.conf`.
- We use the nameservers we just found to issue DNS requests (`sendto` calls)
- Get data back (`recvfrom`)
- ... and we're done!

No `nsswitch.conf`, no lookup of information about local interfaces,
no checking `host.conf` for what we should do with multiple results from `hosts`
(perhaps that would get read if there was something _in_ there?).

Overall, it seems to just do a lot less... but there is one thing that stands
out - whereas `glibc` only issued _one_ DNS request (to my local resolver),
`musl` goes ahead and fires off requests to _all three_ configured nameservers.

That's a bit of a surprise to me. I use the local resolver for blocking trackers
and ads - what would happen if the remote nameservers returned first? Would
`musl` happily come back with the results and effectively ignore my blocker?

A whistlestop tour of the source:
- [`getaddrinfo`](https://git.musl-libc.org/cgit/musl/tree/src/network/getaddrinfo.c)
  calls into...
- [`__lookup_name`](https://git.musl-libc.org/cgit/musl/tree/src/network/lookup_name.c#n291)
  which calls into...
- `name_from_dns_search`, and `name_from_dns` in the same file, which finally
  calls out to...
- [`__res_msend_rc`](https://git.musl-libc.org/cgit/musl/tree/src/network/res_msend.c#n30),
  which issues DNS requests to all the nameservers in parallel.

_My_ reading of `name_from_dns` and `__res_msend_rc` is that the first reply 
from any server will win, but to be confident in that, I'd want to test it out;
one way would be to introduce a delay on my local resolver's responses... 

Something for another post perhaps.

## just one more thing...

Oh hey, `go` does its own thing! I mentioned before that it doesn't use `libc`
for its system calls. But what about using `getaddrinfo`, and the whole `nss`
thing? Well it turns out... it depends! 

The [`net` package documentation](https://golang.org/pkg/net/#pkg-overview) 
has some detail on this. I'll leave it there though, as I'm all `straced` out.

## copyright

One final note: all the source code snippets above (that don't contain my 
name) are taken from either:
- the Rust project's official sources, where [the original authors retain copyright](https://github.com/rust-lang/rust/blob/master/COPYRIGHT); or
- the `glibc` sources (copyright notice [here](https://sourceware.org/git/?p=glibc.git;a=blob;f=COPYING;h=d159169d1050894d3ea3b98e1c965c4058208fe1;hb=HEAD))
